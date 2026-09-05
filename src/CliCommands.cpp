#include "CliCommands.h"
#include "MgmtRpc.h"
#include "TelnetCli.h"
#include "WallpadParser.h"
#include "esp_ota_ops.h"
#include "esp_task_wdt.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>

// ============================================================================
// HELPER: TELNET SESSION & SOCKET CONTEXT EXTRACTION
// ============================================================================

static inline TelnetManager::TelnetSession *getSession(void *ctx) noexcept {
  return static_cast<TelnetManager::TelnetSession *>(ctx);
}

static inline int getSock(void *ctx) noexcept {
  auto *s = getSession(ctx);
  return s ? s->sock : -1;
}

// ============================================================================
// SHARED CLI SCRATCH BUFFER (BSS Memory Optimization)
// Telnet CLI commands execute sequentially in the single Task_Telnet context.
// Sharing this 5KB buffer reclaims ~26KB of static BSS RAM.
// ============================================================================
static char s_cli_scratch_buf[5120];

// ============================================================================
// SECTION 1: CONFIG CLI MODULE (namespace ConfigCli)
// ============================================================================

namespace ConfigCli {

enum ParamType {
  PARAM_UINT32,
  PARAM_UINT16,
  PARAM_UCHAR,
  PARAM_STRING,
  PARAM_PASS_HASH
};

struct ConfigParamDef {
  const char *name;
  ParamType type;
  union {
    uint32_t *u32;
    uint16_t *u16;
    uint8_t *u8;
    char *str;
  } ptr;
  size_t maxLen;
};

static const ConfigParamDef PARAM_TABLE[] = {
    {"uart_baud", PARAM_UINT32, {.u32 = &g_config.uart_baud_rate}, 0},
    {"uart_databits", PARAM_UCHAR, {.u8 = &g_config.uart_data_bits}, 0},
    {"uart_parity", PARAM_UCHAR, {.u8 = &g_config.uart_parity}, 0},
    {"uart_stopbits", PARAM_UCHAR, {.u8 = &g_config.uart_stop_bits}, 0},
    {"door_baud", PARAM_UINT32, {.u32 = &g_config.doorphone_baud_rate}, 0},
    {"door_databits", PARAM_UCHAR, {.u8 = &g_config.doorphone_data_bits}, 0},
    {"door_parity", PARAM_UCHAR, {.u8 = &g_config.doorphone_parity}, 0},
    {"door_stopbits", PARAM_UCHAR, {.u8 = &g_config.doorphone_stop_bits}, 0},
    {"wifi_ssid",
     PARAM_STRING,
     {.str = g_config.wifi_ssid},
     sizeof(g_config.wifi_ssid)},
    {"wifi_pass",
     PARAM_STRING,
     {.str = g_config.wifi_password},
     sizeof(g_config.wifi_password)},
    {"ap_ssid",
     PARAM_STRING,
     {.str = g_config.ap_ssid},
     sizeof(g_config.ap_ssid)},
    {"ap_pass",
     PARAM_STRING,
     {.str = g_config.ap_password},
     sizeof(g_config.ap_password)},
    {"wifi_timeout",
     PARAM_UINT16,
     {.u16 = &g_config.wifi_connect_timeout_s},
     0},
    {"telnet_pass", PARAM_PASS_HASH, {.str = g_config.telnet_pass_hash}, 0},
};
static const size_t PARAM_COUNT = sizeof(PARAM_TABLE) / sizeof(ConfigParamDef);

void printConfig(int sock) {
  char buf[1024], fu[10], fd[10];
  auto formatFraming = [](uint8_t d, uint8_t p, uint8_t s, char *o) {
    snprintf(o, 10, "%u%c%u", d, p == 1 ? 'E' : p == 2 ? 'O' : 'N', s);
  };
  formatFraming(g_config.uart_data_bits, g_config.uart_parity,
                g_config.uart_stop_bits, fu);
  formatFraming(g_config.doorphone_data_bits, g_config.doorphone_parity,
                g_config.doorphone_stop_bits, fd);

  snprintf(buf, sizeof(buf),
           "\r\n================================================================"
           "================\r\n"
           "                         GATEWAY CONFIGURATION (NVS Flash)      "
           "        \r\n"
           "================================================================"
           "================\r\n"
           "WiFi Station SSID : %s\r\n"
           "WiFi SoftAP SSID  : %s\r\n"
           "WiFi Connect Tout : %u sec\r\n"
           "Wallpad Profile   : ID %u\r\n"
           "UART RS-485 Conf  : %u bps, %s\r\n"
           "Doorphone UART    : %u bps, %s\r\n"
           "================================================================"
           "================\r\n\r\n",
           g_config.wifi_ssid, g_config.ap_ssid,
           g_config.wifi_connect_timeout_s,
           static_cast<unsigned>(g_config.wallpad_profile),
           static_cast<unsigned>(g_config.uart_baud_rate), fu,
           static_cast<unsigned>(g_config.doorphone_baud_rate), fd);
  sendTelnetMsg(sock, buf);
}

template <typename T>
static bool applyUintParam(T *dest, const char *value, unsigned long max_val,
                           int sock, const char *key) {
  char *endp = nullptr;
  unsigned long v = strtoul(value, &endp, 10);
  if (!endp || *endp != '\0' || v > max_val) {
    sendTelnetMsgf(sock, "[ERROR] Invalid value for '%s' (max: %lu)\r\n", key, max_val);
    return false;
  }
  *dest = static_cast<T>(v);
  g_config_dirty.store(true, std::memory_order_relaxed);
  sendTelnetMsgf(sock, "[OK] Set %s = %lu (Not saved to NVS yet. Use 'save' to commit)\r\n", key, v);
  return true;
}

void setConfig(void *session_context, const char *key, const char *value) {
  int sock = getSock(session_context);
  if (!key || !value) {
    sendTelnetMsg(sock, "[ERROR] Usage: config set <key> <value>\r\n");
    return;
  }

  for (size_t i = 0; i < PARAM_COUNT; ++i) {
    if (strcasecmp(PARAM_TABLE[i].name, key) == 0) {
      CriticalSectionLocker lock(&g_config_mux);
      switch (PARAM_TABLE[i].type) {
      case PARAM_UINT32:
        applyUintParam(PARAM_TABLE[i].ptr.u32, value, UINT32_MAX, sock, key);
        return;
      case PARAM_UINT16:
        applyUintParam(PARAM_TABLE[i].ptr.u16, value, 65535UL, sock, key);
        return;
      case PARAM_UCHAR:
        applyUintParam(PARAM_TABLE[i].ptr.u8, value, 255UL, sock, key);
        return;
      case PARAM_STRING: {
        if (strlen(value) < PARAM_TABLE[i].maxLen) {
          strncpy(PARAM_TABLE[i].ptr.str, value, PARAM_TABLE[i].maxLen - 1);
          PARAM_TABLE[i].ptr.str[PARAM_TABLE[i].maxLen - 1] = '\0';
          g_config_dirty.store(true, std::memory_order_relaxed);
          sendTelnetMsgf(sock, "[OK] Set %s = '%s' (Not saved to NVS yet. Use 'save' to commit)\r\n", key, value);
        } else {
          sendTelnetMsg(sock, "[ERROR] String exceeds maximum allowed length.\r\n");
        }
        return;
      }
      case PARAM_PASS_HASH: {
        char hash_hex[68];
        System_Sha256ToHex(value, hash_hex);
        strncpy(g_config.telnet_pass_hash, hash_hex, sizeof(g_config.telnet_pass_hash) - 1);
        g_config.telnet_pass_hash[sizeof(g_config.telnet_pass_hash) - 1] = '\0';
        g_config_dirty.store(true, std::memory_order_relaxed);
        sendTelnetMsg(sock, "[OK] Telnet password updated & SHA-256 hashed. Use 'save' to make permanent.\r\n");
        return;
      }
      }
    }
  }

  sendTelnetMsgf(sock, "[ERROR] Unknown configuration parameter '%s'. Type 'config' to see all params.\r\n", key);
}

void cmdConfig(EmbeddedCli *cli, char *args, void *context) {
  int sock = getSock(context);
  int argc = embeddedCliGetTokenCount(args);

  if (argc == 0) {
    printConfig(sock);
    return;
  }

  const char *sub = embeddedCliGetToken(args, 1);
  if (strcasecmp(sub, "set") == 0) {
    if (argc >= 3) {
      setConfig(context, embeddedCliGetToken(args, 2),
                embeddedCliGetToken(args, 3));
    } else {
      sendTelnetMsg(sock, "[ERROR] Usage: config set <key> <value>\r\n");
    }
    return;
  }

  if (strcasecmp(sub, "reset") == 0) {
    Config_ResetDefaults();
    sendTelnetMsg(sock, "[OK] Runtime configuration reset to system factory defaults. (Not saved to NVS)\r\n");
    return;
  }

  sendTelnetMsg(sock, "Usage: config [set <key> <value> | reset]\r\n");
}

void cmdSave(EmbeddedCli *cli, char *args, void *context) {
  int sock = getSock(context);
  Config_Save();
  sendTelnetMsg(sock, "[OK] Configuration successfully committed and saved to NVS flash!\r\n");
}

} // namespace ConfigCli

// ============================================================================
// SECTION 2: SYSTEM CLI MODULE (namespace SystemCli)
// ============================================================================

namespace SystemCli {

void printSystemOverview(AppendBuf &out) {
  uint32_t ts = millis() / 1000;
  time_t now = time(nullptr);
  struct tm timeinfo;
  char time_str[64];
  const char *time_src = "System RTC";
  if (now > 1672531200) {
    localtime_r(&now, &timeinfo);
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &timeinfo);
    time_src = "NTP Synced";
  } else {
    snprintf(time_str, sizeof(time_str), "Uptime: %ud %02uh %02um %02us",
             ts / 86400, (ts % 86400) / 3600, (ts % 3600) / 60, ts % 60);
    time_src = "Unsynchronized";
  }

  auto make_ascii_bar = [](char *b, size_t sz, uint32_t p) {
    if (sz < 14)
      return;
    b[0] = '[';
    for (int i = 1; i <= 10; ++i)
      b[i] = (p >= i * 10) ? '#' : '.';
    b[11] = ']';
    b[12] = ' ';
    b[13] = '\0';
  };

  uint32_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024;
  uint32_t min_free_heap =
      heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT) / 1024;
  uint32_t total_heap = heap_caps_get_total_size(MALLOC_CAP_8BIT) / 1024;
  uint32_t heap_free_pct =
      (total_heap > 0) ? (free_heap * 100 / total_heap) : 0;

  uint32_t sketch_size = ESP.getSketchSize() / 1024;
  uint32_t flash_size = ESP.getFlashChipSize() / 1024;
  uint32_t flash_used_pct =
      (flash_size > 0) ? (sketch_size * 100 / flash_size) : 0;

  char heap_bar[16], flash_bar[16];
  make_ascii_bar(heap_bar, sizeof(heap_bar), heap_free_pct);
  make_ascii_bar(flash_bar, sizeof(flash_bar), flash_used_pct);

  auto *active = WallpadParserFactory::getActiveParser();
  auto desc = g_auto_probing_engine.getDescriptor();
  char wp_status_buf[80];
  if (g_config.wallpad_profile == 0) {
    if (desc.is_locked) {
      snprintf(wp_status_buf, sizeof(wp_status_buf), "Auto [STX 0x%02X ETX 0x%02X / %s] (ID: 0) [LOCKED]",
               desc.stx, desc.etx, AutoProbingEngine::getAlgoName(desc.checksum_algo));
    } else {
      snprintf(wp_status_buf, sizeof(wp_status_buf), "Auto (Learning...) (ID: 0) [LEARNING]");
    }
  } else {
    VendorProfileDescriptor cur_p;
    if (ProfileRepository::getActiveProfile(cur_p)) {
      snprintf(wp_status_buf, sizeof(wp_status_buf), "%s (ID: %u) [SAVED]",
               cur_p.key, static_cast<unsigned>(g_config.wallpad_profile));
    } else {
      snprintf(wp_status_buf, sizeof(wp_status_buf), "%s (ID: %u)",
               active ? active->getVendorName() : "Custom", static_cast<unsigned>(g_config.wallpad_profile));
    }
  }

  out.appendFormat("\r\n==========================================================="
      "=====================\r\n"
      "                    GATEWAY BRIDGE SYSTEM & TRAFFIC METRICS   "
      "                \r\n"
      "==============================================================="
      "=================\r\n"
      "Firmware        : %s\r\n"
      "Wallpad Profile : %s\r\n"
      "System Time     : %s (%s)\r\n"
      "Uptime          : %ud %02uh %02um %02us\r\n"
      "WiFi Connection : %s (%d dBm, IP: %s) [STABLE]\r\n"
      "Heap Memory     : %s %3u%% Free (Free %uKB / Min %uKB)\r\n"
      "Flash Storage   : %s %3u%% Used (%uKB / %uMB)\r\n",
      Config::FIRMWARE_VERSION, wp_status_buf, time_str, time_src, ts / 86400,
      (ts % 86400) / 3600, (ts % 3600) / 60, ts % 60,
      WiFi.isConnected() ? "Connected" : "Disconnected", WiFi.RSSI(),
      WiFi.localIP().toString().c_str(), heap_bar, heap_free_pct, free_heap,
      min_free_heap, flash_bar, flash_used_pct, sketch_size, flash_size / 1024);
}

void printStats(int sock) {
  static std::atomic<bool> s_busy{false};
  if (s_busy.exchange(true, std::memory_order_acquire)) {
    sendTelnetMsg(sock, "[BUSY] Stats is being generated.\r\n");
    return;
  }

  s_cli_scratch_buf[0] = '\0';
  AppendBuf out{s_cli_scratch_buf, sizeof(s_cli_scratch_buf)};

  printSystemOverview(out);

  g_wdt_monitor.feed(5);
  SysSnapshot sys_snap;
  HwSnapshot hw_snap;
  StackSnapshot stack_snap;
  PktSnapshot pkt_snap;
  System_TakeSnapshot(sys_snap, hw_snap, stack_snap, pkt_snap);

  Fmt::FormatHwMetrics(out, hw_snap);
  Fmt::FormatNetworkStats(out, pkt_snap);
  Fmt::FormatRs485Stats(out, pkt_snap);

  out.append(Fmt::DIV80);
  out.appendFormat("%-55s %24s\r\n", "Metric / Event", "Value / Counter / Status");
  out.append(Fmt::DIV80);
  out.appendFormat("%-55s %24u\r\n"
                   "%-55s %24u\r\n"
                   "%-55s %24u\r\n"
                   "%-55s %24u\r\n",
                   "Total Device Polls",
                   static_cast<unsigned>(g_ch1_state_metrics.poll_cnt.load(std::memory_order_relaxed)),
                   "VIP Controls (SmartThings App)",
                   static_cast<unsigned>(g_ch1_state_metrics.vip_cnt.load(std::memory_order_relaxed)),
                   "Normal Controls (Wallpad)",
                   static_cast<unsigned>(g_ch1_state_metrics.normal_cnt.load(std::memory_order_relaxed)),
                   "Stale Emerg Polls",
                   static_cast<unsigned>(g_ch1_state_metrics.stale_poll_cnt.load(std::memory_order_relaxed)));

  Fmt::FormatTaskStacks(out, stack_snap, g_wdt_monitor);
  out.append("================================================================================\r\n\r\n");

  sendTelnetMsgLen(sock, out.buf, out.offset);
  s_busy.store(false, std::memory_order_release);
}

void cmdStats(EmbeddedCli *cli, char *args, void *context) {
  int client = getSock(context);
  int argc = embeddedCliGetTokenCount(args);

  if (argc > 0) {
    const char *sub = embeddedCliGetToken(args, 1);
    if (strcasecmp(sub, "clear") == 0) {
      g_pkt_stats.resetAll();
      g_polling_targets.resetHits();
      g_metrics.reset();
      sendTelnetMsg(client, "All traffic statistics, hits, and metrics history CLEARED to 0.\r\n");
      return;
    }
    sendTelnetMsg(client, "Usage: stats [clear]\r\n");
    return;
  }
  printStats(client);
}

void cmdReboot(EmbeddedCli *cli, char *args, void *context) {
  int client = getSock(context);
  sendTelnetMsg(client, "Rebooting...\r\n");
  g_restart_reason = "Telnet Command";
  g_restart_pending.store(true, std::memory_order_release);
}

void cmdLogView(EmbeddedCli *cli, char *args, void *context) {
  int client = getSock(context);
  const char *sub_cmd = (embeddedCliGetTokenCount(args) > 0)
                            ? embeddedCliGetToken(args, 1)
                            : "list";

  if (strcasecmp(sub_cmd, "clear") == 0) {
    LogManager::clearRebootLog();
    sendTelnetMsg(client, "Reboot log history CLEARED from NVS flash.\r\n");
    return;
  }

  size_t count = LogManager::getLogCount();
  if (count == 0) {
    sendTelnetMsg(client, "\r\n[LOGVIEW] No persistent reboot logs found in NVS.\r\n");
    return;
  }

  if (strcasecmp(sub_cmd, "list") == 0) {
    s_cli_scratch_buf[0] = '\0';
    AppendBuf out{s_cli_scratch_buf, sizeof(s_cli_scratch_buf)};

    out.append("\r\n");
    out.append(Fmt::DIV80EQ);
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "Persistent Reboot Log History (Total: %u / %u)",
             static_cast<unsigned>(count), static_cast<unsigned>(LogManager::MAX_LOG_ENTRIES));
    out.appendFormat("%*s%s\r\n", std::max(0, (80 - static_cast<int>(strlen(hdr))) / 2), "", hdr);
    out.append(Fmt::DIV80EQ);

    for (size_t i = 0; i < count; i++) {
      LogEntry entry;
      if (LogManager::getLogEntry(i, entry)) {
        char time_buf[32] = "N/A";
        if (entry.timestamp > 0) {
          struct tm timeinfo;
          time_t sec = static_cast<time_t>(entry.timestamp);
          localtime_r(&sec, &timeinfo);
          if (timeinfo.tm_year >= 124) {
            strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
          } else {
            snprintf(time_buf, sizeof(time_buf),
                     "%04d-%02d-%02d %02d:%02d:%02d", timeinfo.tm_year + 1900,
                     timeinfo.tm_mon + 1, timeinfo.tm_mday, timeinfo.tm_hour,
                     timeinfo.tm_min, timeinfo.tm_sec);
          }
        }
        uint32_t sec = entry.stats_snapshot.uptime_ms / 1000;
        out.appendFormat("  [#%-2u] %s | Reason: %-28s | Up: %02uh %02um\r\n",
                         static_cast<unsigned>(i + 1), time_buf, entry.reason, sec / 3600,
                         (sec % 3600) / 60);
      }
    }
    out.append(Fmt::DIV80);
    out.append("\r\n");
    sendTelnetMsgLen(client, out.buf, out.offset);
    return;
  }

  size_t target_idx = 0;
  if (strcasecmp(sub_cmd, "last") == 0) {
    target_idx = 0;
  } else {
    char *endp = nullptr;
    long val = strtol(sub_cmd, &endp, 10);
    if (endp != sub_cmd && *endp == '\0' && val >= 1 && static_cast<size_t>(val) <= count) {
      target_idx = static_cast<size_t>(val - 1);
    } else {
      sendTelnetMsg(client, "Usage: logview [list | <1-20> | last | clear]\r\n");
      return;
    }
  }

  LogManager::readRebootLog(s_cli_scratch_buf, sizeof(s_cli_scratch_buf), target_idx);
  sendTelnetMsg(client, s_cli_scratch_buf);
}

void cmdCoreDump(EmbeddedCli *cli, char *args, void *context) {
  int client = getSock(context);
  if (embeddedCliGetTokenCount(args) >= 1 &&
      strcasecmp(embeddedCliGetToken(args, 1), "clear") == 0) {
    esp_core_dump_image_erase();
    sendTelnetMsg(client, "Crash core dump partition successfully ERASED.\r\n");
    return;
  }

  esp_core_dump_summary_t summary;
  esp_err_t err = esp_core_dump_get_summary(&summary);

  if (err != ESP_OK) {
    sendTelnetMsg(client,
                  "\r\n[COREDUMP] No crash core dump summary available (Partition clean or empty).\r\n");
    return;
  }

  char buf[2048];
  int pos = 0;

  pos += snprintf(
      buf + pos, sizeof(buf) - pos,
      "\r\n========================================================================"
      "========\r\n"
      "                    CRASH CORE DUMP BACKTRACE SUMMARY                   "
      "       \r\n"
      "========================================================================"
      "========\r\n"
      "Status          : Valid Core Dump Found\r\n"
      "Crashed Task    : %s\r\n"
      "Program Counter : 0x%08X\r\n"
      "Exception Cause : %lu\r\n"
      "Backtrace Depth : %d frames%s\r\n"
      "Backtrace PCs   :\r\n",
      summary.exc_task, static_cast<unsigned>(summary.exc_pc), static_cast<unsigned long>(summary.ex_info.exc_cause),
      summary.exc_bt_info.depth,
      summary.exc_bt_info.corrupted ? " (CORRUPTED)" : "");

  for (int i = 0; i < summary.exc_bt_info.depth && pos < static_cast<int>(sizeof(buf)) - 64;
       ++i) {
    pos += snprintf(buf + pos, sizeof(buf) - pos, "  [%2d] 0x%08X\r\n", i,
                    static_cast<unsigned>(summary.exc_bt_info.bt[i]));
  }

  pos += snprintf(
      buf + pos, sizeof(buf) - pos,
      "\r\n===================================================================="
      "============\r\n"
      "Use: xtensa-esp32s3-elf-addr2line -pfiaC -e firmware.elf <PC>\r\n"
      "========================================================================"
      "========\r\n\r\n");
  sendTelnetMsg(client, buf);
}

void otaPrintStatus(AppendBuf &out) {
  const esp_partition_t *running = esp_ota_get_running_partition();
  const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
  esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
  if (running) {
    esp_ota_get_state_partition(running, &ota_state);
  }

  const char *state_desc = "Confirmed";
  const char *state_status = "[STABLE]";
  switch (ota_state) {
  case ESP_OTA_IMG_NEW:
    state_desc = "New Image (First Boot)";
    state_status = "[NEW]";
    break;
  case ESP_OTA_IMG_PENDING_VERIFY:
    state_desc = "Evaluating (Rollback Active)";
    state_status = "[PENDING]";
    break;
  case ESP_OTA_IMG_VALID:
    state_desc = "Confirmed";
    state_status = "[STABLE]";
    break;
  case ESP_OTA_IMG_INVALID:
    state_desc = "Invalidated Image";
    state_status = "[INVALID]";
    break;
  case ESP_OTA_IMG_ABORTED:
    state_desc = "Aborted Image";
    state_status = "[ABORTED]";
    break;
  default:
    break;
  }

  char run_val[36], next_val[36], timer_val[36], crash_val[36];
  snprintf(run_val, sizeof(run_val), "%s (0x%06X, %u KB)",
           running ? running->label : "app0",
           running ? static_cast<unsigned>(running->address) : 0x10000,
           running ? static_cast<unsigned>(running->size / 1024) : 3712);

  snprintf(next_val, sizeof(next_val), "%s (0x%06X, %u KB)",
           next ? next->label : "app1",
           next ? static_cast<unsigned>(next->address) : 0x3B0000,
           next ? static_cast<unsigned>(next->size / 1024) : 3712);

  bool val_done = TimeUtils::isElapsed(g_boot_start_ms, Config::Timing::OTA_VALIDATION_PERIOD_MS);
  snprintf(timer_val, sizeof(timer_val), "%s", val_done ? "120s Passed" : "Evaluating (<120s)");
  snprintf(crash_val, sizeof(crash_val), "%u Consecutive Crashes", static_cast<unsigned>(rtc_crash_counter));

  bool is_rescue = g_rescue_mode.load(std::memory_order_relaxed);

  out.append("\r\n");
  out.append(Fmt::DIV80EQ);
  out.append("                    DUAL-PARTITION OTA & ROLLBACK MONITOR                     \r\n");
  out.append(Fmt::DIV80EQ);
  out.append("Category            Parameter       Value / Target                        Status\r\n");
  out.append(Fmt::DIV80);

  out.appendFormat("%-20s%-16s%-32s%12s\r\n", "Running App",    "Partition",     run_val,             "[ACTIVE]");
  out.appendFormat("%-20s%-16s%-32s%12s\r\n", "",               "State",         state_desc,          state_status);
  out.append(Fmt::DIV80);

  esp_ota_img_states_t next_state = ESP_OTA_IMG_UNDEFINED;
  if (next) {
    esp_ota_get_state_partition(next, &next_state);
  }
  const char *next_desc = "Hardware Dual-Slot";
  const char *next_status = "[READY]";
  if (next_state == ESP_OTA_IMG_INVALID) {
    next_desc = "Invalidated (Failed Boot)";
    next_status = "[INVALID]";
  } else if (next_state == ESP_OTA_IMG_ABORTED) {
    next_desc = "Aborted Image";
    next_status = "[ABORTED]";
  }

  out.appendFormat("%-20s%-16s%-32s%12s\r\n", "Backup Target",  "Partition",     next_val,            "[STANDBY]");
  out.appendFormat("%-20s%-16s%-32s%12s\r\n", "",               "Rollback",      next_desc,           next_status);
  out.append(Fmt::DIV80);

  out.appendFormat("%-20s%-16s%-32s%12s\r\n", "Safety Guard",   "Health Timer",  timer_val,           val_done ? "[STABLE]" : "[TESTING]");
  out.appendFormat("%-20s%-16s%-32s%12s\r\n", "",               "Crash Loop",    crash_val,           rtc_crash_counter == 0 ? "[STABLE]" : "[WARNING]");
  out.appendFormat("%-20s%-16s%-32s%12s\r\n", "",               "Rescue Mode",   is_rescue ? "Forced Safe SoftAP" : "Standard Boot", is_rescue ? "[RESCUE]" : "[STABLE]");
  out.append(Fmt::DIV80EQ);
  out.append("\r\n");
}

void otaTriggerRollback(int sock) {
  sendTelnetMsg(sock, "[OTA] Invalidating current app and triggering hardware rollback to previous firmware...\r\n");
  vTaskDelay(pdMS_TO_TICKS(100));
  esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
  if (err != ESP_OK) {
    char err_buf[64];
    snprintf(err_buf, sizeof(err_buf), "[ERROR] Rollback failed (No rollback partition available, err=0x%x)\r\n", err);
    sendTelnetMsg(sock, err_buf);
  }
}

void otaValidate(int sock) {
  esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  if (err == ESP_OK) {
    sendTelnetMsg(sock, "[OTA] Current firmware manually confirmed as VALID. Auto-rollback cancelled.\r\n");
  } else {
    char err_buf[64];
    snprintf(err_buf, sizeof(err_buf), "[ERROR] Failed to mark app valid: 0x%x\r\n", err);
    sendTelnetMsg(sock, err_buf);
  }
}

void cmdOta(EmbeddedCli *cli, char *args, void *context) {
  int sock = getSock(context);
  uint8_t count = embeddedCliGetTokenCount(args);
  const char *subCmd = (count > 0) ? embeddedCliGetToken(args, 1) : "status";

  if (strcasecmp(subCmd, "status") == 0) {
    s_cli_scratch_buf[0] = '\0';
    AppendBuf out{s_cli_scratch_buf, sizeof(s_cli_scratch_buf)};
    otaPrintStatus(out);
    sendTelnetMsgLen(sock, out.buf, out.offset);
  } else if (strcasecmp(subCmd, "rollback") == 0) {
    otaTriggerRollback(sock);
  } else if (strcasecmp(subCmd, "validate") == 0) {
    otaValidate(sock);
  } else if (strcasecmp(subCmd, "cloud") == 0) {
    const char *url = (count >= 2) ? embeddedCliGetToken(args, 2) : nullptr;
    sendTelnetMsg(sock, "[OTA] Initiating GitHub Cloud HTTP(S) OTA in background...\r\n");
    Mgmt_StartHttpOta(url);
  } else {
    sendTelnetMsg(sock, "Usage: ota [status|rollback|validate|cloud [url]]\r\n");
  }
}

void cmdHelp(EmbeddedCli *cli, char *args, void *context) {
  int client = getSock(context);

  s_cli_scratch_buf[0] = '\0';
  AppendBuf out{s_cli_scratch_buf, sizeof(s_cli_scratch_buf)};

  out.append("\r\n");
  out.append(Fmt::DIV80EQ);
  out.append("                       GATEWAY BRIDGE COMMAND REFERENCE                       \r\n");
  out.append(Fmt::DIV80EQ);
  out.append("Command / Parameter               Description & Usage Example\r\n");
  out.append(Fmt::DIV80);

  // 1. System & Diagnostics
  out.append(" [ SYSTEM & DIAGNOSTICS ]\r\n");
  out.append("  stats [clear]                   Show real-time HW metrics & traffic stats (or reset)\r\n");
  out.append("  devs [1|2|clear]                Show device registry & cache (1: T1 Targets, 2: T2 Cache)\r\n");
  out.append("  logview [list|<1-20>|last|clear] View persistent reboot log history from NVS\r\n");
  out.append("  coredump [clear]                View crash core dump summary & backtrace\r\n");
  out.append("  ota [status|rollback|validate]  Dual-partition OTA & auto-rollback management\r\n");
  out.append("  reboot                          Safely commit buffers and reboot gateway hardware\r\n");
  out.append(Fmt::DIV80);

  // 2. RS-485 & Protocol
  out.append(" [ PROTOCOL & PROBING ]\r\n");
  out.append("  wallpad [status]                Show wallpad profile learning status & parameters\r\n");
  out.append("  wallpad list                    List available vendor & saved NVS custom profiles\r\n");
  out.append("  wallpad set <key|id>            Manually switch active wallpad profile\r\n");
  out.append("  wallpad save <name>             Save current auto-learned profile to NVS slot\r\n");
  out.append("  wallpad delete <id>             Reset a saved custom profile slot in NVS\r\n");
  out.append("  wallpad auto                    Switch to Universal Auto-Probing mode\r\n");
  out.append("  wallpad reset                   Reset auto-probing engine and re-learn bus traffic\r\n");
  out.append("  trace [on|off|ctl|ack|pol|...]  Live packet stream monitoring with filters\r\n");
  out.append("  q                               Shortcut to stop live tracing immediately\r\n");
  out.append(Fmt::DIV80);

  // 3. Network & Configuration
  out.append(" [ NETWORK & CONFIG ]\r\n");
  out.append("  wifi [status]                   Show Wi-Fi STA connection status & signal strength\r\n");
  out.append("  wifi scan                       Scan surrounding 2.4GHz Wi-Fi AP networks\r\n");
  out.append("  wifi connect <ssid> [password]  Connect to target Wi-Fi AP network\r\n");
  out.append("  wifi disconnect                 Disconnect current Wi-Fi station\r\n");
  out.append("  config                          View all runtime configuration parameters\r\n");
  out.append("  config set <key> <val>          Modify a configuration parameter (runtime)\r\n");
  out.append("  config reset                    Reset runtime configuration to system defaults\r\n");
  out.append("  save                            Commit and save all configuration to NVS flash\r\n");
  out.append("  exit                            Disconnect current Telnet CLI session\r\n");
  out.append(Fmt::DIV80EQ);
  out.append("\r\n");

  sendTelnetMsgLen(client, out.buf, out.offset);
}

} // namespace SystemCli

// ============================================================================
// SECTION 3: WALLPAD & RS-485 CLI MODULE (namespace WallpadCli)
// ============================================================================

namespace WallpadCli {

void cmdTrace(EmbeddedCli *cli, char *args, void *context) {
  int sock = getSock(context);
  int token_count = embeddedCliGetTokenCount(args);
  const char *sub = (token_count > 0) ? embeddedCliGetToken(args, 1) : "on";

  if (strcasecmp(sub, "off") == 0) {
    g_telnet_tracer.setTrace(false);
    sendTelnetMsg(sock, "Packet trace DISABLED.\r\n");
    return;
  }

  g_telnet_tracer.setClient(sock);
  g_telnet_tracer.setTrace(true);

  if (strcasecmp(sub, "on") == 0) {
    g_telnet_tracer.setFilter(TraceType::ALL);
    sendTelnetMsg(sock, "Packet trace ENABLED: ALL packets.\r\n");
  } else if (strcasecmp(sub, "ctl") == 0) {
    g_telnet_tracer.setFilter(TraceType::CTL);
    sendTelnetMsg(sock, "Packet trace ENABLED: CONTROL packets only.\r\n");
  } else if (strcasecmp(sub, "ack") == 0) {
    g_telnet_tracer.setFilter(TraceType::ACK);
    sendTelnetMsg(sock, "Packet trace ENABLED: ACK/Response packets only.\r\n");
  } else if (strcasecmp(sub, "pol") == 0) {
    g_telnet_tracer.setFilter(TraceType::QRY);
    sendTelnetMsg(sock, "Packet trace ENABLED: Polling queries only.\r\n");
  } else if (strcasecmp(sub, "rmt") == 0) {
    g_telnet_tracer.setFilter(TraceType::RMT);
    sendTelnetMsg(sock, "Packet trace ENABLED: Doorphone packets only.\r\n");
  } else if (strcasecmp(sub, "drp") == 0) {
    g_telnet_tracer.setFilter(TraceType::DRP);
    sendTelnetMsg(sock, "Packet trace ENABLED: Dropped packets only.\r\n");
  } else if (strcasecmp(sub, "ch") == 0 || (strncasecmp(sub, "ch", 2) == 0 && isdigit(static_cast<unsigned char>(sub[2])))) {
    uint8_t ch = 0;
    if (strcasecmp(sub, "ch") == 0 && token_count >= 2) {
      ch = static_cast<uint8_t>(atoi(embeddedCliGetToken(args, 2)));
    } else if (strncasecmp(sub, "ch", 2) == 0 && isdigit(static_cast<unsigned char>(sub[2]))) {
      ch = static_cast<uint8_t>(sub[2] - '0');
    }
    if (ch >= 1 && ch <= 6) {
      g_telnet_tracer.setFilter(TraceType::CH, ch);
      sendTelnetMsgf(sock, "Packet trace ENABLED: Channel %u only.\r\n", ch);
    } else {
      sendTelnetMsg(sock, "[ERROR] Usage: trace ch <1-6>\r\n");
    }
  } else if (strcasecmp(sub, "devid") == 0 || strncasecmp(sub, "0x", 2) == 0) {
    uint8_t id = 0;
    if (strcasecmp(sub, "devid") == 0 && token_count >= 2) {
      id = static_cast<uint8_t>(strtol(embeddedCliGetToken(args, 2), nullptr, 16));
    } else if (strncasecmp(sub, "0x", 2) == 0) {
      id = static_cast<uint8_t>(strtol(sub, nullptr, 16));
    }
    g_telnet_tracer.setFilter(TraceType::DEVID, id);
    sendTelnetMsgf(sock, "Packet trace ENABLED: Device ID 0x%02X only.\r\n", id);
  } else {
    sendTelnetMsg(sock, "Usage: trace [on | off | ctl | ack | pol | rmt | drp | ch <1-6> | devid <hex>]\r\n");
  }
}

void cmdStop(EmbeddedCli *cli, char *args, void *context) {
  int sock = getSock(context);
  g_telnet_tracer.setTrace(false);
  sendTelnetMsg(sock, "Packet trace DISABLED.\r\n");
}

static void formatSources(uint8_t src_mask, char *buf, size_t buf_len) {
  size_t idx = 0;
  if (src_mask & (1 << 2)) {
    idx += snprintf(buf + idx, buf_len - idx, "CH2");
  }
  if (src_mask & (1 << 3)) {
    if (idx > 0 && idx < buf_len)
      idx += snprintf(buf + idx, buf_len - idx, "+");
    idx += snprintf(buf + idx, buf_len - idx, "CH3");
  }
  if (src_mask & (1 << 6)) {
    if (idx > 0 && idx < buf_len)
      idx += snprintf(buf + idx, buf_len - idx, "+");
    idx += snprintf(buf + idx, buf_len - idx, "CH6");
  }
  if (idx == 0) {
    snprintf(buf, buf_len, "None");
  }
}

void devsPrintTier1Targets(AppendBuf &out, uint32_t now) {
  g_polling_targets.sweepExpired(Config::Timing::STALE_DEVICE_THRESHOLD_MS);
  size_t tgt_total = g_polling_targets.totalCount();
  size_t tgt_active = g_polling_targets.activeCount();

  const char *wc_src_str = (g_warm_cache_source == 1) ? "RTC SRAM" : (g_warm_cache_source == 2) ? "NVS Flash" : "Cold Start";

  out.append("\r\n");
  out.append(Fmt::DIV80EQ);
  out.append("          [1st-Tier Cache] Dynamic Polling Target Registry (Wallpad/App)      \r\n");
  out.append(Fmt::DIV80EQ);
  out.appendFormat("  Active Polling Targets: %zu | Total Tracked: %zu | Warm Cache: %s (%u)\r\n",
                   tgt_active, tgt_total, wc_src_str, static_cast<unsigned>(g_warm_cache_restored_count));
  out.append(Fmt::DIV80);
  out.append("No  Status   Last   Sources  Raw Query Frame (Template)\r\n");
  out.append(Fmt::DIV80);

  if (tgt_total == 0) {
    out.append("  (No polling targets registered yet. Waiting for Wallpad/App queries...)\r\n");
  } else {
    for (size_t i = 0; i < tgt_total; ++i) {
      PollingTargetEntry tgt;
      if (!g_polling_targets.getEntry(i, tgt))
        continue;

      char src_buf[32] = {0};
      formatSources(tgt.source_channels, src_buf, sizeof(src_buf));

      char last_req_str[16] = {0};
      Fmt::FormatElapsed(now, tgt.last_requested_ms, last_req_str, sizeof(last_req_str));

      char q_hex[64] = {0};
      if (tgt.raw_query_len > 0) {
        Fmt::FormatHex(tgt.raw_query_data.data(), tgt.raw_query_len, q_hex, sizeof(q_hex));
      } else {
        snprintf(q_hex, sizeof(q_hex), "[ %02X : %02X : %02X ]", tgt.dev_id,
                 tgt.sub1, tgt.sub2);
      }

      const char *status_str = !tgt.is_active ? "OFFLINE" : (!tgt.is_verified ? "UNVERIF" : "ONLINE");

      out.appendFormat(
          "%02u  %-7s  %-5s  %-7s  %s\r\n",
          (unsigned int)(i + 1), status_str, last_req_str,
          src_buf, q_hex);
    }
  }
  out.append(Fmt::DIV80);
}

void devsPrintTier2Cache(AppendBuf &out, uint32_t now) {
  size_t total_count = g_device_repo.count();
  size_t online_count = g_device_repo.getOnlineCount();
  size_t tgt_total = g_polling_targets.totalCount();

  out.append("\r\n");
  out.append(Fmt::DIV80EQ);
  out.append("          [2nd-Tier Cache] Physical Device State & Health Monitor             \r\n");
  out.append(Fmt::DIV80EQ);
  out.appendFormat(
      "  Discovered Devices: %zu Nodes on Bus | Online [OK]: %zu | Offline: %zu\r\n",
      total_count, online_count, (total_count >= online_count) ? (total_count - online_count) : 0);
  out.append(Fmt::DIV80);
  out.append("No  Status   Last   Raw Physical ACK (Response Frame)\r\n");
  out.append(Fmt::DIV80);

  if (tgt_total == 0 && total_count == 0) {
    out.append("  (No physical devices discovered on RS-485 bus yet)\r\n");
  } else if (tgt_total > 0) {
    for (size_t i = 0; i < tgt_total; ++i) {
      PollingTargetEntry tgt;
      if (!g_polling_targets.getEntry(i, tgt))
        continue;

      const auto *dev = g_device_repo.find(tgt.dev_id, tgt.sub1, tgt.sub2);

      char ack_hex[96] = {0};
      char updated_str[16] = "-";
      const char *status_str = "WAITING";

      if (dev && dev->last_ack_len > 0) {
        Fmt::FormatHex(dev->last_ack_data.data(), dev->last_ack_len, ack_hex, sizeof(ack_hex));
        if (dev->last_updated_ms > 0) {
          Fmt::FormatElapsed(now, dev->last_updated_ms, updated_str, sizeof(updated_str));
        }
        status_str = dev->is_online ? "ONLINE" : "OFFLINE";
      } else {
        snprintf(ack_hex, sizeof(ack_hex), "(No ACK received from bus yet)");
      }

      out.appendFormat("%02u  %-7s  %-5s  %s\r\n",
                       (unsigned int)(i + 1), status_str, updated_str, ack_hex);
    }
  } else {
    for (size_t i = 0; i < total_count; ++i) {
      DeviceStateEntry dev;
      if (!g_device_repo.getSnapshot(i, dev))
        continue;

      char ack_hex[96] = {0};
      if (dev.last_ack_len > 0) {
        Fmt::FormatHex(dev.last_ack_data.data(), dev.last_ack_len, ack_hex, sizeof(ack_hex));
      } else {
        snprintf(ack_hex, sizeof(ack_hex), "No ACK Cached");
      }

      char updated_str[16] = "-";
      if (dev.last_updated_ms > 0) {
        Fmt::FormatElapsed(now, dev.last_updated_ms, updated_str, sizeof(updated_str));
      }

      const char *status_str = dev.is_online ? "ONLINE" : "OFFLINE";

      out.appendFormat("%02u  %-7s  %-5s  %s\r\n",
                       (unsigned int)(i + 1), status_str, updated_str, ack_hex);
    }
  }
  out.append(Fmt::DIV80);
  out.append(Fmt::DIV80EQ);
  out.append("\r\n");
}

void cmdDevs(EmbeddedCli *cli, char *args, void *context) {
  int client = getSock(context);
  uint32_t now = millis();
  int argc = embeddedCliGetTokenCount(args);

  bool show_tier1 = true;
  bool show_tier2 = true;

  if (argc > 0) {
    const char *sub = embeddedCliGetToken(args, 1);
    if (strcasecmp(sub, "1") == 0) {
      show_tier1 = true;
      show_tier2 = false;
    } else if (strcasecmp(sub, "2") == 0) {
      show_tier1 = false;
      show_tier2 = true;
    } else if (strcasecmp(sub, "clear") == 0) {
      g_polling_targets.clear();
      g_device_repo.clear();
      sendTelnetMsg(client, "All 1st-tier & 2nd-tier device caches CLEARED.\r\n");
      return;
    } else {
      sendTelnetMsg(client, "Usage: devs [1 | 2 | clear]\r\n");
      return;
    }
  }

  s_cli_scratch_buf[0] = '\0';
  AppendBuf out{s_cli_scratch_buf, sizeof(s_cli_scratch_buf)};

  if (show_tier1) {
    devsPrintTier1Targets(out, now);
  }
  if (show_tier2) {
    devsPrintTier2Cache(out, now);
  }

  sendTelnetMsgLen(client, out.buf, out.offset);
}

void wallpadPrintStatus(AppendBuf &out) {
  auto *active = WallpadParserFactory::getActiveParser();
  auto desc = g_auto_probing_engine.getDescriptor();
  size_t active_targets = g_polling_targets.activeCount();
  size_t verified_targets = g_polling_targets.verifiedCount();
  size_t online_devs = g_device_repo.getOnlineCount();

  const char *phase_str = "Phase 1/3 (Framing Probing)";
  if (desc.offsets_locked) {
    phase_str = "Phase 3/3: Fully Locked";
  } else if (desc.is_locked) {
    phase_str = "Phase 2/3: Cache Syncing";
  }

  out.append("\r\n");
  out.append(Fmt::DIV80EQ);
  out.append("                    WALLPAD PROTOCOL & PROFILE DIAGNOSTICS                   \r\n");
  out.append(Fmt::DIV80EQ);
  out.appendFormat("Active Profile  : %s (ID: %u)\r\n",
                   active ? active->getProfileKey() : "Standard",
                   static_cast<unsigned>(g_config.wallpad_profile));
  if (g_config.wallpad_profile == static_cast<uint8_t>(WallpadProfileIndex::ADAPTIVE)) {
    out.appendFormat("Profile Mode    : Auto Adaptive [%s]\r\n", phase_str);
  } else {
    out.appendFormat("Profile Mode    : Manual Fixed\r\n");
  }
  out.appendFormat("Devices Tracked : %u Active / %u Online (%u Offline) [%s]\r\n",
                   static_cast<unsigned>(active_targets), static_cast<unsigned>(online_devs),
                   static_cast<unsigned>(g_device_repo.count() >= online_devs ? (g_device_repo.count() - online_devs) : 0),
                   (online_devs >= active_targets && active_targets > 0) ? "100% Synced" : "Syncing");
  out.append(Fmt::DIV80);
  out.append("Packet Field    Parameter       Value / Layout Rule                       Status\r\n");
  out.append(Fmt::DIV80);

  // 1. Header (STX, LEN)
  char stx_buf[16], etx_buf[16], len_buf[32], pkt_len_buf[32];
  snprintf(stx_buf, sizeof(stx_buf), "%02X", active ? active->getStx() : 0xF7);
  snprintf(etx_buf, sizeof(etx_buf), "%02X", active ? active->getEtx() : 0xEE);
  snprintf(len_buf, sizeof(len_buf), "Byte #1");

  size_t total_tgts = g_polling_targets.totalCount();

  uint8_t q_lens[8];
  size_t q_len_cnt = 0;
  for (size_t i = 0; i < total_tgts && q_len_cnt < 8; ++i) {
    PollingTargetEntry entry;
    if (g_polling_targets.getEntry(i, entry) && entry.raw_query_len > 0) {
      if (std::find(q_lens, q_lens + q_len_cnt, entry.raw_query_len) == q_lens + q_len_cnt) {
        q_lens[q_len_cnt++] = entry.raw_query_len;
      }
    }
  }
  std::sort(q_lens, q_lens + q_len_cnt);
  if (q_len_cnt > 0) {
    size_t off = 0;
    for (size_t i = 0; i < q_len_cnt; ++i) {
      off += snprintf(pkt_len_buf + off, sizeof(pkt_len_buf) - off, "%s%u", (i == 0 ? "" : ", "), q_lens[i]);
    }
    snprintf(pkt_len_buf + off, sizeof(pkt_len_buf) - off, " Bytes");
  } else {
    snprintf(pkt_len_buf, sizeof(pkt_len_buf), "11 Bytes");
  }

  char len_rule_buf[48];
  snprintf(len_rule_buf, sizeof(len_rule_buf), "Byte #1 (%s)", pkt_len_buf);

  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "Header", "STX", stx_buf, desc.is_locked ? "[LOCKED]" : "[LEARNING]");
  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "", "Length (LEN)", len_rule_buf, desc.is_locked ? "[LOCKED]" : "[LEARNING]");
  out.append(Fmt::DIV80);

  // 2. Addressing (Address Mode, Device Type, Sub-ID)
  uint8_t dev_ids[16], sub1_ids[16], sub2_ids[16];
  size_t dev_id_cnt = 0, sub1_cnt = 0, sub2_cnt = 0;

  for (size_t i = 0; i < total_tgts; ++i) {
    PollingTargetEntry entry;
    if (g_polling_targets.getEntry(i, entry)) {
      if (dev_id_cnt < 16 && std::find(dev_ids, dev_ids + dev_id_cnt, entry.dev_id) == dev_ids + dev_id_cnt) {
        dev_ids[dev_id_cnt++] = entry.dev_id;
      }
      if (sub1_cnt < 16 && std::find(sub1_ids, sub1_ids + sub1_cnt, entry.sub1) == sub1_ids + sub1_cnt) {
        sub1_ids[sub1_cnt++] = entry.sub1;
      }
      if (sub2_cnt < 16 && std::find(sub2_ids, sub2_ids + sub2_cnt, entry.sub2) == sub2_ids + sub2_cnt) {
        sub2_ids[sub2_cnt++] = entry.sub2;
      }
    }
  }

  std::sort(dev_ids, dev_ids + dev_id_cnt);
  std::sort(sub1_ids, sub1_ids + sub1_cnt);
  std::sort(sub2_ids, sub2_ids + sub2_cnt);

  auto format_hex_list = [](const uint8_t *arr, size_t cnt, const char *prefix, char *out, size_t out_sz) {
    if (cnt == 0) {
      snprintf(out, out_sz, "%s", prefix);
      return;
    }
    char hex_str[64] = {0};
    size_t off = 0;
    for (size_t d = 0; d < cnt; ++d) {
      if (off + 5 >= 24) {
        off += snprintf(hex_str + off, sizeof(hex_str) - off, ", ..");
        break;
      }
      off += snprintf(hex_str + off, sizeof(hex_str) - off, "%s%02X",
                      (d == 0 ? "" : ", "), arr[d]);
    }
    snprintf(out, out_sz, "%s (%s)", prefix, hex_str);
  };

  const char *addr_mode = desc.offsets_locked
                              ? (desc.is_swapped_addr ? "Swapped (DA/SA Swap)" : "Direct (1:1 Direct)")
                              : (desc.is_locked ? "Probing..." : "Waiting");
  const char *addr_status = desc.offsets_locked ? "[LOCKED]" : (desc.is_locked ? "[LEARNING]" : "[WAITING]");

  char dev_off_label[32], sub1_off_label[32], sub2_off_label[32];
  if (desc.offsets_locked) {
    snprintf(dev_off_label, sizeof(dev_off_label), "Byte #%u", desc.dev_id_offset);
    snprintf(sub1_off_label, sizeof(sub1_off_label), "Byte #%u", desc.sub1_offset);
    snprintf(sub2_off_label, sizeof(sub2_off_label), "Byte #%u", desc.sub2_offset);
  } else {
    snprintf(dev_off_label, sizeof(dev_off_label), "Byte #3 (Init)");
    snprintf(sub1_off_label, sizeof(sub1_off_label), "Byte #5 (Init)");
    snprintf(sub2_off_label, sizeof(sub2_off_label), "Byte #6 (Init)");
  }

  char dev_list_buf[64], sub1_list_buf[64], sub2_list_buf[64];
  format_hex_list(dev_ids, dev_id_cnt, dev_off_label, dev_list_buf, sizeof(dev_list_buf));
  format_hex_list(sub1_ids, sub1_cnt, sub1_off_label, sub1_list_buf, sizeof(sub1_list_buf));
  format_hex_list(sub2_ids, sub2_cnt, sub2_off_label, sub2_list_buf, sizeof(sub2_list_buf));

  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "Addressing", "Address Mode", addr_mode, addr_status);
  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "", "Device Type", dev_list_buf, addr_status);
  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "", "Sub-ID", sub2_list_buf, addr_status);
  out.append(Fmt::DIV80);

  // 3. Command (Opcode, Sub-Command)
  char op_line_buf[48];
  char ctl_hex[8];
  if (desc.control_seen && desc.control_opcode != 0) {
    snprintf(ctl_hex, sizeof(ctl_hex), "%02X", desc.control_opcode);
  } else {
    // ★ 제어 패킷 미관측 - 가정값("02") 대신 명시적으로 미확인 표시
    snprintf(ctl_hex, sizeof(ctl_hex), "??");
  }
  snprintf(op_line_buf, sizeof(op_line_buf), "Byte #%u (QRY:%02X, CTL:%s, ACK:%02X)",
           desc.opcode_offset, desc.query_opcode, ctl_hex, desc.ack_opcode);

  // ★ [LOCKED] 상태: QRY/ACK 락 여부 + CTL 관측 여부를 구분하여 표시
  const char *opcode_status;
  if (!desc.opcodes_locked) {
    opcode_status = "[LEARNING]";
  } else if (!desc.control_seen || desc.control_opcode == 0) {
    opcode_status = "[LOCKED/CTL:??]";  // QRY+ACK 확정, CTL은 아직 미관측
  } else {
    opcode_status = "[LOCKED]";         // QRY+CTL+ACK 모두 확정
  }

  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "Command", "Opcode Offset", op_line_buf, opcode_status);
  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "", "Sub-Command", sub1_list_buf, addr_status);
  out.append(Fmt::DIV80);

  // 4. Payload (Data Range, Length Rule)
  char payload_range_buf[48];
  char payload_len_buf[32];
  if (desc.offsets_locked) {
    snprintf(payload_range_buf, sizeof(payload_range_buf), "Byte #%u ~ #(N-3)", desc.payload_offset);
    snprintf(payload_len_buf, sizeof(payload_len_buf), "LEN - %u Bytes", desc.payload_offset + 2);
  } else {
    snprintf(payload_range_buf, sizeof(payload_range_buf), "Byte #7 ~ #(N-3) (Est)");
    snprintf(payload_len_buf, sizeof(payload_len_buf), "LEN - 9 Bytes (Est)");
  }
  const char *payload_status = desc.offsets_locked ? "[LOCKED]" : "[ESTIMATE]";
  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "Payload", "Data Range", payload_range_buf, payload_status);
  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "", "Data Length", payload_len_buf, payload_status);
  out.append(Fmt::DIV80);

  // 5. Tail (Checksum, ETX)
  char cs_algo_buf[48];
  snprintf(cs_algo_buf, sizeof(cs_algo_buf), "Byte #(N-2) (%s)", AutoProbingEngine::getAlgoName(desc.checksum_algo));
  char etx_line_buf[32];
  snprintf(etx_line_buf, sizeof(etx_line_buf), "Byte #(N-1) (%s)", etx_buf);

  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "Tail", "Checksum (CS)", cs_algo_buf, desc.is_locked ? "[LOCKED]" : "[LEARNING]");
  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "", "ETX", etx_line_buf, desc.is_locked ? "[LOCKED]" : "[LEARNING]");
  out.append(Fmt::DIV80);

  // 6. Bus Physical (CH1/CH2/CH3 RS-485 Config)
  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "Bus Physical", "Baudrate", "9600 bps (CH1/CH2/CH3)", "[CONFIG]");
  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "", "IPG Silence", "20 ms (CH1/CH2/CH3)", "[CONFIG]");
  out.append(Fmt::DIV80);

  // 7. Doorphone (CH4 Universal IPG Engine)
  Config::Doorphone::FramingStatus dp_status = g_doorphone_tracker.status.load(std::memory_order_relaxed);
  const char *dp_status_str = (dp_status == Config::Doorphone::FramingStatus::LOCKED)   ? "[LOCKED]"
                            : (dp_status == Config::Doorphone::FramingStatus::LEARNING) ? "[LEARNING]"
                            : (dp_status == Config::Doorphone::FramingStatus::NOISY)    ? "[NOISY]"
                                                                                        : "[WAITING]";

  char dp_frame_buf[48];
  if (dp_status == Config::Doorphone::FramingStatus::WAITING) {
    snprintf(dp_frame_buf, sizeof(dp_frame_buf), "-- .. --");
  } else {
    uint8_t dp_stx = g_doorphone_tracker.candidate_stx.load(std::memory_order_relaxed);
    uint8_t dp_etx = g_doorphone_tracker.candidate_etx.load(std::memory_order_relaxed);
    uint8_t dp_len = g_doorphone_tracker.candidate_len.load(std::memory_order_relaxed);
    if (dp_len > 0) {
      snprintf(dp_frame_buf, sizeof(dp_frame_buf), "%02X .. %02X (%u Bytes)", dp_stx, dp_etx, dp_len);
    } else {
      snprintf(dp_frame_buf, sizeof(dp_frame_buf), "%02X .. %02X", dp_stx, dp_etx);
    }
  }

  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "Doorphone (CH4)", "Framing", dp_frame_buf, dp_status_str);
  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "", "Baudrate", "3840 bps", "[CONFIG]");
  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "", "Time-gap", "25 ms", "[CONFIG]");
  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "", "Debounce", "500 ms", "[CONFIG]");
  out.append(Fmt::DIV80);

  // 8. Runtime Sync & Telemetry
  char conv_buf[48];
  uint32_t conv_pct = active_targets ? (verified_targets * 100 / active_targets) : 0;
  snprintf(conv_buf, sizeof(conv_buf), "%u / %u Targets (%u%%)",
           static_cast<unsigned>(verified_targets), static_cast<unsigned>(active_targets),
           static_cast<unsigned>(conv_pct));
  const char *conv_status = (verified_targets >= active_targets && active_targets > 0)
                                ? "[SYNCED]"
                                : (desc.is_locked ? "[SYNCING]" : "[WAITING]");

  char cs_rate_buf[48];
  uint32_t cs_pct = desc.tested_packets ? (desc.matched_packets * 100 / desc.tested_packets) : 100;
  snprintf(cs_rate_buf, sizeof(cs_rate_buf), "%u / %u Packets (%u%%)",
           static_cast<unsigned>(desc.matched_packets), static_cast<unsigned>(desc.tested_packets),
           static_cast<unsigned>(cs_pct));
  const char *cs_status = (cs_pct >= 95) ? "[STABLE]" : (cs_pct >= 80 ? "[NOISY]" : "[ERROR]");

  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "Runtime Sync", "Cache Sync", conv_buf, conv_status);
  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "", "CS Validation", cs_rate_buf, cs_status);
  out.append(Fmt::DIV80EQ);
  out.append("\r\n");
}

void wallpadListProfiles(AppendBuf &out) {
  out.append("\r\n");
  out.append(Fmt::DIV80EQ);
  out.append("                        WALLPAD PROTOCOL PROFILES                               \r\n");
  out.append(Fmt::DIV80EQ);
  out.append(" ID  | Profile Key | Protocol Specification / Description | Status\r\n");
  out.append(Fmt::DIV80);
  for (size_t i = 0; i < ProfileRepository::getProfileCount(); ++i) {
    VendorProfileDescriptor p_desc;
    if (ProfileRepository::getProfile(i, p_desc)) {
      bool is_current = (g_config.wallpad_profile == i);
      bool is_empty = (i > 0 && strncmp(p_desc.name, "[Empty", 6) == 0);
      out.appendFormat(" %2u  | %-11s | %-36s | %s\r\n",
                       static_cast<unsigned>(i), p_desc.key, p_desc.name,
                       is_current ? ">> ACTIVE <<" : (is_empty ? "Available" : "Saved (NVS)"));
    }
  }
  out.append(Fmt::DIV80EQ);
  out.append("\r\n");
}

void wallpadSaveProfile(int sock, const char *name) {
  if (!name || strlen(name) == 0) {
    sendTelnetMsg(sock, "[ERROR] Usage: wallpad save <profile_name> (e.g. 'wallpad save MyHome')\r\n");
    return;
  }
  size_t saved_slot = 0;
  if (ProfileRepository::saveCurrentAutoAs(name, saved_slot)) {
    sendTelnetMsgf(sock, "[OK] Successfully saved current Auto profile as '%s' (Slot #%u) in NVS!\r\n",
                   name, static_cast<unsigned>(saved_slot));
  } else {
    sendTelnetMsg(sock, "[ERROR] Failed to save profile to NVS.\r\n");
  }
}

void wallpadDeleteProfile(int sock, const char *target) {
  if (!target) {
    sendTelnetMsg(sock, "[ERROR] Usage: wallpad delete <name|id>\r\n");
    return;
  }
  char *endp = nullptr;
  long val = strtol(target, &endp, 10);
  size_t idx = 999;
  if (endp != target && *endp == '\0' && val >= 1 && val < static_cast<long>(ProfileRepository::getProfileCount())) {
    idx = static_cast<size_t>(val);
  } else {
    VendorProfileDescriptor pd;
    for (size_t i = 1; i < ProfileRepository::getProfileCount(); ++i) {
      if (ProfileRepository::getProfile(i, pd) && strcasecmp(pd.key, target) == 0) {
        idx = i;
        break;
      }
    }
  }
  if (idx >= 1 && idx < ProfileRepository::getProfileCount()) {
    ProfileRepository::deleteProfile(idx);
    sendTelnetMsgf(sock, "[OK] Custom profile (Slot #%u) reset to empty.\r\n", static_cast<unsigned>(idx));
  } else {
    sendTelnetMsgf(sock, "[ERROR] Cannot delete '%s' (Slot 0 is protected Auto slot).\r\n", target);
  }
}

void wallpadSetProfile(int sock, const char *key) {
  if (!key) {
    sendTelnetMsg(sock, "[ERROR] Usage: wallpad set <key|id>\r\n");
    return;
  }
  bool ok = false;
  char *endp = nullptr;
  long val = strtol(key, &endp, 10);
  if (endp != key && *endp == '\0' && val >= 0 &&
      val < static_cast<long>(ProfileRepository::getProfileCount())) {
    ok = ProfileRepository::setActiveProfileIndex(static_cast<size_t>(val));
  } else {
    ok = ProfileRepository::setActiveProfileByKey(key);
  }

  if (ok) {
    auto *new_p = WallpadParserFactory::getActiveParser();
    sendTelnetMsgf(sock,
                   "[OK] Wallpad profile changed to '%s' (%s) and saved to NVS.\r\n",
                   new_p ? new_p->getVendorName() : key,
                   new_p ? new_p->getProfileKey() : key);
  } else {
    sendTelnetMsgf(sock,
                   "[ERROR] Unknown vendor profile '%s'. Use 'wallpad list' to see available profiles.\r\n",
                   key);
  }
}

void cmdWallpad(EmbeddedCli *cli, char *args, void *context) {
  int sock = getSock(context);
  int argc = embeddedCliGetTokenCount(args);
  const char *sub = (argc > 0) ? embeddedCliGetToken(args, 1) : "status";

  if (argc == 0 || strcasecmp(sub, "status") == 0) {
    s_cli_scratch_buf[0] = '\0';
    AppendBuf out{s_cli_scratch_buf, sizeof(s_cli_scratch_buf)};
    wallpadPrintStatus(out);
    sendTelnetMsgLen(sock, out.buf, out.offset);
    return;
  }

  if (strcasecmp(sub, "list") == 0) {
    s_cli_scratch_buf[0] = '\0';
    AppendBuf out{s_cli_scratch_buf, sizeof(s_cli_scratch_buf)};
    wallpadListProfiles(out);
    sendTelnetMsgLen(sock, out.buf, out.offset);
  } else if (strcasecmp(sub, "set") == 0) {
    if (argc >= 2) {
      wallpadSetProfile(sock, embeddedCliGetToken(args, 2));
    } else {
      sendTelnetMsg(sock, "[ERROR] Usage: wallpad set <key|id>\r\n");
    }
  } else if (strcasecmp(sub, "save") == 0) {
    if (argc >= 2) {
      wallpadSaveProfile(sock, embeddedCliGetToken(args, 2));
    } else {
      sendTelnetMsg(sock, "[ERROR] Usage: wallpad save <name>\r\n");
    }
  } else if (strcasecmp(sub, "delete") == 0) {
    if (argc >= 2) {
      wallpadDeleteProfile(sock, embeddedCliGetToken(args, 2));
    } else {
      sendTelnetMsg(sock, "[ERROR] Usage: wallpad delete <id>\r\n");
    }
  } else if (strcasecmp(sub, "auto") == 0) {
    ProfileRepository::setActiveProfileIndex(0);
    g_auto_probing_engine.reset();
    g_doorphone_tracker.clearNvs();
    sendTelnetMsg(sock, "[OK] Switched to Universal Auto-Probing mode (Wallpad & Doorphone framing reset).\r\n");
  } else if (strcasecmp(sub, "reset") == 0) {
    g_auto_probing_engine.reset();
    g_doorphone_tracker.clearNvs();
    // ★ Task_Ch1의 수렴 상태(s_convergence_done)를 리셋하여 재수렴·재락 허용
    // g_auto_probing_engine.reset()만으로는 Task 내부 static s_convergence_done이
    // true로 유지되어 analyzeCacheMatrix()가 재호출되지 않는 문제를 수정
    g_probe_convergence_reset.store(true, std::memory_order_release);
    sendTelnetMsg(sock, "[OK] Auto-probing engine & Doorphone framing reset. Re-analyzing RS-485 bus traffic...\r\n");
  } else {
    sendTelnetMsg(sock, "Usage: wallpad [status | list | set <key|id> | save <name> | delete <id> | auto | reset]\r\n");
  }
}

} // namespace WallpadCli

// ============================================================================
// SECTION 4: WIFI CLI MODULE (namespace WifiCli)
// ============================================================================

namespace WifiCli {

static void AsyncWifiScanTask(void *pvParameters) {
  if (!pvParameters) {
    vTaskDelete(nullptr);
    return;
  }
  TelnetManager::WifiScanReq req = *static_cast<TelnetManager::WifiScanReq *>(pvParameters);
  vTaskDelay(pdMS_TO_TICKS(100));

  int n = WiFi.scanNetworks(false, true);

  auto scan_buf = std::make_unique<char[]>(4096);
  scan_buf[0] = '\0';
  AppendBuf out{scan_buf.get(), 4096};

  out.append("\r\n");
  out.append(Fmt::DIV80EQ);
  out.append("                      2.4GHz WI-FI ACCESS POINT SCAN RESULTS                  \r\n");
  out.append(Fmt::DIV80EQ);
  out.append("No   SSID                             RSSI      Channel  Encryption\r\n");
  out.append(Fmt::DIV80);

  if (n == 0) {
    out.append("  (No wireless networks found)\r\n");
  } else if (n < 0) {
    out.append("  [ERROR] Wi-Fi hardware scan failed or timed out.\r\n");
  } else {
    for (int i = 0; i < n; ++i) {
      const char *encType = "Open";
      switch (WiFi.encryptionType(i)) {
      case WIFI_AUTH_WEP: encType = "WEP"; break;
      case WIFI_AUTH_WPA_PSK: encType = "WPA"; break;
      case WIFI_AUTH_WPA2_PSK: encType = "WPA2"; break;
      case WIFI_AUTH_WPA_WPA2_PSK: encType = "WPA/WPA2"; break;
      case WIFI_AUTH_WPA2_ENTERPRISE: encType = "Enterprise"; break;
      case WIFI_AUTH_WPA3_PSK: encType = "WPA3"; break;
      case WIFI_AUTH_WPA2_WPA3_PSK: encType = "WPA2/WPA3"; break;
      default: break;
      }
      out.appendFormat("%02d   %-32s %4d dBm   %3d      %s\r\n", i + 1,
                       WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i),
                       encType);
    }
  }
  out.append(Fmt::DIV80EQ);
  out.append("\r\n");
  WiFi.scanDelete();

  g_telnet_manager.sendScanResult(req, out.buf);
  vTaskDelete(nullptr);
}

void cmdWifi(EmbeddedCli *cli, char *args, void *context) {
  auto *sess = getSession(context);
  int sock = sess ? sess->sock : -1;
  uint8_t count = embeddedCliGetTokenCount(args);
  const char *subCmd = (count > 0) ? embeddedCliGetToken(args, 1) : "status";

  if (count == 0 || strcasecmp(subCmd, "status") == 0) {
    s_cli_scratch_buf[0] = '\0';
    AppendBuf out{s_cli_scratch_buf, sizeof(s_cli_scratch_buf)};

    out.append("\r\n");
    out.append(Fmt::DIV80EQ);
    out.append("                      WI-FI HARDWARE & NETWORK STATUS                         \r\n");
    out.append(Fmt::DIV80EQ);
    out.append("Category            Parameter       Value / Target                        Status\r\n");
    out.append(Fmt::DIV80);

    bool sta_ok = (WiFi.status() == WL_CONNECTED);
    out.appendFormat("%-20s%-16s%-32s%12s\r\n", "Station (STA)", "SSID",
                     sta_ok ? WiFi.SSID().c_str() : g_config.wifi_ssid,
                     sta_ok ? "[CONNECTED]" : "[DISCONNECTED]");
    out.appendFormat("%-20s%-16s%-32s%12s\r\n", "", "IP Address",
                     sta_ok ? WiFi.localIP().toString().c_str() : "0.0.0.0",
                     sta_ok ? "[ACTIVE]" : "[IDLE]");
    char rssi_b[16];
    snprintf(rssi_b, sizeof(rssi_b), "%d dBm", WiFi.RSSI());
    out.appendFormat("%-20s%-16s%-32s%12s\r\n", "", "Signal (RSSI)",
                     sta_ok ? rssi_b : "N/A", sta_ok ? "[STABLE]" : "[IDLE]");
    out.append(Fmt::DIV80);

    bool ap_active = (WiFi.getMode() == WIFI_MODE_AP || WiFi.getMode() == WIFI_MODE_APSTA);
    out.appendFormat("%-20s%-16s%-32s%12s\r\n", "SoftAP (AP)", "SSID",
                     g_config.ap_ssid, ap_active ? "[BROADCASTING]" : "[DISABLED]");
    out.appendFormat("%-20s%-16s%-32s%12s\r\n", "", "AP IP",
                     ap_active ? WiFi.softAPIP().toString().c_str() : "0.0.0.0",
                     ap_active ? "[ACTIVE]" : "[INACTIVE]");
    out.appendFormat("%-20s%-16s%-32s%12s\r\n", "", "Clients",
                     ap_active ? "Max 4 Clients" : "0 Clients",
                     ap_active ? "[READY]" : "[OFF]");
    out.append(Fmt::DIV80EQ);
    out.append("\r\n");

    sendTelnetMsgLen(sock, out.buf, out.offset);
    return;
  }

  if (strcasecmp(subCmd, "scan") == 0) {
    sendTelnetMsg(sock, "[WIFI] Scanning background 2.4GHz APs (Takes 2-3s)...\r\n");
    g_wifi_scan_req.clientIp = sess ? sess->clientIp : IPAddress(0, 0, 0, 0);
    g_wifi_scan_req.sessionId = sess ? sess->sessionId : 0;
    xTaskCreatePinnedToCore(
        AsyncWifiScanTask, "WifiScanWorker", 4096, &g_wifi_scan_req, 2,
        NULL, 0);
  } else if (strcasecmp(subCmd, "connect") == 0) {
    if (count < 2) {
      sendTelnetMsg(sock, "[ERROR] Usage: wifi connect <ssid> [password]\r\n");
      return;
    }
    const char *ssid_arg = embeddedCliGetToken(args, 2);
    const char *pass_arg = (count >= 3) ? embeddedCliGetToken(args, 3) : "";

    {
      CriticalSectionLocker lock(&g_config_mux);
      strncpy(g_config.wifi_ssid, ssid_arg, sizeof(g_config.wifi_ssid) - 1);
      g_config.wifi_ssid[sizeof(g_config.wifi_ssid) - 1] = '\0';
      strncpy(g_config.wifi_password, pass_arg, sizeof(g_config.wifi_password) - 1);
      g_config.wifi_password[sizeof(g_config.wifi_password) - 1] = '\0';
    }
    Config_Save();

    sendTelnetMsgf(sock, "[WIFI] Saved SSID '%s' to NVS. Connecting...\r\n", ssid_arg);
    WiFi.disconnect(false);
    vTaskDelay(pdMS_TO_TICKS(100));
    WiFi.begin(g_config.wifi_ssid, g_config.wifi_password);
  } else if (strcasecmp(subCmd, "disconnect") == 0) {
    WiFi.disconnect(false);
    sendTelnetMsg(sock, "[WIFI] Disconnected from Wi-Fi AP.\r\n");
  } else {
    sendTelnetMsg(sock, "Usage: wifi [status | scan | connect <ssid> [password] | disconnect]\r\n");
  }
}

} // namespace WifiCli
