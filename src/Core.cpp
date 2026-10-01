#include "Core.h"
#include "Console.h"
#include "Protocol.h"
#include "Service.h"

#include <ArduinoOTA.h>
#include <Preferences.h>
#include <WiFi.h>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <esp_attr.h>
#include <esp_core_dump.h>
#include <esp_ota_ops.h>
#include <esp_wifi.h>
#include <mbedtls/sha256.h>
#include <mutex>
#include <shared_mutex>

// ============================================================================
// Domain 1: Core Storage Pool & RTOS Global Buffers
// ============================================================================

// ── RTC Retention Fast SRAM Memory (Preserved across Software / WDT Reboots)
// ──
RTC_NOINIT_ATTR uint32_t rtc_magic;
RTC_NOINIT_ATTR uint32_t rtc_last_alive_ms[Config::Task::TASK_COUNT];
RTC_NOINIT_ATTR volatile uint32_t g_telnet_stage = 0;
RTC_NOINIT_ATTR uint32_t rtc_rescue_magic;
RTC_NOINIT_ATTR uint32_t rtc_crash_counter;
RTC_NOINIT_ATTR uint32_t rtc_clean_restart_magic;
RTC_NOINIT_ATTR RtcWarmCache rtc_warm_cache;

// ── System Lifecycle & Status Flags ──
std::atomic<bool> g_rescue_mode{false};
bool g_rollback_detected = false;

bool g_warm_cache_loaded = false;
uint8_t g_warm_cache_source = 0; // 0: None/Cold, 1: RTC SRAM, 2: NVS Flash
uint8_t g_warm_cache_restored_count = 0;
std::atomic<bool> g_warm_cache_dirty{false};
std::atomic<uint32_t> g_warm_cache_dirty_ms{0};

// ── Core Repositories & Metrics Trackers ──
DeviceRepository g_device_repo;
ControlDispatcher g_control_dispatcher;
PacketStatistics g_pkt_stats;
SystemMetricsTracker g_metrics;
TaskWdtMonitor g_wdt_monitor;

// ── Static FreeRTOS Queues & Storage Pools ──
StaticQueue_t g_ch1_ctrl_queue_buf, g_ch4_pass_queue_buf, g_ch1_vip_queue_buf;
uint8_t
    g_ch1_ctrl_storage[Config::Queue::POOL_SIZE_CONTROL * sizeof(StaticPacket)];
uint8_t g_ch4_pass_storage[Config::Queue::POOL_SIZE_CH4_PASS *
                           sizeof(StaticPacket)];
uint8_t g_ch1_vip_storage[Config::Queue::POOL_SIZE_VIP * sizeof(StaticPacket)];

QueueHandle_t g_ch1_control_queue = nullptr, g_ch1_vip_queue = nullptr;
QueueSetHandle_t g_ch1_queue_set = nullptr;
QueueHandle_t g_uart0_event_queue = nullptr, g_uart1_event_queue = nullptr,
              g_uart2_event_queue = nullptr;
QueueHandle_t g_ch4_passthrough_queue = nullptr;

EventGroupHandle_t g_system_event_group = nullptr;

SoftwareSerial g_doorphone_serial;
RuntimeConfig g_config{};

HubClientSlot g_hub_slots[Config::TCP::MAX_EW11_SLOTS];
SemaphoreHandle_t g_ch5_mutex = nullptr;
SemaphoreHandle_t g_ctrl_queue_mutex = nullptr;

// ── Static FreeRTOS Tasks & Stacks ──
StaticTask_t g_task_core1_ch1_buf, g_task_core1_slave_buf,
    g_task_core1_slave2_buf, g_task_core1_ch4_buf, g_task_core0_net_buf,
    g_telnet_task_buf;
StackType_t stackCore1Ch1[Config::Task::STACK_SIZE_CORE1],
    stackCore1Slave[Config::Task::STACK_SIZE_SLAVE],
    stackCore1Slave2[Config::Task::STACK_SIZE_SLAVE],
    stackCore1Ch4[Config::Task::STACK_SIZE_CH4],
    stackCore0Net[Config::Task::STACK_SIZE_CORE0],
    telnetTaskStack[Config::Task::STACK_SIZE_TELNET];
TaskHandle_t g_telnet_task_handle = nullptr, g_ch1_task_handle = nullptr,
             g_ch2_task_handle = nullptr, g_ch3_task_handle = nullptr,
             g_ch4_task_handle = nullptr, g_network_task_handle = nullptr;

uint32_t g_boot_start_ms = 0;
std::atomic<uint32_t> g_ch1_bus_ms{0};
SemaphoreHandle_t g_uart0_mutex = nullptr, g_uart1_mutex = nullptr,
                  g_uart2_mutex = nullptr, g_tracer_sem = nullptr;
Ch1StateMetrics g_ch1_state_metrics;

// [Pillar 6] g_config R/W 보호: std::shared_mutex 기반 다중 동시 읽기 / 배타적
// 쓰기
std::shared_mutex g_config_rw;
// 하위 호환: g_config_mux는 ISR 컨텍스트 전용으로 유지
portMUX_TYPE g_config_mux = portMUX_INITIALIZER_UNLOCKED;

std::atomic<bool> g_config_dirty{false}, g_ota_in_progress{false},
    g_initial_caching_complete{false}, g_probe_convergence_reset{false};
WifiFallbackGuard g_wifi_guard;
Config::Doorphone::DoorphoneState g_doorphone_state{};
CoreDumpInfo g_coredump_info;
Config::Doorphone::FramingTracker g_doorphone_tracker;

// ============================================================================
// Domain 2: Warm Cache & RTC Retention Engine
// ============================================================================

namespace {
// Save와 Restore가 절대 동시 실행되지 않으므로 단일 정적 봉투를 공유하여 RAM
// 절감
static NvsEnvelope<RtcWarmCache> s_warm_cache_env;
} // anonymous namespace

void Cache_SaveToRtc() {
  memset(&rtc_warm_cache, 0, sizeof(rtc_warm_cache));
  rtc_warm_cache.magic = RTC_MAGIC_WARM_CACHE;
  rtc_warm_cache.count =
      static_cast<uint8_t>(g_polling_targets.getWarmCacheEntries(
          rtc_warm_cache.entries, PollingTargetRegistry::MAX_TARGETS));
  if (rtc_warm_cache.count > 0) {
    rtc_warm_cache.crc32 =
        FastCrc32(reinterpret_cast<const uint8_t *>(rtc_warm_cache.entries),
                  sizeof(RtcWarmCacheEntry) * rtc_warm_cache.count);
  }
}

void Cache_SaveToNvs() {
  Cache_SaveToRtc();
  if (rtc_warm_cache.count > 0) {
    Preferences p;
    if (p.begin("wp_wc", false)) {
      s_warm_cache_env.payload = rtc_warm_cache;
      s_warm_cache_env.seal();
      p.putBytes("wc_data", &s_warm_cache_env, sizeof(s_warm_cache_env));
      p.end();
      ::Serial.printf(
          "[WARM CACHE] Synced %u targets to NVS Flash snapshot.\r\n",
          rtc_warm_cache.count);
    }
  }
  g_warm_cache_dirty.store(false, std::memory_order_release);
}

void Cache_RestoreOnBoot() {
  uint32_t now = millis();
  esp_reset_reason_t reason = esp_reset_reason();

  // 1순위: RTC FAST SRAM에서 0ms 즉시 복원
  if (reason != ESP_RST_POWERON &&
      rtc_warm_cache.magic == RTC_MAGIC_WARM_CACHE &&
      rtc_warm_cache.count > 0 &&
      rtc_warm_cache.count <= PollingTargetRegistry::MAX_TARGETS) {
    uint32_t computed_crc =
        FastCrc32(reinterpret_cast<const uint8_t *>(rtc_warm_cache.entries),
                  sizeof(RtcWarmCacheEntry) * rtc_warm_cache.count);
    if (computed_crc == rtc_warm_cache.crc32) {
      g_polling_targets.loadFromWarmCache(rtc_warm_cache.entries,
                                          rtc_warm_cache.count, now);
      g_warm_cache_loaded = true;
      g_warm_cache_source = 1;
      g_warm_cache_restored_count = rtc_warm_cache.count;
      ::Serial.printf("[WARM CACHE] Restored %u targets from RTC Fast SRAM "
                      "(0ms delay)!\r\n",
                      rtc_warm_cache.count);
      return;
    }
  }

  // 2순위: NVS Flash 스냅샷에서 무결성 검증 후 복원
  Preferences p;
  if (p.begin("wp_wc", true)) {
    if (p.isKey("wc_data")) {
      size_t len = p.getBytesLength("wc_data");
      if (len == sizeof(s_warm_cache_env) &&
          p.getBytes("wc_data", &s_warm_cache_env, sizeof(s_warm_cache_env)) ==
              sizeof(s_warm_cache_env)) {
        if (s_warm_cache_env.verify() && s_warm_cache_env.payload.count > 0 &&
            s_warm_cache_env.payload.count <=
                PollingTargetRegistry::MAX_TARGETS) {
          uint32_t computed_crc = FastCrc32(
              reinterpret_cast<const uint8_t *>(
                  s_warm_cache_env.payload.entries),
              sizeof(RtcWarmCacheEntry) * s_warm_cache_env.payload.count);
          if (computed_crc == s_warm_cache_env.payload.crc32) {
            g_polling_targets.loadFromWarmCache(
                s_warm_cache_env.payload.entries,
                s_warm_cache_env.payload.count, now);
            g_warm_cache_loaded = true;
            g_warm_cache_source = 2;
            g_warm_cache_restored_count = s_warm_cache_env.payload.count;
            ::Serial.printf(
                "[WARM CACHE] Restored %u targets from NVS Flash snapshot!\r\n",
                s_warm_cache_env.payload.count);
            p.end();
            return;
          }
        }
      }
    }
    p.end();
  }

  g_warm_cache_loaded = false;
  g_warm_cache_source = 0;
  g_warm_cache_restored_count = 0;
  ::Serial.println(
      F("[WARM CACHE] Cold start initialized (No prior cache found)."));
}

void Cache_CheckNvsDebounce() {
  if (g_warm_cache_dirty.load(std::memory_order_acquire)) {
    uint32_t dirty_ms = g_warm_cache_dirty_ms.load(std::memory_order_relaxed);
    if (dirty_ms > 0 &&
        TimeUtils::isElapsed(dirty_ms,
                             Config::Timing::WARM_CACHE_NVS_DEBOUNCE_MS)) {
      Cache_SaveToNvs();
    }
  }
}

// ============================================================================
// Domain 3: Post-Mortem Crash Diagnostics & Reboot Logger
// ============================================================================

void LogManager::writeRebootLog(const char *reason) {
  if (!reason || strlen(reason) == 0)
    return;
  Preferences p;
  if (!p.begin("logs", false))
    return;

  uint32_t head = p.getUInt("log_head", 0) % MAX_LOG_ENTRIES;

  LogEntry entry = {};
  entry.timestamp = time(nullptr);
  strncpy(entry.reason, reason, sizeof(entry.reason) - 1);
  System_TakeSnapshot(entry.stats_snapshot, entry.hw_snapshot,
                      entry.stack_snapshot, entry.packet_stats_snapshot);

  static NvsEnvelope<LogEntry> new_env;
  new_env.payload = entry;
  new_env.seal();

  char key[16];
  snprintf(key, sizeof(key), "log_%u", (unsigned)head);
  p.putBytes(key, &new_env, sizeof(new_env));

  head = (head + 1) % MAX_LOG_ENTRIES;
  p.putUInt("log_head", head);

  uint32_t count = p.getUInt("count", 0);
  if (count < MAX_LOG_ENTRIES) {
    p.putUInt("count", count + 1);
  }
  p.end();
}

size_t LogManager::getLogCount() {
  Preferences p;
  if (!p.begin("logs", true))
    return 0;
  size_t c = p.getUInt("count", 0);
  p.end();
  return c > MAX_LOG_ENTRIES ? MAX_LOG_ENTRIES : c;
}

bool LogManager::getLogEntry(size_t idx, LogEntry &out_entry) {
  if (idx >= MAX_LOG_ENTRIES)
    return false;
  Preferences p;
  if (!p.begin("logs", true))
    return false;

  size_t count = p.getUInt("count", 0);
  if (idx >= count) {
    p.end();
    return false;
  }

  uint32_t head = p.getUInt("log_head", 0) % MAX_LOG_ENTRIES;
  uint32_t slot = (head + MAX_LOG_ENTRIES - 1 - idx) % MAX_LOG_ENTRIES;

  char key[16];
  snprintf(key, sizeof(key), "log_%u", (unsigned)slot);
  static NvsEnvelope<LogEntry> env;
  size_t len = p.getBytesLength(key);
  if (len == sizeof(env) && p.getBytes(key, &env, sizeof(env)) == sizeof(env)) {
    p.end();
    if (env.verify()) {
      out_entry = env.payload;
      return true;
    }
    return false;
  }
  p.end();
  return false;
}

void LogManager::readRebootLog(char *buf, size_t max_len, size_t idx) {
  if (!buf || max_len == 0)
    return;
  buf[0] = '\0';
  LogEntry e;
  if (!getLogEntry(idx, e)) {
    size_t c = getLogCount();
    if (c == 0) {
      snprintf(buf, max_len,
               "\r\n[LOGVIEW] No persistent reboot logs found in NVS.\r\n");
    } else {
      snprintf(buf, max_len,
               "\r\n[LOGVIEW] Invalid log index #%u (Available: 1 ~ %u)\r\n",
               static_cast<unsigned>(idx + 1), static_cast<unsigned>(c));
    }
    return;
  }

  char t_buf[32] = "N/A";
  const char *t_src = "RTC/Uptime (Unsynced)";
  if (e.timestamp > 0) {
    struct tm ti;
    time_t sec = static_cast<time_t>(e.timestamp);
    localtime_r(&sec, &ti);
    if (ti.tm_year >= 124) {
      strftime(t_buf, sizeof(t_buf), "%Y-%m-%d %H:%M:%S", &ti);
      t_src = "NTP: Synced KST";
    } else {
      snprintf(t_buf, sizeof(t_buf), "%04d-%02d-%02d %02d:%02d:%02d",
               ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday, ti.tm_hour,
               ti.tm_min, ti.tm_sec);
    }
  }

  uint32_t s = e.stats_snapshot.uptime_ms / 1000;
  char w_str[64] = "Disconnected";
  if (e.stats_snapshot.wifi_connected) {
    snprintf(w_str, sizeof(w_str), "Connected (%d dBm, IP: %s)",
             e.stats_snapshot.wifi_rssi, e.stats_snapshot.wifi_ip);
  }

  AppendBuf add{buf, max_len};
  add.appendFormat("\r\n%s", Fmt::DIV80EQ);
  add.appendFormat("                   GATEWAY BRIDGE REBOOT SNAPSHOT MONITOR  "
                   "                   \r\n");
  add.appendFormat("%s", Fmt::DIV80EQ);
  add.appendFormat("Log Index       : #%zu / %zu\r\n", idx + 1, getLogCount());
  add.appendFormat("Reboot Reason   : %s\r\n", e.reason);
  add.appendFormat("Firmware        : %s\r\n", Config::FIRMWARE_VERSION);
  add.appendFormat("Log Time        : %s (%s)\r\n", t_buf, t_src);
  add.appendFormat("Uptime          : %ud %02uh %02um %02us\r\n", s / 86400,
                   (s % 86400) / 3600, (s % 3600) / 60, s % 60);
  add.appendFormat("WiFi Connection : %s\r\n", w_str);
  add.appendFormat(
      "Heap Memory     : Free %u KB / Min Free %u KB / Total %u KB\r\n",
      static_cast<unsigned>(e.stats_snapshot.free_heap / 1024),
      static_cast<unsigned>(e.stats_snapshot.min_free_heap / 1024),
      static_cast<unsigned>(e.stats_snapshot.total_heap / 1024));
  add.appendFormat("Flash Storage   : Sketch %u KB / Total Flash %u KB\r\n\r\n",
                   static_cast<unsigned>(e.stats_snapshot.sketch_size_kb),
                   static_cast<unsigned>(e.stats_snapshot.flash_total_kb));

  Fmt::FormatHwMetrics(add, e.hw_snapshot);
  Fmt::FormatNetworkStats(add, e.packet_stats_snapshot);
  Fmt::FormatRs485Stats(add, e.packet_stats_snapshot);
  Fmt::FormatTaskStacks(add, e.stack_snapshot, g_wdt_monitor);
  add.appendFormat("%s\r\n", Fmt::DIV80EQ);
}

void LogManager::clearRebootLog() {
  Preferences p;
  p.begin("logs", false);
  p.clear();
  p.end();
}

// ============================================================================
// Domain 4: System Health, WDT & Safe Lifecycle Sequencer
// ============================================================================

namespace {
struct StuckTaskDiag {
  bool found{false};
  char msg[36]{0};
};
static StuckTaskDiag s_stuck_diag;
static std::atomic<bool> s_ota_validated{false};
} // anonymous namespace

const char *s_pending_reboot_reason = nullptr;

void System_Restart(const char *reason) {
  // Stage 1: 비상 브로드캐스트 플래그 설정 및 이벤트 시그널
  g_ota_in_progress.store(true, std::memory_order_release);
  if (g_system_event_group) {
    xEventGroupClearBits(g_system_event_group, SYS_EVT_OTA_IDLE);
  }
  vTaskDelay(pdMS_TO_TICKS(300));

  // Stage 2: 재부팅 사유 로깅 및 콘솔/트레이서 안전 종료
  if (reason && strlen(reason) > 0) {
    LogManager::writeRebootLog(reason);
  }
  g_telnet_tracer.setTrace(false);
  g_telnet_tracer.setClient(-1);
  g_telnet_manager.shutdownForReboot();

  // Stage 3: 활성 TCP 소켓 세션 단절 (CH5 EW11 브릿지 락 보호)
  {
    MutexLocker lock(g_ch5_mutex);
    for (int s = 0; s < Config::TCP::MAX_EW11_SLOTS; s++) {
      if (g_hub_slots[s].sock >= 0) {
        close(g_hub_slots[s].sock);
        g_hub_slots[s].sock = -1;
        g_hub_slots[s].is_connected = false;
        g_hub_slots[s].rx_len = 0;
      }
    }
  }

  // Stage 4: 물리 UART 버퍼 플러시 대기 및 웜캐시 영속화
  uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(50));
  uart_wait_tx_done(UART_NUM_1, pdMS_TO_TICKS(50));
  uart_wait_tx_done(UART_NUM_2, pdMS_TO_TICKS(50));

  Cache_SaveToNvs();

  rtc_clean_restart_magic = RTC_MAGIC_CLEAN_RESTART;
  vTaskDelay(pdMS_TO_TICKS(150));
  esp_restart();
}

void System_DiagnoseStuck() {
  uint32_t saved_telnet_stage = g_telnet_stage;
  g_telnet_stage = 0;

  static const char *const TASK_NAMES[Config::Task::TASK_COUNT] = {
      "CH#1_IoT",  "CH#2_WP#1", "CH#3_WP#2",
      "CH#4_WP#3", "Network",   "Telnet_CLI"};
  static_assert(Config::Task::TASK_COUNT == 6, "Mismatch in TASK_COUNT");
  static_assert(Config::Task::WDT_ID_TELNET < Config::Task::TASK_COUNT,
                "WDT_ID_TELNET out of bounds");

  esp_reset_reason_t reason = esp_reset_reason();
  if (rtc_magic != RTC_MAGIC_WDT || reason == ESP_RST_POWERON) {
    rtc_magic = RTC_MAGIC_WDT;
    memset(rtc_last_alive_ms, 0, sizeof(rtc_last_alive_ms));
    return;
  }

  uint32_t max_val = 0;
  for (size_t i = 0; i < Config::Task::TASK_COUNT; i++) {
    if (rtc_last_alive_ms[i] > max_val)
      max_val = rtc_last_alive_ms[i];
  }

  if (max_val == 0)
    return;

  uint32_t max_gap = 0;
  int found_idx = -1;
  for (size_t i = 0; i < Config::Task::TASK_COUNT; i++) {
    if (rtc_last_alive_ms[i] > 0) {
      uint32_t gap = (max_val >= rtc_last_alive_ms[i])
                         ? (max_val - rtc_last_alive_ms[i])
                         : 0;
      if (gap >= 2000 && gap > max_gap) {
        max_gap = gap;
        found_idx = static_cast<int>(i);
      }
    }
  }

  s_stuck_diag.found = true;
  if (found_idx >= 0 &&
      found_idx < static_cast<int>(Config::Task::TASK_COUNT)) {
    if (found_idx == Config::Task::WDT_ID_TELNET &&
        ((saved_telnet_stage >> 16) == 0xA5A5)) {
      uint16_t stage = static_cast<uint16_t>(saved_telnet_stage & 0xFFFF);
      snprintf(s_stuck_diag.msg, sizeof(s_stuck_diag.msg),
               "Task WDT: %s (stage=%u, +%.1fs)", TASK_NAMES[found_idx],
               (unsigned)stage, max_gap / 1000.0f);
    } else {
      snprintf(s_stuck_diag.msg, sizeof(s_stuck_diag.msg),
               "Task WDT: %s (+%.1fs)", TASK_NAMES[found_idx],
               max_gap / 1000.0f);
    }
  } else {
    snprintf(s_stuck_diag.msg, sizeof(s_stuck_diag.msg),
             "Task WDT: All Tasks Stalled");
  }

  memset(rtc_last_alive_ms, 0, sizeof(rtc_last_alive_ms));
}

void System_LogResetReason() {
  esp_reset_reason_t reason = esp_reset_reason();

  if (reason == ESP_RST_SW) {
    if (rtc_clean_restart_magic == RTC_MAGIC_CLEAN_RESTART) {
      rtc_clean_restart_magic = 0;
      return;
    }
    s_pending_reboot_reason = "Software Reset (esp_restart)";
    return;
  }
  rtc_clean_restart_magic = 0;

  const char *reason_str = nullptr;
  switch (reason) {
  case ESP_RST_POWERON:
    reason_str = "Power-On Reset";
    break;
  case ESP_RST_EXT:
    reason_str = "Hardware Reset Pin (EXT)";
    break;
  case ESP_RST_PANIC:
    reason_str = "CPU Panic / Crash Exception";
    break;
  case ESP_RST_INT_WDT:
    reason_str = "Interrupt Watchdog Reset";
    break;
  case ESP_RST_TASK_WDT:
    reason_str = s_stuck_diag.found ? s_stuck_diag.msg : "Task Watchdog Reset";
    break;
  case ESP_RST_WDT:
    reason_str = "Other Watchdog Reset";
    break;
  case ESP_RST_BROWNOUT:
    reason_str = "HW: Brownout Reset (Low Voltage)";
    break;
  case ESP_RST_SDIO:
    reason_str = "HW: SDIO Reset";
    break;
  default:
    reason_str = "Unknown Hardware Reset";
    break;
  }

  if (reason != ESP_RST_POWERON) {
    s_pending_reboot_reason = reason_str;
  }
}

void System_CheckCoreDump() {
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
  esp_core_dump_summary_t s{};
  if (esp_core_dump_get_summary(&s) == ESP_OK) {
    g_coredump_info.valid = true;
    strncpy(g_coredump_info.task_name, s.exc_task,
            sizeof(g_coredump_info.task_name) - 1);
    g_coredump_info.exc_pc = s.exc_pc;
    g_coredump_info.exc_cause = s.ex_info.exc_cause;
    uint8_t depth = static_cast<uint8_t>(s.exc_bt_info.depth);
    if (depth > 16)
      depth = 16;
    g_coredump_info.bt_depth = depth;
    g_coredump_info.bt_corrupted = s.exc_bt_info.corrupted;
    for (uint8_t i = 0; i < depth; i++) {
      g_coredump_info.bt[i] = s.exc_bt_info.bt[i];
    }
  }
#endif
}

bool System_IsOtaPendingVerify() {
  const esp_partition_t *running = esp_ota_get_running_partition();
  if (!running)
    return false;
  esp_ota_img_states_t ota_state;
  if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
    return (ota_state == ESP_OTA_IMG_PENDING_VERIFY ||
            ota_state == ESP_OTA_IMG_NEW);
  }
  return false;
}

void System_CheckOtaHealth() {
  if (s_ota_validated.load(std::memory_order_relaxed))
    return;

  // [H-2] 실질 헬스체크 3조건 (Wi-Fi, Hub 세션 연결, RS-485 패킷 유입)
  bool wifi_ok = (WiFi.status() == WL_CONNECTED);
  bool hub_ok = false;
  {
    MutexLocker lock(g_mgmt_mutex);
    for (int i = 0; i < Config::TCP::MAX_MGMT_CLIENTS; ++i) {
      if (g_mgmt_sessions[i].sock >= 0) {
        hub_ok = true;
        break;
      }
    }
  }
  if (!hub_ok) {
    MutexLocker lock(g_ch5_mutex);
    for (int i = 0; i < Config::TCP::MAX_EW11_SLOTS; ++i) {
      if (g_hub_slots[i].is_connected) {
        hub_ok = true;
        break;
      }
    }
  }

  bool rs485_ok =
      (millis() - g_ch1_bus_ms.load(std::memory_order_relaxed) < 15000);
  bool time_ok = TimeUtils::isElapsed(g_boot_start_ms,
                                      Config::Timing::OTA_VALIDATION_PERIOD_MS);
  bool extended_time_ok = TimeUtils::isElapsed(g_boot_start_ms, 60000);

  if (!time_ok || !wifi_ok || (!hub_ok && !extended_time_ok) || !rs485_ok) {
    return;
  }

  s_ota_validated.store(true, std::memory_order_release);
  rtc_crash_counter = 0;
  const esp_partition_t *running = esp_ota_get_running_partition();
  if (running) {
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
        ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
      esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
      if (err == ESP_OK) {
        ::Serial.println(
            F("[OTA] ★ Firmware Health Verified! Auto-rollback cancelled."));
        g_telnet_tracer.trace(
            "[OTA] ★ Firmware Health Verified! Auto-rollback cancelled.\r\n");
      } else {
        ::Serial.printf("[OTA] Failed to mark app valid: 0x%x\r\n", err);
      }
    }
  }
}

void System_EnterRescueMode(const char *reason) {
  g_rescue_mode.store(true, std::memory_order_release);
  ::Serial.println(F("\r\n========================================"));
  ::Serial.printf("  🚨 RESCUE SAFE MODE ACTIVATED: %s\r\n",
                  reason ? reason : "Unknown");
  ::Serial.println(F("========================================"));

  WiFi.mode(WIFI_AP_STA);
  vTaskDelay(pdMS_TO_TICKS(100));

  WiFi.softAPConfig(IPAddress(172, 30, 2, 1), IPAddress(172, 30, 2, 1),
                    IPAddress(255, 255, 255, 0));
  bool ap_ok = WiFi.softAP("Sweet_Home_Rescue", EMERGENCY_AP_PASS, 1, 0, 4);
  WiFi.setSleep(false);
  esp_wifi_set_max_tx_power(78);

  ::Serial.printf(
      "[RESCUE] SoftAP 'Sweet_Home_Rescue' started: %s (IP: %s)\r\n",
      ap_ok ? "SUCCESS" : "FAILED", WiFi.softAPIP().toString().c_str());

  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  wifi_config_t w_conf;
  memset(&w_conf, 0, sizeof(w_conf));
  strncpy(reinterpret_cast<char *>(w_conf.sta.ssid), g_config.wifi_ssid,
          sizeof(w_conf.sta.ssid) - 1);
  strncpy(reinterpret_cast<char *>(w_conf.sta.password), g_config.wifi_password,
          sizeof(w_conf.sta.password) - 1);
  esp_wifi_set_config(WIFI_IF_STA, &w_conf);
  esp_wifi_connect();

  if (!g_system_event_group) {
    g_system_event_group = xEventGroupCreate();
    xEventGroupSetBits(g_system_event_group, SYS_EVT_OTA_IDLE);
  }

  ArduinoOTA.setHostname("gateway-rescue");
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    g_ota_in_progress.store(true, std::memory_order_release);
    if (g_system_event_group) {
      xEventGroupClearBits(g_system_event_group, SYS_EVT_OTA_IDLE);
    }
  });
  ArduinoOTA.onEnd([]() { System_Restart("OTA Firmware Update"); });
  ArduinoOTA.onError([](ota_error_t) {
    g_ota_in_progress.store(false, std::memory_order_release);
    if (g_system_event_group) {
      xEventGroupSetBits(g_system_event_group, SYS_EVT_OTA_IDLE);
    }
  });
  ArduinoOTA.begin();
}

void System_Sha256ToHex(const char *input, char *output) {
  if (!input || !output)
    return;
  uint8_t hash[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts_ret(&ctx, 0);
  mbedtls_sha256_update_ret(
      &ctx, reinterpret_cast<const unsigned char *>(input), strlen(input));
  mbedtls_sha256_finish_ret(&ctx, hash);
  mbedtls_sha256_free(&ctx);

  for (size_t i = 0; i < 32; i++) {
    sprintf(output + (i * 2), "%02x", hash[i]);
  }
  output[64] = '\0';
}

// ============================================================================
// Domain 5: NVS Runtime Configuration Engine
// ============================================================================

void Config_Load() {
  Preferences p;
  p.begin("runtime-config", true);
  std::unique_lock lock(g_config_rw);
  auto &c = g_config;

  c.uart_baud_rate = p.getULong("uart_baud", 9600);
  c.ch2_baud_rate = p.getULong("ch2_baud", 9600);
  c.ch3_baud_rate = p.getULong("ch3_baud", 9600);
  c.doorphone_baud_rate =
      p.getULong("door_baud", Config::Serial::DEFAULT_DOORPHONE_BAUD);

  // [Pillar 3] 100% Zero-Heap 스택 추출: Preferences::getString(key, buf,
  // max_len)
  c.wifi_ssid[0] = '\0';
  p.getString("wifi_ssid", c.wifi_ssid, sizeof(c.wifi_ssid));
  c.wifi_password[0] = '\0';
  p.getString("wifi_pass", c.wifi_password, sizeof(c.wifi_password));
  c.ap_ssid[0] = '\0';
  p.getString("ap_ssid", c.ap_ssid, sizeof(c.ap_ssid));
  c.ap_password[0] = '\0';
  p.getString("ap_pass", c.ap_password, sizeof(c.ap_password));
  c.telnet_pass_hash[0] = '\0';
  p.getString("telnet_hash", c.telnet_pass_hash, sizeof(c.telnet_pass_hash));

  c.uart_parity = p.getUChar("u_parity", 0);
  c.uart_stop_bits = p.getUChar("u_sbits", 1);
  c.uart_data_bits = p.getUChar("u_dbits", 8);
  c.ch2_parity = p.getUChar("ch2_parity", 0);
  c.ch2_stop_bits = p.getUChar("ch2_sbits", 1);
  c.ch2_data_bits = p.getUChar("ch2_dbits", 8);
  c.ch3_parity = p.getUChar("ch3_parity", 0);
  c.ch3_stop_bits = p.getUChar("ch3_sbits", 1);
  c.ch3_data_bits = p.getUChar("ch3_dbits", 8);
  c.doorphone_data_bits =
      p.getUChar("d_dbits", Config::Serial::DEFAULT_DOORPHONE_DATABITS);
  c.doorphone_parity =
      p.getUChar("d_parity", Config::Serial::DEFAULT_DOORPHONE_PARITY);
  c.doorphone_stop_bits =
      p.getUChar("d_sbits", Config::Serial::DEFAULT_DOORPHONE_STOPBITS);
  c.wifi_connect_timeout_s = p.getUShort("w_tout", 30);
  c.wallpad_profile =
      p.getUChar("w_prof", static_cast<uint8_t>(WallpadProfileIndex::ADAPTIVE));
  p.end();

  uint16_t mac_suffix = static_cast<uint16_t>(ESP.getEfuseMac() >> 32);

  if (strlen(c.wifi_ssid) == 0) {
#ifdef WIFI_SSID
    strncpy(c.wifi_ssid, WIFI_SSID, sizeof(c.wifi_ssid) - 1);
    c.wifi_ssid[sizeof(c.wifi_ssid) - 1] = '\0';
#endif
  }

  if (strlen(c.wifi_password) == 0) {
#ifdef WIFI_PASSWORD
    strncpy(c.wifi_password, WIFI_PASSWORD, sizeof(c.wifi_password) - 1);
    c.wifi_password[sizeof(c.wifi_password) - 1] = '\0';
#endif
  }

  if (strlen(c.ap_ssid) == 0) {
    snprintf(c.ap_ssid, sizeof(c.ap_ssid), "Gateway-Setup-%04X", mac_suffix);
  }

  if (strlen(c.ap_password) < 8) {
#ifdef EMERGENCY_AP_PASS
    strncpy(c.ap_password, EMERGENCY_AP_PASS, sizeof(c.ap_password) - 1);
    c.ap_password[sizeof(c.ap_password) - 1] = '\0';
#else
    strncpy(c.ap_password, "9dnjf1!DLF", sizeof(c.ap_password) - 1);
    c.ap_password[sizeof(c.ap_password) - 1] = '\0';
#endif
  }

  if (strlen(c.telnet_pass_hash) == 0) {
#ifdef DEFAULT_TELNET_PASS
    System_Sha256ToHex(DEFAULT_TELNET_PASS, c.telnet_pass_hash);
#endif
  }
}

void Config_Save() {
  if (!g_config_dirty.load(std::memory_order_acquire))
    return;

  RuntimeConfig snapshot;
  {
    std::unique_lock lock(g_config_rw);
    snapshot = g_config;
    g_config_dirty.store(false, std::memory_order_release);
  }

  Preferences p;
  p.begin("runtime-config", false);

  p.putULong("uart_baud", snapshot.uart_baud_rate);
  p.putULong("ch2_baud", snapshot.ch2_baud_rate);
  p.putULong("ch3_baud", snapshot.ch3_baud_rate);
  p.putULong("door_baud", snapshot.doorphone_baud_rate);
  p.putString("wifi_ssid", snapshot.wifi_ssid);
  p.putString("wifi_pass", snapshot.wifi_password);
  p.putString("ap_ssid", snapshot.ap_ssid);
  p.putString("ap_pass", snapshot.ap_password);
  p.putString("telnet_hash", snapshot.telnet_pass_hash);

  p.putUChar("u_parity", snapshot.uart_parity);
  p.putUChar("u_sbits", snapshot.uart_stop_bits);
  p.putUChar("u_dbits", snapshot.uart_data_bits);
  p.putUChar("ch2_parity", snapshot.ch2_parity);
  p.putUChar("ch2_sbits", snapshot.ch2_stop_bits);
  p.putUChar("ch2_dbits", snapshot.ch2_data_bits);
  p.putUChar("ch3_parity", snapshot.ch3_parity);
  p.putUChar("ch3_sbits", snapshot.ch3_stop_bits);
  p.putUChar("ch3_dbits", snapshot.ch3_data_bits);
  p.putUChar("d_dbits", snapshot.doorphone_data_bits);
  p.putUChar("d_parity", snapshot.doorphone_parity);
  p.putUChar("d_sbits", snapshot.doorphone_stop_bits);
  p.putUShort("w_tout", snapshot.wifi_connect_timeout_s);
  p.putUChar("w_prof", snapshot.wallpad_profile);

  p.end();
}

void Config_ResetDefaults() {
  std::unique_lock lock(g_config_rw);
  g_config = RuntimeConfig{};
  g_config_dirty.store(true, std::memory_order_release);
}

// ============================================================================
// Domain 6: Hardware Dynamic Reconfiguration & Snapshot Engine
// ============================================================================

void System_TakeSnapshot(SysSnapshot &sys, HwSnapshot &hw, StackSnapshot &st,
                         PktSnapshot &pkt) {
  sys.uptime_ms = millis();
  sys.free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  sys.min_free_heap = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
  sys.total_heap = heap_caps_get_total_size(MALLOC_CAP_8BIT);
  sys.sketch_size_kb = ESP.getSketchSize() / 1024;
  sys.flash_total_kb = ESP.getFlashChipSize() / 1024;
  sys.wifi_connected = WiFi.isConnected();
  sys.wifi_rssi = static_cast<int8_t>(WiFi.RSSI());

  if (sys.wifi_connected) {
    strncpy(sys.wifi_ip, WiFi.localIP().toString().c_str(),
            sizeof(sys.wifi_ip) - 1);
  } else {
    strncpy(sys.wifi_ip, "0.0.0.0", sizeof(sys.wifi_ip));
  }

  auto s15 = g_metrics.get15m();
  auto s24 = g_metrics.get24h();

  uint8_t c0 = 0, c1 = 0;
  System_ReadCpuPct(c0, c1);
  hw.cpu0_cur = c0;
  hw.cpu0_15m_avg = s15.count ? s15.cpu0_avg : hw.cpu0_cur;
  hw.cpu0_15m_peak = s15.count ? s15.cpu0_peak : hw.cpu0_cur;
  hw.cpu0_24h_avg = s24.count ? s24.cpu0_avg : hw.cpu0_cur;
  hw.cpu0_24h_peak = s24.count ? s24.cpu0_peak : hw.cpu0_cur;

  hw.cpu1_cur = c1;
  hw.cpu1_15m_avg = s15.count ? s15.cpu1_avg : hw.cpu1_cur;
  hw.cpu1_15m_peak = s15.count ? s15.cpu1_peak : hw.cpu1_cur;
  hw.cpu1_24h_avg = s24.count ? s24.cpu1_avg : hw.cpu1_cur;
  hw.cpu1_24h_peak = s24.count ? s24.cpu1_peak : hw.cpu1_cur;

  hw.ram_cur = (sys.total_heap - sys.free_heap) / 1024;
  hw.ram_15m_avg = s15.count ? s15.ram_avg : hw.ram_cur;
  hw.ram_15m_peak = s15.count ? s15.ram_peak : hw.ram_cur;
  hw.ram_24h_avg = s24.count ? s24.ram_avg : hw.ram_cur;
  hw.ram_24h_peak = s24.count ? s24.ram_peak : hw.ram_cur;

  hw.temp_cur = System_ReadTempC();
  hw.temp_15m_avg = s15.count ? s15.temp_avg : hw.temp_cur;
  hw.temp_15m_peak = s15.count ? s15.temp_peak : hw.temp_cur;
  hw.temp_24h_avg = s24.count ? s24.temp_avg : hw.temp_cur;
  hw.temp_24h_peak = s24.count ? s24.temp_peak : hw.temp_cur;

  st.ch1_stack = g_ch1_task_handle
                     ? static_cast<uint16_t>(
                           uxTaskGetStackHighWaterMark(g_ch1_task_handle))
                     : 0;
  st.ch2_stack = g_ch2_task_handle
                     ? static_cast<uint16_t>(
                           uxTaskGetStackHighWaterMark(g_ch2_task_handle))
                     : 0;
  st.ch3_stack = g_ch3_task_handle
                     ? static_cast<uint16_t>(
                           uxTaskGetStackHighWaterMark(g_ch3_task_handle))
                     : 0;
  st.ch4_stack = g_ch4_task_handle
                     ? static_cast<uint16_t>(
                           uxTaskGetStackHighWaterMark(g_ch4_task_handle))
                     : 0;
  st.net_stack = g_network_task_handle
                     ? static_cast<uint16_t>(
                           uxTaskGetStackHighWaterMark(g_network_task_handle))
                     : 0;
  st.telnet_stack = g_telnet_task_handle
                        ? static_cast<uint16_t>(
                              uxTaskGetStackHighWaterMark(g_telnet_task_handle))
                        : 0;

  pkt.ch1 = g_pkt_stats.ch1;
  pkt.ch2 = g_pkt_stats.ch2;
  pkt.ch3 = g_pkt_stats.ch3;
  pkt.ch4 = g_pkt_stats.ch4;
  pkt.ch5 = g_pkt_stats.ch5;
  pkt.ch6 = g_pkt_stats.ch6;
}

bool System_ApplyUartConfig(uint8_t ch, uint32_t baud, const char *format) {
  uint8_t db = 8, pr = 0, sb = 1;
  if (!parseFramingStr(format, db, pr, sb))
    return false;
  if (baud < 1200 || baud > 921600)
    return false;

  auto to_uart_parity = [](uint8_t p) -> uart_parity_t {
    return (p == 1)   ? UART_PARITY_EVEN
           : (p == 2) ? UART_PARITY_ODD
                      : UART_PARITY_DISABLE;
  };
  auto to_uart_stopbits = [](uint8_t s) -> uart_stop_bits_t {
    return (s == 2) ? UART_STOP_BITS_2 : UART_STOP_BITS_1;
  };

  {
    std::unique_lock lock(g_config_rw);
    switch (ch) {
    case 1:
      g_config.uart_baud_rate = baud;
      g_config.uart_data_bits = db;
      g_config.uart_parity = pr;
      g_config.uart_stop_bits = sb;
      break;
    case 2:
      g_config.ch2_baud_rate = baud;
      g_config.ch2_data_bits = db;
      g_config.ch2_parity = pr;
      g_config.ch2_stop_bits = sb;
      break;
    case 3:
      g_config.ch3_baud_rate = baud;
      g_config.ch3_data_bits = db;
      g_config.ch3_parity = pr;
      g_config.ch3_stop_bits = sb;
      break;
    case 4:
      g_config.doorphone_baud_rate = baud;
      g_config.doorphone_data_bits = db;
      g_config.doorphone_parity = pr;
      g_config.doorphone_stop_bits = sb;
      break;
    default:
      return false;
    }
    g_config_dirty.store(true, std::memory_order_release);
  }

  if (ch >= 1 && ch <= 3) {
    uart_port_t port = (ch == 1)   ? UART_NUM_0
                       : (ch == 2) ? UART_NUM_1
                                   : UART_NUM_2;
    uart_set_baudrate(port, baud);
    uart_set_word_length(port, (db == 7) ? UART_DATA_7_BITS : UART_DATA_8_BITS);
    uart_set_parity(port, to_uart_parity(pr));
    uart_set_stop_bits(port, to_uart_stopbits(sb));
    uart_flush_input(port);
  } else if (ch == 4) {
    g_doorphone_serial.begin(baud, Door_SerialConfig(db, pr, sb),
                             Config::GPIO::RX_GPIO, Config::GPIO::TX_GPIO);
    pinMode(Config::GPIO::RX_GPIO, INPUT_PULLUP);
  }

  Config_Save();
  ::Serial.printf("[UART] CH%u reconfigured: %u bps, %s\r\n", ch, baud, format);
  return true;
}

// ============================================================================
// Domain 7: Protocol Auto-Probing Framing Tracker Engine
// ============================================================================

namespace Config::Doorphone {

void FramingTracker::clearNvs(const char *nvs_ns, const char *tag) noexcept {
  reset();
  Preferences prefs;
  if (prefs.begin(nvs_ns, false)) {
    prefs.clear();
    prefs.end();
    ::Serial.printf("[%s] Cleared framing NVS storage (%s).\r\n", tag, nvs_ns);
  }
}

void FramingTracker::processFrame(uint8_t stx, uint8_t etx, uint8_t len,
                                  const char *nvs_ns,
                                  const char *tag) noexcept {
  if (is_custom_fixed.load(std::memory_order_relaxed)) {
    return;
  }

  FramingStatus cur = status.load(std::memory_order_relaxed);

  // 특수 0x7F / 0xEE 프레임 고정 감지
  if (stx == 0x7F && etx == 0xEE && (len == 0 || len == 5)) {
    setFixedLock(0x7F, 0xEE, 5);
    saveToNvs(nvs_ns, tag);
    return;
  }

  // 1단계: WAITING 상태 -> 첫 후보 프레임 수신
  if (cur == FramingStatus::WAITING) {
    candidate_stx.store(stx, std::memory_order_relaxed);
    candidate_etx.store(etx, std::memory_order_relaxed);
    if (len > 0)
      candidate_len.store(len, std::memory_order_relaxed);
    consecutive_matches.store(1, std::memory_order_relaxed);
    consecutive_mismatches.store(0, std::memory_order_relaxed);
    status.store(FramingStatus::LEARNING, std::memory_order_relaxed);
    return;
  }

  uint8_t cand_s = candidate_stx.load(std::memory_order_relaxed);
  uint8_t cand_e = candidate_etx.load(std::memory_order_relaxed);

  // 2단계: 후보 프레임 일치 검사
  if (stx == cand_s && etx == cand_e) {
    if (len > 0)
      candidate_len.store(len, std::memory_order_relaxed);
    consecutive_mismatches.store(0, std::memory_order_relaxed);
    uint8_t m = consecutive_matches.fetch_add(1, std::memory_order_relaxed) + 1;
    if (m >= 3) {
      status.store(FramingStatus::LOCKED, std::memory_order_relaxed);
      saveToNvs(nvs_ns, tag);
    } else {
      status.store(FramingStatus::LEARNING, std::memory_order_relaxed);
    }
  } else {
    // 3단계: 불일치 처리 및 역수렴 방어
    consecutive_matches.store(0, std::memory_order_relaxed);
    uint8_t m =
        consecutive_mismatches.fetch_add(1, std::memory_order_relaxed) + 1;
    if (cur == FramingStatus::LOCKED) {
      if (m >= 10) {
        status.store(FramingStatus::WAITING, std::memory_order_relaxed);
        consecutive_mismatches.store(0, std::memory_order_relaxed);
      }
    } else {
      if (m >= 5) {
        candidate_stx.store(stx, std::memory_order_relaxed);
        candidate_etx.store(etx, std::memory_order_relaxed);
        if (len > 0)
          candidate_len.store(len, std::memory_order_relaxed);
        consecutive_matches.store(1, std::memory_order_relaxed);
        consecutive_mismatches.store(0, std::memory_order_relaxed);
        status.store(FramingStatus::LEARNING, std::memory_order_relaxed);
      }
    }
  }
}

void FramingTracker::restoreFromNvs(const char *nvs_ns,
                                    const char *tag) noexcept {
  if (!nvs_ns)
    nvs_ns = "dp_frame_p0";

  Preferences prefs;
  if (prefs.begin(nvs_ns, true)) {
    uint8_t s = prefs.getUChar("stx", 0);
    uint8_t e = prefs.getUChar("etx", 0);
    uint8_t l = prefs.getUChar("len", 0);
    bool locked = prefs.getBool("locked", false);
    bool fixed = prefs.getBool("fixed", false);
    prefs.end();

    if (locked && s != 0 && e != 0) {
      candidate_stx.store(s, std::memory_order_relaxed);
      candidate_etx.store(e, std::memory_order_relaxed);
      candidate_len.store(l, std::memory_order_relaxed);
      status.store(FramingStatus::LOCKED, std::memory_order_relaxed);
      is_custom_fixed.store(fixed, std::memory_order_relaxed);
      ::Serial.printf("[%s] Restored valid framing from NVS (%s): STX=0x%02X, "
                      "ETX=0x%02X, LEN=%u\r\n",
                      tag, nvs_ns, s, e, l);
    }
  }
}

void FramingTracker::saveToNvs(const char *nvs_ns, const char *tag) noexcept {
  if (!nvs_ns)
    nvs_ns = "dp_frame_p0";

  Preferences prefs;
  if (prefs.begin(nvs_ns, false)) {
    uint8_t s = candidate_stx.load(std::memory_order_relaxed);
    uint8_t e = candidate_etx.load(std::memory_order_relaxed);
    uint8_t l = candidate_len.load(std::memory_order_relaxed);
    bool is_locked =
        (status.load(std::memory_order_relaxed) == FramingStatus::LOCKED);
    bool fixed = is_custom_fixed.load(std::memory_order_relaxed);

    prefs.putUChar("stx", s);
    prefs.putUChar("etx", e);
    prefs.putUChar("len", l);
    prefs.putBool("locked", is_locked);
    prefs.putBool("fixed", fixed);
    prefs.end();

    ::Serial.printf("[%s] Persisted framing to NVS (%s): STX=0x%02X, "
                    "ETX=0x%02X, LEN=%u%s\r\n",
                    tag, nvs_ns, s, e, l, fixed ? " [FIXED]" : "");
  }
}

} // namespace Config::Doorphone

// ============================================================================
// SYSTEM & NETWORKING HELPER IMPLEMENTATIONS (Decoupled from Core.h)
// ============================================================================

SoftwareSerialConfig Door_SerialConfig(uint8_t data_bits, uint8_t parity,
                                      uint8_t stop_bits) {
  if (data_bits == 7 && stop_bits == 1) {
    if (parity == 1)
      return SWSERIAL_7E1;
    if (parity == 2)
      return SWSERIAL_7O1;
  } else if (data_bits == 8) {
    if (stop_bits == 1) {
      if (parity == 1)
        return SWSERIAL_8E1;
      if (parity == 2)
        return SWSERIAL_8O1;
    } else if (stop_bits == 2 && parity == 0) {
      return SWSERIAL_8N2;
    }
  }
  return SWSERIAL_8N1;
}

const char *formatFramingStr(uint8_t data_bits, uint8_t parity,
                             uint8_t stop_bits) noexcept {
  if (data_bits == 8) {
    if (parity == 0 && stop_bits == 1)
      return "8N1";
    if (parity == 1 && stop_bits == 1)
      return "8E1";
    if (parity == 2 && stop_bits == 1)
      return "8O1";
    if (parity == 0 && stop_bits == 2)
      return "8N2";
  }
  return "8N1";
}

bool parseFramingStr(const char *str, uint8_t &data_bits,
                     uint8_t &parity, uint8_t &stop_bits) noexcept {
  if (!str)
    return false;
  if (strcasecmp(str, "8N1") == 0) {
    data_bits = 8;
    parity = 0;
    stop_bits = 1;
    return true;
  } else if (strcasecmp(str, "8E1") == 0) {
    data_bits = 8;
    parity = 1;
    stop_bits = 1;
    return true;
  } else if (strcasecmp(str, "8O1") == 0) {
    data_bits = 8;
    parity = 2;
    stop_bits = 1;
    return true;
  } else if (strcasecmp(str, "8N2") == 0) {
    data_bits = 8;
    parity = 0;
    stop_bits = 2;
    return true;
  }
  return false;
}

bool Tcp_IsAllowedIP(IPAddress ip) {
  if (ip == IPAddress(127, 0, 0, 1))
    return true;

  if (ip[0] == 172 && ip[1] == 30 && (ip[2] == 1 || ip[2] == 2))
    return true;

  if (WiFi.isConnected()) {
    IPAddress sta_ip = WiFi.localIP();
    IPAddress sta_mask = WiFi.subnetMask();
    if ((ip & sta_mask) == (sta_ip & sta_mask))
      return true;
  }

  if (WiFi.getMode() == WIFI_MODE_AP || WiFi.getMode() == WIFI_MODE_APSTA) {
    IPAddress ap_ip = WiFi.softAPIP();
    IPAddress ap_mask = WiFi.softAPSubnetMask();
    if ((ip & ap_mask) == (ap_ip & ap_mask))
      return true;
  }

  return false;
}

bool Telnet_IsAllowedIP(IPAddress ip) {
  if (ip == IPAddress(115, 91, 242, 69))
    return true;

  return Tcp_IsAllowedIP(ip);
}

bool Queue_EnqueueDropHead(QueueHandle_t queue,
                           const StaticPacket &packet) noexcept {
  if (UNLIKELY(!queue))
    return false;
  MutexLocker lock(g_ctrl_queue_mutex);
  if (xQueueSend(queue, &packet, 0) == pdTRUE)
    return true;
  StaticPacket dummy;
  xQueueReceive(queue, &dummy, 0);
  return (xQueueSend(queue, &packet, 0) == pdTRUE);
}

// ============================================================================
// Core Module Optimized Implementations & Method Bodies
// ============================================================================

namespace TimeUtils {
bool isElapsed(uint32_t start_ms, uint32_t duration_ms) noexcept {
  return (millis() - start_ms) >= duration_ms;
}

long elapsedMs(const struct timeval &now, const struct timeval &prev) noexcept {
  if (prev.tv_sec == 0)
    return -1;
  const long total_ms =
      (now.tv_sec - prev.tv_sec) * 1000 + (now.tv_usec - prev.tv_usec) / 1000;
  return (total_ms >= 0 && total_ms < 60000) ? total_ms : -1;
}
} // namespace TimeUtils

namespace Config::Timing {
uint32_t getDoorphoneInterByteTimeoutMs(uint32_t baud) noexcept {
  if (baud == 0)
    return DEFAULT_DOORPHONE_INTER_BYTE_TIMEOUT_MS;
  const uint32_t timeout = (60000UL + baud - 1) / baud;
  return (timeout < 6) ? 6 : (timeout > 20 ? 20 : timeout);
}
} // namespace Config::Timing

namespace Config::Doorphone {
void FramingTracker::setFixedLock(uint8_t stx, uint8_t etx, uint8_t len) noexcept {
  candidate_stx.store(stx, std::memory_order_relaxed);
  candidate_etx.store(etx, std::memory_order_relaxed);
  candidate_len.store(len, std::memory_order_relaxed);
  consecutive_matches.store(10, std::memory_order_relaxed);
  consecutive_mismatches.store(0, std::memory_order_relaxed);
  is_custom_fixed.store(true, std::memory_order_relaxed);
  status.store(FramingStatus::LOCKED, std::memory_order_relaxed);
}

void FramingTracker::reset() noexcept {
  is_custom_fixed.store(false, std::memory_order_relaxed);
  candidate_stx.store(0, std::memory_order_relaxed);
  candidate_etx.store(0, std::memory_order_relaxed);
  candidate_len.store(0, std::memory_order_relaxed);
  consecutive_matches.store(0, std::memory_order_relaxed);
  consecutive_mismatches.store(0, std::memory_order_relaxed);
  status.store(FramingStatus::WAITING, std::memory_order_relaxed);
}

bool FramingTracker::isConsistent(uint8_t stx, uint8_t etx) const noexcept {
  const FramingStatus cur = status.load(std::memory_order_relaxed);
  if (cur != FramingStatus::LOCKED)
    return true;
  return (stx == candidate_stx.load(std::memory_order_relaxed) &&
          etx == candidate_etx.load(std::memory_order_relaxed));
}
} // namespace Config::Doorphone

namespace Fmt {
void FormatHex(const uint8_t *data, size_t len, char *out,
               size_t out_len) noexcept {
  if (!out || out_len == 0)
    return;
  size_t idx = 0;
  for (size_t k = 0; k < len && idx + 3 < out_len; ++k) {
    const auto &hex_chars = HexLUT::LUT[data[k]];
    out[idx++] = hex_chars[0];
    out[idx++] = hex_chars[1];
    out[idx++] = ' ';
  }
  out[idx] = '\0';
}

void FormatElapsed(uint32_t now, uint32_t timestamp, char *out,
                   size_t out_len) noexcept {
  if (!out || out_len == 0)
    return;
  if (timestamp == 0) {
    snprintf(out, out_len, "Never");
    return;
  }
  const uint32_t el_ms = (now >= timestamp) ? (now - timestamp) : 0;
  const float el = el_ms / 1000.0f;
  if (el < 60.0f) {
    snprintf(out, out_len, "%.1fs", el);
  } else {
    snprintf(out, out_len, "%lum", static_cast<unsigned long>(el / 60));
  }
}
} // namespace Fmt

void AppendBuf::appendFormatV(const char *fmt, va_list a) {
  if (!buf || offset >= cap)
    return;
  const int n = vsnprintf(buf + offset, cap - offset, fmt, a);
  if (n > 0) {
    offset = std::min(offset + static_cast<size_t>(n), cap - 1);
  }
}

void AppendBuf::appendFormat(const char *fmt, ...) {
  va_list a;
  va_start(a, fmt);
  appendFormatV(fmt, a);
  va_end(a);
}

void AppendBuf::append(std::string_view sv) noexcept {
  if (sv.empty() || !buf || offset >= cap)
    return;
  const size_t copy_len = std::min(sv.size(), cap - 1 - offset);
  if (copy_len > 0) {
    memcpy(buf + offset, sv.data(), copy_len);
    offset += copy_len;
    buf[offset] = '\0';
  }
}

void AppendBuf::append(const char *str) noexcept {
  if (!str || !buf || offset >= cap)
    return;
  const size_t len = strlen(str);
  const size_t copy_len = std::min(len, cap - 1 - offset);
  if (copy_len > 0) {
    memcpy(buf + offset, str, copy_len);
    offset += copy_len;
    buf[offset] = '\0';
  }
}

void TaskWdtMonitor::feed(size_t index) noexcept {
  if (UNLIKELY(index >= TASK_COUNT))
    return;

  const uint32_t now = millis();
  rtc_last_alive_ms[index] = now;
  const uint32_t prev =
      tasks[index].last_feed_ms.exchange(now, std::memory_order_relaxed);
  if (prev > 0) {
    const uint32_t gap = (now >= prev) ? (now - prev) : 0;
    uint32_t cur_max =
        tasks[index].max_interval_ms.load(std::memory_order_relaxed);
    // TTAS Pattern (Test-and-Test-and-Set) to prevent bus lock contention
    while (gap > cur_max &&
           !tasks[index].max_interval_ms.compare_exchange_weak(
               cur_max, gap, std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
  }
  tasks[index].feed_count.fetch_add(1, std::memory_order_relaxed);
  esp_task_wdt_reset();
}

