#include "MgmtRpc.h"
#include "WallpadParser.h"
#include "esp_core_dump.h"
#include <HTTPClient.h>
#include <Update.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <algorithm>
#include <cstring>
#include <cstdlib>

// ============================================================================
// 전역 인스턴스 및 세션 풀
// ============================================================================
RuntimeTimingConfig g_timing_config{};
HttpOtaState g_http_ota_state{};
MgmtSession g_mgmt_sessions[Config::TCP::MAX_MGMT_CLIENTS];
SemaphoreHandle_t g_mgmt_mutex = nullptr;

static constexpr const char *DEFAULT_CLOUD_OTA_URL =
    "https://raw.githubusercontent.com/Eri-tep/Gateway/beta/bin/firmware.bin";

// ============================================================================
// HTTP(S) Cloud OTA 백그라운드 태스크 (HTTPClient + Update.h 스트리밍)
// ============================================================================
static void Task_HttpOta(void *pvParameters) {
  char *url = static_cast<char *>(pvParameters);
  if (!url) {
    g_http_ota_state.in_progress = false;
    vTaskDelete(nullptr);
    return;
  }

  ::Serial.printf("[OTA] Starting HTTP(S) Stream OTA from URL: %s\r\n", url);
  snprintf(g_http_ota_state.status, sizeof(g_http_ota_state.status), "Connecting...");
  g_http_ota_state.progress_pct = 0;
  g_http_ota_state.last_error[0] = '\0';

  bool is_https = (strncmp(url, "https://", 8) == 0);
  WiFiClient plain_client;
  WiFiClientSecure secure_client;
  WiFiClient *client_ptr = nullptr;

  if (is_https) {
    secure_client.setInsecure(); // GitHub CDN(objects.githubusercontent.com) 리다이렉트 HTTPS 허용
    secure_client.setHandshakeTimeout(15);
    client_ptr = &secure_client;
  } else {
    client_ptr = &plain_client;
  }

  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(15000);

  if (!http.begin(*client_ptr, url)) {
    snprintf(g_http_ota_state.status, sizeof(g_http_ota_state.status), "Failed");
    snprintf(g_http_ota_state.last_error, sizeof(g_http_ota_state.last_error), "HTTP begin failed");
    ::Serial.println(F("[OTA] HTTP begin connection failed."));
    g_http_ota_state.in_progress = false;
    free(url);
    vTaskDelete(nullptr);
    return;
  }

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    snprintf(g_http_ota_state.status, sizeof(g_http_ota_state.status), "Failed");
    snprintf(g_http_ota_state.last_error, sizeof(g_http_ota_state.last_error),
             "HTTP error (%d): %s", httpCode, http.errorToString(httpCode).c_str());
    ::Serial.printf("[OTA] HTTP GET error: %s\r\n", g_http_ota_state.last_error);
    http.end();
    g_http_ota_state.in_progress = false;
    free(url);
    vTaskDelete(nullptr);
    return;
  }

  int contentLength = http.getSize();
  if (contentLength <= 0) {
    snprintf(g_http_ota_state.status, sizeof(g_http_ota_state.status), "Failed");
    snprintf(g_http_ota_state.last_error, sizeof(g_http_ota_state.last_error), "Invalid Content-Length: %d", contentLength);
    ::Serial.printf("[OTA] Invalid firmware size: %d\r\n", contentLength);
    http.end();
    g_http_ota_state.in_progress = false;
    free(url);
    vTaskDelete(nullptr);
    return;
  }

  ::Serial.printf("[OTA] Firmware binary size: %d bytes. Preparing flash partition...\r\n", contentLength);

  if (!Update.begin(contentLength, U_FLASH)) {
    snprintf(g_http_ota_state.status, sizeof(g_http_ota_state.status), "Failed");
    snprintf(g_http_ota_state.last_error, sizeof(g_http_ota_state.last_error),
             "Update.begin failed (0x%x)", Update.getError());
    ::Serial.printf("[OTA] Update.begin failed: %s\r\n", g_http_ota_state.last_error);
    http.end();
    g_http_ota_state.in_progress = false;
    free(url);
    vTaskDelete(nullptr);
    return;
  }

  g_ota_in_progress.store(true, std::memory_order_release);
  if (g_system_event_group) {
    xEventGroupClearBits(g_system_event_group, SYS_EVT_OTA_IDLE);
  }

  WiFiClient *stream = http.getStreamPtr();
  static uint8_t s_ota_buff[4096]; // Flash 4KB Sector 일치 정적 버퍼 (Zero Heap / Zero Stack)
  size_t written = 0;
  uint32_t last_progress_ms = 0;

  while (http.connected() && (written < static_cast<size_t>(contentLength))) {
    size_t sizeAvailable = stream->available();
    if (sizeAvailable > 0) {
      size_t to_read = std::min(sizeAvailable, sizeof(s_ota_buff));
      int c = stream->readBytes(s_ota_buff, to_read);
      if (c > 0) {
        size_t w = Update.write(s_ota_buff, c);
        if (w != static_cast<size_t>(c)) {
          snprintf(g_http_ota_state.status, sizeof(g_http_ota_state.status), "Failed");
          snprintf(g_http_ota_state.last_error, sizeof(g_http_ota_state.last_error), "Flash write error at %u", (unsigned)written);
          ::Serial.printf("[OTA] %s\r\n", g_http_ota_state.last_error);
          break;
        }
        written += w;
        uint32_t now = millis();
        if (now - last_progress_ms >= 200 || written == static_cast<size_t>(contentLength)) {
          last_progress_ms = now;
          uint8_t pct = static_cast<uint8_t>((written * 100) / contentLength);
          g_http_ota_state.progress_pct = pct;
          snprintf(g_http_ota_state.status, sizeof(g_http_ota_state.status), "Downloading (%u%%)", pct);
        }
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }

  if (written == static_cast<size_t>(contentLength) && Update.end(true)) {
    if (Update.isFinished()) {
      snprintf(g_http_ota_state.status, sizeof(g_http_ota_state.status), "Success (Rebooting)");
      ::Serial.println(F("[OTA] Update OK! Rebooting into new firmware in 1s..."));
      http.end();
      vTaskDelay(pdMS_TO_TICKS(1000));
      esp_restart();
    }
  } else {
    snprintf(g_http_ota_state.status, sizeof(g_http_ota_state.status), "Failed");
    snprintf(g_http_ota_state.last_error, sizeof(g_http_ota_state.last_error),
             "Update write failed (written %u / %d, err: 0x%x)",
             (unsigned)written, contentLength, Update.getError());
    ::Serial.printf("[OTA] Update failed: %s\r\n", g_http_ota_state.last_error);
  }

  http.end();
  g_http_ota_state.in_progress = false;
  g_ota_in_progress.store(false, std::memory_order_release);
  if (g_system_event_group) {
    xEventGroupSetBits(g_system_event_group, SYS_EVT_OTA_IDLE);
  }
  free(url);
  vTaskDelete(nullptr);
}

void Mgmt_StartHttpOta(const char *url) {
  const char *target_url = (url && strlen(url) > 0) ? url : DEFAULT_CLOUD_OTA_URL;
  if (g_http_ota_state.in_progress.load()) {
    ::Serial.println(F("[OTA] Update already in progress, ignoring duplicate request."));
    return;
  }

  g_http_ota_state.in_progress = true;
  snprintf(g_http_ota_state.status, sizeof(g_http_ota_state.status), "Starting...");
  g_http_ota_state.progress_pct = 0;
  g_http_ota_state.last_error[0] = '\0';

  char *url_copy = strdup(target_url);
  if (!url_copy) {
    g_http_ota_state.in_progress = false;
    return;
  }

  // Priority 12 (Network 태스크와 동급 상향 조정으로 CPU 기아 방지)
  xTaskCreatePinnedToCore(Task_HttpOta, "HttpOtaTask", 8192, url_copy, 12, nullptr, 0);
}

// ============================================================================
// 3대 타이밍 NVS 로드 및 저장
// ============================================================================
void TimingConfig_Load() {
  Preferences p;
  if (p.begin("timing_cfg", true)) {
    g_timing_config.ch1_poll_interval_ms = p.getUShort("ch1_poll", 1000);
    g_timing_config.ch2_cache_delay_ms   = p.getUShort("ch2_del", 30);
    g_timing_config.ch3_cache_delay_ms   = p.getUShort("ch3_del", 240);
    p.end();
  } else {
    g_timing_config.ch1_poll_interval_ms = 1000;
    g_timing_config.ch2_cache_delay_ms   = 30;
    g_timing_config.ch3_cache_delay_ms   = 240;
  }

  // 안전 범위 클램핑
  if (g_timing_config.ch1_poll_interval_ms < 200 || g_timing_config.ch1_poll_interval_ms > 5000)
    g_timing_config.ch1_poll_interval_ms = 1000;
  if (g_timing_config.ch2_cache_delay_ms < 5 || g_timing_config.ch2_cache_delay_ms > 300)
    g_timing_config.ch2_cache_delay_ms = 30;
  if (g_timing_config.ch3_cache_delay_ms < 20 || g_timing_config.ch3_cache_delay_ms > 1000)
    g_timing_config.ch3_cache_delay_ms = 240;

  ::Serial.printf("[TIMING] Loaded: CH1 Poll %u ms, CH2 Delay %u ms, CH3 Delay %u ms\r\n",
                  g_timing_config.ch1_poll_interval_ms,
                  g_timing_config.ch2_cache_delay_ms,
                  g_timing_config.ch3_cache_delay_ms);
}

void TimingConfig_Save() {
  Preferences p;
  if (p.begin("timing_cfg", false)) {
    p.putUShort("ch1_poll", g_timing_config.ch1_poll_interval_ms);
    p.putUShort("ch2_del", g_timing_config.ch2_cache_delay_ms);
    p.putUShort("ch3_del", g_timing_config.ch3_cache_delay_ms);
    p.end();
    ::Serial.printf("[TIMING] Saved to NVS: CH1 Poll %u ms, CH2 Delay %u ms, CH3 Delay %u ms\r\n",
                    g_timing_config.ch1_poll_interval_ms,
                    g_timing_config.ch2_cache_delay_ms,
                    g_timing_config.ch3_cache_delay_ms);
  }
}

void Mgmt_Init() {
  if (!g_mgmt_mutex) {
    g_mgmt_mutex = xSemaphoreCreateMutex();
  }
  for (size_t i = 0; i < Config::TCP::MAX_MGMT_CLIENTS; i++) {
    g_mgmt_sessions[i].sock = -1;
    g_mgmt_sessions[i].len = 0;
    g_mgmt_sessions[i].connected_at_ms = 0;
  }
  TimingConfig_Load();
}

// ============================================================================
// 텔레메트리 JSON 고속 직렬화
// ============================================================================
void Mgmt_SerializeTelemetry(AppendBuf &out) {
  // 1. System Metrics
  uint8_t c0 = 0, c1 = 0;
  System_ReadCpuPct(c0, c1);
  int8_t temp_c = System_ReadTempC();
  int8_t rssi = WiFi.isConnected() ? WiFi.RSSI() : 0;
  uint32_t uptime_s = millis() / 1000;
  uint32_t free_heap_kb = heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024;
  uint32_t min_free_heap_kb = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT) / 1024;
  bool ntp_synced = (time(nullptr) > 1672531200);

  // 2. Profile Info
  VendorProfileDescriptor active_prof{};
  ProfileRepository::getActiveProfile(active_prof);
  auto auto_desc = g_auto_probing_engine.getDescriptor();

  // 3. Cache Info
  const char *wc_src = (g_warm_cache_source == 1) ? "RTC_SRAM" : (g_warm_cache_source == 2 ? "NVS_FLASH" : "COLD_BOOT");
  size_t total_devs = g_device_repo.count();
  size_t online_devs = g_device_repo.getOnlineCount();
  size_t stale_devs = (total_devs >= online_devs) ? (total_devs - online_devs) : 0;

  // 4. Channel Stats
  uint32_t ch1_rx = g_pkt_stats.ch1.rx_pkts.load(std::memory_order_relaxed);
  uint32_t ch1_tx = g_pkt_stats.ch1.tx_pkts.load(std::memory_order_relaxed);
  uint32_t ch1_crc = g_pkt_stats.ch1.crc_errors.load(std::memory_order_relaxed);
  uint32_t ch1_tout = g_pkt_stats.ch1.timeouts.load(std::memory_order_relaxed);

  uint32_t ch2_rx = g_pkt_stats.ch2.rx_pkts.load(std::memory_order_relaxed);
  uint32_t ch2_tx = g_pkt_stats.ch2.tx_pkts.load(std::memory_order_relaxed);
  uint32_t ch2_uncached = g_pkt_stats.ch2.uncached_pkts.load(std::memory_order_relaxed);

  uint32_t ch3_rx = g_pkt_stats.ch3.rx_pkts.load(std::memory_order_relaxed);
  uint32_t ch3_tx = g_pkt_stats.ch3.tx_pkts.load(std::memory_order_relaxed);
  uint32_t ch3_uncached = g_pkt_stats.ch3.uncached_pkts.load(std::memory_order_relaxed);

  uint32_t ch4_rx = g_pkt_stats.ch4.rx_pkts.load(std::memory_order_relaxed);
  uint32_t ch4_tx = g_pkt_stats.ch4.tx_pkts.load(std::memory_order_relaxed);
  uint32_t ch4_inv = g_pkt_stats.ch4.invalid_frames.load(std::memory_order_relaxed);

  uint32_t ch5_rx = g_pkt_stats.ch5.rx_pkts.load(std::memory_order_relaxed);
  uint32_t ch5_tx = g_pkt_stats.ch5.tx_pkts.load(std::memory_order_relaxed);
  uint32_t ch5_drp = g_pkt_stats.ch5.dropped_pkts.load(std::memory_order_relaxed);

  uint32_t ch6_rx = g_pkt_stats.ch6.rx_pkts.load(std::memory_order_relaxed);
  uint32_t ch6_tx = g_pkt_stats.ch6.tx_pkts.load(std::memory_order_relaxed);
  uint32_t ch6_drp = g_pkt_stats.ch6.dropped_pkts.load(std::memory_order_relaxed);

  uint32_t ch7_rx = g_pkt_stats.ch7.rx_pkts.load(std::memory_order_relaxed);
  uint32_t ch7_tx = g_pkt_stats.ch7.tx_pkts.load(std::memory_order_relaxed);

  // CRC 에러율 계산
  float crc_rate = (ch1_rx > 0) ? (static_cast<float>(ch1_crc) * 100.0f / static_cast<float>(ch1_rx)) : 0.0f;

  // 5. Diagnostics: Reset Reason
  const char *rst_reason = "Normal Boot";
  esp_reset_reason_t rr = esp_reset_reason();
  switch (rr) {
    case ESP_RST_POWERON: rst_reason = "Power-On Reset"; break;
    case ESP_RST_EXT: rst_reason = "Hardware Reset Pin (EXT)"; break;
    case ESP_RST_PANIC: rst_reason = "CPU Panic / Crash Exception"; break;
    case ESP_RST_TASK_WDT: rst_reason = "Task Watchdog Reset"; break;
    case ESP_RST_BROWNOUT: rst_reason = "HW: Brownout (Low Voltage)"; break;
    case ESP_RST_SW: rst_reason = "Software Restart"; break;
    default: rst_reason = "Other Reset"; break;
  }

  out.append("{\"res\":\"ok\",");

  // System
  out.appendFormat("\"system\":{\"temp_c\":%d,\"wifi_rssi\":%d,\"uptime_s\":%u,"
                   "\"cpu0_load\":%u,\"cpu1_load\":%u,\"free_heap_kb\":%u,\"min_free_heap_kb\":%u,"
                   "\"ntp_synced\":%s,\"firmware\":\"%s\",\"latest_firmware\":\"Ready\"},",
                   static_cast<int>(temp_c), static_cast<int>(rssi), uptime_s,
                   static_cast<unsigned>(c0), static_cast<unsigned>(c1),
                   free_heap_kb, min_free_heap_kb,
                   ntp_synced ? "true" : "false", Config::FIRMWARE_VERSION);

  // Wi-Fi
  wifi_mode_t cur_wmode = WIFI_MODE_NULL;
  esp_wifi_get_mode(&cur_wmode);
  const char *mode_str = "STA";
  if (cur_wmode == WIFI_MODE_AP) mode_str = "AP";
  else if (cur_wmode == WIFI_MODE_APSTA) mode_str = "AP_STA";

  out.appendFormat("\"wifi\":{\"ssid\":\"%s\",\"rssi\":%d,\"ip\":\"%s\",\"mode\":\"%s\"},",
                   WiFi.status() == WL_CONNECTED ? WiFi.SSID().c_str() : "Disconnected",
                   WiFi.status() == WL_CONNECTED ? static_cast<int>(WiFi.RSSI()) : -100,
                   WiFi.localIP().toString().c_str(),
                   mode_str);

  // OTA State
  out.appendFormat("\"ota\":{\"in_progress\":%s,\"status\":\"%s\",\"progress_pct\":%u,\"last_error\":\"%s\"},",
                   g_http_ota_state.in_progress.load() ? "true" : "false",
                   g_http_ota_state.status,
                   g_http_ota_state.progress_pct,
                   g_http_ota_state.last_error);

  // Profile
  ProfileRepository::getActiveProfile(active_prof);

  out.appendFormat("\"profile\":{\"active_slot\":%u,\"active_key\":\"%s\",\"active_name\":\"%s\","
                   "\"is_locked\":%s,\"stx\":\"0x%02X\",\"etx\":\"0x%02X\","
                   "\"cs_algo\":\"%s\",\"opcodes\":{\"query\":\"0x%02X\",\"control\":\"0x%02X\",\"ack\":\"0x%02X\"},"
                   "\"match_count\":%u},",
                   static_cast<unsigned>(g_config.wallpad_profile), active_prof.key, active_prof.name,
                   auto_desc.is_locked ? "true" : "false",
                   auto_desc.stx, auto_desc.etx,
                   AutoProbingEngine::getAlgoName(auto_desc.checksum_algo),
                   auto_desc.query_opcode, auto_desc.control_opcode, auto_desc.ack_opcode,
                   auto_desc.matched_packets);

  // Timing
  out.appendFormat("\"timing\":{\"ch1_poll_interval_ms\":%u,\"ch2_ack_delay_ms\":%u,\"ch3_ack_delay_ms\":%u,"
                   "\"vip_preemptions\":%u,\"last_cmd_latency_ms\":%u},",
                   static_cast<unsigned>(g_timing_config.ch1_poll_interval_ms),
                   static_cast<unsigned>(g_timing_config.ch2_cache_delay_ms),
                   static_cast<unsigned>(g_timing_config.ch3_cache_delay_ms),
                   g_ch1_state_metrics.vip_cnt.load(std::memory_order_relaxed),
                   22);

  // Cache
  out.appendFormat("\"cache\":{\"source\":\"%s\",\"total_devices\":%u,\"online_devices\":%u,"
                   "\"stale_devices\":%u,\"cache_hit_rate\":%.1f},",
                   wc_src, static_cast<unsigned>(total_devs), static_cast<unsigned>(online_devs),
                   static_cast<unsigned>(stale_devs),
                   (ch2_rx > 0 ? (100.0f - (static_cast<float>(ch2_uncached) * 100.0f / ch2_rx)) : 100.0f));

  // UART Configuration for CH1 ~ CH4
  const char *f1 = formatFramingStr(g_config.uart_data_bits, g_config.uart_parity, g_config.uart_stop_bits);
  const char *f2 = formatFramingStr(g_config.ch2_data_bits, g_config.ch2_parity, g_config.ch2_stop_bits);
  const char *f3 = formatFramingStr(g_config.ch3_data_bits, g_config.ch3_parity, g_config.ch3_stop_bits);
  const char *f4 = formatFramingStr(g_config.doorphone_data_bits, g_config.doorphone_parity, g_config.doorphone_stop_bits);

  out.appendFormat("\"uart\":{"
                   "\"ch1\":{\"baud\":%u,\"format\":\"%s\"},"
                   "\"ch2\":{\"baud\":%u,\"format\":\"%s\"},"
                   "\"ch3\":{\"baud\":%u,\"format\":\"%s\"},"
                   "\"ch4\":{\"baud\":%u,\"format\":\"%s\"}},",
                   static_cast<unsigned>(g_config.uart_baud_rate), f1,
                   static_cast<unsigned>(g_config.ch2_baud_rate), f2,
                   static_cast<unsigned>(g_config.ch3_baud_rate), f3,
                   static_cast<unsigned>(g_doorphone_serial.baudRate() > 0 ? g_doorphone_serial.baudRate() : g_config.doorphone_baud_rate), f4);

  // Channels
  out.appendFormat("\"channels\":{\"ch1\":{\"rx\":%u,\"tx\":%u,\"crc_err\":%u,\"timeout\":%u,\"crc_rate\":%.2f},"
                   "\"ch2\":{\"rx\":%u,\"tx\":%u,\"uncached\":%u},"
                   "\"ch3\":{\"rx\":%u,\"tx\":%u,\"uncached\":%u},"
                   "\"ch4\":{\"rx\":%u,\"tx\":%u,\"inv\":%u},"
                   "\"ch5\":{\"rx\":%u,\"tx\":%u,\"dropped\":%u},"
                   "\"ch6\":{\"rx\":%u,\"tx\":%u,\"dropped\":%u},"
                   "\"ch7\":{\"rx\":%u,\"tx\":%u}},",
                   ch1_rx, ch1_tx, ch1_crc, ch1_tout, crc_rate,
                   ch2_rx, ch2_tx, ch2_uncached,
                   ch3_rx, ch3_tx, ch3_uncached,
                   ch4_rx, ch4_tx, ch4_inv,
                   ch5_rx, ch5_tx, ch5_drp,
                   ch6_rx, ch6_tx, ch6_drp,
                   ch7_rx, ch7_tx);

  // Diagnostics & CoreDump
  out.append("\"diagnostics\":{");
  out.appendFormat("\"last_reboot_reason\":\"%s\",\"rollback_detected\":%s,\"rescue_mode\":%s,",
                   rst_reason, g_rollback_detected ? "true" : "false",
                   g_rescue_mode.load(std::memory_order_relaxed) ? "true" : "false");

  // CoreDump Object
  out.append("\"coredump\":{");
  if (g_coredump_info.valid) {
    out.appendFormat("\"valid\":true,\"task\":\"%s\",\"pc\":\"0x%08X\",\"cause\":%u,\"bt_depth\":%u,"
                     "\"summary\":\"⚠️ Crash in %s at 0x%08X (Cause %u)\"},",
                     g_coredump_info.task_name, g_coredump_info.exc_pc, g_coredump_info.exc_cause,
                     g_coredump_info.bt_depth, g_coredump_info.task_name, g_coredump_info.exc_pc,
                     g_coredump_info.exc_cause);
  } else {
    out.append("\"valid\":false,\"task\":\"\",\"pc\":\"0x00000000\",\"cause\":0,\"bt_depth\":0,"
               "\"summary\":\"No Crash Dump (Flash Clean)\"},");
  }

  // Reboot Logs Array (up to 5 entries)
  out.append("\"reboot_logs\":[");
  size_t log_cnt = LogManager::getLogCount();
  size_t max_logs_to_emit = (log_cnt > 5) ? 5 : log_cnt;
  for (size_t i = 0; i < max_logs_to_emit; i++) {
    LogEntry e{};
    if (LogManager::getLogEntry(i, e)) {
      char time_buf[32] = "N/A";
      if (e.timestamp > 0) {
        struct tm timeinfo;
        time_t sec = static_cast<time_t>(e.timestamp);
        localtime_r(&sec, &timeinfo);
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
      }
      uint32_t up_s = e.stats_snapshot.uptime_ms / 1000;
      char up_str[24];
      snprintf(up_str, sizeof(up_str), "%02uh %02um", up_s / 3600, (up_s % 3600) / 60);

      if (i > 0) out.append(",");
      out.appendFormat("{\"id\":%u,\"time\":\"%s\",\"reason\":\"%s\",\"uptime\":\"%s\"}",
                       static_cast<unsigned>(i + 1), time_buf, e.reason, up_str);
    }
  }
  out.append("]}}");
}

// ============================================================================
// JSON-RPC 디스패처
// ============================================================================
static inline const char *findJsonStringValue(const char *json, const char *key, char *out_val, size_t max_len) {
  if (!json || !key || !out_val || max_len == 0) return nullptr;
  char pattern[64];
  snprintf(pattern, sizeof(pattern), "\"%s\"", key);
  const char *p = strstr(json, pattern);
  if (!p) return nullptr;
  p += strlen(pattern);
  while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\"') {
    if (*p == '\"') { p++; break; }
    p++;
  }
  size_t idx = 0;
  while (*p && *p != '\"' && *p != '\r' && *p != '\n' && idx + 1 < max_len) {
    out_val[idx++] = *p++;
  }
  out_val[idx] = '\0';
  return out_val;
}

static inline long findJsonIntValue(const char *json, const char *key, long default_val = -1) {
  if (!json || !key) return default_val;
  char pattern[64];
  snprintf(pattern, sizeof(pattern), "\"%s\"", key);
  const char *p = strstr(json, pattern);
  if (!p) return default_val;
  p += strlen(pattern);
  while (*p == ' ' || *p == ':' || *p == '\t') p++;
  char *endp = nullptr;
  long val = strtol(p, &endp, 10);
  if (endp == p) return default_val;
  return val;
}

void Mgmt_DispatchJsonRpc(int sock, const char *json_str) {
  if (sock < 0 || !json_str) return;

  char cmd[64] = {0};
  if (!findJsonStringValue(json_str, "cmd", cmd, sizeof(cmd))) {
    const char *err_msg = "{\"res\":\"error\",\"msg\":\"Missing 'cmd' field\"}\n";
    send(sock, err_msg, strlen(err_msg), MSG_DONTWAIT);
    return;
  }

  // 1. get_telemetry
  if (strcasecmp(cmd, "get_telemetry") == 0) {
    static char tel_buf[3072];
    tel_buf[0] = '\0';
    AppendBuf ab{tel_buf, sizeof(tel_buf)};
    Mgmt_SerializeTelemetry(ab);
    ab.append("\n");
    send(sock, ab.buf, ab.offset, MSG_DONTWAIT);
    g_pkt_stats.ch7.tx_pkts.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  // 2. set_profile
  if (strcasecmp(cmd, "set_profile") == 0) {
    long slot = findJsonIntValue(json_str, "slot", -1);
    if (slot >= 0 && slot < static_cast<long>(ProfileRepository::MAX_PROFILES)) {
      ProfileRepository::setActiveProfileIndex(static_cast<size_t>(slot));
      g_config.wallpad_profile = static_cast<uint8_t>(slot);
      const char *ok_msg = "{\"res\":\"ok\",\"msg\":\"Profile updated\"}\n";
      send(sock, ok_msg, strlen(ok_msg), MSG_DONTWAIT);
    } else {
      const char *err_msg = "{\"res\":\"error\",\"msg\":\"Invalid profile slot (0~3)\"}\n";
      send(sock, err_msg, strlen(err_msg), MSG_DONTWAIT);
    }
    return;
  }

  // 3. save_auto_to_slot
  if (strcasecmp(cmd, "save_auto_to_slot") == 0) {
    char name_buf[36] = {0};
    findJsonStringValue(json_str, "name", name_buf, sizeof(name_buf));
    if (strlen(name_buf) == 0) strncpy(name_buf, "Saved Custom", sizeof(name_buf) - 1);

    size_t saved_slot = 1;
    if (ProfileRepository::saveCurrentAutoAs(name_buf, saved_slot)) {
      char resp[96];
      snprintf(resp, sizeof(resp), "{\"res\":\"ok\",\"saved_slot\":%u,\"msg\":\"Auto profile saved\"}\n",
               static_cast<unsigned>(saved_slot));
      send(sock, resp, strlen(resp), MSG_DONTWAIT);
    } else {
      const char *err_msg = "{\"res\":\"error\",\"msg\":\"Failed to save auto profile (Auto not locked or slots full)\"}\n";
      send(sock, err_msg, strlen(err_msg), MSG_DONTWAIT);
    }
    return;
  }

  // 4. set_timing
  if (strcasecmp(cmd, "set_timing") == 0) {
    long ch1_poll = findJsonIntValue(json_str, "ch1_poll_intvl", -1);
    long ch2_del  = findJsonIntValue(json_str, "ch2_delay", -1);
    long ch3_del  = findJsonIntValue(json_str, "ch3_delay", -1);

    bool updated = false;
    if (ch1_poll >= 50 && ch1_poll <= 5000) {
      g_timing_config.ch1_poll_interval_ms = static_cast<uint16_t>(ch1_poll);
      updated = true;
    }
    if (ch2_del >= 0 && ch2_del <= 500) {
      g_timing_config.ch2_cache_delay_ms = static_cast<uint16_t>(ch2_del);
      updated = true;
    }
    if (ch3_del >= 0 && ch3_del <= 1000) {
      g_timing_config.ch3_cache_delay_ms = static_cast<uint16_t>(ch3_del);
      updated = true;
    }

    if (updated) {
      TimingConfig_Save();
      const char *ok_msg = "{\"res\":\"ok\",\"msg\":\"Timing config updated & saved to NVS\"}\n";
      send(sock, ok_msg, strlen(ok_msg), MSG_DONTWAIT);
    } else {
      const char *err_msg = "{\"res\":\"error\",\"msg\":\"No valid timing parameters provided\"}\n";
      send(sock, err_msg, strlen(err_msg), MSG_DONTWAIT);
    }
    return;
  }

  // 5. cache_sync
  if (strcasecmp(cmd, "cache_sync") == 0) {
    WarmCache_SaveToNvs();
    const char *ok_msg = "{\"res\":\"ok\",\"msg\":\"Warm cache synced to NVS\"}\n";
    send(sock, ok_msg, strlen(ok_msg), MSG_DONTWAIT);
    return;
  }

  // 6. cache_purge_rescan
  if (strcasecmp(cmd, "cache_purge_rescan") == 0) {
    g_auto_probing_engine.reset();
    g_doorphone_tracker.clearNvs();
    g_polling_targets.clear();
    g_device_repo.clear();
    const char *ok_msg = "{\"res\":\"ok\",\"msg\":\"Auto-probing reset and cache purged, bus rescan triggered\"}\n";
    send(sock, ok_msg, strlen(ok_msg), MSG_DONTWAIT);
    return;
  }

  // 6.1. wallpad_reset (Explicit Wallpad Auto-probing Reset)
  if (strcasecmp(cmd, "wallpad_reset") == 0) {
    g_auto_probing_engine.reset();
    g_doorphone_tracker.clearNvs();
    g_polling_targets.clear();
    g_device_repo.clear();
    const char *ok_msg = "{\"res\":\"ok\",\"msg\":\"Wallpad auto-probing and framing reset completed\"}\n";
    send(sock, ok_msg, strlen(ok_msg), MSG_DONTWAIT);
    return;
  }

  // 7. clear_coredump
  if (strcasecmp(cmd, "clear_coredump") == 0) {
    esp_core_dump_image_erase();
    g_coredump_info.valid = false;
    memset(&g_coredump_info, 0, sizeof(g_coredump_info));
    const char *ok_msg = "{\"res\":\"ok\",\"msg\":\"Flash core dump erased\"}\n";
    send(sock, ok_msg, strlen(ok_msg), MSG_DONTWAIT);
    return;
  }

  // 8. clear_reboot_logs
  if (strcasecmp(cmd, "clear_reboot_logs") == 0) {
    LogManager::clearRebootLog();
    const char *ok_msg = "{\"res\":\"ok\",\"msg\":\"Reboot logs cleared from NVS\"}\n";
    send(sock, ok_msg, strlen(ok_msg), MSG_DONTWAIT);
    return;
  }

  // 9. wifi_scan (Top 4 Strongest SSIDs with signal percentage, skipping empty/hidden)
  if (strcasecmp(cmd, "wifi_scan") == 0) {
    int n = WiFi.scanNetworks(false, true);
    if (n <= 0) {
      WiFi.scanDelete();
      const char *no_ap_msg = "{\"res\":\"ok\",\"count\":0,\"ap_count\":0,\"aps\":[],\"msg\":\"No Networks Found\"}\n";
      send(sock, no_ap_msg, strlen(no_ap_msg), MSG_DONTWAIT);
      return;
    }

    // 인덱스 배열 정렬 (RSSI 내림차순)
    std::vector<int> indices(n);
    for (int i = 0; i < n; ++i) indices[i] = i;
    std::sort(indices.begin(), indices.end(), [](int a, int b) {
      return WiFi.RSSI(a) > WiFi.RSSI(b);
    });

    struct ApInfo {
      String ssid;
      int pct;
    };
    std::vector<ApInfo> top_aps;
    top_aps.reserve(4);

    for (int idx : indices) {
      String s = WiFi.SSID(idx);
      s.trim();
      if (s.length() == 0) continue;

      // 중복 SSID 방지
      bool duplicate = false;
      for (const auto &item : top_aps) {
        if (item.ssid == s) {
          duplicate = true;
          break;
        }
      }
      if (duplicate) continue;

      int rssi = WiFi.RSSI(idx);
      int pct = std::min(100, std::max(0, 2 * (rssi + 100)));
      s.replace("\"", "\\\""); // JSON escape
      top_aps.push_back({s, pct});
      if (top_aps.size() >= 4) break;
    }
    WiFi.scanDelete();

    char resp[512];
    int offset = snprintf(resp, sizeof(resp), "{\"res\":\"ok\",\"count\":%d,\"ap_count\":%u,\"aps\":[",
                          n, static_cast<unsigned>(top_aps.size()));
    for (size_t i = 0; i < top_aps.size(); ++i) {
      offset += snprintf(resp + offset, sizeof(resp) - offset,
                         "%s{\"ssid\":\"%s\",\"pct\":%d}",
                         (i > 0 ? "," : ""),
                         top_aps[i].ssid.c_str(),
                         top_aps[i].pct);
      if (offset >= (int)sizeof(resp) - 8) break;
    }
    snprintf(resp + offset, sizeof(resp) - offset, "]}\n");
    send(sock, resp, strlen(resp), MSG_DONTWAIT);
    return;
  }

  // 10. start_http_ota (Cloud OTA from GitHub Release)
  if (strcasecmp(cmd, "start_ota") == 0) {
    char url[256] = {0};
    findJsonStringValue(json_str, "url", url, sizeof(url));
    Mgmt_StartHttpOta(url[0] ? url : DEFAULT_CLOUD_OTA_URL);
    const char *ok_msg = "{\"res\":\"ok\",\"msg\":\"Cloud HTTP OTA started in background\"}\n";
    send(sock, ok_msg, strlen(ok_msg), MSG_DONTWAIT);
    return;
  }

  // 10. system_reboot
  if (strcasecmp(cmd, "system_reboot") == 0) {
    char reason[48] = "ST Remote Reboot";
    findJsonStringValue(json_str, "reason", reason, sizeof(reason));
    const char *ok_msg = "{\"res\":\"ok\",\"msg\":\"System rebooting...\"}\n";
    send(sock, ok_msg, strlen(ok_msg), MSG_DONTWAIT);
    vTaskDelay(pdMS_TO_TICKS(100));
    System_Restart(reason);
    return;
  }

  // 11. set_wifi_mode
  if (strcasecmp(cmd, "set_wifi_mode") == 0) {
    char mode_buf[16] = {0};
    if (findJsonStringValue(json_str, "mode", mode_buf, sizeof(mode_buf))) {
      if (strcasecmp(mode_buf, "AP") == 0) {
        WiFi.mode(WIFI_AP);
      } else if (strcasecmp(mode_buf, "AP_STA") == 0 || strcasecmp(mode_buf, "AP+STA") == 0) {
        WiFi.mode(WIFI_AP_STA);
      } else {
        WiFi.mode(WIFI_STA);
      }
      const char *ok_msg = "{\"res\":\"ok\",\"msg\":\"Wi-Fi mode updated\"}\n";
      send(sock, ok_msg, strlen(ok_msg), MSG_DONTWAIT);
    } else {
      const char *err_msg = "{\"res\":\"error\",\"msg\":\"Missing mode parameter\"}\n";
      send(sock, err_msg, strlen(err_msg), MSG_DONTWAIT);
    }
    return;
  }

  // 12. set_wifi
  if (strcasecmp(cmd, "set_wifi") == 0) {
    char new_ssid[64] = {0};
    char new_pass[64] = {0};
    bool has_ssid = findJsonStringValue(json_str, "ssid", new_ssid, sizeof(new_ssid));
    bool has_pass = findJsonStringValue(json_str, "password", new_pass, sizeof(new_pass));

    if (!has_ssid || strlen(new_ssid) == 0) {
      const char *err_msg = "{\"res\":\"error\",\"msg\":\"Missing or empty SSID\"}\n";
      send(sock, err_msg, strlen(err_msg), MSG_DONTWAIT);
      return;
    }

    if (has_pass && strlen(new_pass) > 0 && strlen(new_pass) < 8) {
      const char *err_msg = "{\"res\":\"error\",\"msg\":\"Wi-Fi password must be at least 8 characters (or empty for open network)\"}\n";
      send(sock, err_msg, strlen(err_msg), MSG_DONTWAIT);
      return;
    }

    // 현재 정상 동작 중인 Wi-Fi 정보를 백업하여 15초 내 접속 실패 시 자동 롤백 준비
    strncpy(g_wifi_guard.prev_ssid, g_config.wifi_ssid, sizeof(g_wifi_guard.prev_ssid) - 1);
    strncpy(g_wifi_guard.prev_pass, g_config.wifi_password, sizeof(g_wifi_guard.prev_pass) - 1);
    g_wifi_guard.start_ms = millis();
    g_wifi_guard.testing.store(true, std::memory_order_release);

    strncpy(g_config.wifi_ssid, new_ssid, sizeof(g_config.wifi_ssid) - 1);
    strncpy(g_config.wifi_password, new_pass, sizeof(g_config.wifi_password) - 1);

    const char *ok_msg = "{\"res\":\"ok\",\"msg\":\"Testing new Wi-Fi credentials (15s automatic fallback guard active)...\"}\n";
    send(sock, ok_msg, strlen(ok_msg), MSG_DONTWAIT);
    vTaskDelay(pdMS_TO_TICKS(50));

    WiFi.disconnect(false);
    vTaskDelay(pdMS_TO_TICKS(50));
    WiFi.begin(g_config.wifi_ssid, g_config.wifi_password);
    return;
  }

  // 13. set_uart (Edge Driver RS-485 Dynamic Configuration)
  if (strcasecmp(cmd, "set_uart") == 0) {
    long ch = findJsonIntValue(json_str, "ch", 0);
    long baud = findJsonIntValue(json_str, "baud", 0);
    char format[16] = {0};
    findJsonStringValue(json_str, "format", format, sizeof(format));

    if (ch >= 1 && ch <= 4 && baud >= 1200 && baud <= 921600 && format[0]) {
      if (System_ApplyUartConfig(static_cast<uint8_t>(ch), static_cast<uint32_t>(baud), format)) {
        char ok_msg[128];
        snprintf(ok_msg, sizeof(ok_msg),
                 "{\"res\":\"ok\",\"msg\":\"CH%ld UART updated to %ld %s and saved to NVS\"}\n",
                 ch, baud, format);
        send(sock, ok_msg, strlen(ok_msg), MSG_DONTWAIT);
        return;
      }
    }
    const char *err_msg = "{\"res\":\"error\",\"msg\":\"Invalid ch (1-4), baud (1200-921600), or format (8N1/8E1/8O1/8N2)\"}\n";
    send(sock, err_msg, strlen(err_msg), MSG_DONTWAIT);
    return;
  }

  // Unknown Command
  const char *unk_msg = "{\"res\":\"error\",\"msg\":\"Unknown command\"}\n";
  send(sock, unk_msg, strlen(unk_msg), MSG_DONTWAIT);
}

// ============================================================================
// Mgmt TCP 세션 데이터 수신 핸들러 (스트림 프레이밍)
// ============================================================================
void Mgmt_Data(MgmtSession *s, const uint8_t *data, size_t len) {
  if (!s || s->sock < 0 || !data || len == 0) return;

  g_pkt_stats.ch7.rx_pkts.fetch_add(1, std::memory_order_relaxed);

  if (s->len + len > sizeof(s->buffer)) {
    s->len = 0; // 버퍼 오버플로우 방어
  }

  std::copy(data, data + len, s->buffer + s->len);
  s->len += len;

  size_t p = 0;
  while (p < s->len) {
    if (s->buffer[p] == '\n' || s->buffer[p] == '\r') {
      s->buffer[p] = '\0';
      if (p > 0) {
        // 완전한 한 줄 JSON 수신!
        Mgmt_DispatchJsonRpc(s->sock, reinterpret_cast<const char *>(s->buffer));
      }
      // 앞선 라인 소진 후 시프트
      size_t rem = s->len - (p + 1);
      if (rem > 0) {
        memmove(s->buffer, s->buffer + p + 1, rem);
      }
      s->len = rem;
      p = 0;
      continue;
    }
    p++;
  }
}
