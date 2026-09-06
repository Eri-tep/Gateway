#include "Common.h"
#include "ESP.h"
#include "MgmtRpc.h"
#include "TelnetCli.h"
#include "WallpadParser.h"
#include "ControlTemplate.h"
#include "esp_sntp.h"
#include "lwip/ip.h"
#include "lwip/tcp.h"

// ============================================================================
// SECTION 1: GLOBAL STATE, MUTEXES & RUNTIME CONFIG
// ============================================================================

#include "esp_attr.h"
#include "esp_ota_ops.h"

RTC_NOINIT_ATTR uint32_t rtc_magic;
RTC_NOINIT_ATTR uint32_t rtc_last_alive_ms[6];
constexpr uint32_t RTC_MAGIC_WDT = 0x57445431;

RTC_NOINIT_ATTR uint32_t rtc_rescue_magic;
RTC_NOINIT_ATTR uint32_t rtc_crash_counter;
constexpr uint32_t RTC_MAGIC_RESCUE = 0x52455343; // 'RESC'

RTC_NOINIT_ATTR uint32_t rtc_clean_restart_magic;
constexpr uint32_t RTC_MAGIC_CLEAN_RESTART = 0x5AA55AA5;

RTC_NOINIT_ATTR RtcWarmCache rtc_warm_cache;

std::atomic<bool> g_rescue_mode{false};
bool g_rollback_detected = false;
static std::atomic<bool> s_ota_validated{false};
static uint32_t s_last_sta_retry_ms = 0;
static uint32_t s_wifi_disconnect_count = 0;

struct StuckTaskDiag {
  bool found{false};
  char msg[36]{0};
};

static StuckTaskDiag s_stuck_diag;

static void System_DiagnoseStuck() {
  static const char *const TASK_NAMES[6] = {"CH#1_IoT",  "CH#2_WP#1",
                                            "CH#3_WP#2", "CH#4_WP#3",
                                            "Network",   "Telnet_CLI"};

  esp_reset_reason_t reason = esp_reset_reason();

  if (rtc_magic != RTC_MAGIC_WDT || reason == ESP_RST_POWERON) {
    rtc_magic = RTC_MAGIC_WDT;
    memset(rtc_last_alive_ms, 0, sizeof(rtc_last_alive_ms));
    return;
  }

  uint32_t max_val = 0;
  for (size_t i = 0; i < 6; i++) {
    if (rtc_last_alive_ms[i] > max_val)
      max_val = rtc_last_alive_ms[i];
  }

  if (max_val == 0)
    return;

  uint32_t max_gap = 0;
  int found_idx = -1;

  for (size_t i = 0; i < 6; i++) {
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
  if (found_idx >= 0 && found_idx < 6) {
    snprintf(s_stuck_diag.msg, sizeof(s_stuck_diag.msg),
             "Task WDT: %s (+%.1fs)", TASK_NAMES[found_idx], max_gap / 1000.0f);
  } else {
    snprintf(s_stuck_diag.msg, sizeof(s_stuck_diag.msg),
             "Task WDT: All Tasks Stalled");
  }

  memset(rtc_last_alive_ms, 0, sizeof(rtc_last_alive_ms));
}

static const char *s_pending_reboot_reason = nullptr;

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
    if (s_stuck_diag.found) {
      reason_str = s_stuck_diag.msg;
    } else {
      reason_str = "Task Watchdog Reset";
    }
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

CoreDumpInfo g_coredump_info;
Config::Doorphone::FramingTracker g_doorphone_tracker;

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
    for (uint8_t i = 0; i < depth; i++)
      g_coredump_info.bt[i] = s.exc_bt_info.bt[i];
  }
#endif
}

bool System_IsOtaPendingVerify() {
  const esp_partition_t *running = esp_ota_get_running_partition();
  if (!running)
    return false;
  esp_ota_img_states_t ota_state;
  if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
    return (ota_state == ESP_OTA_IMG_PENDING_VERIFY || ota_state == ESP_OTA_IMG_NEW);
  }
  return false;
}

void System_CheckOtaHealth() {
  if (s_ota_validated.load(std::memory_order_relaxed)) {
    return;
  }
  if (WiFi.status() == WL_CONNECTED &&
      TimeUtils::isElapsed(g_boot_start_ms, Config::Timing::OTA_VALIDATION_PERIOD_MS)) {
    s_ota_validated.store(true, std::memory_order_release);
    rtc_crash_counter = 0;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running) {
      esp_ota_img_states_t ota_state;
      if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
          ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
          Serial.println(F("[OTA] ★ Firmware Health Verified! Auto-rollback cancelled."));
          g_telnet_tracer.trace("[OTA] ★ Firmware Health Verified! Auto-rollback cancelled.\r\n");
        } else {
          Serial.printf("[OTA] Failed to mark app valid: 0x%x\r\n", err);
        }
      }
    }
  }
}

void System_EnterRescueMode(const char *reason) {
  g_rescue_mode.store(true, std::memory_order_release);
  Serial.println(F("\r\n========================================"));
  Serial.printf("  🚨 RESCUE SAFE MODE ACTIVATED: %s\r\n", reason ? reason : "Unknown");
  Serial.println(F("========================================"));

  WiFi.mode(WIFI_AP_STA);
  vTaskDelay(pdMS_TO_TICKS(100));

  WiFi.softAPConfig(IPAddress(172, 30, 2, 1), IPAddress(172, 30, 2, 1),
                    IPAddress(255, 255, 255, 0));
  bool ap_ok = WiFi.softAP("Sweet_Home_Rescue", EMERGENCY_AP_PASS, 1, 0, 4);
  WiFi.setSleep(false);
  esp_wifi_set_max_tx_power(78);

  Serial.printf("[RESCUE] SoftAP 'Sweet_Home_Rescue' started: %s (IP: %s)\r\n",
                ap_ok ? "SUCCESS" : "FAILED", WiFi.softAPIP().toString().c_str());

  // STA (기존 홈 공유기 172.30.1.3) 접속도 함께 유지하여 공유기망에서도 복구 가능하도록 지원
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
  ArduinoOTA.onEnd([]() {
    g_ota_in_progress.store(false, std::memory_order_release);
    if (g_system_event_group) {
      xEventGroupSetBits(g_system_event_group, SYS_EVT_OTA_IDLE);
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    System_Restart("OTA Firmware Update");
  });
  ArduinoOTA.onError([](ota_error_t error) {
    g_ota_in_progress.store(false, std::memory_order_release);
    if (g_system_event_group) {
      xEventGroupSetBits(g_system_event_group, SYS_EVT_OTA_IDLE);
    }
  });
  ArduinoOTA.begin();
}

// ============================================================================
// 1ST-TIER WARM-START CACHE ENGINE (RTC FAST SRAM & NVS SNAPSHOT)
// ============================================================================

bool g_warm_cache_loaded = false;
uint8_t g_warm_cache_source = 0; // 0: None/Cold, 1: RTC SRAM, 2: NVS Flash
uint8_t g_warm_cache_restored_count = 0;
std::atomic<bool> g_warm_cache_dirty{false};
std::atomic<uint32_t> g_warm_cache_dirty_ms{0};

void WarmCache_SaveToRtc() {
  memset(&rtc_warm_cache, 0, sizeof(rtc_warm_cache));
  rtc_warm_cache.magic = RTC_MAGIC_WARM_CACHE;
  rtc_warm_cache.count = static_cast<uint8_t>(g_polling_targets.getWarmCacheEntries(
      rtc_warm_cache.entries, PollingTargetRegistry::MAX_TARGETS));
  if (rtc_warm_cache.count > 0) {
    rtc_warm_cache.crc32 = FastCrc32(reinterpret_cast<const uint8_t *>(rtc_warm_cache.entries),
                                    sizeof(RtcWarmCacheEntry) * rtc_warm_cache.count);
  }
}

void WarmCache_SaveToNvs() {
  WarmCache_SaveToRtc();
  if (rtc_warm_cache.count > 0) {
    Preferences p;
    if (p.begin("wp_wc", false)) {
      static NvsEnvelope<RtcWarmCache> env;
      env.payload = rtc_warm_cache;
      env.seal();
      p.putBytes("wc_data", &env, sizeof(env));
      p.end();
      Serial.printf("[WARM CACHE] Synced %u targets to NVS Flash snapshot.\r\n",
                    rtc_warm_cache.count);
    }
  }
  g_warm_cache_dirty.store(false, std::memory_order_release);
}

void WarmCache_RestoreOnBoot() {
  uint32_t now = millis();
  esp_reset_reason_t reason = esp_reset_reason();

  // 1. Try RTC Fast SRAM (Available across soft reboots, WDT, OTA)
  if (reason != ESP_RST_POWERON && rtc_warm_cache.magic == RTC_MAGIC_WARM_CACHE &&
      rtc_warm_cache.count > 0 && rtc_warm_cache.count <= PollingTargetRegistry::MAX_TARGETS) {
    uint32_t computed_crc = FastCrc32(
        reinterpret_cast<const uint8_t *>(rtc_warm_cache.entries),
        sizeof(RtcWarmCacheEntry) * rtc_warm_cache.count);
    if (computed_crc == rtc_warm_cache.crc32) {
      g_polling_targets.loadFromWarmCache(rtc_warm_cache.entries,
                                         rtc_warm_cache.count, now);
      g_warm_cache_loaded = true;
      g_warm_cache_source = 1;
      g_warm_cache_restored_count = rtc_warm_cache.count;
      Serial.printf("[WARM CACHE] Restored %u targets from RTC Fast SRAM (0ms delay)!\r\n",
                    rtc_warm_cache.count);
      return;
    }
  }

  // 2. Try NVS Flash Snapshot (Fallback after power loss)
  Preferences p;
  if (p.begin("wp_wc", true)) {
    if (p.isKey("wc_data")) {
      static NvsEnvelope<RtcWarmCache> env;
      size_t len = p.getBytesLength("wc_data");
      if (len == sizeof(env) && p.getBytes("wc_data", &env, sizeof(env)) == sizeof(env)) {
        if (env.verify() && env.payload.count > 0 &&
            env.payload.count <= PollingTargetRegistry::MAX_TARGETS) {
          uint32_t computed_crc = FastCrc32(
              reinterpret_cast<const uint8_t *>(env.payload.entries),
              sizeof(RtcWarmCacheEntry) * env.payload.count);
          if (computed_crc == env.payload.crc32) {
            g_polling_targets.loadFromWarmCache(env.payload.entries,
                                               env.payload.count, now);
            g_warm_cache_loaded = true;
            g_warm_cache_source = 2;
            g_warm_cache_restored_count = env.payload.count;
            Serial.printf("[WARM CACHE] Restored %u targets from NVS Flash snapshot!\r\n",
                          env.payload.count);
            p.end();
            return;
          }
        }
      }
    }
    p.end();
  }

  // 3. Cold Start
  g_warm_cache_loaded = false;
  g_warm_cache_source = 0;
  g_warm_cache_restored_count = 0;
  Serial.println(F("[WARM CACHE] Cold start initialized (No prior cache found)."));
}

void WarmCache_CheckNvsDebounce() {
  if (g_warm_cache_dirty.load(std::memory_order_acquire)) {
    uint32_t dirty_ms = g_warm_cache_dirty_ms.load(std::memory_order_relaxed);
    if (dirty_ms > 0 &&
        TimeUtils::isElapsed(dirty_ms, Config::Timing::WARM_CACHE_NVS_DEBOUNCE_MS)) {
      WarmCache_SaveToRtc();
      WarmCache_SaveToNvs();
    }
  }
}

DeviceRepository g_device_repo;
ControlDispatcher g_control_dispatcher;
PacketStatistics g_pkt_stats;
SystemMetricsTracker g_metrics;
TaskWdtMonitor g_wdt_monitor;

StaticQueue_t g_ch1_ctrl_queue_buf, g_ch4_pass_queue_buf, g_ch4_to_tcp_queue_buf,
    g_ch1_vip_queue_buf, g_ch6_to_tcp_queue_buf;
uint8_t g_ch1_ctrl_storage[Config::Queue::POOL_SIZE_CONTROL * sizeof(StaticPacket)];
uint8_t g_ch4_pass_storage[Config::Queue::POOL_SIZE_CONTROL * sizeof(StaticPacket)];
uint8_t
    g_ch4_to_tcp_storage[Config::Queue::POOL_SIZE_CONTROL * sizeof(StaticPacket)];
uint8_t g_ch1_vip_storage[Config::Queue::POOL_SIZE_CONTROL * sizeof(StaticPacket)];
uint8_t
    g_ch6_to_tcp_storage[Config::Queue::POOL_SIZE_CONTROL * sizeof(StaticPacket)];

QueueHandle_t g_ch1_control_queue = nullptr, g_ch1_vip_queue = nullptr;
QueueSetHandle_t g_ch1_queue_set = nullptr;
QueueHandle_t g_uart0_event_queue = nullptr, g_uart1_event_queue = nullptr,
              g_uart2_event_queue = nullptr;
QueueHandle_t g_ch4_passthrough_queue = nullptr, g_ch4_to_tcp_queue = nullptr,
              g_ch6_to_tcp_queue = nullptr;

EventGroupHandle_t g_wifi_event_group = nullptr;
EventGroupHandle_t g_system_event_group = nullptr;
static constexpr EventBits_t WIFI_BIT_CONNECTED    = BIT0;
static constexpr EventBits_t WIFI_BIT_DISCONNECTED = BIT1;
static constexpr EventBits_t WIFI_BIT_GOT_IP       = BIT2;

static void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
  case ARDUINO_EVENT_WIFI_STA_START:
    Serial.println(F("[WIFI EVENT] STA Started"));
    break;
  case ARDUINO_EVENT_WIFI_STA_CONNECTED:
    Serial.println(F("[WIFI EVENT] STA Connected to AP"));
    if (g_wifi_event_group) {
      xEventGroupSetBits(g_wifi_event_group, WIFI_BIT_CONNECTED);
      xEventGroupClearBits(g_wifi_event_group, WIFI_BIT_DISCONNECTED);
    }
    break;
  case ARDUINO_EVENT_WIFI_STA_GOT_IP:
    s_wifi_disconnect_count = 0;
    Serial.printf("[WIFI EVENT] STA Got IP: %s\r\n",
                  IPAddress(info.got_ip.ip_info.ip.addr).toString().c_str());
    if (g_wifi_event_group) {
      xEventGroupSetBits(g_wifi_event_group, WIFI_BIT_GOT_IP);
    }
    break;
  case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
    s_wifi_disconnect_count++;
    Serial.printf("[WIFI EVENT] STA Disconnected (Reason: %d, Count: %u)\r\n",
                  info.wifi_sta_disconnected.reason, s_wifi_disconnect_count);
    if (g_wifi_event_group) {
      xEventGroupSetBits(g_wifi_event_group, WIFI_BIT_DISCONNECTED);
      xEventGroupClearBits(g_wifi_event_group, WIFI_BIT_CONNECTED);
    }
    break;
  case ARDUINO_EVENT_WIFI_AP_START:
    Serial.println(F("[WIFI EVENT] SoftAP Started"));
    break;
  case ARDUINO_EVENT_WIFI_AP_STOP:
    Serial.println(F("[WIFI EVENT] SoftAP Stopped"));
    break;
  case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
    Serial.printf("[WIFI EVENT] AP Station Connected! MAC: "
                  "%02X:%02X:%02X:%02X:%02X:%02X, AID: %d\r\n",
                  info.wifi_ap_staconnected.mac[0],
                  info.wifi_ap_staconnected.mac[1],
                  info.wifi_ap_staconnected.mac[2],
                  info.wifi_ap_staconnected.mac[3],
                  info.wifi_ap_staconnected.mac[4],
                  info.wifi_ap_staconnected.mac[5],
                  info.wifi_ap_staconnected.aid);
    break;
  case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
    Serial.printf("[WIFI EVENT] AP Station Disconnected! MAC: "
                  "%02X:%02X:%02X:%02X:%02X:%02X, AID: %d\r\n",
                  info.wifi_ap_stadisconnected.mac[0],
                  info.wifi_ap_stadisconnected.mac[1],
                  info.wifi_ap_stadisconnected.mac[2],
                  info.wifi_ap_stadisconnected.mac[3],
                  info.wifi_ap_stadisconnected.mac[4],
                  info.wifi_ap_stadisconnected.mac[5],
                  info.wifi_ap_stadisconnected.aid);
    break;
  default:
    break;
  }
}

SoftwareSerial g_doorphone_serial;
RuntimeConfig g_config{};

struct TcpFragSession {
  int sock = -1;
  uint8_t buffer[256];
  size_t len = 0;
  uint32_t connected_at_ms = 0;
};
static TcpFragSession hub_sessions[Config::TCP::MAX_HUB_CLIENTS];
SemaphoreHandle_t g_ch6_mutex = nullptr;

struct DoorphoneSession {
  int sock = -1;
  uint8_t buffer[256];
  size_t len = 0;
  uint32_t last_tcp_cmd_ms = 0;
  uint8_t last_tcp_cmd = 0;
  uint32_t connected_at_ms = 0;
};
static DoorphoneSession doorphone_sessions[Config::TCP::MAX_DOORPHONE_CLIENTS];
SemaphoreHandle_t g_ch5_mutex = nullptr;
SemaphoreHandle_t g_ctrl_queue_mutex = nullptr;

template <typename SessionType, size_t N>
static void Tcp_CloseAllSessions(SessionType (&sessions)[N], SemaphoreHandle_t mux) noexcept {
  MutexLocker lock(mux);
  for (size_t i = 0; i < N; i++) {
    if (sessions[i].sock >= 0) {
      close(sessions[i].sock);
      sessions[i].sock = -1;
      sessions[i].len = 0;
    }
  }
}

template <typename SessionType, size_t N>
[[nodiscard]] static bool Tcp_HasActiveSession(SessionType (&sessions)[N], SemaphoreHandle_t mux) noexcept {
  MutexLocker lock(mux);
  for (size_t i = 0; i < N; i++) {
    if (sessions[i].sock >= 0)
      return true;
  }
  return false;
}

template <typename SessionType, size_t N, typename DataHandler>
static void Tcp_PollAndReceive(SessionType (&sessions)[N], SemaphoreHandle_t mux,
                              fd_set &readfds, fd_set &errorfds,
                              DataHandler handler) {
  MutexLocker lock(mux);
  for (size_t i = 0; i < N; i++) {
    int s = sessions[i].sock;
    if (s < 0)
      continue;

    if (FD_ISSET(s, &errorfds)) {
      close(s);
      sessions[i].sock = -1;
      sessions[i].len = 0;
      continue;
    }

    if (FD_ISSET(s, &readfds)) {
      uint8_t rx_buf[128];
      int r = recv(s, rx_buf, sizeof(rx_buf), 0);
      if (r > 0) {
        handler(&sessions[i], rx_buf, r);
      } else if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        close(s);
        sessions[i].sock = -1;
        sessions[i].len = 0;
      }
    }
  }
}

static TokenBucket s_ch5_bucket(Config::TCP::CH5_TOKEN_BURST,
                                Config::TCP::CH5_TOKEN_REFILL_MS);
static TokenBucket s_ch6_bucket(Config::TCP::CH6_TOKEN_BURST,
                                Config::TCP::CH6_TOKEN_REFILL_MS);

StaticTask_t g_task_core1_ch1_buf, g_task_core1_slave_buf, g_task_core1_slave2_buf,
    g_task_core1_ch4_buf, g_task_core0_net_buf, g_telnet_task_buf;
StackType_t stackCore1Ch1[Config::Task::STACK_SIZE_CORE1],
    stackCore1Slave[Config::Task::STACK_SIZE_CORE1],
    stackCore1Slave2[Config::Task::STACK_SIZE_CORE1],
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
portMUX_TYPE g_config_mux = portMUX_INITIALIZER_UNLOCKED;
std::atomic<bool> g_config_dirty{false}, g_ota_in_progress{false},
    g_initial_caching_complete{false}, g_probe_convergence_reset{false};
WifiFallbackGuard g_wifi_guard;

static void Tcp_EnableKeepalive(int sock, int idle, int intvl, int cnt) {
  if (sock < 0)
    return;
  int keepalive = 1;
  setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
  setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
  setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
  setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
}

template <typename SessionType, size_t N>
static int Tcp_AcceptAndAssignSlot(int server_fd,
                                   SessionType (&sessions)[N],
                                   SemaphoreHandle_t mux,
                                   int keepalive_idle,
                                   int keepalive_intvl,
                                   int keepalive_cnt,
                                   TcpSocketStats &stat) {
  struct sockaddr_in caddr;
  socklen_t clen = sizeof(caddr);
  int new_sock = accept(server_fd, reinterpret_cast<struct sockaddr *>(&caddr), &clen);
  if (new_sock < 0)
    return -1;

  const uint8_t *b = reinterpret_cast<const uint8_t *>(&caddr.sin_addr.s_addr);
  IPAddress remote_ip(b[0], b[1], b[2], b[3]);
  if (!Tcp_IsAllowedIP(remote_ip)) {
    close(new_sock);
    return -1;
  }

  int flags = fcntl(new_sock, F_GETFL, 0);
  fcntl(new_sock, F_SETFL, flags | O_NONBLOCK);
  int nodelay = 1;
  setsockopt(new_sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
  int sockbuf = Config::TCP::SOCKET_BUFFER_SIZE;
  setsockopt(new_sock, SOL_SOCKET, SO_RCVBUF, &sockbuf, sizeof(sockbuf));
  setsockopt(new_sock, SOL_SOCKET, SO_SNDBUF, &sockbuf, sizeof(sockbuf));
  Tcp_EnableKeepalive(new_sock, keepalive_idle, keepalive_intvl, keepalive_cnt);

  MutexLocker lock(mux);
  int slot = -1;
  for (size_t i = 0; i < N; i++) {
    if (sessions[i].sock < 0) {
      slot = static_cast<int>(i);
      break;
    }
  }
  if (slot == -1) {
    uint32_t oldest_time = 0xFFFFFFFF;
    int oldest_idx = 0;
    for (size_t i = 0; i < N; i++) {
      if (sessions[i].connected_at_ms < oldest_time) {
        oldest_time = sessions[i].connected_at_ms;
        oldest_idx = static_cast<int>(i);
      }
    }
    close(sessions[oldest_idx].sock);
    sessions[oldest_idx].sock = -1;
    sessions[oldest_idx].len = 0;
    slot = oldest_idx;
  }
  sessions[slot].sock = new_sock;
  sessions[slot].len = 0;
  sessions[slot].connected_at_ms = millis();
  stat.is_connected.store(true, std::memory_order_relaxed);
  stat.connection_count.fetch_add(1, std::memory_order_relaxed);
  return new_sock;
}

static void Ch6_SendAck_Direct(const StaticPacket &ack) {
  if (!s_ch6_bucket.consume()) {
    g_telnet_tracer.trace(6, true, TraceType::DRP, ack);
    g_pkt_stats.ch6.dropped_pkts.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  MutexLocker lock(g_ch6_mutex);
  bool sent = false;
  for (int i = 0; i < Config::TCP::MAX_HUB_CLIENTS; i++) {
    if (hub_sessions[i].sock >= 0) {
      int s =
          send(hub_sessions[i].sock, ack.data.data(), ack.length, MSG_DONTWAIT);
      if (s == static_cast<int>(ack.length))
        sent = true;
    }
  }

  if (sent) {
    g_telnet_tracer.trace(6, true, TraceType::ACK, ack);
    g_pkt_stats.ch6.tx_pkts.fetch_add(1, std::memory_order_relaxed);
  } else {
    s_ch6_bucket.restore();
    g_telnet_tracer.trace(6, true, TraceType::DRP, ack);
    g_pkt_stats.ch6.dropped_pkts.fetch_add(1, std::memory_order_relaxed);
  }
}

void Ch6_SendAck(const StaticPacket &ack) {
  if (g_ch6_to_tcp_queue) {
    xQueueSend(g_ch6_to_tcp_queue, &ack, 0);
  }
}

void Ch6_Data(TcpFragSession *s, const uint8_t *data, size_t len) {
  if (!s || s->sock < 0 || !data || len == 0)
    return;
  auto *parser = WallpadParserFactory::getActiveParser();
  uint8_t stx = parser ? parser->getStx() : PKT_STX;

  if (s->len + len > sizeof(s->buffer)) {
    size_t stx_pos = 0;
    while (stx_pos < s->len && s->buffer[stx_pos] != stx) {
      stx_pos++;
    }
    if (stx_pos == 0 && s->len > 0)
      stx_pos = 1;
    if (stx_pos < s->len) {
      memmove(s->buffer, s->buffer + stx_pos, s->len - stx_pos);
      s->len -= stx_pos;
    } else {
      s->len = 0;
    }
    if (s->len + len > sizeof(s->buffer)) {
      s->len = 0;
      close(s->sock);
      s->sock = -1;
      return;
    }
  }

  std::copy(data, data + len, s->buffer + s->len);
  s->len += len;
  size_t p = 0;

  while (p < s->len) {
    if (s->buffer[p] != stx) {
      p++;
      continue;
    }

    int len_res =
        parser ? parser->extractPacketLength(s->buffer, s->len, p) : -1;
    if (len_res == 0) {
      // 불완전 패킷 (추가 데이터 수신 대기)
      break;
    }
    if (len_res < 0) {
      p++;
      continue;
    }

    uint8_t p_len = static_cast<uint8_t>(len_res);
    span<const uint8_t> frame(&s->buffer[p], p_len);
    if (!parser->validatePacket(frame)) {
      StaticPacket drp_pkt{6, p_len};
      std::copy(&s->buffer[p], &s->buffer[p + p_len], drp_pkt.data.begin());
      g_telnet_tracer.trace(6, false, TraceType::DRP, drp_pkt);
      g_pkt_stats.ch6.dropped_pkts.fetch_add(1, std::memory_order_relaxed);
      p += p_len;
      continue;
    }

    g_pkt_stats.ch6.rx_pkts.fetch_add(1, std::memory_order_relaxed);
    StaticPacket req{6, p_len};
    std::copy(&s->buffer[p], &s->buffer[p + p_len], req.data.begin());

    bool is_query = parser->isQueryPacket(frame);
    g_telnet_tracer.trace(6, false, is_query ? TraceType::QRY : TraceType::CTL, req);

    if (is_query) {
      uint8_t dev_id = 0, sub1 = 0, sub2 = 0;
      if (parser->extractDeviceKey(frame, dev_id, sub1, sub2)) {
        g_polling_targets.registerOrTouch(6, dev_id, sub1, sub2, req.data.data(),
                                          req.length);
      }
      StaticPacket v_ack{};
      if (g_control_dispatcher.dispatch(req, v_ack)) {
        Ch6_SendAck(v_ack);
      } else {
        g_pkt_stats.ch6.uncached_pkts.fetch_add(1, std::memory_order_relaxed);
      }
    } else if (parser->isControlPacket(frame)) {
      StaticPacket dummy{};
      g_control_dispatcher.dispatch(req, dummy);
    }

    p += p_len;
  }
  if (p > 0) {
    s->len -= p;
    if (s->len > 0) {
      memmove(s->buffer, s->buffer + p, s->len);
    }
  }
}

void Ch5_Data(DoorphoneSession *s, const uint8_t *data, size_t len) {
  if (!s || s->sock < 0 || !data || len == 0)
    return;

  // 동적으로 학습 및 복원된 도어폰 프레이밍 값 사용 (기본 fallback: 0x7F, 0xEE, 5)
  uint8_t stx = g_doorphone_tracker.candidate_stx.load(std::memory_order_relaxed);
  uint8_t etx = g_doorphone_tracker.candidate_etx.load(std::memory_order_relaxed);
  uint8_t pkt_len = g_doorphone_tracker.candidate_len.load(std::memory_order_relaxed);
  if (stx == 0) stx = Config::Doorphone::STX;
  if (etx == 0) etx = Config::Doorphone::ETX;
  if (pkt_len < 3 || pkt_len > 64) pkt_len = Config::Doorphone::PKT_LEN;

  if (s->len + len > sizeof(s->buffer)) {
    size_t stx_pos = 0;
    while (stx_pos < s->len && s->buffer[stx_pos] != stx) {
      stx_pos++;
    }
    if (stx_pos == 0 && s->len > 0)
      stx_pos = 1;
    if (stx_pos < s->len) {
      memmove(s->buffer, s->buffer + stx_pos, s->len - stx_pos);
      s->len -= stx_pos;
    } else {
      s->len = 0;
    }
    if (s->len + len > sizeof(s->buffer)) {
      s->len = 0;
      close(s->sock);
      s->sock = -1;
      return;
    }
  }

  std::copy(data, data + len, s->buffer + s->len);
  s->len += len;
  size_t p = 0;

  while (s->len - p >= pkt_len) {
    if (s->buffer[p] != stx) {
      p++;
      continue;
    }
    if (s->buffer[p + pkt_len - 1] != etx) {
      p++;
      continue;
    }

    uint8_t cmd = s->buffer[p + 1];
    if (!Config::Doorphone::isValidOpcode(cmd)) {
      p++;
      continue;
    }

    uint32_t now = millis();
    if (!(cmd == s->last_tcp_cmd && (now - s->last_tcp_cmd_ms < Config::Timing::DOORPHONE_DEBOUNCE_MS))) {
      s->last_tcp_cmd = cmd;
      s->last_tcp_cmd_ms = now;
      StaticPacket pkt{5, pkt_len};
      std::copy(&s->buffer[p], &s->buffer[p + pkt_len],
                pkt.data.begin());

      g_pkt_stats.ch5.rx_pkts.fetch_add(1, std::memory_order_relaxed);
      g_telnet_tracer.trace(5, false, TraceType::RMT, pkt);
      if (g_ch4_passthrough_queue) {
        xQueueSend(g_ch4_passthrough_queue, &pkt, 0);
      }
    }
    p += pkt_len;
  }
  if (p > 0) {
    s->len -= p;
    if (s->len > 0) {
      memmove(s->buffer, s->buffer + p, s->len);
    }
  }
}

// ============================================================================
// SECTION 2: NETWORK & TCP SESSION MANAGEMENT TASKS
// ============================================================================

void Task_Network(void *pvParameters) {
  esp_task_wdt_add(nullptr);
  uint32_t t_chk = millis(), t_met = millis(), t_tcp = millis();
  uint32_t t_dp_heartbeat = millis();
  uint32_t last_ch5_activity_ms = 0;

  int hub_server_fd = -1;
  int door_server_fd = -1;
  int mgmt_server_fd = -1;

  if (!g_rescue_mode.load(std::memory_order_relaxed)) {
    hub_server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (hub_server_fd >= 0) {
      int opt = 1;
      setsockopt(hub_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
      int flags = fcntl(hub_server_fd, F_GETFL, 0);
      fcntl(hub_server_fd, F_SETFL, flags | O_NONBLOCK);

      struct sockaddr_in saddr;
      memset(&saddr, 0, sizeof(saddr));
      saddr.sin_family = AF_INET;
      saddr.sin_addr.s_addr = htonl(INADDR_ANY);
      saddr.sin_port = htons(Config::TCP::HUB_PORT);
      if (bind(hub_server_fd, reinterpret_cast<struct sockaddr *>(&saddr), sizeof(saddr)) < 0 ||
          listen(hub_server_fd, Config::TCP::MAX_HUB_CLIENTS) < 0) {
        ESP_LOGE("NET", "Failed to bind/listen hub server: errno %d", errno);
        close(hub_server_fd);
        hub_server_fd = -1;
      }
    }

    door_server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (door_server_fd >= 0) {
      int opt = 1;
      setsockopt(door_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
      int flags = fcntl(door_server_fd, F_GETFL, 0);
      fcntl(door_server_fd, F_SETFL, flags | O_NONBLOCK);

      struct sockaddr_in saddr;
      memset(&saddr, 0, sizeof(saddr));
      saddr.sin_family = AF_INET;
      saddr.sin_addr.s_addr = htonl(INADDR_ANY);
      saddr.sin_port = htons(Config::TCP::DOORPHONE_PORT);
      if (bind(door_server_fd, reinterpret_cast<struct sockaddr *>(&saddr), sizeof(saddr)) < 0 ||
          listen(door_server_fd, Config::TCP::MAX_DOORPHONE_CLIENTS) < 0) {
        ESP_LOGE("NET", "Failed to bind/listen doorphone server: errno %d", errno);
        close(door_server_fd);
        door_server_fd = -1;
      }
    }

    mgmt_server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (mgmt_server_fd >= 0) {
      int opt = 1;
      setsockopt(mgmt_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
      int flags = fcntl(mgmt_server_fd, F_GETFL, 0);
      fcntl(mgmt_server_fd, F_SETFL, flags | O_NONBLOCK);

      struct sockaddr_in saddr;
      memset(&saddr, 0, sizeof(saddr));
      saddr.sin_family = AF_INET;
      saddr.sin_addr.s_addr = htonl(INADDR_ANY);
      saddr.sin_port = htons(Config::TCP::MGMT_PORT);
      if (bind(mgmt_server_fd, reinterpret_cast<struct sockaddr *>(&saddr), sizeof(saddr)) < 0 ||
          listen(mgmt_server_fd, Config::TCP::MAX_MGMT_CLIENTS) < 0) {
        ESP_LOGE("NET", "Failed to bind/listen mgmt server (8900): errno %d", errno);
        close(mgmt_server_fd);
        mgmt_server_fd = -1;
      }
    }
  } else {
    Serial.println(F("[RESCUE] CH5, CH6 & CH7 TCP server ports disabled in Rescue Mode. Dedicated to OTA & Telnet."));
  }

  for (;;) {
    g_wdt_monitor.feed(4);
    ArduinoOTA.handle();
    if (g_ota_in_progress.load(std::memory_order_relaxed)) {
      for (int i = 0; i < 8; i++) {
        ArduinoOTA.handle();
      }
      taskYIELD();
      continue;
    }

    System_CheckOtaHealth();
    WarmCache_CheckNvsDebounce();

    if (!g_rescue_mode.load(std::memory_order_relaxed) && g_wifi_event_group) {
      EventBits_t bits = xEventGroupGetBits(g_wifi_event_group);

      if (bits & WIFI_BIT_GOT_IP) {
        xEventGroupClearBits(g_wifi_event_group, WIFI_BIT_GOT_IP);
        if (g_wifi_guard.testing.load(std::memory_order_acquire)) {
          g_wifi_guard.testing.store(false, std::memory_order_release);
          Config_Save();
          Serial.printf("[WIFI] ★ New Wi-Fi '%s' connected successfully! Saved to NVS.\r\n", g_config.wifi_ssid);
        }
        if (WiFi.getMode() == WIFI_MODE_APSTA || WiFi.getMode() == WIFI_MODE_AP) {
          WiFi.softAPdisconnect(true);
          WiFi.mode(WIFI_STA);
          WiFi.setSleep(false);
          Serial.println(F("[WIFI] Event: GOT_IP! Fallback SoftAP disabled, restored STA mode."));
        }
        configTime(0, 0, "pool.ntp.org", "asia.pool.ntp.org");
        setenv("TZ", "KST-9", 1);
        tzset();
      }

      if (g_wifi_guard.testing.load(std::memory_order_acquire)) {
        if (TimeUtils::isElapsed(g_wifi_guard.start_ms, 15000)) {
          g_wifi_guard.testing.store(false, std::memory_order_release);
          Serial.printf("[WIFI] ⚠️ New Wi-Fi '%s' failed to connect within 15s! Reverting to '%s'...\r\n",
                        g_config.wifi_ssid, g_wifi_guard.prev_ssid);
          strncpy(g_config.wifi_ssid, g_wifi_guard.prev_ssid, sizeof(g_config.wifi_ssid) - 1);
          strncpy(g_config.wifi_password, g_wifi_guard.prev_pass, sizeof(g_config.wifi_password) - 1);
          WiFi.disconnect(false);
          vTaskDelay(pdMS_TO_TICKS(100));
          WiFi.begin(g_config.wifi_ssid, g_config.wifi_password);
        }
      }

      if (bits & WIFI_BIT_DISCONNECTED) {
        if (TimeUtils::isElapsed(s_last_sta_retry_ms, Config::Timing::WIFI_BACKGROUND_RETRY_INTERVAL_MS)) {
          s_last_sta_retry_ms = millis();
          Serial.println(F("[WIFI] Event: DISCONNECTED. Background STA reconnection attempt..."));
          esp_wifi_connect();
        }
      }
    }

    if (s_pending_reboot_reason && millis() > Config::Timing::POST_BOOT_LOG_DELAY_MS) {
      LogManager::writeRebootLog(s_pending_reboot_reason);
      s_pending_reboot_reason = nullptr;
    }

    fd_set readfds, errorfds;
    FD_ZERO(&readfds);
    FD_ZERO(&errorfds);
    int max_fd = -1;

    auto add_fd = [&](int fd) {
      if (fd >= 0) {
        FD_SET(fd, &readfds);
        FD_SET(fd, &errorfds);
        if (fd > max_fd)
          max_fd = fd;
      }
    };

    add_fd(hub_server_fd);
    add_fd(door_server_fd);
    add_fd(mgmt_server_fd);

    {
      MutexLocker lock(g_ch6_mutex);
      for (int i = 0; i < Config::TCP::MAX_HUB_CLIENTS; i++) {
        if (hub_sessions[i].sock >= 0) {
          add_fd(hub_sessions[i].sock);
        }
      }
    }

    {
      MutexLocker lock(g_ch5_mutex);
      for (int k = 0; k < Config::TCP::MAX_DOORPHONE_CLIENTS; k++) {
        if (doorphone_sessions[k].sock >= 0) {
          add_fd(doorphone_sessions[k].sock);
        }
      }
    }

    {
      MutexLocker lock(g_mgmt_mutex);
      for (int m = 0; m < Config::TCP::MAX_MGMT_CLIENTS; m++) {
        if (g_mgmt_sessions[m].sock >= 0) {
          add_fd(g_mgmt_sessions[m].sock);
        }
      }
    }

    struct timeval tv = {0, 10000}; // 10ms 커널 레벨 Event-Driven 블로킹 (소켓 이벤트 발생 시 0ms 즉각 반환, vTaskDelay 불필요)
    int act = select(max_fd + 1, &readfds, nullptr, &errorfds, &tv);

    if (act > 0) {
      if (hub_server_fd >= 0 && FD_ISSET(hub_server_fd, &readfds)) {
        Tcp_AcceptAndAssignSlot(hub_server_fd, hub_sessions, g_ch6_mutex,
                                Config::TCP::DEFAULT_KEEPALIVE_IDLE_SEC,
                                Config::TCP::DEFAULT_KEEPALIVE_INTVL_SEC,
                                Config::TCP::DEFAULT_KEEPALIVE_CNT,
                                g_pkt_stats.ch6);
      }

      if (door_server_fd >= 0 && FD_ISSET(door_server_fd, &readfds)) {
        Tcp_AcceptAndAssignSlot(door_server_fd, doorphone_sessions, g_ch5_mutex,
                                Config::TCP::CH5_KEEPALIVE_IDLE_SEC,
                                Config::TCP::CH5_KEEPALIVE_INTVL_SEC,
                                Config::TCP::CH5_KEEPALIVE_CNT,
                                g_pkt_stats.ch5);
      }

      if (mgmt_server_fd >= 0 && FD_ISSET(mgmt_server_fd, &readfds)) {
        Tcp_AcceptAndAssignSlot(mgmt_server_fd, g_mgmt_sessions, g_mgmt_mutex,
                                Config::TCP::DEFAULT_KEEPALIVE_IDLE_SEC,
                                Config::TCP::DEFAULT_KEEPALIVE_INTVL_SEC,
                                Config::TCP::DEFAULT_KEEPALIVE_CNT,
                                g_pkt_stats.ch7);
      }

      Tcp_PollAndReceive(hub_sessions, g_ch6_mutex, readfds, errorfds,
                         [](TcpFragSession *s, const uint8_t *data, size_t len) {
                           Ch6_Data(s, data, len);
                         });

      Tcp_PollAndReceive(doorphone_sessions, g_ch5_mutex, readfds, errorfds,
                         [](DoorphoneSession *s, const uint8_t *data, size_t len) {
                           Ch5_Data(s, data, len);
                         });

      Tcp_PollAndReceive(g_mgmt_sessions, g_mgmt_mutex, readfds, errorfds,
                         [](MgmtSession *s, const uint8_t *data, size_t len) {
                           Mgmt_Data(s, data, len);
                         });
    }

    StaticPacket ch6_pkt;
    while (g_ch6_to_tcp_queue &&
           xQueueReceive(g_ch6_to_tcp_queue, &ch6_pkt, 0) == pdTRUE) {
      Ch6_SendAck_Direct(ch6_pkt);
    }

    StaticPacket pkt;
    uint32_t now_ms = millis();

    if (xQueueReceive(g_ch4_to_tcp_queue, &pkt, 0) == pdTRUE) {
      last_ch5_activity_ms = now_ms;
      static StaticPacket packets[Config::Queue::POOL_SIZE_CONTROL];
      size_t num_packets = 0;
      packets[num_packets++] = pkt;
      while (num_packets < Config::Queue::POOL_SIZE_CONTROL &&
             xQueueReceive(g_ch4_to_tcp_queue, &packets[num_packets], 0) == pdTRUE) {
        num_packets++;
      }

      MutexLocker lock(g_ch5_mutex);
      for (size_t i = 0; i < num_packets; i++) {
        auto &current_pkt = packets[i];

        if (!s_ch5_bucket.consume()) {
          g_telnet_tracer.trace(5, false, TraceType::DRP, current_pkt);
          g_pkt_stats.ch5.dropped_pkts.fetch_add(1, std::memory_order_relaxed);
          continue;
        }

        bool pkt_sent = false;
        for (int k = 0; k < Config::TCP::MAX_DOORPHONE_CLIENTS; k++) {
          if (doorphone_sessions[k].sock >= 0) {
            int sent = send(doorphone_sessions[k].sock, current_pkt.data.data(),
                            current_pkt.length, MSG_DONTWAIT);
            if (sent == static_cast<int>(current_pkt.length))
              pkt_sent = true;
          }
        }

        if (pkt_sent) {
          g_telnet_tracer.trace(5, true, TraceType::RMT, current_pkt);
          g_pkt_stats.ch5.tx_pkts.fetch_add(1, std::memory_order_relaxed);
        } else {
          s_ch5_bucket.restore();
          g_telnet_tracer.trace(5, true, TraceType::DRP, current_pkt);
          g_pkt_stats.ch5.dropped_pkts.fetch_add(1, std::memory_order_relaxed);
        }
      }
    }

    uint32_t now = millis();
    if (TimeUtils::isElapsed(t_chk,
                             Config::Timing::SYSTEM_MONITOR_INTERVAL_MS)) {
      t_chk = now;
      size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
      if (!g_http_ota_state.in_progress.load(std::memory_order_relaxed)) {
        if (free_heap < Config::Memory::MIN_HEAP_THRESHOLD_KB * 1024) {
          System_Restart("Low Heap Memory");
        }
      } else {
        if (free_heap < 8 * 1024) {
          System_Restart("Low Heap Memory (OTA)");
        }
      }
    }

    if (TimeUtils::isElapsed(t_met, Config::Metrics::SAMPLE_INTERVAL_MS)) {
      t_met = now;
      uint16_t used_ram = (heap_caps_get_total_size(MALLOC_CAP_8BIT) -
                           heap_caps_get_free_size(MALLOC_CAP_8BIT)) /
                          1024;
      uint8_t c0 = 0, c1 = 0;
      System_ReadCpuPct(c0, c1);
      g_metrics.addSample(c0, c1, used_ram, System_ReadTempC());
    }

    if (TimeUtils::isElapsed(t_tcp, Config::TCP::CLEANUP_INTERVAL_MS)) {
      t_tcp = now;
      g_pkt_stats.ch6.is_connected.store(
          Tcp_HasActiveSession(hub_sessions, g_ch6_mutex),
          std::memory_order_relaxed);
      g_pkt_stats.ch5.is_connected.store(
          Tcp_HasActiveSession(doorphone_sessions, g_ch5_mutex),
          std::memory_order_relaxed);
      g_pkt_stats.ch7.is_connected.store(
          Tcp_HasActiveSession(g_mgmt_sessions, g_mgmt_mutex),
          std::memory_order_relaxed);
    }

    // ★ CH5 도어폰 TCP 세션 유지용 무조건 1시간 주기 하트비트 더미 패킷 송신 (STX: 0xF7, ETX: 0xEE)
    if (TimeUtils::isElapsed(t_dp_heartbeat, Config::Timing::DOORPHONE_HEARTBEAT_INTERVAL_MS)) {
      t_dp_heartbeat = now;

      uint8_t len = g_doorphone_tracker.candidate_len.load(std::memory_order_relaxed);
      if (len < 3 || len > 64) len = Config::Doorphone::PKT_LEN;

      StaticPacket dummy_pkt{5, len};
      dummy_pkt.data.fill(0x00);
      dummy_pkt.data[0] = PKT_STX;
      dummy_pkt.data[len - 1] = PKT_ETX;

      MutexLocker lock(g_ch5_mutex);
      bool pkt_sent = false;
      for (int k = 0; k < Config::TCP::MAX_DOORPHONE_CLIENTS; k++) {
        if (doorphone_sessions[k].sock >= 0) {
          int sent = send(doorphone_sessions[k].sock, dummy_pkt.data.data(),
                          dummy_pkt.length, MSG_DONTWAIT);
          if (sent == static_cast<int>(dummy_pkt.length))
            pkt_sent = true;
        }
      }

      if (pkt_sent) {
        g_telnet_tracer.trace(5, true, TraceType::RMT, dummy_pkt);
        g_pkt_stats.ch5.tx_pkts.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }
}

void System_Sha256ToHex(const char *input, char *output) {
  if (!input || !output)
    return;

  uint8_t hash[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts_ret(&ctx, 0);
  mbedtls_sha256_update_ret(&ctx, (const unsigned char *)input, strlen(input));
  mbedtls_sha256_finish_ret(&ctx, hash);
  mbedtls_sha256_free(&ctx);

  static const char hex[] = "0123456789abcdef";
  for (int i = 0; i < 32; i++) {
    output[i * 2] = hex[hash[i] >> 4];
    output[i * 2 + 1] = hex[hash[i] & 0xF];
  }
  output[64] = '\0';
}

void Config_Load() {
  Preferences p;
  p.begin("runtime-config", true);
  auto &c = g_config;

  c.uart_baud_rate = p.getULong("uart_baud", 9600);
  c.ch2_baud_rate = p.getULong("ch2_baud", 9600);
  c.ch3_baud_rate = p.getULong("ch3_baud", 9600);
  c.doorphone_baud_rate =
      p.getULong("door_baud", Config::Serial::DEFAULT_DOORPHONE_BAUD);
  p.getString("wifi_ssid", "").toCharArray(c.wifi_ssid, sizeof(c.wifi_ssid));
  p.getString("wifi_pass", "")
      .toCharArray(c.wifi_password, sizeof(c.wifi_password));
  p.getString("ap_ssid", "").toCharArray(c.ap_ssid, sizeof(c.ap_ssid));
  p.getString("ap_pass", "").toCharArray(c.ap_password, sizeof(c.ap_password));
  p.getString("telnet_hash", "")
      .toCharArray(c.telnet_pass_hash, sizeof(c.telnet_pass_hash));

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

  // NVS가 비어있을 경우 빌드 플래그 / 기본값 사용 (NVS자동저장 없음)
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

  Preferences p;
  p.begin("runtime-config", false);
  RuntimeConfig snapshot;
  {
    CriticalSectionLocker lock(&g_config_mux);
    snapshot = g_config;
    g_config_dirty.store(false, std::memory_order_release);
  }

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
  CriticalSectionLocker lock(&g_config_mux);
  g_config = RuntimeConfig{};
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

  st.ch1_stack = g_ch1_task_handle ? static_cast<uint16_t>(uxTaskGetStackHighWaterMark(g_ch1_task_handle)) : 0;
  st.ch2_stack = g_ch2_task_handle ? static_cast<uint16_t>(uxTaskGetStackHighWaterMark(g_ch2_task_handle)) : 0;
  st.ch3_stack = g_ch3_task_handle ? static_cast<uint16_t>(uxTaskGetStackHighWaterMark(g_ch3_task_handle)) : 0;
  st.ch4_stack = g_ch4_task_handle ? static_cast<uint16_t>(uxTaskGetStackHighWaterMark(g_ch4_task_handle)) : 0;
  st.net_stack = g_network_task_handle ? static_cast<uint16_t>(uxTaskGetStackHighWaterMark(g_network_task_handle)) : 0;
  st.telnet_stack = g_telnet_task_handle ? static_cast<uint16_t>(uxTaskGetStackHighWaterMark(g_telnet_task_handle)) : 0;

  pkt.ch1 = g_pkt_stats.ch1;
  pkt.ch2 = g_pkt_stats.ch2;
  pkt.ch3 = g_pkt_stats.ch3;
  pkt.ch4 = g_pkt_stats.ch4;
  pkt.ch5 = g_pkt_stats.ch5;
  pkt.ch6 = g_pkt_stats.ch6;
}

// ============================================================================
// SECTION 3: LOGMANAGER & PERSISTENT CRASH LOG SYSTEM
// ============================================================================

void LogManager::writeRebootLog(const char *reason) {
  if (!reason || strlen(reason) == 0)
    return;
  Preferences p;
  if (!p.begin("logs", false))
    return;

  size_t count = p.getUInt("count", 0);
  if (count > MAX_LOG_ENTRIES)
    count = MAX_LOG_ENTRIES;

  LogEntry entry = {};
  entry.timestamp = time(nullptr);
  strncpy(entry.reason, reason, sizeof(entry.reason) - 1);
  System_TakeSnapshot(entry.stats_snapshot, entry.hw_snapshot,
                      entry.stack_snapshot, entry.packet_stats_snapshot);

  char key[16];
  for (size_t i = count; i > 0; --i) {
    if (i < MAX_LOG_ENTRIES) {
      char old_k[16], new_k[16];
      snprintf(old_k, sizeof(old_k), "log_%zu", i - 1);
      snprintf(new_k, sizeof(new_k), "log_%zu", i);
      static NvsEnvelope<LogEntry> env;
      if (p.getBytes(old_k, &env, sizeof(env)) == sizeof(env)) {
        p.putBytes(new_k, &env, sizeof(env));
      }
    }
  }

  static NvsEnvelope<LogEntry> new_env;
  new_env.payload = entry;
  new_env.seal();
  p.putBytes("log_0", &new_env, sizeof(new_env));

  if (count < MAX_LOG_ENTRIES) {
    count++;
  }
  p.putUInt("count", static_cast<uint32_t>(count));
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

  char key[16];
  snprintf(key, sizeof(key), "log_%zu", idx);
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
      snprintf(buf, max_len, "\r\n[LOGVIEW] No persistent reboot logs found in NVS.\r\n");
    } else {
      snprintf(buf, max_len, "\r\n[LOGVIEW] Invalid log index #%u (Available: 1 ~ %u)\r\n",
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

  add("\r\n%s", Fmt::DIV80EQ);
  add("                   GATEWAY BRIDGE REBOOT SNAPSHOT MONITOR               "
      "      \r\n");
  add("%s", Fmt::DIV80EQ);
  add("Log Index       : #%zu / %zu\r\n", idx + 1, getLogCount());
  add("Reboot Reason   : %s\r\n", e.reason);
  add("Firmware        : %s\r\n", Config::FIRMWARE_VERSION);
  add("Log Time        : %s (%s)\r\n", t_buf, t_src);
  add("Uptime          : %ud %02uh %02um %02us\r\n", s / 86400,
      (s % 86400) / 3600, (s % 3600) / 60, s % 60);
  add("WiFi Connection : %s\r\n", w_str);
  add("Heap Memory     : Free %u KB / Min Free %u KB / Total %u KB\r\n",
      static_cast<unsigned>(e.stats_snapshot.free_heap / 1024),
      static_cast<unsigned>(e.stats_snapshot.min_free_heap / 1024),
      static_cast<unsigned>(e.stats_snapshot.total_heap / 1024));
  add("Flash Storage   : Sketch %u KB / Total Flash %u KB\r\n\r\n",
      static_cast<unsigned>(e.stats_snapshot.sketch_size_kb),
      static_cast<unsigned>(e.stats_snapshot.flash_total_kb));

  // 4대 테이블 렌더러 (공통 Fmt 함수 호출)
  Fmt::FormatHwMetrics(add, e.hw_snapshot);
  Fmt::FormatNetworkStats(add, e.packet_stats_snapshot);
  Fmt::FormatRs485Stats(add, e.packet_stats_snapshot);
  Fmt::FormatTaskStacks(add, e.stack_snapshot, g_wdt_monitor);
  add("%s\r\n", Fmt::DIV80EQ);
}

void LogManager::clearRebootLog() {
  Preferences p;
  p.begin("logs", false);
  p.clear();
  p.end();
}

void System_Restart(const char *reason) {
  if (reason && strlen(reason) > 0) {
    LogManager::writeRebootLog(reason);
  }
  g_telnet_tracer.setTrace(false);
  g_telnet_tracer.setClient(-1);
  g_telnet_manager.shutdownForReboot();

  Tcp_CloseAllSessions(hub_sessions, g_ch6_mutex);
  Tcp_CloseAllSessions(doorphone_sessions, g_ch5_mutex);

  uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(50));
  uart_wait_tx_done(UART_NUM_1, pdMS_TO_TICKS(50));
  uart_wait_tx_done(UART_NUM_2, pdMS_TO_TICKS(50));

  WarmCache_SaveToRtc();
  WarmCache_SaveToNvs();

  rtc_clean_restart_magic = RTC_MAGIC_CLEAN_RESTART;
  vTaskDelay(pdMS_TO_TICKS(150));
  esp_restart();
}

// ============================================================================
// SECTION 4: ARDUINO SETUP & SYSTEM ENTRY POINT
// ============================================================================

static WallpadChannelConfig ch2_config = {
    .uart_num = UART_NUM_1,
    .event_queue_ptr = &g_uart1_event_queue,
    .channel_id = 2,
};
static WallpadChannelConfig ch3_config = {
    .uart_num = UART_NUM_2,
    .event_queue_ptr = &g_uart2_event_queue,
    .channel_id = 3,
};

static void Boot_CheckCrashLoop() {
  Serial.begin(115200);
  g_boot_start_ms = millis();
  System_DiagnoseStuck();
  System_CheckCoreDump();
  System_LogResetReason();
  g_metrics.init();

  Serial.println(F("\r\n========================================"));
  Serial.printf("  GATEWAY BRIDGE %s BOOT INITIALIZATION\r\n", Config::FIRMWARE_VERSION);
  Serial.println(F("========================================"));
  if (s_pending_reboot_reason) {
    Serial.printf("[BOOT] Last Reset Reason: %s\r\n", s_pending_reboot_reason);
  }

  // [하드웨어 자동 롤백 감지] Standby 파티션이 무효화(INVALID)되었는지 검사
  const esp_partition_t *next_p = esp_ota_get_next_update_partition(nullptr);
  esp_ota_img_states_t next_state = ESP_OTA_IMG_UNDEFINED;
  if (next_p && esp_ota_get_state_partition(next_p, &next_state) == ESP_OK) {
    if (next_state == ESP_OTA_IMG_INVALID || next_state == ESP_OTA_IMG_ABORTED) {
      g_rollback_detected = true;
      const esp_partition_t *run_p = esp_ota_get_running_partition();
      Serial.printf("[BOOT] ★ AUTO-ROLLBACK ACTIVE: Rolled back from failed '%s' to stable '%s'!\r\n",
                    next_p->label, run_p ? run_p->label : "app0");
    }
  }

  // [RTC 크래시 감지] 실제 비정상 크래시/워치독만 정밀 추적 (정상 SW 리부팅/전원 인가는 제외)
  esp_reset_reason_t reset_reason = esp_reset_reason();
  bool is_abnormal_crash = (reset_reason == ESP_RST_PANIC || 
                            reset_reason == ESP_RST_TASK_WDT || 
                            reset_reason == ESP_RST_INT_WDT || 
                            reset_reason == ESP_RST_WDT || 
                            reset_reason == ESP_RST_BROWNOUT);

  if (rtc_rescue_magic != RTC_MAGIC_RESCUE || !is_abnormal_crash) {
    rtc_rescue_magic = RTC_MAGIC_RESCUE;
    rtc_crash_counter = 0;
  } else {
    rtc_crash_counter++;
    Serial.printf("[BOOT] Consecutive crash count: %u (Reason: %d)\r\n", rtc_crash_counter, reset_reason);
  }

  // [하드웨어 버튼 비상 복구] AtomS3 화면 물리 버튼 (GPIO 41)을 2.5초간 누르면 강제 복구 모드
  pinMode(Config::GPIO::BTN_PIN, INPUT_PULLUP);
  if (digitalRead(Config::GPIO::BTN_PIN) == LOW) {
    Serial.println(F("[BOOT] Front button pressed, checking 2.5s hold..."));
    uint32_t press_start = millis();
    bool held = true;
    while (!TimeUtils::isElapsed(press_start, Config::Timing::RESCUE_BUTTON_HOLD_MS)) {
      if (digitalRead(Config::GPIO::BTN_PIN) != LOW) {
        held = false;
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (held) {
      Serial.println(F("[SAFE BOOT] ★ Front Button Held (2.5s) -> Forcing Rescue Safe Mode!"));
      System_EnterRescueMode("Hardware Button Override");
    }
  }

  // [크래시 루프 방어 & 자동 롤백] 연속 3회 이상 부팅 실패 감지 시
  if (!g_rescue_mode.load(std::memory_order_relaxed) && rtc_crash_counter >= 3) {
    const esp_partition_t *run_p = esp_ota_get_running_partition();
    const esp_partition_t *next_p = esp_ota_get_next_update_partition(nullptr);

    // 1단계: 상대 파티션(이전 정상 펌웨어)이 존재하면 즉시 롤백 시도
    if (run_p && next_p && strcmp(run_p->label, next_p->label) != 0) {
      Serial.printf("[RESCUE] ★ Crash Loop detected (%u crashes)! Rolling back from '%s' to '%s'...\r\n",
                    rtc_crash_counter, run_p->label, next_p->label);
      rtc_crash_counter = 0; // 롤백 시도 시 카운터 리셋
      esp_err_t err = esp_ota_set_boot_partition(next_p);
      if (err == ESP_OK) {
        Serial.println(F("[RESCUE] Boot partition switched successfully. Rebooting into previous firmware..."));
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
      } else {
        Serial.printf("[RESCUE] esp_ota_set_boot_partition failed (err=0x%x). Fallback to Rescue Safe Mode...\r\n", err);
      }
    }

    // 2단계: 상대 파티션이 없거나 롤백 실패 시 Rescue Safe Mode 진입
    Serial.printf("[RESCUE] ★ Crash Loop detected (%u crashes)! Forcing Rescue Safe Mode...\r\n", rtc_crash_counter);
    System_EnterRescueMode("Consecutive Crash Loop (>=3)");
  }
}

static void Boot_InitSyncPrimitives() {
  g_uart0_mutex = xSemaphoreCreateMutex();
  g_uart1_mutex = xSemaphoreCreateMutex();
  g_uart2_mutex = xSemaphoreCreateMutex();

  auto init_q = [](StaticQueue_t *qb, uint8_t *st) {
    return xQueueCreateStatic(Config::Queue::POOL_SIZE_CONTROL,
                              sizeof(StaticPacket), st, qb);
  };
  g_ch1_control_queue = init_q(&g_ch1_ctrl_queue_buf, g_ch1_ctrl_storage);
  g_ch1_vip_queue = init_q(&g_ch1_vip_queue_buf, g_ch1_vip_storage);
  g_ch4_passthrough_queue = init_q(&g_ch4_pass_queue_buf, g_ch4_pass_storage);
  g_ch4_to_tcp_queue = init_q(&g_ch4_to_tcp_queue_buf, g_ch4_to_tcp_storage);
  g_ch6_to_tcp_queue = init_q(&g_ch6_to_tcp_queue_buf, g_ch6_to_tcp_storage);

  // CH1 Event-Driven 큐셋 생성 및 등록 (VIP: 8 + Control: 8 = 16)
  g_ch1_queue_set = xQueueCreateSet(Config::Queue::POOL_SIZE_CONTROL * 2);
  if (g_ch1_queue_set) {
    BaseType_t res1 = xQueueAddToSet(g_ch1_vip_queue, g_ch1_queue_set);
    BaseType_t res2 = xQueueAddToSet(g_ch1_control_queue, g_ch1_queue_set);
    if (res1 != pdPASS || res2 != pdPASS) {
      Serial.println(F("[FATAL] Failed to add queues to g_ch1_queue_set!"));
    }
  } else {
    Serial.println(F("[FATAL] Failed to create g_ch1_queue_set!"));
  }

  if (!g_ctrl_queue_mutex)
    g_ctrl_queue_mutex = xSemaphoreCreateMutex();
  if (!g_ch6_mutex)
    g_ch6_mutex = xSemaphoreCreateMutex();
  if (!g_ch5_mutex)
    g_ch5_mutex = xSemaphoreCreateMutex();

  if (!g_wifi_event_group)
    g_wifi_event_group = xEventGroupCreate();
  if (!g_system_event_group) {
    g_system_event_group = xEventGroupCreate();
    xEventGroupSetBits(g_system_event_group, SYS_EVT_OTA_IDLE);
  }
  WiFi.onEvent(onWifiEvent);
}

static void Boot_InitHardwareAndDevices() {
  auto to_uart_databits = [](uint8_t d) -> uart_word_length_t {
    return (d == 7) ? UART_DATA_7_BITS : UART_DATA_8_BITS;
  };

  auto to_uart_parity = [](uint8_t p) -> uart_parity_t {
    return (p == 1)   ? UART_PARITY_EVEN
           : (p == 2) ? UART_PARITY_ODD
                      : UART_PARITY_DISABLE;
  };

  auto to_uart_stopbits = [](uint8_t s) -> uart_stop_bits_t {
    return s == 2 ? UART_STOP_BITS_2 : UART_STOP_BITS_1;
  };

  auto init_uart = [&](uart_port_t port, int tx, int rx, uint32_t baud,
                       uint8_t dbits, uint8_t parity, uint8_t stopbits,
                       QueueHandle_t *q) {
    uart_config_t cfg = {
        .baud_rate = static_cast<int>(baud),
        .data_bits = to_uart_databits(dbits),
        .parity = to_uart_parity(parity),
        .stop_bits = to_uart_stopbits(stopbits),
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_APB};
    uart_param_config(port, &cfg);
    uart_set_pin(port, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(port, Config::Packet::UART_HW_RX_BUF_SIZE, 0,
                        Config::Queue::UART_EVENT_QUEUE_SIZE, q, 0);
  };

  init_uart(UART_NUM_0, 2, 1, g_config.uart_baud_rate, g_config.uart_data_bits,
            g_config.uart_parity, g_config.uart_stop_bits, &g_uart0_event_queue);
  init_uart(UART_NUM_1, 6, 5, g_config.ch2_baud_rate, g_config.ch2_data_bits,
            g_config.ch2_parity, g_config.ch2_stop_bits, &g_uart1_event_queue);
  init_uart(UART_NUM_2, 8, 7, g_config.ch3_baud_rate, g_config.ch3_data_bits,
            g_config.ch3_parity, g_config.ch3_stop_bits, &g_uart2_event_queue);

  g_doorphone_serial.begin(g_config.doorphone_baud_rate,
                           Door_SerialConfig(g_config.doorphone_data_bits,
                                             g_config.doorphone_parity,
                                             g_config.doorphone_stop_bits),
                           Config::GPIO::RX_GPIO, Config::GPIO::TX_GPIO);
  pinMode(Config::GPIO::RX_GPIO, INPUT_PULLUP);
  g_device_repo.initDevices();
}

bool System_ApplyUartConfig(uint8_t ch, uint32_t baud, const char *format) {
  uint8_t db = 8, pr = 0, sb = 1;
  if (!parseFramingStr(format, db, pr, sb)) return false;
  if (baud < 1200 || baud > 921600) return false;

  auto to_uart_parity = [](uint8_t p) -> uart_parity_t {
    return (p == 1)   ? UART_PARITY_EVEN
           : (p == 2) ? UART_PARITY_ODD
                      : UART_PARITY_DISABLE;
  };
  auto to_uart_stopbits = [](uint8_t s) -> uart_stop_bits_t {
    return s == 2 ? UART_STOP_BITS_2 : UART_STOP_BITS_1;
  };

  {
    CriticalSectionLocker lock(&g_config_mux);
    if (ch == 1) {
      g_config.uart_baud_rate = baud;
      g_config.uart_data_bits = db;
      g_config.uart_parity = pr;
      g_config.uart_stop_bits = sb;
    } else if (ch == 2) {
      g_config.ch2_baud_rate = baud;
      g_config.ch2_data_bits = db;
      g_config.ch2_parity = pr;
      g_config.ch2_stop_bits = sb;
    } else if (ch == 3) {
      g_config.ch3_baud_rate = baud;
      g_config.ch3_data_bits = db;
      g_config.ch3_parity = pr;
      g_config.ch3_stop_bits = sb;
    } else if (ch == 4) {
      g_config.doorphone_baud_rate = baud;
      g_config.doorphone_data_bits = db;
      g_config.doorphone_parity = pr;
      g_config.doorphone_stop_bits = sb;
    } else {
      return false;
    }
    g_config_dirty.store(true, std::memory_order_release);
  }

  // 런타임 하드웨어 즉시 적용
  if (ch >= 1 && ch <= 3) {
    uart_port_t port = (ch == 1) ? UART_NUM_0 : (ch == 2) ? UART_NUM_1 : UART_NUM_2;
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

static void Boot_InitWifiAndOta() {
  if (!g_rescue_mode.load(std::memory_order_relaxed)) {
    Serial.printf("[WIFI] Connecting to '%s' (Timeout: %us)...\r\n",
                  g_config.wifi_ssid, g_config.wifi_connect_timeout_s);
    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    esp_wifi_set_ps(WIFI_PS_NONE);

    wifi_config_t w_conf;
    memset(&w_conf, 0, sizeof(w_conf));
    strncpy(reinterpret_cast<char *>(w_conf.sta.ssid), g_config.wifi_ssid,
            sizeof(w_conf.sta.ssid) - 1);
    strncpy(reinterpret_cast<char *>(w_conf.sta.password), g_config.wifi_password,
            sizeof(w_conf.sta.password) - 1);
    w_conf.sta.scan_method = WIFI_FAST_SCAN;
    w_conf.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    w_conf.sta.pmf_cfg.capable = true;
    w_conf.sta.pmf_cfg.required = false;
    w_conf.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_set_config(WIFI_IF_STA, &w_conf);
    esp_wifi_connect();

    uint32_t t_start = millis();
    uint32_t max_wait =
        (g_config.wifi_connect_timeout_s ? g_config.wifi_connect_timeout_s : 30) *
        1000;
    bool connected = false;

    while (millis() - t_start < max_wait) {
      if ((connected = (WiFi.status() == WL_CONNECTED)))
        break;
      vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (connected) {
      WiFi.setSleep(false);
      Serial.printf("[WIFI] Connected successfully! IP: %s, RSSI: %d dBm\r\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
      configTime(0, 0, "pool.ntp.org", "asia.pool.ntp.org");
      setenv("TZ", "KST-9", 1);
      tzset();
    } else {
      WiFi.mode(WIFI_AP_STA);
      vTaskDelay(pdMS_TO_TICKS(100));

      WiFi.softAPConfig(IPAddress(172, 30, 2, 1), IPAddress(172, 30, 2, 1),
                        IPAddress(255, 255, 255, 0));
      bool ap_ok = WiFi.softAP(g_config.ap_ssid, g_config.ap_password, 1, 0, 4);

      WiFi.setSleep(false);
      esp_wifi_set_max_tx_power(78);
      Serial.printf(
          "[WIFI] STA connect failed. Fallback SoftAP '%s' started: %s (IP: %s)\r\n",
          g_config.ap_ssid, ap_ok ? "SUCCESS" : "FAILED",
          WiFi.softAPIP().toString().c_str());
    }

    ArduinoOTA.setHostname("gateway-bridge");
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.onStart([]() {
      g_ota_in_progress.store(true, std::memory_order_release);
      if (g_system_event_group) {
        xEventGroupClearBits(g_system_event_group, SYS_EVT_OTA_IDLE);
      }
    });
    ArduinoOTA.onEnd([]() {
      g_ota_in_progress.store(false, std::memory_order_release);
      if (g_system_event_group) {
        xEventGroupSetBits(g_system_event_group, SYS_EVT_OTA_IDLE);
      }
      vTaskDelay(pdMS_TO_TICKS(200));
      System_Restart("OTA Firmware Update");
    });
    ArduinoOTA.onError([](ota_error_t error) {
      g_ota_in_progress.store(false, std::memory_order_release);
      if (g_system_event_group) {
        xEventGroupSetBits(g_system_event_group, SYS_EVT_OTA_IDLE);
      }
    });
    ArduinoOTA.begin();
  }
}

static void Boot_StartTasks() {
  uint32_t now = millis();
  for (size_t i = 0; i < 6; i++) {
    rtc_last_alive_ms[i] = now;
  }

  // FreeRTOS 태스크들이 실제로 시작되는 시점에 WDT 활성화
  esp_task_wdt_init(30, true);

  auto cr_task = [](TaskFunction_t fn, const char *name, uint32_t stack,
                    void *param, UBaseType_t prio, StackType_t *buf,
                    StaticTask_t *tcb) {
    BaseType_t core =
        (strcmp(name, "Network") == 0 || strcmp(name, "Telnet_CLI") == 0) ? 0
                                                                          : 1;
    return xTaskCreateStaticPinnedToCore(fn, name, stack, param, prio, buf, tcb,
                                         core);
  };

  if (!g_rescue_mode.load(std::memory_order_relaxed)) {
    g_ch1_task_handle = cr_task(Task_Ch1, "CH#1_IoT", Config::Task::STACK_SIZE_CORE1,
                                nullptr, 13, stackCore1Ch1, &g_task_core1_ch1_buf);
    g_ch2_task_handle =
        cr_task(Task_Ch2Ch3, "CH#2_WP#1", Config::Task::STACK_SIZE_CORE1,
                &ch2_config, 10, stackCore1Slave, &g_task_core1_slave_buf);
    g_ch3_task_handle =
        cr_task(Task_Ch2Ch3, "CH#3_WP#2", Config::Task::STACK_SIZE_CORE1,
                &ch3_config, 10, stackCore1Slave2, &g_task_core1_slave2_buf);
    g_ch4_task_handle = cr_task(Task_Ch4, "CH#4_WP#3", Config::Task::STACK_SIZE_CH4,
                                nullptr, 11, stackCore1Ch4, &g_task_core1_ch4_buf);
  } else {
    Serial.println(F("[RESCUE] RS-485 Tasks bypassed. Only Network & Telnet tasks active."));
  }
  g_network_task_handle =
      cr_task(Task_Network, "Network", Config::Task::STACK_SIZE_CORE0, nullptr, 12,
              stackCore0Net, &g_task_core0_net_buf);
  g_telnet_task_handle =
      cr_task(Task_Telnet, "Telnet_CLI", Config::Task::STACK_SIZE_TELNET, nullptr,
              10, telnetTaskStack, &g_telnet_task_buf);
}

void setup() {
  Boot_CheckCrashLoop();
  Boot_InitSyncPrimitives();
  Config_Load();
  Serial.printf("[CONFIG] WiFi SSID: '%s', Timeout: %us, AP SSID: '%s'\r\n",
                g_config.wifi_ssid, g_config.wifi_connect_timeout_s,
                g_config.ap_ssid);
  WarmCache_RestoreOnBoot();
  g_doorphone_tracker.restoreFromNvs();
  g_control_registry.init();
  Mgmt_Init();
  Boot_InitHardwareAndDevices();
  Boot_InitWifiAndOta();
  Boot_StartTasks();

  Serial.println(F("[BOOT] All FreeRTOS tasks started successfully."));
  esp_task_wdt_delete(nullptr);
  if (g_system_event_group) {
    xEventGroupSetBits(g_system_event_group, SYS_EVT_SYSTEM_RUNNING);
  }
  vTaskDelete(nullptr);
}

void loop() {}