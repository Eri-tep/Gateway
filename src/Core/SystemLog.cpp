#include "core/Platform.h"
#include "core/Config.h"
#include "core/Buffers.h"
#include "core/Metrics.h"
#include "core/Devices.h"
#include "WallpadParser.h"
#include "TelnetCli.h"
#include <Preferences.h>
#include <ctime>

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

  add.appendFormat("\r\n%s", Fmt::DIV80EQ);
  add.appendFormat("                   GATEWAY BRIDGE REBOOT SNAPSHOT MONITOR               "
      "      \r\n");
  add.appendFormat("%s", Fmt::DIV80EQ);
  add.appendFormat("Log Index       : #%zu / %zu\r\n", idx + 1, getLogCount());
  add.appendFormat("Reboot Reason   : %s\r\n", e.reason);
  add.appendFormat("Firmware        : %s\r\n", Config::FIRMWARE_VERSION);
  add.appendFormat("Log Time        : %s (%s)\r\n", t_buf, t_src);
  add.appendFormat("Uptime          : %ud %02uh %02um %02us\r\n", s / 86400,
      (s % 86400) / 3600, (s % 3600) / 60, s % 60);
  add.appendFormat("WiFi Connection : %s\r\n", w_str);
  add.appendFormat("Heap Memory     : Free %u KB / Min Free %u KB / Total %u KB\r\n",
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
      if (g_hub_slots[s].sock >= 0) {
        close(g_hub_slots[s].sock);
        g_hub_slots[s].sock = -1;
        g_hub_slots[s].is_connected = false;
        g_hub_slots[s].rx_len = 0;
      }
    }
  }

  uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(50));
  uart_wait_tx_done(UART_NUM_1, pdMS_TO_TICKS(50));
  uart_wait_tx_done(UART_NUM_2, pdMS_TO_TICKS(50));

  Cache_SaveToRtc();
  Cache_SaveToNvs();

  rtc_clean_restart_magic = RTC_MAGIC_CLEAN_RESTART;
  vTaskDelay(pdMS_TO_TICKS(150));
  esp_restart();
}
