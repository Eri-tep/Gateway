#include "Common.h"
#include "ControlTemplate.h"
#include "ESP.h"
#include "MgmtRpc.h"
#include "TelnetCli.h"
#include "WallpadParser.h"
#include "esp_sntp.h"
#include "lwip/ip.h"
#include "lwip/tcp.h"
#include "esp_attr.h"
#include "esp_ota_ops.h"
#include "esp_task_wdt.h"
#include <mbedtls/sha256.h>

void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info);

void WarmCache_SaveToRtc() {
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
      Serial.printf("[WARM CACHE] Restored %u targets from RTC Fast SRAM (0ms "
                    "delay)!\r\n",
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
      if (len == sizeof(env) &&
          p.getBytes("wc_data", &env, sizeof(env)) == sizeof(env)) {
        if (env.verify() && env.payload.count > 0 &&
            env.payload.count <= PollingTargetRegistry::MAX_TARGETS) {
          uint32_t computed_crc =
              FastCrc32(reinterpret_cast<const uint8_t *>(env.payload.entries),
                        sizeof(RtcWarmCacheEntry) * env.payload.count);
          if (computed_crc == env.payload.crc32) {
            g_polling_targets.loadFromWarmCache(env.payload.entries,
                                                env.payload.count, now);
            g_warm_cache_loaded = true;
            g_warm_cache_source = 2;
            g_warm_cache_restored_count = env.payload.count;
            Serial.printf(
                "[WARM CACHE] Restored %u targets from NVS Flash snapshot!\r\n",
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
  Serial.println(
      F("[WARM CACHE] Cold start initialized (No prior cache found)."));
}

void WarmCache_CheckNvsDebounce() {
  if (g_warm_cache_dirty.load(std::memory_order_acquire)) {
    uint32_t dirty_ms = g_warm_cache_dirty_ms.load(std::memory_order_relaxed);
    if (dirty_ms > 0 &&
        TimeUtils::isElapsed(dirty_ms,
                             Config::Timing::WARM_CACHE_NVS_DEBOUNCE_MS)) {
      WarmCache_SaveToRtc();
      WarmCache_SaveToNvs();
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

  {
    MutexLocker lock(g_ch5_mutex);
    for (int s = 0; s < Config::TCP::MAX_EW11_SLOTS; s++) {
      if (g_ew11_slots[s].sock >= 0) {
        close(g_ew11_slots[s].sock);
        g_ew11_slots[s].sock = -1;
        g_ew11_slots[s].is_connected = false;
        g_ew11_slots[s].rx_len = 0;
      }
    }
  }

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
  Serial.printf("  GATEWAY BRIDGE %s BOOT INITIALIZATION\r\n",
                Config::FIRMWARE_VERSION);
  Serial.println(F("========================================"));
  if (s_pending_reboot_reason) {
    Serial.printf("[BOOT] Last Reset Reason: %s\r\n", s_pending_reboot_reason);
  }

  // [하드웨어 자동 롤백 감지] Standby 파티션이 무효화(INVALID)되었는지 검사
  const esp_partition_t *next_p = esp_ota_get_next_update_partition(nullptr);
  esp_ota_img_states_t next_state = ESP_OTA_IMG_UNDEFINED;
  if (next_p && esp_ota_get_state_partition(next_p, &next_state) == ESP_OK) {
    if (next_state == ESP_OTA_IMG_INVALID ||
        next_state == ESP_OTA_IMG_ABORTED) {
      g_rollback_detected = true;
      const esp_partition_t *run_p = esp_ota_get_running_partition();
      Serial.printf("[BOOT] ★ AUTO-ROLLBACK ACTIVE: Rolled back from failed "
                    "'%s' to stable '%s'!\r\n",
                    next_p->label, run_p ? run_p->label : "app0");
    }
  }

  // [RTC 크래시 감지] 실제 비정상 크래시/워치독만 정밀 추적 (정상 SW
  // 리부팅/전원 인가는 제외)
  esp_reset_reason_t reset_reason = esp_reset_reason();
  bool is_abnormal_crash =
      (reset_reason == ESP_RST_PANIC || reset_reason == ESP_RST_TASK_WDT ||
       reset_reason == ESP_RST_INT_WDT || reset_reason == ESP_RST_WDT ||
       reset_reason == ESP_RST_BROWNOUT);

  if (rtc_rescue_magic != RTC_MAGIC_RESCUE || !is_abnormal_crash) {
    rtc_rescue_magic = RTC_MAGIC_RESCUE;
    rtc_crash_counter = 0;
  } else {
    rtc_crash_counter++;
    Serial.printf("[BOOT] Consecutive crash count: %u (Reason: %d)\r\n",
                  rtc_crash_counter, reset_reason);
  }

  // [하드웨어 버튼 비상 복구] AtomS3 화면 물리 버튼 (GPIO 41)을 2.5초간 누르면
  // 강제 복구 모드
  pinMode(Config::GPIO::BTN_PIN, INPUT_PULLUP);
  if (digitalRead(Config::GPIO::BTN_PIN) == LOW) {
    Serial.println(F("[BOOT] Front button pressed, checking 2.5s hold..."));
    uint32_t press_start = millis();
    bool held = true;
    while (!TimeUtils::isElapsed(press_start,
                                 Config::Timing::RESCUE_BUTTON_HOLD_MS)) {
      if (digitalRead(Config::GPIO::BTN_PIN) != LOW) {
        held = false;
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (held) {
      Serial.println(F("[SAFE BOOT] ★ Front Button Held (2.5s) -> Forcing "
                       "Rescue Safe Mode!"));
      System_EnterRescueMode("Hardware Button Override");
    }
  }

  // [크래시 루프 방어 & 자동 롤백] 연속 3회 이상 부팅 실패 감지 시
  if (!g_rescue_mode.load(std::memory_order_relaxed) &&
      rtc_crash_counter >= 3) {
    const esp_partition_t *run_p = esp_ota_get_running_partition();
    const esp_partition_t *next_p = esp_ota_get_next_update_partition(nullptr);

    // 1단계: 상대 파티션(이전 정상 펌웨어)이 존재하면 즉시 롤백 시도
    if (run_p && next_p && strcmp(run_p->label, next_p->label) != 0) {
      Serial.printf("[RESCUE] ★ Crash Loop detected (%u crashes)! Rolling back "
                    "from '%s' to '%s'...\r\n",
                    rtc_crash_counter, run_p->label, next_p->label);
      rtc_crash_counter = 0; // 롤백 시도 시 카운터 리셋
      esp_err_t err = esp_ota_set_boot_partition(next_p);
      if (err == ESP_OK) {
        Serial.println(F("[RESCUE] Boot partition switched successfully. "
                         "Rebooting into previous firmware..."));
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
      } else {
        Serial.printf("[RESCUE] esp_ota_set_boot_partition failed (err=0x%x). "
                      "Fallback to Rescue Safe Mode...\r\n",
                      err);
      }
    }

    // 2단계: 상대 파티션이 없거나 롤백 실패 시 Rescue Safe Mode 진입
    Serial.printf("[RESCUE] ★ Crash Loop detected (%u crashes)! Forcing Rescue "
                  "Safe Mode...\r\n",
                  rtc_crash_counter);
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
    uart_config_t cfg = {.baud_rate = static_cast<int>(baud),
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
            g_config.uart_parity, g_config.uart_stop_bits,
            &g_uart0_event_queue);
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
    strncpy(reinterpret_cast<char *>(w_conf.sta.password),
            g_config.wifi_password, sizeof(w_conf.sta.password) - 1);
    w_conf.sta.scan_method = WIFI_FAST_SCAN;
    w_conf.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    w_conf.sta.pmf_cfg.capable = true;
    w_conf.sta.pmf_cfg.required = false;
    w_conf.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_set_config(WIFI_IF_STA, &w_conf);
    esp_wifi_connect();

    uint32_t t_start = millis();
    uint32_t max_wait =
        (g_config.wifi_connect_timeout_s ? g_config.wifi_connect_timeout_s
                                         : 30) *
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
      Serial.printf("[WIFI] STA connect failed. Fallback SoftAP '%s' started: "
                    "%s (IP: %s)\r\n",
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
      ::Serial.println(F("[ArduinoOTA] Start transfer..."));
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
      esp_task_wdt_reset();
      g_wdt_monitor.feed(4);
    });
    ArduinoOTA.onEnd([]() {
      g_ota_in_progress.store(false, std::memory_order_release);
      if (g_system_event_group) {
        xEventGroupSetBits(g_system_event_group, SYS_EVT_OTA_IDLE);
      }
      ::Serial.println(F("[ArduinoOTA] Finished successfully!"));
      vTaskDelay(pdMS_TO_TICKS(200));
      System_Restart("OTA Firmware Update");
    });
    ArduinoOTA.onError([](ota_error_t error) {
      g_ota_in_progress.store(false, std::memory_order_release);
      if (g_system_event_group) {
        xEventGroupSetBits(g_system_event_group, SYS_EVT_OTA_IDLE);
      }
      ::Serial.printf("[ArduinoOTA] Error (%u)\r\n", (unsigned)error);
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
    g_ch1_task_handle =
        cr_task(Task_Ch1, "CH#1_IoT", Config::Task::STACK_SIZE_CORE1, nullptr,
                13, stackCore1Ch1, &g_task_core1_ch1_buf);
    g_ch2_task_handle =
        cr_task(Task_Ch2Ch3, "CH#2_WP#1", Config::Task::STACK_SIZE_CORE1,
                &ch2_config, 10, stackCore1Slave, &g_task_core1_slave_buf);
    g_ch3_task_handle =
        cr_task(Task_Ch2Ch3, "CH#3_WP#2", Config::Task::STACK_SIZE_CORE1,
                &ch3_config, 10, stackCore1Slave2, &g_task_core1_slave2_buf);
    g_ch4_task_handle =
        cr_task(Task_Ch4, "CH#4_WP#3", Config::Task::STACK_SIZE_CH4, nullptr,
                11, stackCore1Ch4, &g_task_core1_ch4_buf);
  } else {
    Serial.println(F(
        "[RESCUE] RS-485 Tasks bypassed. Only Network & Telnet tasks active."));
  }
  g_network_task_handle =
      cr_task(Task_Network, "Network", Config::Task::STACK_SIZE_CORE0, nullptr,
              12, stackCore0Net, &g_task_core0_net_buf);
  g_telnet_task_handle =
      cr_task(Task_Telnet, "Telnet_CLI", Config::Task::STACK_SIZE_TELNET,
              nullptr, 10, telnetTaskStack, &g_telnet_task_buf);
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