#include "L1_HAL/Diagnostics_Driver.h"
#include "L0_Foundation/System_Buffer.h"
#include "L0_Foundation/System_Config.h"
#include "L0_Foundation/System_Platform.h"
#include <Arduino.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <WiFi.h>
#include <driver/uart.h>
#include <esp_ota_ops.h>
#include <esp_wifi.h>
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
#include <esp_core_dump.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif
float temperatureRead(void);
#ifdef __cplusplus
}
#endif

static uint32_t s_boot_start_ms = 0;

uint32_t Diag_GetBootTimeMs() noexcept {
  return s_boot_start_ms;
}

void Diag_SetBootTimeMs(uint32_t ms) noexcept {
  s_boot_start_ms = ms;
}

static const char *s_pending_reboot_reason = nullptr;

const char *Diag_GetPendingRebootReason() noexcept {
  return s_pending_reboot_reason;
}

const char *Diag_ConsumePendingRebootReason() noexcept {
  const char *reason = s_pending_reboot_reason;
  s_pending_reboot_reason = nullptr;
  return reason;
}

void Diag_SetPendingRebootReason(const char *reason) noexcept {
  s_pending_reboot_reason = reason;
}

// ── Unified System Trace Sink & Shutdown Hooks ──
static SystemTraceSink s_trace_sink{};
constexpr size_t MAX_SHUTDOWN_HOOKS = 4;
static ShutdownHook s_shutdown_hooks[MAX_SHUTDOWN_HOOKS]{nullptr};
static std::atomic<size_t> s_shutdown_hook_count{0};

void System_RegisterTraceSink(const SystemTraceSink &sink) noexcept {
  s_trace_sink = sink;
  System_RegisterTraceMessageSink(sink.trace_msg);
  System_RegisterTracePacketSink(sink.trace_packet);
}

void System_RegisterShutdownHook(ShutdownHook hook) noexcept {
  if (!hook)
    return;
  size_t idx = s_shutdown_hook_count.fetch_add(1, std::memory_order_relaxed);
  if (idx < MAX_SHUTDOWN_HOOKS) {
    s_shutdown_hooks[idx] = hook;
  }
}

// ── Task Identifier & Handles (Encapsulated) ──
static TaskHandle_t s_task_handles[static_cast<size_t>(SystemTaskId::COUNT)]{nullptr};

void System_RegisterTaskHandle(SystemTaskId id, TaskHandle_t handle) noexcept {
  size_t idx = static_cast<size_t>(id);
  if (idx < static_cast<size_t>(SystemTaskId::COUNT)) {
    s_task_handles[idx] = handle;
  }
}

TaskHandle_t System_GetTaskHandle(SystemTaskId id) noexcept {
  size_t idx = static_cast<size_t>(id);
  if (idx < static_cast<size_t>(SystemTaskId::COUNT)) {
    return s_task_handles[idx];
  }
  return nullptr;
}

// ── Global Diagnostics Instances ──

SystemMetricsTracker g_metrics;
static TaskWdtMonitor s_wdt_monitor;
PacketStatistics g_pkt_stats;
Ch1StateMetrics g_ch1_state_metrics;

// ── Sealed RTC Fast SRAM Retention State (L1-owned) ──
static RTC_NOINIT_ATTR uint32_t s_rtc_magic;
static RTC_NOINIT_ATTR uint32_t s_rtc_last_alive_ms[Config::Task::TASK_COUNT];
static RTC_NOINIT_ATTR uint32_t s_rtc_rescue_magic;
static RTC_NOINIT_ATTR uint32_t s_rtc_crash_counter;
static RTC_NOINIT_ATTR uint32_t s_rtc_clean_restart_magic;

static void Diagnostics_FeedWdtImpl(size_t index) noexcept {
  s_wdt_monitor.feed(index);
}

void Diagnostics_Init() noexcept {
  System_RegisterWdtHook(Diagnostics_FeedWdtImpl);
}

namespace {
struct AutoRegisterWdtHook {
  AutoRegisterWdtHook() {
    System_RegisterWdtHook(Diagnostics_FeedWdtImpl);
  }
} s_auto_wdt_hook;
}




// ── TaskWdtMonitor Implementation ──

void TaskWdtMonitor::feed(size_t index) noexcept {
  if (UNLIKELY(index >= TASK_COUNT))
    return;

  const uint32_t now = millis();
  s_rtc_last_alive_ms[index] = now;
  const uint32_t prev =
      tasks[index].last_feed_ms.exchange(now, std::memory_order_relaxed);
  if (prev > 0) {
    const uint32_t gap = (now >= prev) ? (now - prev) : 0;
    uint32_t cur_max =
        tasks[index].max_interval_ms.load(std::memory_order_relaxed);
    while (gap > cur_max && !tasks[index].max_interval_ms.compare_exchange_weak(
                                cur_max, gap, std::memory_order_relaxed,
                                std::memory_order_relaxed)) {
    }
  }
  tasks[index].feed_count.fetch_add(1, std::memory_order_relaxed);
  esp_task_wdt_reset();
}


// ── Diagnostics Functions Implementation ──

namespace {
struct StuckTaskDiag {
  bool found{false};
  char msg[36]{0};
};
static StuckTaskDiag s_stuck_diag;
static std::atomic<bool> s_ota_validated{false};
} // anonymous namespace


void System_Restart(const char *reason) {
  g_ota_in_progress.store(true, std::memory_order_release);
  if (g_system_event_group) {
    xEventGroupClearBits(g_system_event_group, SYS_EVT_OTA_IDLE);
  }
  vTaskDelay(pdMS_TO_TICKS(300));

  if (reason && strlen(reason) > 0) {
    LogManager::writeRebootLog(reason);
  }
  size_t count = s_shutdown_hook_count.load(std::memory_order_acquire);
  if (count > MAX_SHUTDOWN_HOOKS) {
    count = MAX_SHUTDOWN_HOOKS;
  }
  for (size_t i = 0; i < count; ++i) {
    if (s_shutdown_hooks[i]) {
      s_shutdown_hooks[i]();
    }
  }

  uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(50));
  uart_wait_tx_done(UART_NUM_1, pdMS_TO_TICKS(50));
  uart_wait_tx_done(UART_NUM_2, pdMS_TO_TICKS(50));

  s_rtc_clean_restart_magic = RTC_MAGIC_CLEAN_RESTART;
  vTaskDelay(pdMS_TO_TICKS(150));
  esp_restart();
}

// ── Sealed System State (L1-owned; exposed via System_* contracts) ──
static RTC_NOINIT_ATTR volatile uint32_t s_stage_marker = 0;
static std::atomic<bool> s_rescue_mode{false};
static std::atomic<bool> s_rollback_detected{false};

void System_MarkStage(uint32_t stage) noexcept { s_stage_marker = stage; }
bool System_IsRescueMode() noexcept {
  return s_rescue_mode.load(std::memory_order_relaxed);
}
void System_SetRollbackDetected() noexcept {
  s_rollback_detected.store(true, std::memory_order_release);
}
bool System_IsRollbackDetected() noexcept {
  return s_rollback_detected.load(std::memory_order_acquire);
}

uint32_t Diag_EvaluateCrashCounter(esp_reset_reason_t reason) noexcept {
  bool is_abnormal_crash =
      (reason == ESP_RST_PANIC || reason == ESP_RST_TASK_WDT ||
       reason == ESP_RST_INT_WDT || reason == ESP_RST_WDT ||
       reason == ESP_RST_BROWNOUT);

  if (s_rtc_rescue_magic != RTC_MAGIC_RESCUE || !is_abnormal_crash) {
    s_rtc_rescue_magic = RTC_MAGIC_RESCUE;
    s_rtc_crash_counter = 0;
  } else {
    s_rtc_crash_counter++;
    ::Serial.printf("[BOOT] Consecutive crash count: %u (Reason: %d)\r\n",
                    s_rtc_crash_counter, reason);
  }
  return s_rtc_crash_counter;
}

uint32_t Diag_GetCrashCounter() noexcept {
  return s_rtc_crash_counter;
}

void Diag_ResetCrashCounter() noexcept {
  s_rtc_crash_counter = 0;
}

void Diag_ResetTaskWdtAlive() noexcept {
  uint32_t now = millis();
  for (size_t i = 0; i < Config::Task::TASK_COUNT; i++) {
    s_rtc_last_alive_ms[i] = now;
  }
}

uint32_t System_GetCrashCounter() noexcept {
  return Diag_GetCrashCounter();
}

void Diag_DiagnoseStuck() {
  uint32_t saved_telnet_stage = s_stage_marker;
  s_stage_marker = 0;

  static const char *const TASK_NAMES[Config::Task::TASK_COUNT] = {
      "CH#1_IoT",  "CH#2_WP#1", "CH#3_WP#2",
      "CH#4_WP#3", "Network",   "Telnet_CLI"};
  static_assert(Config::Task::TASK_COUNT == 6, "Mismatch in TASK_COUNT");
  static_assert(Config::Task::WDT_ID_TELNET < Config::Task::TASK_COUNT,
                "WDT_ID_TELNET out of bounds");

  esp_reset_reason_t reason = esp_reset_reason();
  if (s_rtc_magic != RTC_MAGIC_WDT || reason == ESP_RST_POWERON) {
    s_rtc_magic = RTC_MAGIC_WDT;
    memset(s_rtc_last_alive_ms, 0, sizeof(s_rtc_last_alive_ms));
    return;
  }

  uint32_t max_val = 0;
  for (size_t i = 0; i < Config::Task::TASK_COUNT; i++) {
    if (s_rtc_last_alive_ms[i] > max_val)
      max_val = s_rtc_last_alive_ms[i];
  }

  if (max_val == 0)
    return;

  uint32_t max_gap = 0;
  int found_idx = -1;
  for (size_t i = 0; i < Config::Task::TASK_COUNT; i++) {
    if (s_rtc_last_alive_ms[i] > 0) {
      uint32_t gap = (max_val >= s_rtc_last_alive_ms[i])
                         ? (max_val - s_rtc_last_alive_ms[i])
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

  memset(s_rtc_last_alive_ms, 0, sizeof(s_rtc_last_alive_ms));
}

void Diag_LogResetReason() {
  esp_reset_reason_t reason = esp_reset_reason();

  if (reason == ESP_RST_SW) {
    if (s_rtc_clean_restart_magic == RTC_MAGIC_CLEAN_RESTART) {
      s_rtc_clean_restart_magic = 0;
      return;
    }
    s_pending_reboot_reason = "Software Reset (esp_restart)";
    return;
  }
  s_rtc_clean_restart_magic = 0;

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

void Diag_CheckCoreDump() {
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

void Diag_CheckOtaHealth() {
  if (s_ota_validated.load(std::memory_order_relaxed))
    return;

  bool wifi_ok = (WiFi.status() == WL_CONNECTED);
  bool hub_ok = g_pkt_stats.ch6.is_connected.load(std::memory_order_relaxed) ||
                g_pkt_stats.ch5.is_connected.load(std::memory_order_relaxed);

  bool rs485_ok =
      (millis() - g_pkt_stats.ch1.last_activity_ms.load(std::memory_order_relaxed) < 15000);
  bool time_ok = TimeUtils::isElapsed(s_boot_start_ms,
                                      Config::Timing::OTA_VALIDATION_PERIOD_MS);
  bool extended_time_ok = TimeUtils::isElapsed(s_boot_start_ms, 60000);

  if (!time_ok || !wifi_ok || (!hub_ok && !extended_time_ok) || !rs485_ok) {
    return;
  }

  s_ota_validated.store(true, std::memory_order_release);
  s_rtc_crash_counter = 0;
  const esp_partition_t *running = esp_ota_get_running_partition();
  if (running) {
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
        ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
      esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
      if (err == ESP_OK) {
        ::Serial.println(
            F("[OTA] ★ Firmware Health Verified! Auto-rollback cancelled."));
        System_TraceMessage(
            "[OTA] ★ Firmware Health Verified! Auto-rollback cancelled.\r\n");
      } else {
        ::Serial.printf("[OTA] Failed to mark app valid: 0x%x\r\n", err);
      }
    }
  }
}

// ── L0 Platform Contract Bridge ──
void System_CheckOtaHealth() {
  Diag_CheckOtaHealth();
}

void Diag_StartRescueAp(const RescueHwConfig &cfg) {
  s_rescue_mode.store(true, std::memory_order_release);
  ::Serial.println(F("\r\n========================================"));
  ::Serial.printf("  🚨 RESCUE SAFE MODE ACTIVATED: %s\r\n",
                  cfg.reason ? cfg.reason : "Unknown");
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

  if (cfg.sta_ssid && cfg.sta_ssid[0]) {
    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);
    wifi_config_t w_conf;
    memset(&w_conf, 0, sizeof(w_conf));
    strncpy(reinterpret_cast<char *>(w_conf.sta.ssid), cfg.sta_ssid,
            sizeof(w_conf.sta.ssid) - 1);
    if (cfg.sta_password && cfg.sta_password[0]) {
      strncpy(reinterpret_cast<char *>(w_conf.sta.password), cfg.sta_password,
              sizeof(w_conf.sta.password) - 1);
    }
    esp_wifi_set_config(WIFI_IF_STA, &w_conf);
    esp_wifi_connect();
  }

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

// ── Canonical System Platform Implementations (L0 Platform Facade) ───────────


void System_FormatTaskStacks(AppendBuf &out, const StackSnapshot &st) noexcept {
  auto gtag = [](uint16_t b) {
    return b >= 1000 ? "SAFE" : b >= 500 ? "WARN" : "CRIT";
  };

  const uint16_t stacks[6] = {st.ch1_stack, st.ch2_stack, st.ch3_stack,
                              st.ch4_stack, st.net_stack, st.telnet_stack};
  const char *names[6] = {"CH#1_IoT",  "CH#2_WP#1", "CH#3_WP#2",
                          "CH#4_WP#3", "Network",   "Telnet_CLI"};
  const char *scopes[6] = {"IoT Master Comm",    "Wallpad#1 HW Slave",
                           "Wallpad#2 HW Slave", "Wallpad#3 SW Slave",
                           "WiFi & TCP Manager", "Telnet CLI Server"};

  out.append(Fmt::DIV80);
  out.appendFormat("%-11s %-12s %-10s %-12s %-8s %-18s\r\n", "Task Name",
                   "Min Stack", "Last Feed", "Peak Intvl", "Status",
                   "Task Scope");
  out.append(Fmt::DIV80);

  uint32_t now = millis();
  for (size_t i = 0; i < 6; ++i) {
    uint32_t last_feed =
        s_wdt_monitor.tasks[i].last_feed_ms.load(std::memory_order_relaxed);
    uint32_t elapsed =
        (last_feed > 0 && now >= last_feed) ? (now - last_feed) : 0;
    uint32_t peak =
        s_wdt_monitor.tasks[i].max_interval_ms.load(std::memory_order_relaxed);

    out.appendFormat("%-11s %5u Bytes  %5u ms     %5u ms       %-7s %-18s\r\n",
                     names[i], stacks[i], static_cast<unsigned>(elapsed),
                     static_cast<unsigned>(peak), gtag(stacks[i]), scopes[i]);
  }
}


const char *System_ConsumePendingRebootReason() noexcept {
  return Diag_ConsumePendingRebootReason();
}

uint32_t System_GetBootTimeMs() noexcept {
  return Diag_GetBootTimeMs();
}

// ── LogManager & Persistent Reboot Log Implementation ────────────────────────

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

[[nodiscard]] std::expected<LogEntry, LogReadError>
LogManager::getLogEntry(size_t idx) noexcept {
  if (idx >= MAX_LOG_ENTRIES)
    return std::unexpected(LogReadError::OutOfBounds);
  Preferences p;
  if (!p.begin("logs", true))
    return std::unexpected(LogReadError::Empty);

  size_t count = p.getUInt("count", 0);
  if (idx >= count) {
    p.end();
    return std::unexpected(LogReadError::OutOfBounds);
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
      return env.payload;
    }
    return std::unexpected(LogReadError::OutOfBounds);
  }
  p.end();
  return std::unexpected(LogReadError::OutOfBounds);
}

bool LogManager::getLogEntry(size_t idx, LogEntry &out_entry) noexcept {
  auto res = getLogEntry(idx);
  if (!res.has_value())
    return false;
  out_entry = *res;
  return true;
}

void LogManager::clearRebootLog() {
  Preferences p;
  p.begin("logs", false);
  p.clear();
  p.end();
}

size_t System_GetRebootLogCount() noexcept {
  return LogManager::getLogCount();
}

bool System_GetRebootLogEntry(size_t index, LogEntry &out_entry) noexcept {
  return LogManager::getLogEntry(index, out_entry);
}

void System_WriteRebootLog(const char *reason) noexcept {
  LogManager::writeRebootLog(reason);
}

void System_ClearRebootLog() noexcept {
  LogManager::clearRebootLog();
}

// ── SystemMetricsTracker & Telemetry Implementations ─────────────────────────

namespace {
constexpr uint32_t MIN_SAMPLE_INTERVAL_MS = 100;
constexpr uint32_t CPU0_BASE_LOAD = 3;
constexpr uint32_t CPU0_PPS_DIVISOR = 3;
constexpr uint32_t CPU1_BASE_LOAD = 2;
constexpr uint32_t CPU1_PPS_DIVISOR = 8;

struct MetricAccumulator {
  uint32_t cpu0_sum = 0, cpu1_sum = 0, ram_sum = 0, flash_sum = 0;
  int32_t temp_sum = 0;
  uint8_t cpu0_peak = 0, cpu1_peak = 0;
  uint16_t ram_peak = 0, flash_peak = 0;
  int8_t temp_peak = -127;
  uint32_t count = 0;

  void add(uint8_t c0, uint8_t c1, uint16_t ram, uint16_t flash, int8_t temp) {
    cpu0_sum += c0;
    cpu1_sum += c1;
    ram_sum += ram;
    flash_sum += flash;
    temp_sum += temp;
    count++;
    cpu0_peak = std::max(cpu0_peak, c0);
    cpu1_peak = std::max(cpu1_peak, c1);
    ram_peak = std::max(ram_peak, ram);
    flash_peak = std::max(flash_peak, flash);
    temp_peak = std::max(temp_peak, temp);
  }

  void addBucket(const MetricBucket &b) {
    if (!b.count)
      return;
    cpu0_sum += b.cpu0_sum;
    cpu1_sum += b.cpu1_sum;
    ram_sum += b.ram_sum;
    temp_sum += b.temp_sum;
    count += b.count;
    cpu0_peak = std::max(cpu0_peak, b.cpu0_peak);
    cpu1_peak = std::max(cpu1_peak, b.cpu1_peak);
    ram_peak = std::max(ram_peak, b.ram_peak);
    temp_peak = std::max(temp_peak, b.temp_peak);
  }

  StatSummary finalize(uint16_t fallback_flash = 0) const {
    StatSummary r = {};
    if (!count)
      return r;
    r.cpu0_avg = cpu0_sum / count;
    r.cpu0_peak = cpu0_peak;
    r.cpu1_avg = cpu1_sum / count;
    r.cpu1_peak = cpu1_peak;
    r.ram_avg = ram_sum / count;
    r.ram_peak = ram_peak;
    r.flash_avg = flash_sum ? (flash_sum / count) : fallback_flash;
    r.flash_peak = flash_peak ? flash_peak : fallback_flash;
    r.temp_avg = temp_sum / static_cast<int32_t>(count);
    r.temp_peak = temp_peak;
    r.count = static_cast<uint16_t>(std::min<uint32_t>(count, 65535U));
    return r;
  }
};
} // namespace

void SystemMetricsTracker::init() {
  if (!_metrics_mutex)
    _metrics_mutex = xSemaphoreCreateMutex();
  _cached_flash_kb = _current.flash_kb =
      static_cast<uint16_t>(ESP.getSketchSize() / 1024);
  memset(&_cur_bucket, 0, sizeof(_cur_bucket));
}

void SystemMetricsTracker::reset() {
  MutexLocker lock(_metrics_mutex);
  _ring15_head = 0;
  _ring15_count = 0;
  _ring24_head = 0;
  _ring24_count = 0;
  _bucket_sample_count = 0;
  memset(&_cur_bucket, 0, sizeof(_cur_bucket));
}

void SystemMetricsTracker::addSample(uint8_t cpu0_pct, uint8_t cpu1_pct,
                                     uint16_t ram_kb, int8_t temp_c) {
  const uint16_t flash_kb = _cached_flash_kb;
  MutexLocker lock(_metrics_mutex);
  _current = {cpu0_pct, cpu1_pct, ram_kb, flash_kb, temp_c};

  _ring15[_ring15_head] = _current;
  _ring15_head = (_ring15_head + 1) % SAMPLES_15M;
  if (_ring15_count < SAMPLES_15M)
    _ring15_count++;

  _cur_bucket.cpu0_sum += cpu0_pct;
  _cur_bucket.cpu1_sum += cpu1_pct;
  _cur_bucket.ram_sum += ram_kb;
  _cur_bucket.temp_sum += temp_c;

  _cur_bucket.cpu0_peak = std::max(_cur_bucket.cpu0_peak, cpu0_pct);
  _cur_bucket.cpu1_peak = std::max(_cur_bucket.cpu1_peak, cpu1_pct);
  _cur_bucket.ram_peak = std::max(_cur_bucket.ram_peak, ram_kb);
  if (_cur_bucket.count == 0 || temp_c > _cur_bucket.temp_peak) {
    _cur_bucket.temp_peak = temp_c;
  }

  _cur_bucket.count++;
  _bucket_sample_count++;

  if (_bucket_sample_count >= SAMPLES_15M) {
    _ring24[_ring24_head] = _cur_bucket;
    _ring24_head = (_ring24_head + 1) % BUCKETS_24H;
    if (_ring24_count < BUCKETS_24H)
      _ring24_count++;
    memset(&_cur_bucket, 0, sizeof(_cur_bucket));
    _bucket_sample_count = 0;
  }
}

StatSummary SystemMetricsTracker::get15m() const {
  MutexLocker lock(_metrics_mutex);
  MetricAccumulator acc;
  for (size_t i = 0; i < _ring15_count; i++) {
    const auto &ms = _ring15[i];
    acc.add(ms.cpu0_pct, ms.cpu1_pct, ms.ram_kb, ms.flash_kb, ms.temp_c);
  }
  return acc.finalize();
}

StatSummary SystemMetricsTracker::get24h() const {
  MutexLocker lock(_metrics_mutex);
  MetricAccumulator acc;
  for (size_t i = 0; i < _ring24_count; i++) {
    acc.addBucket(_ring24[i]);
  }
  acc.addBucket(_cur_bucket);
  return acc.finalize(_cached_flash_kb);
}

MetricSample SystemMetricsTracker::getCurrent() const noexcept {
  MutexLocker lock(_metrics_mutex);
  return _current;
}

int8_t System_ReadTempC() { return static_cast<int8_t>(temperatureRead()); }

void System_ReadCpuPct(uint8_t &cpu0_out, uint8_t &cpu1_out) {
  static std::atomic<uint32_t> s_last_time_ms{0}, s_last_ch1{0}, s_last_ch23{0},
      s_last_tcp{0};

  uint32_t now_ms = millis();
  uint32_t prev_ms = s_last_time_ms.load(std::memory_order_relaxed);

  uint32_t cur_ch1 = g_pkt_stats.ch1.rx_pkts.load(std::memory_order_relaxed) +
                     g_pkt_stats.ch1.tx_pkts.load(std::memory_order_relaxed);
  uint32_t cur_ch23 = g_pkt_stats.ch2.rx_pkts.load(std::memory_order_relaxed) +
                      g_pkt_stats.ch2.tx_pkts.load(std::memory_order_relaxed) +
                      g_pkt_stats.ch3.rx_pkts.load(std::memory_order_relaxed) +
                      g_pkt_stats.ch3.tx_pkts.load(std::memory_order_relaxed) +
                      g_pkt_stats.ch4.rx_pkts.load(std::memory_order_relaxed) +
                      g_pkt_stats.ch4.tx_pkts.load(std::memory_order_relaxed);
  uint32_t cur_tcp = g_pkt_stats.ch5.rx_pkts.load(std::memory_order_relaxed) +
                     g_pkt_stats.ch5.tx_pkts.load(std::memory_order_relaxed) +
                     g_pkt_stats.ch6.rx_pkts.load(std::memory_order_relaxed) +
                     g_pkt_stats.ch6.tx_pkts.load(std::memory_order_relaxed);

  uint32_t elapsed_ms = now_ms - prev_ms;
  if (!prev_ms || elapsed_ms < MIN_SAMPLE_INTERVAL_MS) {
    if (!prev_ms) {
      s_last_time_ms.store(now_ms, std::memory_order_relaxed);
      s_last_ch1.store(cur_ch1, std::memory_order_relaxed);
      s_last_ch23.store(cur_ch23, std::memory_order_relaxed);
      s_last_tcp.store(cur_tcp, std::memory_order_relaxed);
    }
    cpu0_out = 4;
    cpu1_out = 3;
    return;
  }

  auto get_delta = [](uint32_t cur, std::atomic<uint32_t> &last) {
    uint32_t prev = last.exchange(cur, std::memory_order_relaxed);
    return (cur >= prev) ? (cur - prev) : cur;
  };

  s_last_time_ms.store(now_ms, std::memory_order_relaxed);
  uint32_t delta_tcp = get_delta(cur_tcp, s_last_tcp);
  uint32_t delta_uart =
      get_delta(cur_ch1, s_last_ch1) + get_delta(cur_ch23, s_last_ch23);

  uint32_t tcp_pps = static_cast<uint32_t>(
      (static_cast<uint64_t>(delta_tcp) * 1000) / elapsed_ms);
  uint32_t load0 = CPU0_BASE_LOAD + (tcp_pps / CPU0_PPS_DIVISOR);
  if (WiFi.isConnected())
    load0 += 1;
  if (g_pkt_stats.ch6.is_connected.load(std::memory_order_relaxed))
    load0 += 1;

  uint32_t uart_pps = static_cast<uint32_t>(
      (static_cast<uint64_t>(delta_uart) * 1000) / elapsed_ms);
  uint32_t load1 = CPU1_BASE_LOAD + (uart_pps / CPU1_PPS_DIVISOR);

  cpu0_out = static_cast<uint8_t>(std::min<uint32_t>(load0, 99UL));
  cpu1_out = static_cast<uint8_t>(std::min<uint32_t>(load1, 99UL));
}

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

  auto get_stack = [](SystemTaskId id) -> uint16_t {
    TaskHandle_t h = System_GetTaskHandle(id);
    return h ? static_cast<uint16_t>(uxTaskGetStackHighWaterMark(h)) : 0;
  };

  st.ch1_stack = get_stack(SystemTaskId::CH1);
  st.ch2_stack = get_stack(SystemTaskId::CH2);
  st.ch3_stack = get_stack(SystemTaskId::CH3);
  st.ch4_stack = get_stack(SystemTaskId::CH4);
  st.net_stack = get_stack(SystemTaskId::NETWORK);
  st.telnet_stack = get_stack(SystemTaskId::TELNET);

  pkt.ch1 = SingleChannelToSnapshot(g_pkt_stats.ch1);
  pkt.ch2 = SingleChannelToSnapshot(g_pkt_stats.ch2);
  pkt.ch3 = SingleChannelToSnapshot(g_pkt_stats.ch3);
  pkt.ch4 = SingleChannelToSnapshot(g_pkt_stats.ch4);
  pkt.ch5 = TcpSocketToSnapshot(g_pkt_stats.ch5);
  pkt.ch6 = TcpSocketToSnapshot(g_pkt_stats.ch6);
}

void System_GetCpuAndTemp(uint8_t &cpu0, uint8_t &cpu1, int8_t &temp_c) noexcept {
  System_ReadCpuPct(cpu0, cpu1);
  temp_c = System_ReadTempC();
}

void System_GetPktSnapshot(PktSnapshot &pkt) noexcept {
  pkt.ch1 = SingleChannelToSnapshot(g_pkt_stats.ch1);
  pkt.ch2 = SingleChannelToSnapshot(g_pkt_stats.ch2);
  pkt.ch3 = SingleChannelToSnapshot(g_pkt_stats.ch3);
  pkt.ch4 = SingleChannelToSnapshot(g_pkt_stats.ch4);
  pkt.ch5 = TcpSocketToSnapshot(g_pkt_stats.ch5);
  pkt.ch6 = TcpSocketToSnapshot(g_pkt_stats.ch6);
}

void System_GetCh1Metrics(uint32_t &poll_cnt, uint32_t &vip_cnt, uint32_t &normal_cnt) noexcept {
  poll_cnt = g_ch1_state_metrics.poll_cnt.load(std::memory_order_relaxed);
  vip_cnt = g_ch1_state_metrics.vip_cnt.load(std::memory_order_relaxed);
  normal_cnt = g_ch1_state_metrics.normal_cnt.load(std::memory_order_relaxed);
}

void System_RecordMetricsSample(uint16_t used_ram_kb) noexcept {
  uint8_t c0 = 0, c1 = 0;
  System_ReadCpuPct(c0, c1);
  g_metrics.addSample(c0, c1, used_ram_kb, System_ReadTempC());
}

void System_ResetTrafficStats() noexcept {
  g_pkt_stats.resetAll();
  g_metrics.reset();
}

void System_RecordCh5Tx() noexcept {
  g_pkt_stats.ch5.tx_pkts.fetch_add(1, std::memory_order_relaxed);
}

void System_RecordCh5Rx() noexcept {
  g_pkt_stats.ch5.rx_pkts.fetch_add(1, std::memory_order_relaxed);
}

void System_RecordCh5Dropped() noexcept {
  g_pkt_stats.ch5.dropped_pkts.fetch_add(1, std::memory_order_relaxed);
}

void System_SetCh5Connected(bool conn) noexcept {
  g_pkt_stats.ch5.is_connected.store(conn, std::memory_order_relaxed);
}

void System_RecordCh5Connection() noexcept {
  g_pkt_stats.ch5.is_connected.store(true, std::memory_order_relaxed);
  g_pkt_stats.ch5.connection_count.fetch_add(1, std::memory_order_relaxed);
}

void System_RecordCh6Tx() noexcept {
  g_pkt_stats.ch6.tx_pkts.fetch_add(1, std::memory_order_relaxed);
}

void System_RecordCh6Rx() noexcept {
  g_pkt_stats.ch6.rx_pkts.fetch_add(1, std::memory_order_relaxed);
}

void System_SetCh6Connected(bool conn) noexcept {
  g_pkt_stats.ch6.is_connected.store(conn, std::memory_order_relaxed);
}

void System_RecordCh6Connection() noexcept {
  g_pkt_stats.ch6.is_connected.store(true, std::memory_order_relaxed);
  g_pkt_stats.ch6.connection_count.fetch_add(1, std::memory_order_relaxed);
}


