#include "Common.h"
#include "TelnetCli.h"
#include "esp_ota_ops.h"
#include "esp_core_dump.h"
#include "esp_wifi.h"
#include <ArduinoOTA.h>
#include <WiFi.h>
#include <cstdio>
#include <cstring>

struct StuckTaskDiag { bool found{false}; char msg[36]{0}; };
static StuckTaskDiag s_stuck_diag;
const char *s_pending_reboot_reason = nullptr;
static std::atomic<bool> s_ota_validated{false};

CoreDumpInfo g_coredump_info;
Config::Doorphone::FramingTracker g_doorphone_tracker;

void System_DiagnoseStuck() {
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
    return (ota_state == ESP_OTA_IMG_PENDING_VERIFY ||
            ota_state == ESP_OTA_IMG_NEW);
  }
  return false;
}

void System_CheckOtaHealth() {
  if (s_ota_validated.load(std::memory_order_relaxed)) {
    return;
  }
  if (WiFi.status() == WL_CONNECTED &&
      TimeUtils::isElapsed(g_boot_start_ms,
                           Config::Timing::OTA_VALIDATION_PERIOD_MS)) {
    s_ota_validated.store(true, std::memory_order_release);
    rtc_crash_counter = 0;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running) {
      esp_ota_img_states_t ota_state;
      if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
          ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
          Serial.println(
              F("[OTA] ★ Firmware Health Verified! Auto-rollback cancelled."));
          g_telnet_tracer.trace(
              "[OTA] ★ Firmware Health Verified! Auto-rollback cancelled.\r\n");
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
  Serial.printf("  🚨 RESCUE SAFE MODE ACTIVATED: %s\r\n",
                reason ? reason : "Unknown");
  Serial.println(F("========================================"));

  WiFi.mode(WIFI_AP_STA);
  vTaskDelay(pdMS_TO_TICKS(100));

  WiFi.softAPConfig(IPAddress(172, 30, 2, 1), IPAddress(172, 30, 2, 1),
                    IPAddress(255, 255, 255, 0));
  bool ap_ok = WiFi.softAP("Sweet_Home_Rescue", EMERGENCY_AP_PASS, 1, 0, 4);
  WiFi.setSleep(false);
  esp_wifi_set_max_tx_power(78);

  Serial.printf("[RESCUE] SoftAP 'Sweet_Home_Rescue' started: %s (IP: %s)\r\n",
                ap_ok ? "SUCCESS" : "FAILED",
                WiFi.softAPIP().toString().c_str());

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
