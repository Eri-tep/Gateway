#include "Service/ConsoleCommands.h"
#include "Service/BridgeService.h"
#include "Service/ConsoleCli.h"
#include "Service/EngineTask.h"
#include "Service/RemoteService.h"
#include <WiFi.h>
#include <esp_ota_ops.h>

// ============================================================================
// From src/CLI/Cli.cpp
// ============================================================================

static char g_cli_scratch_buf[5120];

// ============================================================================
// CLI 80-COLUMN UNIFIED FORMATTING & BUFFER HELPERS
// ============================================================================

namespace CliFmt {
constexpr char BOX80_EQ[] = "+================================================="
                            "=============================+\r\n";
constexpr char BOX80_DASH[] = "+-----------------------------------------------"
                              "-------------------------------+\r\n";

inline bool ParseInt(const char *s, int &out_val, int min_v = INT_MIN,
                     int max_v = INT_MAX) noexcept {
  if (!s || !*s)
    return false;
  char *endp = nullptr;
  long v = strtol(s, &endp, 10);
  if (endp == s || *endp != '\0' || v < min_v || v > max_v)
    return false;
  out_val = static_cast<int>(v);
  return true;
}

inline void PrintBoxHeader(AppendBuf &out, const char *title) {
  out.append("\r\n");
  out.append(BOX80_EQ);
  int tlen = title ? static_cast<int>(strlen(title)) : 0;
  if (tlen > 78)
    tlen = 78;
  int pad_l = (78 - tlen) / 2;
  int pad_r = 78 - tlen - pad_l;
  out.appendFormat("|%*s%.*s%*s|\r\n", pad_l, "", tlen, title ? title : "",
                   pad_r, "");
  out.append(BOX80_EQ);
}

inline void PrintBoxSubtitle(AppendBuf &out, const char *subtitle) {
  int slen = subtitle ? static_cast<int>(strlen(subtitle)) : 0;
  if (slen > 78)
    slen = 78;
  int pad_l = (78 - slen) / 2;
  int pad_r = 78 - slen - pad_l;
  out.appendFormat("|%*s%.*s%*s|\r\n", pad_l, "", slen,
                   subtitle ? subtitle : "", pad_r, "");
}

inline void PrintBoxFooter(AppendBuf &out, const char *tip) {
  int tlen = tip ? static_cast<int>(strlen(tip)) : 0;
  if (tlen > 78)
    tlen = 78;
  int pad_l = (78 - tlen) / 2;
  int pad_r = 78 - tlen - pad_l;
  out.appendFormat("|%*s%.*s%*s|\r\n", pad_l, "", tlen, tip ? tip : "", pad_r,
                   "");
  out.append(BOX80_EQ);
  out.append("\r\n");
}

inline bool IsHelp(const char *s) {
  return s && (s[0] == '?' || strcasecmp(s, "help") == 0);
}

// Unified subcommand descriptor: carries dispatch handler AND help strings.
// handler == nullptr marks a help-only (separator / header) row.
struct SubCmdDef {
  const char *name;   // token matched against args.get(1), e.g. "frame"
  const char *syntax; // help table left column
  const char *desc;   // help table right column
  void (*handler)(int sock, int argc,
                  const Args &args); // nullptr = help row only
};

} // namespace CliFmt

template <typename F> inline void withScratchBuf(int sock, F &&fn) {
  g_cli_scratch_buf[0] = '\0';
  AppendBuf out{g_cli_scratch_buf, sizeof(g_cli_scratch_buf)};
  fn(out);
  if (sock >= 0 && out.offset > 0) {
    sendTelnetMsgLen(sock, out.buf, out.offset);
  }
}

namespace CliFmt {

inline void PrintSubCmdHelp(int sock, const char *title, const SubCmdDef *defs,
                            size_t count, const char *tip = nullptr) {
  withScratchBuf(sock, [title, defs, count, tip](AppendBuf &out) {
    PrintBoxHeader(out, title);
    static constexpr Column SUB_HELP_COLS[] = {
        {"Subcommand / Syntax", 34, Align::LEFT, Align::LEFT},
        {"Description", 39, Align::LEFT, Align::LEFT},
    };
    TableRenderer table(out, SUB_HELP_COLS, 2);
    table.header(false);
    for (size_t i = 0; i < count; ++i) {
      if (defs[i].syntax)
        table.row({defs[i].syntax, defs[i].desc ? defs[i].desc : ""});
    }
    table.end('-');
    if (tip) {
      PrintBoxFooter(out, tip);
    } else {
      out.append(BOX80_EQ);
      out.append("\r\n");
    }
  });
}

// Dispatch: scan defs[], call matching handler. Returns true if matched.
inline bool DispatchSubCmd(const char *sub, int sock, int argc,
                           const Args &args, const SubCmdDef *defs,
                           size_t count) {
  for (size_t i = 0; i < count; ++i) {
    if (defs[i].handler && defs[i].name && strcasecmp(sub, defs[i].name) == 0) {
      defs[i].handler(sock, argc, args);
      return true;
    }
  }
  return false;
}

} // namespace CliFmt

// ============================================================================
// From src/CLI/CliNetwork.cpp
// ============================================================================

namespace WifiCli {

static std::atomic<bool> s_wifi_scan_running{false};

static void AsyncWifiScanTask(void *pvParameters) {
  if (!pvParameters) {
    s_wifi_scan_running.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
    return;
  }
  TelnetManager::WifiScanReq req =
      *static_cast<TelnetManager::WifiScanReq *>(pvParameters);
  vTaskDelay(pdMS_TO_TICKS(100));

  int n = WiFi.scanNetworks(false, true);

  g_cli_scratch_buf[0] = '\0';
  AppendBuf out{g_cli_scratch_buf, sizeof(g_cli_scratch_buf)};

  out.append("\r\n");
  out.append("+================================================================"
             "==============+\r\n");
  out.append("|                          NEARBY WIFI ACCESS POINTS             "
             "              |\r\n");
  out.append("+================================================================"
             "==============+\r\n");
  static constexpr Column SCAN_COLS[] = {
      {"No", 2, Align::LEFT, Align::CENTER},
      {"SSID", 18, Align::LEFT, Align::CENTER},
      {"Signal (RSSI)", 13, Align::LEFT, Align::CENTER},
      {"CH", 4, Align::LEFT, Align::CENTER},
      {"Security", 27, Align::LEFT, Align::CENTER},
  };

  TableRenderer table(out, SCAN_COLS, 5);
  table.header(false);

  if (n == 0) {
    table.empty("(No wireless networks found)");
  } else if (n < 0) {
    table.empty("[ERROR] Wi-Fi hardware scan failed or timed out.");
  } else {
    int max_display = std::min(n, 40);
    for (int i = 0; i < max_display; ++i) {
      const char *encType = "OPEN";
      switch (WiFi.encryptionType(i)) {
      case WIFI_AUTH_WEP:
        encType = "WEP";
        break;
      case WIFI_AUTH_WPA_PSK:
        encType = "WPA-PSK";
        break;
      case WIFI_AUTH_WPA2_PSK:
        encType = "WPA2-PSK";
        break;
      case WIFI_AUTH_WPA_WPA2_PSK:
        encType = "WPA / WPA2";
        break;
      case WIFI_AUTH_WPA2_ENTERPRISE:
        encType = "WPA2-Enterprise";
        break;
      case WIFI_AUTH_WPA3_PSK:
        encType = "WPA3-SAE";
        break;
      case WIFI_AUTH_WPA2_WPA3_PSK:
        encType = "WPA2-PSK / WPA3-SAE";
        break;
      default:
        break;
      }
      char no_buf[4], sig_buf[16], ch_buf[8];
      snprintf(no_buf, sizeof(no_buf), "%d", i + 1);
      int rssi = WiFi.RSSI(i);
      snprintf(sig_buf, sizeof(sig_buf), "%d dBm (%d%%)", rssi,
               std::min(std::max(2 * (rssi + 100), 0), 100));
      snprintf(ch_buf, sizeof(ch_buf), "%d", WiFi.channel(i));
      table.row({no_buf, WiFi.SSID(i).c_str(), sig_buf, ch_buf, encType});
    }
  }
  table.end('-');
  CliFmt::PrintBoxFooter(out,
                         "Use 'wifi connect <ssid> <password>' to switch AP");
  out.append("\r\n");
  WiFi.scanDelete();

  g_telnet_manager.sendScanResult(req, out.buf);
  s_wifi_scan_running.store(false, std::memory_order_release);
  vTaskDelete(nullptr);
}

void cmdWifi(CliContext &ctx) {
  int sock = ctx.sock;
  uint8_t count = ctx.args.count();
  const char *subCmd = (count > 0) ? ctx.args.get(1) : "status";

  static const CliFmt::SubCmdDef kWifiDefs[] = {
      {"status", "status", "Show current Wi-Fi connection status",
       [](int s, int, const Args &) {
         withScratchBuf(s, [](AppendBuf &out) {
           CliFmt::PrintBoxHeader(out, "WI-FI HARDWARE & NETWORK STATUS");
           static constexpr Column WIFI_COLS[] = {
               {"Category", 13, Align::LEFT, Align::CENTER},
               {"Parameter", 13, Align::LEFT, Align::CENTER},
               {"Value / Target", 27, Align::LEFT, Align::CENTER},
               {"Status", 14, Align::LEFT, Align::CENTER},
           };
           TableRenderer table(out, WIFI_COLS, 4);
           table.header(false);

           bool sta_ok = (WiFi.status() == WL_CONNECTED);
           char rssi_b[16];
           snprintf(rssi_b, sizeof(rssi_b), "%d dBm", WiFi.RSSI());

           table.row({"Station (STA)", "SSID",
                      sta_ok ? WiFi.SSID().c_str() : g_config.wifi_ssid,
                      sta_ok ? "[CONNECTED]" : "[DISCONNECTED]"});
           table.row({"", "IP Address",
                      sta_ok ? WiFi.localIP().toString().c_str() : "0.0.0.0",
                      sta_ok ? "[ACTIVE]" : "[IDLE]"});
           table.row({"", "Signal (RSSI)", sta_ok ? rssi_b : "N/A",
                      sta_ok ? "[STABLE]" : "[IDLE]"});
           table.separator('-');

           bool ap_active = (WiFi.getMode() == WIFI_MODE_AP ||
                             WiFi.getMode() == WIFI_MODE_APSTA);
           table.row({"SoftAP (AP)", "SSID", g_config.ap_ssid,
                      ap_active ? "[BROADCASTING]" : "[DISABLED]"});
           table.row(
               {"", "AP IP",
                ap_active ? WiFi.softAPIP().toString().c_str() : "0.0.0.0",
                ap_active ? "[ACTIVE]" : "[INACTIVE]"});
           table.row({"", "Clients", ap_active ? "Max 4 Clients" : "0 Clients",
                      ap_active ? "[READY]" : "[OFF]"});
           out.append(CliFmt::BOX80_EQ);
           out.append("\r\n");
         });
       }},
      {"scan", "scan", "Scan nearby 2.4GHz Wi-Fi APs",
       [](int s, int, const Args &) {
         bool expected = false;
         if (!s_wifi_scan_running.compare_exchange_strong(expected, true)) {
           sendTelnetMsg(
               s, "[WARN] Wi-Fi scan already in progress. Please wait...\r\n");
           return;
         }
         sendTelnetMsg(
             s, "[WIFI] Scanning background 2.4GHz APs (Takes 2-3s)...\r\n");
         g_wifi_scan_req.clientIp = IPAddress(0, 0, 0, 0);
         g_wifi_scan_req.sessionId = 0;
         xTaskCreatePinnedToCore(AsyncWifiScanTask, "WifiScanWorker", 4096,
                                 &g_wifi_scan_req, 2, NULL, 0);
       }},
      {"connect", "connect <ssid> [password]",
       "Connect to specified AP and save to NVS",
       [](int s, int ac, const Args &args) {
         if (ac < 2) {
           sendTelnetMsg(
               s, "[ERROR] Missing SSID: wifi connect <ssid> [password]\r\n");
           return;
         }
         const char *ssid_arg = args.get(2);
         const char *pass_arg = (ac >= 3) ? args.get(3) : "";
         {
           CriticalSectionLocker lock(&g_config_mux);
           strncpy(g_config.wifi_ssid, ssid_arg,
                   sizeof(g_config.wifi_ssid) - 1);
           g_config.wifi_ssid[sizeof(g_config.wifi_ssid) - 1] = '\0';
           strncpy(g_config.wifi_password, pass_arg,
                   sizeof(g_config.wifi_password) - 1);
           g_config.wifi_password[sizeof(g_config.wifi_password) - 1] = '\0';
         }
         Config_Save();
         sendTelnetMsgf(s, "[WIFI] Saved SSID '%s' to NVS. Connecting...\r\n",
                        ssid_arg);
         WiFi.disconnect(false);
         vTaskDelay(pdMS_TO_TICKS(100));
         WiFi.begin(g_config.wifi_ssid, g_config.wifi_password);
       }},
      {"disconnect", "disconnect", "Disconnect from current Wi-Fi AP",
       [](int s, int, const Args &) {
         WiFi.disconnect(false);
         sendTelnetMsg(s, "[WIFI] Disconnected from Wi-Fi AP.\r\n");
       }},
  };

  if (CliFmt::DispatchSubCmd(subCmd, sock, count, ctx.args, kWifiDefs,
                             sizeof(kWifiDefs) / sizeof(kWifiDefs[0])))
    return;

  CliFmt::PrintSubCmdHelp(sock, "WIFI COMMAND REFERENCE", kWifiDefs,
                          sizeof(kWifiDefs) / sizeof(kWifiDefs[0]),
                          "Tip: Configuration persists to NVS flash memory");
}

} // namespace WifiCli

// ============================================================================
// From src/CLI/CliSystem.cpp
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
  char vendor_name_buf[UniversalProtocolEngine::kVendorNameMaxLen] = "Unknown";
  if (active) {
    active->getVendorName(vendor_name_buf, sizeof(vendor_name_buf));
  }
  const char *catalog_vendor = vendor_name_buf;

  if (g_config.wallpad_profile == 0) {
    snprintf(wp_status_buf, sizeof(wp_status_buf),
             desc.is_locked ? "Auto Detect (%s)" : "Auto Detect (Learning...)",
             catalog_vendor);
  } else {
    VendorProfileDescriptor cur_p;
    const char *p_name = ProfileRepository::getActiveProfile(cur_p)
                             ? (cur_p.name[0] ? cur_p.name : cur_p.key)
                             : nullptr;
    if (p_name)
      snprintf(wp_status_buf, sizeof(wp_status_buf), "%s (%s)", p_name,
               catalog_vendor);
    else
      snprintf(wp_status_buf, sizeof(wp_status_buf), "%s", catalog_vendor);
  }

  out.appendFormat(
      "\r\n==========================================================="
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

  g_cli_scratch_buf[0] = '\0';
  AppendBuf out{g_cli_scratch_buf, sizeof(g_cli_scratch_buf)};

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
  out.appendFormat("%-55s %24s\r\n", "Metric / Event",
                   "Value / Counter / Status");
  out.append(Fmt::DIV80);
  out.appendFormat(
      "%-55s %24u\r\n"
      "%-55s %24u\r\n"
      "%-55s %24u\r\n"
      "%-55s %24u\r\n",
      "Total Device Polls",
      static_cast<unsigned>(
          g_ch1_state_metrics.poll_cnt.load(std::memory_order_relaxed)),
      "VIP Controls (SmartThings App)",
      static_cast<unsigned>(
          g_ch1_state_metrics.vip_cnt.load(std::memory_order_relaxed)),
      "Normal Controls (Wallpad)",
      static_cast<unsigned>(
          g_ch1_state_metrics.normal_cnt.load(std::memory_order_relaxed)),
      "Stale Emerg Polls",
      static_cast<unsigned>(
          g_ch1_state_metrics.stale_poll_cnt.load(std::memory_order_relaxed)));

  Fmt::FormatTaskStacks(out, stack_snap, g_wdt_monitor);
  out.append("================================================================="
             "===============\r\n\r\n");

  sendTelnetMsgLen(sock, out.buf, out.offset);
  s_busy.store(false, std::memory_order_release);
}

void cmdStats(CliContext &ctx) {
  int client = ctx.sock;
  int argc = ctx.args.count();

  if (argc > 0) {
    const char *sub = ctx.args.get(1);
    if (strcasecmp(sub, "clear") == 0) {
      g_pkt_stats.resetAll();
      g_polling_targets.resetHits();
      g_metrics.reset();
      sendTelnetMsg(client, "All traffic statistics, hits, and metrics history "
                            "CLEARED to 0.\r\n");
      return;
    }
  }
  printStats(client);
}

void cmdReboot(CliContext &ctx) {
  int client = ctx.sock;
  sendTelnetMsg(client, "Rebooting...\r\n");
  g_restart_reason = "Telnet Command";
  g_restart_pending.store(true, std::memory_order_release);
}

void cmdLogView(CliContext &ctx) {
  int client = ctx.sock;
  const char *sub_cmd = (ctx.args.count() > 0) ? ctx.args.get(1) : "list";

  if (strcasecmp(sub_cmd, "clear") == 0) {
    LogManager::clearRebootLog();
    sendTelnetMsg(client, "Reboot log history CLEARED from NVS flash.\r\n");
    return;
  }

  size_t count = LogManager::getLogCount();
  if (count == 0) {
    sendTelnetMsg(client,
                  "\r\n[LOGVIEW] No persistent reboot logs found in NVS.\r\n");
    return;
  }

  if (strcasecmp(sub_cmd, "list") == 0) {
    withScratchBuf(client, [count](AppendBuf &out) {
      CliFmt::PrintBoxHeader(out, "PERSISTENT REBOOT LOG HISTORY");
      char sub_buf[64];
      snprintf(sub_buf, sizeof(sub_buf),
               "Total Stored: %u / %u Logs | Non-Volatile RTC/NVS",
               static_cast<unsigned>(count),
               static_cast<unsigned>(LogManager::MAX_LOG_ENTRIES));
      CliFmt::PrintBoxSubtitle(out, sub_buf);

      static constexpr Column REBOOT_COLS[] = {
          {"No", 3, Align::CENTER, Align::CENTER},
          {"Timestamp", 19, Align::CENTER, Align::CENTER},
          {"Reboot Reason", 33, Align::LEFT, Align::CENTER},
          {"Uptime", 12, Align::CENTER, Align::CENTER},
      };
      TableRenderer table(out, REBOOT_COLS, 4);
      table.header(false);

      for (size_t i = 0; i < count; i++) {
        LogEntry entry;
        if (LogManager::getLogEntry(i, entry)) {
          char time_buf[32] = "N/A";
          if (entry.timestamp > 0) {
            struct tm timeinfo;
            time_t sec = static_cast<time_t>(entry.timestamp);
            localtime_r(&sec, &timeinfo);
            if (timeinfo.tm_year >= 124) {
              strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S",
                       &timeinfo);
            } else {
              snprintf(time_buf, sizeof(time_buf),
                       "%04d-%02d-%02d %02d:%02d:%02d", timeinfo.tm_year + 1900,
                       timeinfo.tm_mon + 1, timeinfo.tm_mday, timeinfo.tm_hour,
                       timeinfo.tm_min, timeinfo.tm_sec);
            }
          }
          uint32_t sec = entry.stats_snapshot.uptime_ms / 1000;
          char up_buf[16];
          snprintf(up_buf, sizeof(up_buf), "%02uh %02um %02us", sec / 3600,
                   (sec % 3600) / 60, sec % 60);

          char no_buf[8];
          snprintf(no_buf, sizeof(no_buf), "#%u", static_cast<unsigned>(i + 1));
          table.row({no_buf, time_buf, entry.reason, up_buf});
        }
      }
      table.end('-');
      CliFmt::PrintBoxFooter(
          out,
          "Use 'logview <1-20>' for details, 'logview clear' to wipe history");
    });
    return;
  }

  size_t target_idx = 0;
  if (strcasecmp(sub_cmd, "last") == 0) {
    target_idx = 0;
  } else {
    char *endp = nullptr;
    long val = strtol(sub_cmd, &endp, 10);
    if (endp != sub_cmd && *endp == '\0' && val >= 1 &&
        static_cast<size_t>(val) <= count) {
      target_idx = static_cast<size_t>(val - 1);
    } else {
      sendTelnetMsg(
          client,
          "[ERROR] Invalid log index. Use 'logview' or 'logview <1-20>'\r\n");
      return;
    }
  }

  LogManager::readRebootLog(g_cli_scratch_buf, sizeof(g_cli_scratch_buf),
                            target_idx);
  sendTelnetMsg(client, g_cli_scratch_buf);
}

void cmdCoreDump(CliContext &ctx) {
  int client = ctx.sock;
  if (ctx.args.count() >= 1 && strcasecmp(ctx.args.get(1), "clear") == 0) {
    esp_core_dump_image_erase();
    sendTelnetMsg(client, "Crash core dump partition successfully ERASED.\r\n");
    return;
  }

  esp_core_dump_summary_t summary;
  esp_err_t err = esp_core_dump_get_summary(&summary);

  if (err != ESP_OK) {
    sendTelnetMsg(client, "\r\n[COREDUMP] No crash core dump summary available "
                          "(Partition clean or empty).\r\n");
    return;
  }

  withScratchBuf(client, [&summary](AppendBuf &out) {
    CliFmt::PrintBoxHeader(out, "CRASH CORE DUMP ANALYSIS SUMMARY");
    out.append("| Status          : Valid Core Dump Found                      "
               "                |\r\n");
    out.appendFormat("| Crashed Task    : %-58.58s |\r\n", summary.exc_task);
    out.appendFormat("| Program Counter : 0x%08X                               "
                     "                  |\r\n",
                     static_cast<unsigned>(summary.exc_pc));
    out.appendFormat("| Exception Cause : %-58lu |\r\n",
                     static_cast<unsigned long>(summary.ex_info.exc_cause));
    out.appendFormat("| Backtrace Depth : %-2d frames%-48s |\r\n",
                     summary.exc_bt_info.depth,
                     summary.exc_bt_info.corrupted ? " (CORRUPTED)" : "");
    out.append("| Backtrace PCs   :                                            "
               "                |\r\n");

    for (int i = 0; i < summary.exc_bt_info.depth; ++i) {
      out.appendFormat("|   [%2d] 0x%08X                                       "
                       "                      |\r\n",
                       i, static_cast<unsigned>(summary.exc_bt_info.bt[i]));
    }
    out.append("+--------------------------------------------------------------"
               "----------------+\r\n");
    CliFmt::PrintBoxFooter(
        out, "Use: xtensa-esp32s3-elf-addr2line -pfiaC -e firmware.elf <PC>");
  });
}

void otaPrintStatus(AppendBuf &out) {
  const esp_partition_t *running = esp_ota_get_running_partition();
  const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
  esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
  if (running) {
    esp_ota_get_state_partition(running, &ota_state);
  }

  const char *state_desc =
      (ota_state == ESP_OTA_IMG_NEW) ? "New Image (First Boot)"
      : (ota_state == ESP_OTA_IMG_PENDING_VERIFY)
          ? "Evaluating (Rollback Active)"
      : (ota_state == ESP_OTA_IMG_INVALID) ? "Invalidated Image"
      : (ota_state == ESP_OTA_IMG_ABORTED) ? "Aborted Image"
                                           : "Confirmed";
  const char *state_status = (ota_state == ESP_OTA_IMG_NEW) ? "[NEW]"
                             : (ota_state == ESP_OTA_IMG_PENDING_VERIFY)
                                 ? "[PENDING]"
                             : (ota_state == ESP_OTA_IMG_INVALID) ? "[INVALID]"
                             : (ota_state == ESP_OTA_IMG_ABORTED) ? "[ABORTED]"
                                                                  : "[STABLE]";

  char run_val[36], next_val[36], timer_val[36], crash_val[36];
  snprintf(run_val, sizeof(run_val), "%s (0x%06X, %u KB)",
           running ? running->label : "app0",
           running ? static_cast<unsigned>(running->address) : 0x10000,
           running ? static_cast<unsigned>(running->size / 1024) : 3712);
  snprintf(next_val, sizeof(next_val), "%s (0x%06X, %u KB)",
           next ? next->label : "app1",
           next ? static_cast<unsigned>(next->address) : 0x3B0000,
           next ? static_cast<unsigned>(next->size / 1024) : 3712);

  bool val_done = TimeUtils::isElapsed(
      g_boot_start_ms, Config::Timing::OTA_VALIDATION_PERIOD_MS);
  snprintf(timer_val, sizeof(timer_val), "%s",
           val_done ? "120s Passed" : "Evaluating (<120s)");
  snprintf(crash_val, sizeof(crash_val), "%u Consecutive Crashes",
           static_cast<unsigned>(rtc_crash_counter));
  bool is_rescue = g_rescue_mode.load(std::memory_order_relaxed);

  CliFmt::PrintBoxHeader(out, "DUAL-PARTITION OTA & ROLLBACK MONITOR");
  static constexpr Column OTA_COLS[] = {
      {"Category", 13, Align::LEFT, Align::CENTER},
      {"Parameter", 13, Align::LEFT, Align::CENTER},
      {"Value / Target", 27, Align::LEFT, Align::CENTER},
      {"Status", 14, Align::LEFT, Align::CENTER},
  };
  TableRenderer table(out, OTA_COLS, 4);
  table.header(false);

  table.row({"Running App", "Partition", run_val, "[ACTIVE]"});
  table.row({"", "State", state_desc, state_status});
  table.separator('-');

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

  table.row({"Backup Target", "Partition", next_val, "[STANDBY]"});
  table.row({"", "Rollback", next_desc, next_status});
  table.separator('-');

  table.row({"Safety Guard", "Health Timer", timer_val,
             val_done ? "[STABLE]" : "[TESTING]"});
  table.row({"", "Crash Loop", crash_val,
             rtc_crash_counter == 0 ? "[STABLE]" : "[WARNING]"});
  table.row({"", "Rescue Mode",
             is_rescue ? "Forced Safe SoftAP" : "Standard Boot",
             is_rescue ? "[RESCUE]" : "[STABLE]"});
  out.append(CliFmt::BOX80_EQ);
  out.append("\r\n");
}

void otaTriggerRollback(int sock) {
  sendTelnetMsg(sock, "[OTA] Invalidating current app and triggering hardware "
                      "rollback to previous firmware...\r\n");
  vTaskDelay(pdMS_TO_TICKS(100));
  esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
  if (err != ESP_OK) {
    char err_buf[64];
    snprintf(err_buf, sizeof(err_buf),
             "[ERROR] Rollback failed (No rollback partition available, "
             "err=0x%x)\r\n",
             err);
    sendTelnetMsg(sock, err_buf);
  }
}

void otaValidate(int sock) {
  esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  if (err == ESP_OK) {
    sendTelnetMsg(sock, "[OTA] Current firmware manually confirmed as VALID. "
                        "Auto-rollback cancelled.\r\n");
  } else {
    char err_buf[64];
    snprintf(err_buf, sizeof(err_buf),
             "[ERROR] Failed to mark app valid: 0x%x\r\n", err);
    sendTelnetMsg(sock, err_buf);
  }
}

void cmdOta(CliContext &ctx) {
  int sock = ctx.sock;
  uint8_t count = ctx.args.count();
  const char *subCmd = (count > 0) ? ctx.args.get(1) : "status";

  static const CliFmt::SubCmdDef kOtaDefs[] = {
      {"status", "status", "Show active partition & rollback state",
       [](int s, int, const Args &) {
         withScratchBuf(s, [](AppendBuf &out) { otaPrintStatus(out); });
       }},
      {"rollback", "rollback", "Rollback to previous firmware partition",
       [](int s, int, const Args &) { otaTriggerRollback(s); }},
      {"validate", "validate", "Confirm current firmware running valid",
       [](int s, int, const Args &) { otaValidate(s); }},
      {"cloud", "cloud [url]", "Trigger cloud OTA download & update",
       [](int s, int ac, const Args &args) {
         const char *url = (ac >= 2) ? args.get(2) : nullptr;
         sendTelnetMsg(
             s,
             "[OTA] Initiating GitHub Cloud HTTP(S) OTA in background...\r\n");
         Mgmt_StartHttpOta(url);
       }},
  };

  if (CliFmt::DispatchSubCmd(subCmd, sock, count, ctx.args, kOtaDefs,
                             sizeof(kOtaDefs) / sizeof(kOtaDefs[0])))
    return;

  CliFmt::PrintSubCmdHelp(
      sock, "OTA COMMAND REFERENCE", kOtaDefs,
      sizeof(kOtaDefs) / sizeof(kOtaDefs[0]),
      "Tip: Unvalidated firmware auto-rolls back on reboot");
}

void cmdHelp(CliContext &ctx) {
  int client = ctx.sock;
  withScratchBuf(client, [](AppendBuf &out) {
    CliFmt::PrintBoxHeader(out, "GATEWAY TELNET COMMAND REFERENCE");
    static constexpr Column HELP_COLS[] = {
        {"Command", 9, Align::LEFT, Align::LEFT},
        {"Description & Usage Syntax", 64, Align::LEFT, Align::LEFT},
    };
    TableRenderer table(out, HELP_COLS, 2);
    table.header(false);
    for (size_t i = 0; i < kConsoleCmdsCount; ++i) {
      table.row({kConsoleCmds[i].name, kConsoleCmds[i].help});
    }
    table.end('-');
    CliFmt::PrintBoxFooter(
        out, "Type '<command> ?' or '<command> help' for detailed reference");
  });
}

} // namespace SystemCli

// ============================================================================
// From src/CLI/CliConfig.cpp
// ============================================================================

namespace ConfigCli {

enum ParamType {
  PARAM_UINT32,
  PARAM_UINT16,
  PARAM_UCHAR,
  PARAM_STRING,
  PARAM_PASS_HASH,
  PARAM_FRAMING_CH1,
  PARAM_FRAMING_CH2,
  PARAM_FRAMING_CH3,
  PARAM_FRAMING_CH4,
  PARAM_TIMING_CH1,
  PARAM_TIMING_CH2,
  PARAM_TIMING_CH3
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
  uint32_t minVal;
  uint32_t maxVal;
  const char *desc;
};

static const ConfigParamDef PARAM_TABLE[] = {
    // Channel Baudrates
    {"ch1_baud",
     PARAM_UINT32,
     {.u32 = &g_config.uart_baud_rate},
     1200,
     115200,
     "CH1 Device Master Baudrate (bps)"},
    {"ch2_baud",
     PARAM_UINT32,
     {.u32 = &g_config.ch2_baud_rate},
     1200,
     115200,
     "CH2 Main Wallpad Baudrate (bps)"},
    {"ch3_baud",
     PARAM_UINT32,
     {.u32 = &g_config.ch3_baud_rate},
     1200,
     115200,
     "CH3 Sub Wallpad Baudrate (bps)"},
    {"ch4_baud",
     PARAM_UINT32,
     {.u32 = &g_config.doorphone_baud_rate},
     1200,
     115200,
     "CH4 Doorphone Baudrate (bps)"},

    // Channel Framings (8N1, 8E1, 8O1, 8N2)
    {"ch1_framing",
     PARAM_FRAMING_CH1,
     {.u8 = nullptr},
     0,
     0,
     "CH1 Framing (8N1, 8E1, 8O1, 8N2)"},
    {"ch2_framing",
     PARAM_FRAMING_CH2,
     {.u8 = nullptr},
     0,
     0,
     "CH2 Framing (8N1, 8E1, 8O1, 8N2)"},
    {"ch3_framing",
     PARAM_FRAMING_CH3,
     {.u8 = nullptr},
     0,
     0,
     "CH3 Framing (8N1, 8E1, 8O1, 8N2)"},
    {"ch4_framing",
     PARAM_FRAMING_CH4,
     {.u8 = nullptr},
     0,
     0,
     "CH4 Framing (8N1, 8E1, 8O1, 8N2)"},

    // Channel Delays
    {"ch1_poll",
     PARAM_TIMING_CH1,
     {.u16 = &g_timing_config.ch1_poll_interval_ms},
     200,
     5000,
     "CH1 Master Polling Interval (ms)"},
    {"ch2_ack",
     PARAM_TIMING_CH2,
     {.u16 = &g_timing_config.ch2_cache_delay_ms},
     5,
     300,
     "CH2 Main Wallpad Virtual ACK (ms)"},
    {"ch3_ack",
     PARAM_TIMING_CH3,
     {.u16 = &g_timing_config.ch3_cache_delay_ms},
     20,
     1000,
     "CH3 Sub Wallpad Virtual ACK (ms)"},

    // Wallpad Profile
    {"profile",
     PARAM_UCHAR,
     {.u8 = &g_config.wallpad_profile},
     0,
     3,
     "Wallpad Profile Slot (0=Auto, 1=Custom1, etc)"},

    // Wi-Fi & Network
    {"wifi_ssid",
     PARAM_STRING,
     {.str = g_config.wifi_ssid},
     0,
     sizeof(g_config.wifi_ssid) - 1,
     "Station Wi-Fi SSID"},
    {"wifi_pass",
     PARAM_STRING,
     {.str = g_config.wifi_password},
     0,
     sizeof(g_config.wifi_password) - 1,
     "Station Wi-Fi Password"},
    {"ap_ssid",
     PARAM_STRING,
     {.str = g_config.ap_ssid},
     0,
     sizeof(g_config.ap_ssid) - 1,
     "SoftAP SSID"},
    {"ap_pass",
     PARAM_STRING,
     {.str = g_config.ap_password},
     0,
     sizeof(g_config.ap_password) - 1,
     "SoftAP Password"},
    {"wifi_timeout",
     PARAM_UINT16,
     {.u16 = &g_config.wifi_connect_timeout_s},
     5,
     120,
     "Wi-Fi Connection Timeout (seconds)"},

    // Security
    {"telnet_pass",
     PARAM_PASS_HASH,
     {.str = g_config.telnet_pass_hash},
     0,
     0,
     "Telnet Login Password"},
};
static const size_t PARAM_COUNT = sizeof(PARAM_TABLE) / sizeof(ConfigParamDef);

void printConfig(int sock) {
  withScratchBuf(sock, [](AppendBuf &out) {
    CliFmt::PrintBoxHeader(out, "RUNTIME GATEWAY CONFIGURATION (NVS)");
    static constexpr Column CFG_COLS[] = {
        {"Parameter Key", 25, Align::LEFT, Align::CENTER},
        {"Configured Value", 48, Align::LEFT, Align::CENTER},
    };
    TableRenderer table(out, CFG_COLS, 2);
    table.header(false);

    auto rowf = [&](const char *k, const char *fmt, ...) {
      char v[64];
      va_list va;
      va_start(va, fmt);
      vsnprintf(v, sizeof(v), fmt, va);
      va_end(va);
      table.row({k, v});
    };

    rowf("wifi_ssid", "\"%s\"",
         g_config.wifi_ssid[0] ? g_config.wifi_ssid : "(Not Configured)");
    rowf("ap_ssid", "\"%s\"",
         g_config.ap_ssid[0] ? g_config.ap_ssid : "(Disabled)");
    rowf("wifi_timeout", "%u sec", g_config.wifi_connect_timeout_s);
    table.row({"telnet_pass", g_config.telnet_pass_hash[0]
                                  ? "Configured (SHA-256)"
                                  : "Default (None)"});
    table.row({"wallpad_profile",
               (g_config.wallpad_profile == 1)   ? "1 (Custom Slot 1)"
               : (g_config.wallpad_profile == 2) ? "2 (Custom Slot 2)"
               : (g_config.wallpad_profile == 3)
                   ? "3 (Custom Slot 3)"
                   : "0 (Auto-Discovered & Learned)"});
    rowf("uart_baud_rate (CH1)", "%u bps (%s)",
         static_cast<unsigned>(g_config.uart_baud_rate),
         formatFramingStr(g_config.uart_data_bits, g_config.uart_parity,
                          g_config.uart_stop_bits));
    rowf("ch2_baud_rate (CH2)", "%u bps (%s)",
         static_cast<unsigned>(g_config.ch2_baud_rate),
         formatFramingStr(g_config.ch2_data_bits, g_config.ch2_parity,
                          g_config.ch2_stop_bits));
    rowf("ch3_baud_rate (CH3)", "%u bps (%s)",
         static_cast<unsigned>(g_config.ch3_baud_rate),
         formatFramingStr(g_config.ch3_data_bits, g_config.ch3_parity,
                          g_config.ch3_stop_bits));
    rowf("doorphone_baud_rate (CH4)", "%u bps (%s)",
         static_cast<unsigned>(g_config.doorphone_baud_rate),
         formatFramingStr(g_config.doorphone_data_bits,
                          g_config.doorphone_parity,
                          g_config.doorphone_stop_bits));
    rowf("ch1_poll_interval", "%u ms", g_timing_config.ch1_poll_interval_ms);
    rowf("ch2_cache_delay", "%u ms", g_timing_config.ch2_cache_delay_ms);
    rowf("ch3_cache_delay", "%u ms", g_timing_config.ch3_cache_delay_ms);
    table.end('-');
    CliFmt::PrintBoxFooter(
        out, "Use 'config set <key> <val>' and 'save' to persist to NVS");
  });
}

void printConfigHelp(int sock) {
  withScratchBuf(sock, [](AppendBuf &out) {
    CliFmt::PrintBoxHeader(out, "CONFIGURABLE PARAMETERS GUIDE");
    static constexpr Column CONFIG_HELP_COLS[] = {
        {"Parameter Key", 16, Align::LEFT, Align::LEFT},
        {"Allowed Range / Type", 20, Align::LEFT, Align::LEFT},
        {"Description", 34, Align::LEFT, Align::LEFT},
    };
    TableRenderer table(out, CONFIG_HELP_COLS, 3);
    table.header(false);

    for (size_t i = 0; i < PARAM_COUNT; ++i) {
      const auto &p = PARAM_TABLE[i];
      char range_buf[24];
      if (p.type <= PARAM_UCHAR ||
          (p.type >= PARAM_TIMING_CH1 && p.type <= PARAM_TIMING_CH3)) {
        snprintf(range_buf, sizeof(range_buf), "%lu ~ %lu",
                 (unsigned long)p.minVal, (unsigned long)p.maxVal);
      } else if (p.type >= PARAM_FRAMING_CH1 && p.type <= PARAM_FRAMING_CH4) {
        strcpy(range_buf, "8N1,8E1,8O1,8N2");
      } else {
        strcpy(range_buf,
               (p.type == PARAM_PASS_HASH) ? "string (raw)" : "string");
      }
      table.row({p.name, range_buf, p.desc});
    }

    table.end('-');
    out.append("|  config set <key> <value>   : Modify parameter (RAM only)    "
               "                |\r\n");
    out.append("|  save                       : Commit modified parameters to "
               "NVS flash        |\r\n");
    out.append("|  config reset               : Restore all configuration to "
               "factory defaults  |\r\n");
    CliFmt::PrintBoxFooter(out,
                           "Tip: Use 'save' to commit changes to NVS flash");
  });
}

template <typename T>
static bool applyUintParam(T *dest, const char *value, unsigned long min_val,
                           unsigned long max_val, int sock, const char *key) {
  char *endp = nullptr;
  unsigned long v = strtoul(value, &endp, 10);
  if (!endp || *endp != '\0' || v < min_val || v > max_val) {
    sendTelnetMsgf(
        sock, "[ERROR] Invalid value '%s' for '%s' (Allowed: %lu ~ %lu)\r\n",
        value, key, min_val, max_val);
    return false;
  }
  *dest = static_cast<T>(v);
  g_config_dirty.store(true, std::memory_order_relaxed);
  sendTelnetMsgf(
      sock, "[OK] Set %s = %lu (RAM only. Use 'save' to commit to NVS)\r\n",
      key, v);
  return true;
}

static bool applyFraming(uint8_t &dbits, uint8_t &parity, uint8_t &sbits,
                         const char *value, int sock, const char *key) {
  uint8_t d, p, s;
  if (!parseFramingStr(value, d, p, s)) {
    sendTelnetMsgf(
        sock,
        "[ERROR] Invalid framing '%s'. Choose from: 8N1, 8E1, 8O1, 8N2\r\n",
        value);
    return false;
  }
  dbits = d;
  parity = p;
  sbits = s;
  g_config_dirty.store(true, std::memory_order_relaxed);
  sendTelnetMsgf(
      sock, "[OK] Set %s = '%s' (RAM only. Use 'save' to commit to NVS)\r\n",
      key, value);
  return true;
}

void setConfig(int sock, const char *key, const char *value) {
  if (!key || !value) {
    sendTelnetMsg(sock,
                  "[ERROR] Missing argument: config set <key> <value>\r\n");
    return;
  }

  for (size_t i = 0; i < PARAM_COUNT; ++i) {
    const auto &p = PARAM_TABLE[i];
    if (strcasecmp(p.name, key) == 0) {
      CriticalSectionLocker lock(&g_config_mux);
      switch (p.type) {
      case PARAM_UINT32:
        return (void)applyUintParam(p.ptr.u32, value, p.minVal, p.maxVal, sock,
                                    key);
      case PARAM_UINT16:
        return (void)applyUintParam(p.ptr.u16, value, p.minVal, p.maxVal, sock,
                                    key);
      case PARAM_UCHAR:
        return (void)applyUintParam(p.ptr.u8, value, p.minVal, p.maxVal, sock,
                                    key);
      case PARAM_TIMING_CH1:
      case PARAM_TIMING_CH2:
      case PARAM_TIMING_CH3: {
        char *endp = nullptr;
        unsigned long v = strtoul(value, &endp, 10);
        if (!endp || *endp != '\0' || v < p.minVal || v > p.maxVal) {
          sendTelnetMsgf(
              sock,
              "[ERROR] Invalid delay '%s' for '%s' (Allowed: %lu ~ %lu ms)\r\n",
              value, key, (unsigned long)p.minVal, (unsigned long)p.maxVal);
          return;
        }
        *p.ptr.u16 = static_cast<uint16_t>(v);
        TimingConfig_Save();
        sendTelnetMsgf(
            sock,
            "[OK] Set %s = %lu ms (Saved to timing_cfg NVS immediately)\r\n",
            key, v);
        return;
      }
      case PARAM_FRAMING_CH1:
        return (void)applyFraming(g_config.uart_data_bits, g_config.uart_parity,
                                  g_config.uart_stop_bits, value, sock, key);
      case PARAM_FRAMING_CH2:
        return (void)applyFraming(g_config.ch2_data_bits, g_config.ch2_parity,
                                  g_config.ch2_stop_bits, value, sock, key);
      case PARAM_FRAMING_CH3:
        return (void)applyFraming(g_config.ch3_data_bits, g_config.ch3_parity,
                                  g_config.ch3_stop_bits, value, sock, key);
      case PARAM_FRAMING_CH4:
        return (void)applyFraming(
            g_config.doorphone_data_bits, g_config.doorphone_parity,
            g_config.doorphone_stop_bits, value, sock, key);
      case PARAM_STRING: {
        if (strlen(value) <= p.maxVal) {
          strncpy(p.ptr.str, value, p.maxVal);
          p.ptr.str[p.maxVal] = '\0';
          g_config_dirty.store(true, std::memory_order_relaxed);
          sendTelnetMsgf(
              sock,
              "[OK] Set %s = '%s' (RAM only. Use 'save' to commit to NVS)\r\n",
              key, value);
        } else {
          sendTelnetMsgf(
              sock,
              "[ERROR] String exceeds maximum length of %lu characters.\r\n",
              (unsigned long)p.maxVal);
        }
        return;
      }
      case PARAM_PASS_HASH: {
        char hash_hex[68];
        System_Sha256ToHex(value, hash_hex);
        strncpy(g_config.telnet_pass_hash, hash_hex,
                sizeof(g_config.telnet_pass_hash) - 1);
        g_config.telnet_pass_hash[sizeof(g_config.telnet_pass_hash) - 1] = '\0';
        g_config_dirty.store(true, std::memory_order_relaxed);
        sendTelnetMsg(sock, "[OK] Telnet password updated & SHA-256 hashed. "
                            "Use 'save' to commit to NVS.\r\n");
        return;
      }
      default:
        break;
      }
    }
  }

  sendTelnetMsgf(sock,
                 "[ERROR] Unknown parameter '%s'. Type 'config ?' to list all "
                 "valid parameters.\r\n",
                 key);
}

void cmdConfig(CliContext &ctx) {
  int sock = ctx.sock;
  int argc = ctx.args.count();
  if (argc == 0) {
    printConfig(sock);
    return;
  }

  static const CliFmt::SubCmdDef kConfigDefs[] = {
      {"list", "list", "Display runtime configuration table",
       [](int s, int, const Args &) { printConfig(s); }},
      {"set", "set <key> <value>", "Set configuration parameter in RAM",
       [](int s, int ac, const Args &args) {
         if (ac >= 3)
           setConfig(s, args.get(2), args.get(3));
         else
           sendTelnetMsg(
               s, "[ERROR] Missing argument: config set <key> <value>\r\n");
       }},
      {"reset", "reset", "Reset configuration to defaults",
       [](int s, int, const Args &) {
         Config_ResetDefaults();
         sendTelnetMsg(s, "[OK] Runtime configuration reset to system factory "
                          "defaults. (RAM only. Use 'save' to commit)\r\n");
       }},
  };

  const char *sub = ctx.args.get(1);
  if (CliFmt::DispatchSubCmd(sub, sock, argc, ctx.args, kConfigDefs,
                             sizeof(kConfigDefs) / sizeof(kConfigDefs[0])))
    return;

  printConfigHelp(sock);
}

void cmdSave(CliContext &ctx) {
  int sock = ctx.sock;
  Config_Save();
  sendTelnetMsg(
      sock,
      "[OK] Configuration successfully committed and saved to NVS flash!\r\n");
}

static bool ew11ParseSlot(int sock, const char *arg, int &slot,
                          const char *cmd) {
  if (!arg) {
    sendTelnetMsgf(sock, "[ERROR] Missing slot: ew11 %s <slot:0-4>\r\n", cmd);
    return false;
  }
  if (!CliFmt::ParseInt(arg, slot, 0, Config::TCP::MAX_EW11_SLOTS - 1)) {
    sendTelnetMsgf(sock, "[ERROR] Slot index must be 0 to %d\r\n",
                   Config::TCP::MAX_EW11_SLOTS - 1);
    return false;
  }
  return true;
}

static void ew11SetEnable(int sock, int slot, bool enabled) {
  {
    MutexLocker lock(g_ch5_mutex);
    g_hub_slots[slot].enabled = enabled;
    if (!enabled && g_hub_slots[slot].sock >= 0) {
      close(g_hub_slots[slot].sock);
      g_hub_slots[slot].sock = -1;
      g_hub_slots[slot].is_connected = false;
      g_hub_slots[slot].rx_len = 0;
    }
  }
  Hub_SaveConfig();
  sendTelnetMsgf(sock, "[OK] EW11 Slot #%d %s and saved to NVS flash.\r\n",
                 slot, enabled ? "ENABLED" : "DISABLED");
}

void cmdEw11(CliContext &ctx) {
  int sock = ctx.sock;
  int argc = ctx.args.count();

  if (argc == 0 || (argc == 1 && strcasecmp(ctx.args.get(1), "list") == 0) ||
      (argc == 1 && strcasecmp(ctx.args.get(1), "status") == 0)) {
    withScratchBuf(sock, [](AppendBuf &out) {
      CliFmt::PrintBoxHeader(out, "CH5 EW11 TCP CLIENT SOCKET STATUS");
      static constexpr Column EW11_COLS[] = {
          {"Slot", 4, Align::CENTER, Align::CENTER},
          {"Name", 10, Align::LEFT, Align::CENTER},
          {"Port", 4, Align::CENTER, Align::CENTER},
          {"Client IP", 15, Align::CENTER, Align::CENTER},
          {"Status", 11, Align::CENTER, Align::CENTER},
          {"Packets", 17, Align::CENTER, Align::CENTER},
      };
      TableRenderer table_sock(out, EW11_COLS, 6);
      table_sock.header(false);

      {
        MutexLocker lock(g_ch5_mutex);
        for (int s = 0; s < Config::TCP::MAX_EW11_SLOTS; s++) {
          auto &slot = g_hub_slots[s];
          const char *status_str = !slot.enabled         ? "Disabled"
                                   : !slot.is_connected  ? "Listening"
                                   : (slot.rx_pkts == 0) ? "Idle"
                                                         : "Connected";
          const char *ip_str =
              slot.is_connected
                  ? (slot.target_ip[0] ? slot.target_ip : "Connected")
                  : (slot.target_ip[0] ? slot.target_ip : "-");
          char s_buf[8], p_buf[8], pkt_buf[24];
          snprintf(s_buf, sizeof(s_buf), "#%d", s);
          snprintf(p_buf, sizeof(p_buf), "%u", slot.target_port);
          snprintf(pkt_buf, sizeof(pkt_buf), "%8u / %-8u",
                   static_cast<unsigned>(slot.rx_pkts),
                   static_cast<unsigned>(slot.tx_pkts));
          table_sock.row(
              {s_buf, slot.name, p_buf, ip_str, status_str, pkt_buf});
        }
      }
      table_sock.end('-');
      CliFmt::PrintBoxFooter(
          out, "Configured Max Slots: 5 | Bridge Target: CH1 & CH2/3");

      // ── FCU Modbus 실시간 상태 테이블 ──
      CliFmt::PrintBoxHeader(out, "CH5 FCU MODBUS DEVICE STATUS");
      static constexpr Column FCU_COLS[] = {
          {"Slot", 4, Align::CENTER, Align::CENTER},
          {"Name", 6, Align::LEFT, Align::CENTER},
          {"Port", 4, Align::CENTER, Align::CENTER},
          {"Client IP", 14, Align::CENTER, Align::CENTER},
          {"Pwr", 3, Align::CENTER, Align::CENTER},
          {"Mode", 4, Align::CENTER, Align::CENTER},
          {"Fan", 4, Align::CENTER, Align::CENTER},
          {"Swng", 4, Align::CENTER, Align::CENTER},
          {"Tgt", 3, Align::CENTER, Align::CENTER},
          {"Room", 3, Align::CENTER, Align::CENTER},
      };
      TableRenderer table_fcu(out, FCU_COLS, 10);
      table_fcu.header(false);

      {
        MutexLocker lock(g_ch5_mutex);
        for (uint8_t s = 1; s < Config::TCP::MAX_EW11_SLOTS; ++s) {
          const HubClientSlot &slot = g_hub_slots[s];
          Fcu::SlotRuntime rt;
          Fcu::GetSlotRuntime(s, rt);

          static constexpr const char *kModes[] = {"-", "Cool", "Heat", "Fan"};
          static constexpr const char *kFans[] = {"OFF", "Low", "Mid", "High",
                                                  "Auto"};
          uint16_t m_idx = static_cast<uint16_t>(rt.snap.mode);
          uint16_t f_idx = static_cast<uint16_t>(rt.snap.fan_speed);
          const char *pwr_str = rt.snap.power ? "ON" : "OFF";
          const char *mode_str =
              (m_idx >= 1 && m_idx <= 3) ? kModes[m_idx] : "-";
          const char *fan_str = (f_idx <= 4) ? kFans[f_idx] : "-";
          const char *swng_str =
              (rt.snap.swing == Fcu::Swing::On) ? "ON" : "OFF";

          char tgt_str[8] = "-", room_str[8] = "-";
          if (rt.is_online) {
            snprintf(tgt_str, sizeof(tgt_str), "%uC", rt.snap.target_temp);
            snprintf(room_str, sizeof(room_str), "%uC", rt.snap.room_temp);
          }
          const char *ip_str =
              (slot.is_connected && slot.target_ip[0]) ? slot.target_ip : "-";

          char s_buf[8], p_buf[8];
          snprintf(s_buf, sizeof(s_buf), "#%u", s);
          snprintf(p_buf, sizeof(p_buf), "%u", slot.target_port);

          table_fcu.row({s_buf, slot.name, p_buf, ip_str, pwr_str, mode_str,
                         fan_str, swng_str, tgt_str, room_str});
        }
      }
      table_fcu.end('-');
      out.append(CliFmt::BOX80_EQ);
      out.append("\r\n");
    });
    return;
  }

  const char *sub = ctx.args.get(1);

  // Unified table: help strings + handlers in one place.
  static const CliFmt::SubCmdDef kEw11Defs[] = {
      {"list", "list", "Show EW11 sockets & FCU runtime status", nullptr},
      {"set", "set <slot> [port] [ip] [name] [en]",
       "Configure EW11 bridge socket settings",
       [](int sock, int argc, const Args &args) {
         int slot = -1;
         if (!ew11ParseSlot(sock, args.get(2), slot, "set"))
           return;
         uint16_t default_port = Config::TCP::EW11_SLOT_PORTS[slot];
         uint16_t port = g_hub_slots[slot].target_port > 0
                             ? g_hub_slots[slot].target_port
                             : default_port;
         const char *ip_str = nullptr;
         const char *name_str = nullptr;
         bool enabled = g_hub_slots[slot].enabled;
         for (int i = 3; i <= argc; ++i) {
           const char *tok = args.get(i);
           if (!tok || !*tok)
             continue;
           int v = 0;
           if (strchr(tok, '.') || strcmp(tok, "-") == 0 ||
               strcmp(tok, "none") == 0) {
             ip_str =
                 (strcmp(tok, "-") == 0 || strcmp(tok, "none") == 0) ? "" : tok;
           } else if (port == default_port &&
                      CliFmt::ParseInt(tok, v, 1, 65535)) {
             port = static_cast<uint16_t>(v);
           } else if (!name_str && !isdigit(tok[0])) {
             name_str = tok;
           } else if (CliFmt::ParseInt(tok, v, 0, 1)) {
             enabled = (v != 0);
           }
         }
         if (Hub_SetSlot(static_cast<uint8_t>(slot), enabled, ip_str, port,
                         name_str)) {
           sendTelnetMsgf(
               sock,
               "[OK] EW11 Slot #%d configured (Name: %s, Listen Port: %u, "
               "Allowed IP: %s, Enabled: %s) and saved to NVS!\r\n",
               slot, g_hub_slots[slot].name, g_hub_slots[slot].target_port,
               g_hub_slots[slot].target_ip[0] ? g_hub_slots[slot].target_ip
                                              : "Any",
               g_hub_slots[slot].enabled ? "true" : "false");
         } else {
           sendTelnetMsg(sock, "[ERROR] Failed to configure EW11 slot.\r\n");
         }
       }},
      {"frame", "frame <slot> <stx> <etx> [len]",
       "Set custom framing delimiters for slot",
       [](int sock, int argc, const Args &args) {
         if (argc < 4) {
           sendTelnetMsg(sock, "[ERROR] Format: ew11 frame <slot:0-4> "
                               "<stx:hex> <etx:hex> [len:dec]\r\n");
           return;
         }
         int slot = -1;
         if (!ew11ParseSlot(sock, args.get(2), slot, "frame"))
           return;
         uint8_t stx = static_cast<uint8_t>(strtoul(args.get(3), nullptr, 16));
         uint8_t etx = static_cast<uint8_t>(strtoul(args.get(4), nullptr, 16));
         uint8_t len = 0;
         if (argc >= 5) {
           int parsed_len = 0;
           if (CliFmt::ParseInt(args.get(5), parsed_len, 0, 255))
             len = static_cast<uint8_t>(parsed_len);
         }
         char ns[16], tag[16];
         snprintf(ns, sizeof(ns), "e%d_frame", slot);
         snprintf(tag, sizeof(tag), "EW11_#%d", slot);
         g_hub_slots[slot].tracker.setFixedLock(stx, etx, len);
         g_hub_slots[slot].tracker.saveToNvs(ns, tag);
         sendTelnetMsgf(sock,
                        "[OK] EW11 Slot #%d framing permanently fixed to STX "
                        "0x%02X, ETX 0x%02X, Len %u.\r\n",
                        slot, stx, etx, len);
       }},
      {"reset", "reset <slot>", "Reset EW11 socket slot to defaults",
       [](int sock, int, const Args &args) {
         int slot = -1;
         if (!ew11ParseSlot(sock, args.get(2), slot, "reset"))
           return;
         char ns[16], tag[16];
         snprintf(ns, sizeof(ns), "e%d_frame", slot);
         snprintf(tag, sizeof(tag), "EW11_#%d", slot);
         g_hub_slots[slot].tracker.clearNvs(ns, tag);
         sendTelnetMsgf(sock,
                        "[OK] EW11 Slot #%d framing tracker reset to "
                        "autonomous auto-probing.\r\n",
                        slot);
       }},
      {"enable", "enable <slot>", "Enable specified EW11 socket slot",
       [](int sock, int, const Args &args) {
         int slot = -1;
         if (ew11ParseSlot(sock, args.get(2), slot, "enable"))
           ew11SetEnable(sock, slot, true);
       }},
      {"disable", "disable <slot>", "Disable specified EW11 socket slot",
       [](int sock, int, const Args &args) {
         int slot = -1;
         if (ew11ParseSlot(sock, args.get(2), slot, "disable"))
           ew11SetEnable(sock, slot, false);
       }},
  };

  if (CliFmt::IsHelp(sub)) {
    CliFmt::PrintSubCmdHelp(
        sock, "EW11 COMMAND REFERENCE", kEw11Defs,
        sizeof(kEw11Defs) / sizeof(kEw11Defs[0]),
        "Tip: Slot index 0 is Master Hub, 1-4 are FCU Bridges");
    return;
  }

  if (CliFmt::DispatchSubCmd(sub, sock, argc, ctx.args, kEw11Defs,
                             sizeof(kEw11Defs) / sizeof(kEw11Defs[0])))
    return;

  CliFmt::PrintSubCmdHelp(
      sock, "EW11 COMMAND REFERENCE", kEw11Defs,
      sizeof(kEw11Defs) / sizeof(kEw11Defs[0]),
      "Tip: Slot index 0 is Master Hub, 1-4 are FCU Bridges");
}

void cmdRoutes(CliContext &ctx) {
  int sock = ctx.sock;
  int argc = ctx.args.count();

  if (argc == 1 && strcasecmp(ctx.args.get(1), "clear") == 0) {
    g_route_registry.clear();
    sendTelnetMsg(sock,
                  "[OK] Dynamic device ingress routing table cleared.\r\n");
    return;
  }

  static DeviceRouteEntry entries[DeviceRouteRegistry::MAX_ROUTES];
  size_t count =
      g_route_registry.getRoutes(entries, DeviceRouteRegistry::MAX_ROUTES);

  withScratchBuf(sock, [count](AppendBuf &out) {
    CliFmt::PrintBoxHeader(
        out, "DYNAMIC DEVICE INGRESS ROUTING TABLE (Zero Hardcode)");
    static constexpr Column ROUTES_COLS[] = {
        {"Target (DevID:Sub1:Sub2)", 24, Align::CENTER, Align::CENTER},
        {"Egress Destination", 33, Align::LEFT, Align::CENTER},
        {"Last Seen", 13, Align::CENTER, Align::CENTER},
    };
    TableRenderer table(out, ROUTES_COLS, 3);
    table.header(false);

    if (count == 0) {
      table.empty(
          "(No device routes learned yet. Waiting for bus/EW11 packets...)");
    } else {
      uint32_t now = millis();
      for (size_t i = 0; i < count; i++) {
        const auto &e = entries[i];
        char tgt_str[24], dst_str[36], el_str[20];
        snprintf(tgt_str, sizeof(tgt_str), "0x%02X:%02X:%02X", e.dev_id, e.sub1,
                 e.sub2);
        if (e.endpoint.channel_id == 5 && e.endpoint.slot_idx >= 0) {
          snprintf(dst_str, sizeof(dst_str), "CH#5 Slot %d",
                   e.endpoint.slot_idx);
        } else {
          snprintf(dst_str, sizeof(dst_str), "CH#%u", e.endpoint.channel_id);
        }
        Fmt::FormatElapsed(now, e.endpoint.last_seen_ms, el_str,
                           sizeof(el_str));
        table.row({tgt_str, dst_str, el_str});
      }
    }
    table.end('-');
    CliFmt::PrintBoxFooter(out,
                           "Use 'routes clear' to reset dynamic route table");
  });
}

} // namespace ConfigCli

// ============================================================================
// From src/CLI/CliStatus.cpp
// ============================================================================

namespace WallpadCli {

void wallpadPrintStatus(AppendBuf &out) {
  auto *active = WallpadParserFactory::getActiveParser();
  auto desc = g_auto_probing_engine.getDescriptor();
  VendorProfileDescriptor active_prof;
  bool is_manual_prof = false;
  if (ProfileRepository::getActiveProfile(active_prof) &&
      strcasecmp(active_prof.key, "auto") != 0) {
    is_manual_prof = true;
    desc.stx = active_prof.stx;
    desc.etx = active_prof.etx;
    desc.checksum_algo = active_prof.cs_algo;
    desc.opcode_offset = active_prof.opcode_offset;
    desc.query_opcode = active_prof.query_op;
    desc.control_opcode = active_prof.ctrl_op;
    desc.ack_opcode = active_prof.ack_op;
    desc.control_seen = (active_prof.ctrl_op != 0);
    desc.dev_id_offset = active_prof.dev_id_offset;
    desc.sub1_offset = active_prof.sub1_offset;
    desc.sub2_offset = active_prof.sub2_offset;
    desc.is_swapped_addr = (active_prof.is_swapped_addr != 0);
    desc.is_locked = true;
    desc.opcodes_locked = true;
    desc.offsets_locked = true;
    desc.payload_offset =
        std::max({active_prof.opcode_offset, active_prof.dev_id_offset,
                  active_prof.sub1_offset, active_prof.sub2_offset}) +
        1;
  }

  size_t active_targets = g_polling_targets.activeCount();
  size_t verified_targets = g_polling_targets.verifiedCount();
  size_t online_devs = g_device_repo.getOnlineCount();

  const char *phase_str = "Phase 1/3 (Framing Probing)";
  if (is_manual_prof || desc.offsets_locked) {
    phase_str = "Phase 3/3: Fully Locked";
  } else if (desc.is_locked) {
    phase_str = "Phase 2/3: Cache Syncing";
  }

  CliFmt::PrintBoxHeader(out, "WALLPAD PROTOCOL AUTO-PROBING ENGINE STATUS");
  char prof_key_buf[UniversalProtocolEngine::kProfileKeyMaxLen] = "Standard";
  if (active) {
    active->getActiveProfileKey(prof_key_buf, sizeof(prof_key_buf));
  }

  auto print_meta = [&](const char *fmt, ...) {
    char buf[128];
    va_list va;
    va_start(va, fmt);
    vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);
    out.appendFormat("| %-76.76s |\r\n", buf);
  };

  print_meta("Active Profile  : %s (ID: %u)", prof_key_buf,
             static_cast<unsigned>(g_config.wallpad_profile));
  if (g_config.wallpad_profile ==
      static_cast<uint8_t>(WallpadProfileIndex::ADAPTIVE)) {
    print_meta("Profile Mode    : Auto Adaptive [%s]", phase_str);
  } else {
    print_meta("Profile Mode    : Manual Fixed");
  }
  const auto *matched_p = ProfileMatcher::getActiveProfile();
  if (matched_p) {
    print_meta("Catalog Match   : %s (%u Devices Spec Injected)",
               matched_p->vendor_name,
               static_cast<unsigned>(matched_p->device_count));
  } else {
    print_meta("Catalog Match   : None (Generic Framing Only)");
  }
  print_meta(
      "Devices Tracked : %u Active / %u Online (%u Offline) [%s]",
      static_cast<unsigned>(active_targets), static_cast<unsigned>(online_devs),
      static_cast<unsigned>(g_device_repo.count() >= online_devs
                                ? (g_device_repo.count() - online_devs)
                                : 0),
      (online_devs >= active_targets && active_targets > 0) ? "100% Synced"
                                                            : "Syncing");

  static constexpr Column WP_COLS[] = {
      {"Packet Field", 12, Align::LEFT, Align::CENTER},
      {"Parameter", 13, Align::LEFT, Align::CENTER},
      {"Value / Layout Rule", 32, Align::LEFT, Align::CENTER},
      {"Status", 10, Align::CENTER, Align::CENTER},
  };
  TableRenderer table(out, WP_COLS, 4);
  table.header(false);

  auto print_row = [&](const char *f, const char *p, const char *v,
                       const char *s) {
    char clean_s[16] = {0};
    if (s && s[0] == '[' && s[strlen(s) - 1] == ']') {
      size_t slen = strlen(s);
      if (slen >= 2 && slen - 2 < sizeof(clean_s)) {
        strncpy(clean_s, s + 1, slen - 2);
        clean_s[slen - 2] = '\0';
      }
    } else if (s) {
      strncpy(clean_s, s, sizeof(clean_s) - 1);
    }
    table.row({f ? f : "", p ? p : "", v ? v : "", clean_s});
  };

  auto rowf = [&](const char *f, const char *p, const char *s, const char *fmt,
                  ...) {
    char v[64];
    va_list args;
    va_start(args, fmt);
    vsnprintf(v, sizeof(v), fmt, args);
    va_end(args);
    print_row(f, p, v, s);
  };

  uint8_t dev_ids[16], sub1_ids[16], sub2_ids[16];
  size_t dev_id_cnt = 0, sub1_cnt = 0, sub2_cnt = 0;

  constexpr uint8_t CH23_MASK = (1 << 2) | (1 << 3);
  size_t total_tgts = g_polling_targets.totalCount();
  for (size_t i = 0; i < total_tgts; ++i) {
    PollingTargetEntry entry;
    if (g_polling_targets.getEntry(i, entry)) {
      if ((entry.source_channels & CH23_MASK) == 0)
        continue;
      if (dev_id_cnt < 16 && std::find(dev_ids, dev_ids + dev_id_cnt,
                                       entry.dev_id) == dev_ids + dev_id_cnt) {
        dev_ids[dev_id_cnt++] = entry.dev_id;
      }
      if (sub1_cnt < 16 && std::find(sub1_ids, sub1_ids + sub1_cnt,
                                     entry.sub1) == sub1_ids + sub1_cnt) {
        sub1_ids[sub1_cnt++] = entry.sub1;
      }
      if (sub2_cnt < 16 && std::find(sub2_ids, sub2_ids + sub2_cnt,
                                     entry.sub2) == sub2_ids + sub2_cnt) {
        sub2_ids[sub2_cnt++] = entry.sub2;
      }
    }
  }
  std::sort(dev_ids, dev_ids + dev_id_cnt);
  std::sort(sub1_ids, sub1_ids + sub1_cnt);
  std::sort(sub2_ids, sub2_ids + sub2_cnt);

  auto format_hex_list = [](const uint8_t *arr, size_t cnt, const char *prefix,
                            char *out, size_t out_sz) {
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
    snprintf(out, out_sz, "%s : %s", prefix, hex_str);
  };

  const char *addr_status = desc.offsets_locked
                                ? "[LOCKED]"
                                : (desc.is_locked ? "[LEARNING]" : "[WAITING]");
  if (desc.offsets_locked) {
    if (desc.is_swapped_addr) {
      rowf("Addressing", "Addr Mode", addr_status,
           "Swapped (GW:Byte#%u <-> ID:Byte#%u)", desc.gw_addr_offset,
           desc.dev_id_offset);
    } else {
      print_row("Addressing", "Addr Mode", "Direct (Single Address)",
                addr_status);
    }
    if (desc.gw_addr_offset != 0xFF) {
      rowf("", "[GW] Master", addr_status, "Byte #%u : %02X",
           desc.gw_addr_offset, desc.gw_addr);
    } else {
      rowf("", "[GW] Master", addr_status, "Val: %02X", desc.gw_addr);
    }
  } else {
    print_row("Addressing", "Addr Mode",
              desc.is_locked ? "Probing..." : "Waiting", addr_status);
  }

  char sub1_list_buf[64];
  auto print_addr_field = [&](const char *param, uint8_t off,
                              const uint8_t *ids, size_t cnt,
                              char *saved_buf = nullptr) {
    char label[32], list_buf[64];
    if (desc.offsets_locked)
      snprintf(label, sizeof(label), "Byte #%u", off);
    else
      strcpy(label, "Probing...");
    format_hex_list(ids, cnt, label, list_buf, sizeof(list_buf));
    if (saved_buf)
      strcpy(saved_buf, list_buf);
    print_row("", param, list_buf, addr_status);
  };
  print_addr_field("[ID] Device", desc.dev_id_offset, dev_ids, dev_id_cnt);
  print_addr_field("[S1] Sub Addr", desc.sub1_offset, sub1_ids, sub1_cnt,
                   sub1_list_buf);
  if (desc.sub2_offset != 0xFF && desc.sub2_offset != desc.sub1_offset &&
      sub2_cnt > 0) {
    print_addr_field("[S2] Sub Addr", desc.sub2_offset, sub2_ids, sub2_cnt);
  }
  table.separator('-');

  char ctl_hex[8];
  if (desc.control_seen && desc.control_opcode != 0) {
    snprintf(ctl_hex, sizeof(ctl_hex), "%02X", desc.control_opcode);
  } else {
    strcpy(ctl_hex, "??");
  }

  const char *opcode_status = !desc.opcodes_locked ? "[LEARNING]"
                              : (!desc.control_seen || desc.control_opcode == 0)
                                  ? "[WAITING]"
                                  : "[LOCKED]";
  rowf("Command", "[OP] Opcode", opcode_status,
       "Byte #%u : QRY:%02X, CTL:%s, ACK:%02X", desc.opcode_offset,
       desc.query_opcode, ctl_hex, desc.ack_opcode);

  const char *seq_status =
      desc.offsets_locked ? (desc.has_seq_counter ? "[LOCKED]" : "[UNUSED]")
                          : "[WAITING]";
  if (desc.has_seq_counter) {
    rowf("", "Sequence", seq_status, "Byte #%u : +1 Counter", desc.seq_offset);
  } else {
    print_row("", "Sequence", desc.offsets_locked ? "-" : "None", seq_status);
  }
  print_row("", "[CX] Context", sub1_list_buf, addr_status);
  table.separator('-');

  const char *payload_status = desc.offsets_locked ? "[LOCKED]" : "[ESTIMATE]";
  char pl_r[32], pl_l[32];
  if (desc.offsets_locked) {
    snprintf(pl_r, sizeof(pl_r), "Byte #%u ~ #[N-3]", desc.payload_offset);
    snprintf(pl_l, sizeof(pl_l), "Data = [LEN - %u] Byte",
             desc.payload_offset + 2);
  } else {
    strcpy(pl_r, "Byte #7 ~ #[N-3] : Est");
    strcpy(pl_l, "Data = [LEN - 9] Byte : Est");
  }
  print_row("Payload", "[PL] Data Range", pl_r, payload_status);
  print_row("", "[PL] Length", pl_l, payload_status);
  table.separator('-');

  const char *tail_status = desc.is_locked ? "[LOCKED]" : "[LEARNING]";
  rowf("Tail", "[CS] Checksum", tail_status, "Byte #[N-2] : %s",
       AutoProbingEngine::getAlgoName(desc.checksum_algo));
  rowf("", "[ET] ETX", tail_status, "Byte #[N-1] : %02X",
       active ? active->getEtx() : 0xEE);
  table.separator('-');

  uint32_t b1 = g_config.uart_baud_rate, b2 = g_config.ch2_baud_rate,
           b3 = g_config.ch3_baud_rate;
  if (b1 == b2 && b2 == b3) {
    rowf("Bus Physical", "Baudrate", "[CONFIG]", "%u bps : CH1~3",
         static_cast<unsigned>(b1));
  } else {
    rowf("Bus Physical", "Baudrate", "[CONFIG]", "CH1:%u, CH2:%u, CH3:%u",
         static_cast<unsigned>(b1), static_cast<unsigned>(b2),
         static_cast<unsigned>(b3));
  }
  rowf("", "IPG Silence", "[CONFIG]", "%u ms : CH1~3",
       static_cast<unsigned>(Config::Timing::WALLPAD_AUTO_IPG_MS));
  table.separator('-');

  Config::Doorphone::FramingStatus dp_status =
      g_doorphone_tracker.status.load(std::memory_order_relaxed);
  const char *dp_status_str =
      (dp_status == Config::Doorphone::FramingStatus::LOCKED)     ? "[LOCKED]"
      : (dp_status == Config::Doorphone::FramingStatus::LEARNING) ? "[LEARNING]"
      : (dp_status == Config::Doorphone::FramingStatus::NOISY)    ? "[NOISY]"
                                                                  : "[WAITING]";

  uint8_t cur_dp_stx =
      g_doorphone_tracker.candidate_stx.load(std::memory_order_relaxed);
  uint8_t cur_dp_etx =
      g_doorphone_tracker.candidate_etx.load(std::memory_order_relaxed);
  uint8_t cur_dp_len =
      g_doorphone_tracker.candidate_len.load(std::memory_order_relaxed);

  if (dp_status == Config::Doorphone::FramingStatus::WAITING) {
    print_row("Doorphone (CH4)", "Framing", "-- .. --", dp_status_str);
  } else if (cur_dp_len > 0) {
    rowf("Doorphone (CH4)", "Framing", dp_status_str, "%02X .. %02X (%u Bytes)",
         cur_dp_stx, cur_dp_etx, cur_dp_len);
  } else {
    rowf("Doorphone (CH4)", "Framing", dp_status_str, "%02X .. %02X",
         cur_dp_stx, cur_dp_etx);
  }

  const DoorphoneSpec *dp_prof =
      ProfileMatcher::matchDoorphone(cur_dp_stx, cur_dp_etx, cur_dp_len);
  const char *dp_m_st =
      dp_prof ? ((dp_status == Config::Doorphone::FramingStatus::LOCKED)
                     ? "[LOCKED]"
                     : "[LEARNING]")
              : ((dp_status == Config::Doorphone::FramingStatus::WAITING)
                     ? "[WAITING]"
                     : "[UNKNOWN]");
  char op_f[48], op_l[48];
  const char *dp_desc = nullptr;
  if (dp_prof) {
    dp_desc = dp_prof->desc;
    snprintf(op_f, sizeof(op_f), "Bell:%02X, Call:%02X, Open:%02X, End:%02X",
             dp_prof->bell_front, dp_prof->call_front, dp_prof->open_front,
             dp_prof->end_front);
    snprintf(op_l, sizeof(op_l), "Bell:%02X, Call:%02X, Open:%02X, End:%02X",
             dp_prof->bell_lobby, dp_prof->call_lobby, dp_prof->open_lobby,
             dp_prof->end_lobby);
  } else if (dp_status == Config::Doorphone::FramingStatus::WAITING) {
    dp_desc = "Waiting for traffic...";
    strcpy(op_f, "Waiting...");
    strcpy(op_l, "Waiting...");
  } else {
    dp_desc = "No Catalog Match";
    strcpy(op_f, "Bell:B5, Call:B9, Open:B4, End:B8");
    strcpy(op_l, "Bell:5A, Call:5F, Open:61, End:60");
  }
  print_row("", "Catalog Match", dp_desc, dp_m_st);
  print_row("", "Opcodes(F)", op_f, dp_m_st);
  print_row("", "Opcodes(L)", op_l, dp_m_st);

  rowf("", "Baudrate", "[CONFIG]", "%u bps",
       static_cast<unsigned>(g_config.doorphone_baud_rate));
  rowf("", "Time-gap", "[CONFIG]", "%u ms",
       static_cast<unsigned>(Config::Timing::DOORPHONE_IPG_MS));
  rowf("", "Debounce", "[CONFIG]", "%u ms",
       static_cast<unsigned>(Config::Timing::DOORPHONE_DEBOUNCE_MS));
  table.separator('-');

  {
    MutexLocker lock(g_ch5_mutex);
    for (int s = 0; s < Config::TCP::MAX_EW11_SLOTS; s++) {
      const auto &slot = g_hub_slots[s];
      char p_buf[16], val_buf[32];
      snprintf(p_buf, sizeof(p_buf), "%u",
               slot.target_port ? slot.target_port
                                : Config::TCP::EW11_SLOT_PORTS[s]);
      const char *f_label = (s == 0) ? "EW11 (CH5)" : "";
      const char *st = "[WAITING]";
      if (!slot.enabled && !slot.is_connected && slot.target_ip[0] == '\0') {
        strcpy(val_buf, "Disabled");
        st = "[UNUSED]";
      } else if (slot.is_connected) {
        snprintf(val_buf, sizeof(val_buf), "Connect: %s",
                 slot.target_ip[0] ? slot.target_ip : "-");
        st = "[ACTIVE]";
      } else {
        strcpy(val_buf, "Listening");
      }
      print_row(f_label, p_buf, val_buf, st);
    }
  }
  table.separator('-');

  uint32_t conv_pct =
      active_targets ? (verified_targets * 100 / active_targets) : 0;
  const char *conv_status =
      (verified_targets >= active_targets && active_targets > 0)
          ? "[SYNCED]"
          : (desc.is_locked ? "[SYNCING]" : "[WAITING]");
  rowf("Runtime Sync", "Cache Sync", conv_status, "%u / %u Targets (%u%%)",
       static_cast<unsigned>(verified_targets),
       static_cast<unsigned>(active_targets), static_cast<unsigned>(conv_pct));

  uint32_t cs_pct = desc.tested_packets
                        ? (desc.matched_packets * 100 / desc.tested_packets)
                        : 100;
  auto format_compact = [](char *buf, size_t sz, uint32_t count) {
    if (count >= 1000000)
      snprintf(buf, sz, "%.1fM", count / 1000000.0);
    else if (count >= 1000)
      snprintf(buf, sz, "%.1fk", count / 1000.0);
    else
      snprintf(buf, sz, "%u", static_cast<unsigned>(count));
  };
  char m_str[16], t_str[16];
  format_compact(m_str, sizeof(m_str), desc.matched_packets);
  format_compact(t_str, sizeof(t_str), desc.tested_packets);
  const char *cs_status =
      (cs_pct >= 95) ? "[STABLE]" : (cs_pct >= 80 ? "[NOISY]" : "[ERROR]");
  rowf("", "CS Validation", cs_status, "%s / %s Packets (%u%%)", m_str, t_str,
       static_cast<unsigned>(cs_pct));
  table.end('=');
  out.append("\r\n");
}

void wallpadListProfiles(AppendBuf &out) {
  CliFmt::PrintBoxHeader(out, "WALLPAD PROTOCOL PROFILES");
  static constexpr Column PROFILE_COLS[] = {
      {"ID", 4, Align::CENTER, Align::CENTER},
      {"Profile Key", 11, Align::LEFT, Align::CENTER},
      {"Protocol Specification / Description", 36, Align::LEFT, Align::CENTER},
      {"Status", 16, Align::CENTER, Align::CENTER},
  };
  TableRenderer table(out, PROFILE_COLS, 4);
  table.header(false);

  for (size_t i = 0; i < ProfileRepository::getProfileCount(); ++i) {
    VendorProfileDescriptor p_desc;
    if (ProfileRepository::getProfile(i, p_desc)) {
      bool is_current = (g_config.wallpad_profile == i);
      bool is_empty = (i > 0 && strncmp(p_desc.name, "[Empty", 6) == 0);
      const char *status_str = is_current
                                   ? ">> ACTIVE <<"
                                   : (is_empty ? "Available" : "Saved (NVS)");
      char id_buf[8];
      snprintf(id_buf, sizeof(id_buf), "%2u", static_cast<unsigned>(i));
      table.row({id_buf, p_desc.key, p_desc.name, status_str});
    }
  }
  table.end('-');
  CliFmt::PrintBoxFooter(
      out, "Use 'wallpad set <id>' to switch, 'wallpad save <name>' to store");
}

void wallpadSaveProfile(int sock, const char *name) {
  if (!name || strlen(name) == 0) {
    sendTelnetMsg(
        sock, "[ERROR] Missing profile name: wallpad save <profile_name>\r\n");
    return;
  }
  size_t saved_slot = 0;
  if (ProfileRepository::saveCurrentAutoAs(name, saved_slot)) {
    sendTelnetMsgf(sock,
                   "[OK] Successfully saved current Auto profile as '%s' (Slot "
                   "#%u) in NVS!\r\n",
                   name, static_cast<unsigned>(saved_slot));
  } else {
    sendTelnetMsg(sock, "[ERROR] Failed to save profile to NVS.\r\n");
  }
}

void wallpadDeleteProfile(int sock, const char *target) {
  if (!target) {
    sendTelnetMsg(sock, "[ERROR] Missing target: wallpad delete <name|id>\r\n");
    return;
  }
  char *endp = nullptr;
  long val = strtol(target, &endp, 10);
  size_t idx = 999;
  if (endp != target && *endp == '\0' && val >= 1 &&
      val < static_cast<long>(ProfileRepository::getProfileCount())) {
    idx = static_cast<size_t>(val);
  } else {
    VendorProfileDescriptor pd;
    for (size_t i = 1; i < ProfileRepository::getProfileCount(); ++i) {
      if (ProfileRepository::getProfile(i, pd) &&
          strcasecmp(pd.key, target) == 0) {
        idx = i;
        break;
      }
    }
  }
  if (idx >= 1 && idx < ProfileRepository::getProfileCount()) {
    ProfileRepository::deleteProfile(idx);
    sendTelnetMsgf(sock, "[OK] Custom profile (Slot #%u) reset to empty.\r\n",
                   static_cast<unsigned>(idx));
  } else {
    sendTelnetMsgf(
        sock, "[ERROR] Cannot delete '%s' (Slot 0 is protected Auto slot).\r\n",
        target);
  }
}

void wallpadSetProfile(int sock, const char *key) {
  if (!key) {
    sendTelnetMsg(sock,
                  "[ERROR] Missing profile key/id: wallpad set <key|id>\r\n");
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
    char v_name[UniversalProtocolEngine::kVendorNameMaxLen] = {0};
    char p_key[UniversalProtocolEngine::kProfileKeyMaxLen] = {0};
    if (new_p) {
      new_p->getVendorName(v_name, sizeof(v_name));
      new_p->getActiveProfileKey(p_key, sizeof(p_key));
    }
    sendTelnetMsgf(
        sock, "[OK] Wallpad profile changed to '%s' (%s) and saved to NVS.\r\n",
        v_name[0] ? v_name : key, p_key[0] ? p_key : key);
  } else {
    sendTelnetMsgf(sock,
                   "[ERROR] Unknown vendor profile '%s'. Use 'wallpad list' to "
                   "see available profiles.\r\n",
                   key);
  }
}

} // namespace WallpadCli

// ============================================================================
// From src/CLI/CliControl.cpp
// ============================================================================

namespace WallpadCli {

void cmdTrace(CliContext &ctx) {
  int sock = ctx.sock;
  int token_count = ctx.args.count();
  const char *sub = (token_count > 0) ? ctx.args.get(1) : "on";

  static const CliFmt::SubCmdDef kTraceHelp[] = {
      {nullptr, "on / off", "Start or stop real-time packet stream", nullptr},
      {nullptr, "ctl / ack / pol / rmt / drp", "Toggle packet type filter",
       nullptr},
      {nullptr, "ch <1-6>", "Filter packets by hardware channel", nullptr},
      {nullptr, "devid <hex>", "Filter packets by target device ID", nullptr}};

  if (CliFmt::IsHelp(sub)) {
    CliFmt::PrintSubCmdHelp(
        sock, "TRACE COMMAND REFERENCE", kTraceHelp,
        sizeof(kTraceHelp) / sizeof(kTraceHelp[0]),
        "Tip: Use 'q' shortcut to quickly stop active tracing");
    return;
  }

  if (strcasecmp(sub, "off") == 0) {
    g_telnet_tracer.setTrace(false);
    sendTelnetMsg(sock, "Packet trace DISABLED.\r\n");
    return;
  }

  g_telnet_tracer.setClient(sock);
  g_telnet_tracer.setTrace(true);

  struct TraceFilterDef {
    const char *key;
    TraceType type;
    const char *desc;
  };
  static constexpr TraceFilterDef kTraceFilters[] = {
      {"on", TraceType::ALL, "ALL packets"},
      {"ctl", TraceType::CTL, "CONTROL packets only"},
      {"ack", TraceType::ACK, "ACK/Response packets only"},
      {"pol", TraceType::QRY, "Polling queries only"},
      {"rmt", TraceType::RMT, "Doorphone packets only"},
      {"drp", TraceType::DRP, "Dropped packets only"},
  };

  for (const auto &f : kTraceFilters) {
    if (strcasecmp(sub, f.key) == 0) {
      g_telnet_tracer.setFilter(f.type);
      sendTelnetMsgf(sock, "Packet trace ENABLED: %s.\r\n", f.desc);
      return;
    }
  }

  if (strcasecmp(sub, "ch") == 0 ||
      (strncasecmp(sub, "ch", 2) == 0 &&
       isdigit(static_cast<unsigned char>(sub[2])))) {
    int ch_val = 0;
    bool ch_ok = false;
    if (strcasecmp(sub, "ch") == 0 && token_count >= 2) {
      ch_ok = CliFmt::ParseInt(ctx.args.get(2), ch_val, 1, 6);
    } else if (strncasecmp(sub, "ch", 2) == 0 &&
               isdigit(static_cast<unsigned char>(sub[2]))) {
      ch_val = sub[2] - '0';
      ch_ok = (ch_val >= 1 && ch_val <= 6);
    }
    if (ch_ok) {
      g_telnet_tracer.setFilter(TraceType::CH, static_cast<uint8_t>(ch_val));
      sendTelnetMsgf(sock, "Packet trace ENABLED: Channel %u only.\r\n",
                     static_cast<unsigned>(ch_val));
    } else {
      sendTelnetMsg(sock, "[ERROR] Invalid channel: trace ch <1-6>\r\n");
    }
  } else if (strcasecmp(sub, "devid") == 0 || strncasecmp(sub, "0x", 2) == 0) {
    uint8_t id = 0;
    if (strcasecmp(sub, "devid") == 0 && token_count >= 2) {
      id = static_cast<uint8_t>(strtol(ctx.args.get(2), nullptr, 16));
    } else if (strncasecmp(sub, "0x", 2) == 0) {
      id = static_cast<uint8_t>(strtol(sub, nullptr, 16));
    }
    g_telnet_tracer.setFilter(TraceType::DEVID, id);
    sendTelnetMsgf(sock, "Packet trace ENABLED: Device ID 0x%02X only.\r\n",
                   id);
    CliFmt::PrintSubCmdHelp(
        sock, "TRACE COMMAND REFERENCE", kTraceHelp,
        sizeof(kTraceHelp) / sizeof(kTraceHelp[0]),
        "Tip: Use 'q' shortcut to quickly stop active tracing");
  }
}

void cmdStop(CliContext &ctx) {
  int sock = ctx.sock;
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
  if (src_mask & (1 << 5)) {
    if (idx > 0 && idx < buf_len)
      idx += snprintf(buf + idx, buf_len - idx, "+");
    idx += snprintf(buf + idx, buf_len - idx, "CH5");
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
  const char *wc_src_str = (g_warm_cache_source == 1)   ? "RTC SRAM"
                           : (g_warm_cache_source == 2) ? "NVS Flash"
                                                        : "Cold Start";

  CliFmt::PrintBoxHeader(out,
                         "[1ST-TIER CACHE] DYNAMIC POLLING TARGET REGISTRY");

  char sub_buf[78];
  snprintf(sub_buf, sizeof(sub_buf),
           "Active Targets: %zu | Tracked: %zu | Warm Cache: %s (%u)",
           tgt_active, tgt_total, wc_src_str,
           static_cast<unsigned>(g_warm_cache_restored_count));
  CliFmt::PrintBoxSubtitle(out, sub_buf);

  static constexpr Column TIER1_COLS[] = {
      {"No", 3, Align::CENTER, Align::CENTER},
      {"Last", 8, Align::CENTER, Align::CENTER},
      {"Src", 5, Align::CENTER, Align::CENTER},
      {"Raw Query Packet Frame", 51, Align::LEFT, Align::LEFT},
  };
  TableRenderer table(out, TIER1_COLS, 4);
  table.header(false);

  constexpr uint8_t ALLOWED_MASK = (1 << 2) | (1 << 3) | (1 << 5);

  if (tgt_total == 0) {
    table.empty("(No polling targets registered yet. Waiting for queries...)");
  } else {
    unsigned int display_idx = 1;
    for (size_t i = 0; i < tgt_total; ++i) {
      PollingTargetEntry tgt;
      if (!g_polling_targets.getEntry(i, tgt))
        continue;
      if (tgt.source_channels != 0 &&
          (tgt.source_channels & ALLOWED_MASK) == 0) {
        continue;
      }

      char src_buf[16] = {0};
      formatSources(tgt.source_channels, src_buf, sizeof(src_buf));

      char last_req_str[16] = {0};
      Fmt::FormatElapsed(now, tgt.last_requested_ms, last_req_str,
                         sizeof(last_req_str));

      char q_hex[64] = {0};
      if (tgt.raw_query_len > 0) {
        Fmt::FormatHex(tgt.raw_query_data.data(), tgt.raw_query_len, q_hex,
                       sizeof(q_hex));
      } else {
        snprintf(q_hex, sizeof(q_hex), "DevID 0x%02X (Sub1 0x%02X)", tgt.dev_id,
                 tgt.sub1);
      }

      char no_s[8];
      snprintf(no_s, sizeof(no_s), "#%02u", display_idx++);
      table.row({no_s, last_req_str, src_buf, q_hex});
    }
  }
  table.end('-');
  CliFmt::PrintBoxFooter(
      out, "Tip: 1st-Tier cache monitors active queries from Wallpad & App");
}

void devsPrintTier2Cache(AppendBuf &out, uint32_t now) {
  size_t total_count = g_device_repo.count();
  size_t online_count = g_device_repo.getOnlineCount();

  CliFmt::PrintBoxHeader(out,
                         "[2ND-TIER CACHE] PHYSICAL DEVICE HEALTH MONITOR");

  char sub_buf[78];
  snprintf(sub_buf, sizeof(sub_buf),
           "Discovered: %zu Nodes on Bus | Online [OK]: %zu | Offline: %zu",
           total_count, online_count,
           (total_count >= online_count) ? (total_count - online_count) : 0);
  CliFmt::PrintBoxSubtitle(out, sub_buf);

  static constexpr Column TIER2_COLS[] = {
      {"No", 3, Align::CENTER, Align::CENTER},
      {"Last", 9, Align::CENTER, Align::CENTER},
      {"Raw Physical ACK Response Frame", 58, Align::LEFT, Align::LEFT},
  };
  TableRenderer table(out, TIER2_COLS, 3);
  table.header(false);

  if (total_count == 0) {
    table.empty("(No physical devices discovered on RS-485 bus yet)");
  } else {
    for (size_t i = 0; i < total_count; ++i) {
      DeviceStateEntry dev;
      if (!g_device_repo.getSnapshot(i, dev) || dev.dev_id == 0)
        continue;

      char ack_hex[96] = {0};
      if (dev.last_ack_len > 0) {
        Fmt::FormatHex(dev.last_ack_data.data(), dev.last_ack_len, ack_hex,
                       sizeof(ack_hex));
      } else {
        snprintf(ack_hex, sizeof(ack_hex), "(No ACK received from bus yet)");
      }

      char updated_str[16] = "-";
      if (dev.last_updated_ms > 0) {
        Fmt::FormatElapsed(now, dev.last_updated_ms, updated_str,
                           sizeof(updated_str));
      }

      char no_s[8];
      snprintf(no_s, sizeof(no_s), "#%02u", static_cast<unsigned int>(i + 1));
      table.row({no_s, updated_str, ack_hex});
    }
  }
  table.end('-');
  CliFmt::PrintBoxFooter(
      out, "Tip: 2nd-Tier cache reflects physical responses on RS-485");
}

void devsPrintSummary(AppendBuf &out, uint32_t now) {
  CliFmt::PrintBoxHeader(out, "REGISTERED DEVICE REGISTRY");

  struct DevSummary {
    uint8_t dev_id{0};
    char name[16]{0};
    char cls_str[10]{0};
    bool online{false};
    uint8_t sub_cnt{0};
    uint32_t qry_cnt{0};
    uint32_t last_seen_ms{0};
  };

  DevSummary devs[16]{};
  size_t dev_count = 0;

  size_t tgt_total = g_polling_targets.totalCount();
  for (size_t i = 0; i < tgt_total; ++i) {
    PollingTargetEntry tgt;
    if (!g_polling_targets.getEntry(i, tgt) || tgt.dev_id == 0)
      continue;

    DevSummary *found = nullptr;
    for (size_t d = 0; d < dev_count; ++d) {
      if (devs[d].dev_id == tgt.dev_id) {
        found = &devs[d];
        break;
      }
    }
    if (!found && dev_count < 16) {
      found = &devs[dev_count++];
      found->dev_id = tgt.dev_id;
    }
    if (found) {
      found->sub_cnt++;
      found->qry_cnt += tgt.hit_count;
      if (tgt.is_active)
        found->online = true;
      if (tgt.last_requested_ms > found->last_seen_ms)
        found->last_seen_ms = tgt.last_requested_ms;
    }
  }

  for (size_t d = 0; d < dev_count; ++d) {
    GroupControlTemplate grp{};
    if (g_control_registry.findGroup(devs[d].dev_id, grp)) {
      if (grp.group_name[0])
        strncpy(devs[d].name, grp.group_name, sizeof(devs[d].name) - 1);
      const char *c_str = DeviceClassToCliString(grp.coverage.dev_class);
      if (c_str)
        strncpy(devs[d].cls_str, c_str, sizeof(devs[d].cls_str) - 1);
    }
    if (devs[d].name[0] == '\0')
      snprintf(devs[d].name, sizeof(devs[d].name), "Dev_0x%02X",
               devs[d].dev_id);
    if (devs[d].cls_str[0] == '\0')
      strncpy(devs[d].cls_str, "DEVICE", sizeof(devs[d].cls_str) - 1);
  }

  char sub_buf[78] = {0};
  snprintf(sub_buf, sizeof(sub_buf),
           "Active Devices: %zu Cached | Ingress: CH1 Wallpad RS-485",
           dev_count);
  CliFmt::PrintBoxSubtitle(out, sub_buf);

  static constexpr Column DEVS_COLS[] = {
      {"DevID", 6, Align::LEFT, Align::CENTER},
      {"Name", 10, Align::LEFT, Align::CENTER},
      {"Class", 6, Align::LEFT, Align::CENTER},
      {"State", 7, Align::LEFT, Align::CENTER},
      {"Ch", 3, Align::LEFT, Align::CENTER},
      {"SubCnt", 6, Align::CENTER, Align::CENTER},
      {"QryCnt", 9, Align::CENTER, Align::CENTER},
      {"LastSeen", 8, Align::CENTER, Align::CENTER},
  };
  TableRenderer table(out, DEVS_COLS, 8);
  table.header(false);

  if (dev_count == 0) {
    table.empty("(No active devices registered in 1st/2nd tier cache)");
  } else {
    for (size_t d = 0; d < dev_count; ++d) {
      char dev_hex[10], sub_str[10], qry_str[12], elapsed_raw[16] = "-";
      snprintf(dev_hex, sizeof(dev_hex), "0x%02X", devs[d].dev_id);
      snprintf(sub_str, sizeof(sub_str), "%u", devs[d].sub_cnt);
      snprintf(qry_str, sizeof(qry_str), "%lu", (unsigned long)devs[d].qry_cnt);
      if (devs[d].last_seen_ms > 0) {
        Fmt::FormatElapsed(now, devs[d].last_seen_ms, elapsed_raw,
                           sizeof(elapsed_raw));
        char *ago_pos = strstr(elapsed_raw, " ago");
        if (ago_pos)
          *ago_pos = '\0';
        else if ((ago_pos = strstr(elapsed_raw, "ago")) != nullptr)
          *ago_pos = '\0';
      }
      table.row({dev_hex, devs[d].name, devs[d].cls_str,
                 devs[d].online ? "ONLINE" : "OFFLINE", "CH1", sub_str, qry_str,
                 elapsed_raw});
    }
  }
  table.end('-');
  CliFmt::PrintBoxFooter(
      out, "Use 'devs 1' for Polling Targets, 'devs 2' for Raw ACK Cache");
}

void cmdDevs(CliContext &ctx) {
  int client = ctx.sock;
  int argc = ctx.args.count();
  const char *sub = (argc > 0) ? ctx.args.get(1) : "summary";

  static const CliFmt::SubCmdDef kDevsDefs[] = {
      {"summary", "summary", "Show summary list of registered devices",
       [](int s, int, const Args &) {
         withScratchBuf(
             s, [](AppendBuf &out) { devsPrintSummary(out, millis()); });
       }},
      {"1", "1", "Show 1st-tier dynamic polling targets",
       [](int s, int, const Args &) {
         withScratchBuf(
             s, [](AppendBuf &out) { devsPrintTier1Targets(out, millis()); });
       }},
      {"2", "2", "Show 2nd-tier physical ACK response cache",
       [](int s, int, const Args &) {
         withScratchBuf(
             s, [](AppendBuf &out) { devsPrintTier2Cache(out, millis()); });
       }},
      {"all", "all", "Display summary, targets, and ACK cache all at once",
       [](int s, int, const Args &) {
         withScratchBuf(s, [](AppendBuf &out) {
           uint32_t now = millis();
           devsPrintSummary(out, now);
           devsPrintTier1Targets(out, now);
           devsPrintTier2Cache(out, now);
         });
       }},
      {"clear", "clear",
       "Clear all 1st-tier targets and 2nd-tier device caches",
       [](int s, int, const Args &) {
         g_polling_targets.clear();
         g_device_repo.clear();
         sendTelnetMsg(s, "All 1st-tier & 2nd-tier device caches CLEARED.\r\n");
       }},
  };

  if (CliFmt::DispatchSubCmd(sub, client, argc, ctx.args, kDevsDefs,
                             sizeof(kDevsDefs) / sizeof(kDevsDefs[0])))
    return;

  withScratchBuf(client,
                 [](AppendBuf &out) { devsPrintSummary(out, millis()); });
}

void cmdWallpad(CliContext &ctx) {
  int sock = ctx.sock;
  int argc = ctx.args.count();
  const char *sub = (argc > 0) ? ctx.args.get(1) : "status";

  if (argc == 0 || strcasecmp(sub, "status") == 0) {
    withScratchBuf(sock, [](AppendBuf &out) { wallpadPrintStatus(out); });
    return;
  }

  // Unified table: help strings + handlers in one place.
  static const CliFmt::SubCmdDef kWallpadDefs[] = {
      {"status", "status", "Show auto-probing status & locked profile",
       nullptr}, // handled above
      {"list", "list", "List available vendor & saved NVS profiles",
       [](int s, int, const Args &) {
         withScratchBuf(s, [](AppendBuf &out) { wallpadListProfiles(out); });
       }},
      {"set", "set <key|id>", "Manually switch active wallpad profile",
       [](int s, int ac, const Args &a) {
         if (ac >= 2)
           wallpadSetProfile(s, a.get(2));
         else
           sendTelnetMsg(
               s, "[ERROR] Missing profile key/id: wallpad set <key|id>\r\n");
       }},
      {"save", "save <name>", "Save learned profile to NVS custom slot",
       [](int s, int ac, const Args &a) {
         if (ac >= 2)
           wallpadSaveProfile(s, a.get(2));
         else
           sendTelnetMsg(s, "[ERROR] Missing name: wallpad save <name>\r\n");
       }},
      {"delete", "delete <id>", "Reset a saved custom profile slot in NVS",
       [](int s, int ac, const Args &a) {
         if (ac >= 2)
           wallpadDeleteProfile(s, a.get(2));
         else
           sendTelnetMsg(s,
                         "[ERROR] Missing profile id: wallpad delete <id>\r\n");
       }},
      {"auto", "auto", "Switch to Universal Auto-Probing mode",
       [](int s, int, const Args &) {
         char dp_ns[16];
         Config::Doorphone::FramingTracker::getNvsNamespace(0, dp_ns,
                                                            sizeof(dp_ns));
         ProfileRepository::setActiveProfileIndex(0);
         g_auto_probing_engine.reset();
         g_doorphone_tracker.clearNvs(dp_ns);
         sendTelnetMsg(s, "[OK] Switched to Universal Auto-Probing mode "
                          "(Wallpad & Doorphone framing reset).\r\n");
       }},
      {"reset", "reset", "Reset auto-probing engine and re-learn",
       [](int s, int, const Args &) {
         char dp_ns[16];
         Config::Doorphone::FramingTracker::getNvsNamespace(
             g_config.wallpad_profile, dp_ns, sizeof(dp_ns));
         g_auto_probing_engine.reset();
         g_doorphone_tracker.clearNvs(dp_ns);
         g_probe_convergence_reset.store(true, std::memory_order_release);
       }},
      {"simulate", "simulate <hex...>",
       "Inject raw hex packet into probing engine",
       [](int s, int ac, const Args &a) {
         if (ac < 2) {
           sendTelnetMsg(
               s, "[ERROR] Missing bytes: wallpad simulate <hex_bytes...>\r\n");
           return;
         }
         uint8_t sim_buf[64]{0};
         size_t sim_len = 0;
         for (int i = 2; i <= ac && sim_len < sizeof(sim_buf); ++i) {
           const char *tok = a.get(i);
           if (!tok || !tok[0])
             break;
           char *endp = nullptr;
           unsigned long val = strtoul(tok, &endp, 16);
           if (endp != tok)
             sim_buf[sim_len++] = static_cast<uint8_t>(val);
         }
         if (sim_len < 3) {
           sendTelnetMsg(
               s, "[ERROR] Simulated packet must be at least 3 bytes.\r\n");
           return;
         }
         g_auto_probing_engine.feedFrame(span<const uint8_t>(sim_buf, sim_len));
         sendTelnetMsgf(
             s, "[OK] Fed %u simulated bytes into Auto-Probing Engine.\r\n",
             sim_len);
       }},
  };

  if (CliFmt::IsHelp(sub)) {
    CliFmt::PrintSubCmdHelp(
        sock, "WALLPAD COMMAND REFERENCE", kWallpadDefs,
        sizeof(kWallpadDefs) / sizeof(kWallpadDefs[0]),
        "Tip: Use 'ctl' for device control blueprints & slots");
    return;
  }

  if (CliFmt::DispatchSubCmd(sub, sock, argc, ctx.args, kWallpadDefs,
                             sizeof(kWallpadDefs) / sizeof(kWallpadDefs[0])))
    return;

  CliFmt::PrintSubCmdHelp(
      sock, "WALLPAD COMMAND REFERENCE", kWallpadDefs,
      sizeof(kWallpadDefs) / sizeof(kWallpadDefs[0]),
      "Tip: Use 'ctl' for device control blueprints & slots");
}
static void ctlHandleName(int sock, int argc, const Args &args) {
  if (argc >= 3) {
    uint8_t dev_id = static_cast<uint8_t>(strtoul(args.get(2), nullptr, 0));
    const char *name = args.get(3);
    if (dev_id == 0 || !name || !*name) {
      sendTelnetMsg(
          sock, "[ERROR] Missing name: ctl name <dev_id> <custom_name>\r\n");
    } else if (g_control_registry.setGroupName(dev_id, name)) {
      sendTelnetMsgf(sock,
                     "[OK] DevID 0x%02X group name set to '%s' and saved to "
                     "NVS flash.\r\n",
                     dev_id, name);
    } else {
      sendTelnetMsgf(
          sock, "[ERROR] DevID 0x%02X not found in blueprint registry.\r\n",
          dev_id);
    }
  } else {
    sendTelnetMsg(
        sock, "[ERROR] Missing argument: ctl name <dev_id> <custom_name>\r\n");
  }
}

static void ctlHandleClass(int sock, int argc, const Args &args) {
  if (argc >= 3) {
    uint8_t dev_id = static_cast<uint8_t>(strtoul(args.get(2), nullptr, 0));
    const char *cls_str = args.get(3);
    const char *custom_name = (argc >= 4) ? args.get(4) : nullptr;
    struct DeviceClassEntry {
      const char *key;
      DeviceClass cls;
      const char *def_name;
    };
    static constexpr DeviceClassEntry kDeviceClassTable[] = {
        {"light", DeviceClass::SWITCH, "Light"},
        {"switch", DeviceClass::SWITCH, "Light"},
        {"outlet", DeviceClass::SWITCH, "Outlet"},
        {"vent", DeviceClass::VENT, "Vent"},
        {"fan", DeviceClass::VENT, "Vent"},
        {"thermo", DeviceClass::THERMOSTAT, "Thermo"},
        {"thermostat", DeviceClass::THERMOSTAT, "Thermo"},
        {"heat", DeviceClass::THERMOSTAT, "Thermo"},
        {"gas", DeviceClass::GAS, "Gas"},
        {"aircon", DeviceClass::AIRCON, "Aircon"},
        {"ac", DeviceClass::AIRCON, "Aircon"},
        {"ev", DeviceClass::MOMENTARY, "Elevator"},
        {"elevator", DeviceClass::MOMENTARY, "Elevator"},
    };
    DeviceClass cls = DeviceClass::UNKNOWN;
    const char *def_name = cls_str;
    for (const auto &entry : kDeviceClassTable) {
      if (strcasecmp(cls_str, entry.key) == 0) {
        cls = entry.cls;
        def_name = entry.def_name;
        break;
      }
    }
    if (dev_id == 0 || cls == DeviceClass::UNKNOWN) {
      sendTelnetMsg(sock,
                    "[ERROR] Missing class: ctl class <dev_id> "
                    "<light|outlet|vent|thermo|gas|aircon|ev> [name]\r\n");
    } else {
      const char *final_name =
          (custom_name && strlen(custom_name) > 0) ? custom_name : def_name;
      if (g_control_registry.setGroupClass(dev_id, cls, final_name)) {
        sendTelnetMsgf(sock,
                       "[OK] DevID 0x%02X class set to %s ('%s') and saved to "
                       "NVS flash.\r\n",
                       dev_id, cls_str, final_name);
      } else {
        sendTelnetMsgf(
            sock, "[ERROR] DevID 0x%02X not found in blueprint registry.\r\n",
            dev_id);
      }
    }
  } else {
    sendTelnetMsg(
        sock,
        "[ERROR] Missing argument: ctl class <dev_id> <class> [name]\r\n");
  }
}

void cmdCtl(CliContext &ctx) {
  int sock = ctx.sock;
  if (sock < 0)
    return;
  int argc = ctx.args.count();
  if (argc == 0) {
    withScratchBuf(sock, [](AppendBuf &out) { wallpadPrintControlTable(out); });
    return;
  }

  static const CliFmt::SubCmdDef kCtlDefs[] = {
      {nullptr, "ctl", "Display control blueprint table", nullptr},
      {nullptr, "<dev_id>", "Inspect blueprint & slots (e.g. ctl 0x18)",
       nullptr},
      {"name", "name <id> <name>", "Set custom group name", ctlHandleName},
      {"setname", nullptr, nullptr, ctlHandleName},
      {"class", "class <id> <cls> [name]",
       "Set device class (light/outlet/vent...)", ctlHandleClass},
      {"setclass", nullptr, nullptr, ctlHandleClass},
      {"reset", "reset <id>", "Reset action slots for device",
       [](int s, int ac, const Args &a) {
         if (ac < 2) {
           sendTelnetMsg(
               s, "[ERROR] Missing target: ctl reset <all | dev_id>\r\n");
           return;
         }
         const char *arg = a.get(2);
         if (strcasecmp(arg, "all") == 0) {
           g_control_registry.resetGroup(0, true);
           sendTelnetMsg(s, "[OK] All control blueprints reset & "
                            "re-synthesized from catalog specs.\r\n");
         } else {
           char *endp = nullptr;
           uint8_t dev_id = static_cast<uint8_t>(strtoul(arg, &endp, 0));
           if (endp == arg || dev_id == 0) {
             sendTelnetMsgf(s,
                            "[ERROR] Invalid target '%s'. Use 'ctl reset all' "
                            "or 'ctl reset <dev_id>'.\r\n",
                            arg);
             return;
           }
           g_control_registry.resetGroup(dev_id, false);
           sendTelnetMsgf(s,
                          "[OK] Control template for DevID 0x%02X action slots "
                          "reset completed.\r\n",
                          dev_id);
         }
       }},
      {nullptr, "reset all", "Factory wipe & re-inject blueprints from catalog",
       nullptr},
      {"table", nullptr, nullptr,
       [](int s, int, const Args &) {
         withScratchBuf(s,
                        [](AppendBuf &out) { wallpadPrintControlTable(out); });
       }},
      {"list", nullptr, nullptr,
       [](int s, int, const Args &) {
         withScratchBuf(s,
                        [](AppendBuf &out) { wallpadPrintControlTable(out); });
       }},
      {"view", nullptr, nullptr,
       [](int s, int, const Args &) {
         withScratchBuf(s,
                        [](AppendBuf &out) { wallpadPrintControlTable(out); });
       }},
  };

  const char *sub = ctx.args.get(1);
  if (CliFmt::DispatchSubCmd(sub, sock, argc, ctx.args, kCtlDefs,
                             sizeof(kCtlDefs) / sizeof(kCtlDefs[0])))
    return;

  char *endp = nullptr;
  uint8_t dev_id = static_cast<uint8_t>(strtoul(sub, &endp, 0));
  if (!CliFmt::IsHelp(sub) && endp != sub && dev_id != 0) {
    withScratchBuf(sock, [dev_id](AppendBuf &out) {
      wallpadPrintControlDetail(out, dev_id);
    });
  } else {
    CliFmt::PrintSubCmdHelp(
        sock, "CONTROL BLUEPRINT COMMANDS", kCtlDefs,
        sizeof(kCtlDefs) / sizeof(kCtlDefs[0]),
        "Tip: Use 'ctl 0x18' to view detailed frame action offsets");
  }
}

void wallpadPrintControlTable(AppendBuf &out) {
  CliFmt::PrintBoxHeader(out, "DEVICE CONTROL BLUEPRINTS & ACTION SLOTS");

  GroupControlTemplate grps[ControlTemplateRegistry::MAX_GROUPS];
  size_t count = g_control_registry.getGroupsSnapshot(
      grps, ControlTemplateRegistry::MAX_GROUPS);

  char sub_buf[78] = {0};
  snprintf(sub_buf, sizeof(sub_buf),
           "Registered Blueprints: %zu Groups | Auto-Mapped & NVS Persisted",
           count);
  CliFmt::PrintBoxSubtitle(out, sub_buf);

  static constexpr Column CTL_COLS[] = {
      {"DevID", 5, Align::LEFT, Align::CENTER},
      {"Name", 8, Align::LEFT, Align::CENTER},
      {"Class", 6, Align::LEFT, Align::CENTER},
      {"CTL", 3, Align::LEFT, Align::CENTER},
      {"Power Slot", 10, Align::LEFT, Align::CENTER},
      {"QRY", 3, Align::LEFT, Align::CENTER},
      {"Status Offsets", 23, Align::LEFT, Align::CENTER},
  };
  TableRenderer table(out, CTL_COLS, 7);
  table.header(false);

  if (count == 0) {
    table.empty("(No control blueprints registered yet. Waiting for profile or "
                "learning...)");
    out.append(CliFmt::BOX80_EQ);
    out.append("\r\n");
    return;
  }

  for (size_t i = 0; i < count; ++i) {
    const GroupControlTemplate &grp = grps[i];
    if (grp.dev_id == 0)
      continue;

    const char *cls_str = DeviceClassToCliString(grp.coverage.dev_class);

    char name_safe[9] = {0};
    strncpy(name_safe, grp.group_name, sizeof(name_safe) - 1);

    char pwr_buf[16] = {0};
    if (grp.power_slot.discovered) {
      snprintf(pwr_buf, sizeof(pwr_buf), "#%u [%02X/%02X]",
               grp.power_slot.action_offset, grp.power_slot.on_val,
               grp.power_slot.off_val);
    } else {
      snprintf(pwr_buf, sizeof(pwr_buf), "-");
    }

    char ctl_len_str[8] = {0};
    if (grp.frame_len > 0)
      snprintf(ctl_len_str, sizeof(ctl_len_str), "%uB", grp.frame_len);
    else
      snprintf(ctl_len_str, sizeof(ctl_len_str), "-");

    char qry_len_str[8] = {0};
    if (grp.query_slots.expected_len > 0)
      snprintf(qry_len_str, sizeof(qry_len_str), "%uB",
               grp.query_slots.expected_len);
    else
      snprintf(qry_len_str, sizeof(qry_len_str), "-");

    char extra_slots[40] = {0};
    size_t e_off = 0;
    if (grp.query_slots.power_offset != 0xFF) {
      e_off += snprintf(extra_slots + e_off, sizeof(extra_slots) - e_off, "#%u",
                        grp.query_slots.power_offset);
    } else {
      e_off += snprintf(extra_slots + e_off, sizeof(extra_slots) - e_off, "-");
    }

    bool has_sub = (grp.query_slots.target_temp_offset != 0xFF ||
                    grp.query_slots.current_temp_offset != 0xFF ||
                    grp.query_slots.fan_speed_offset != 0xFF ||
                    grp.query_slots.power_w_offset != 0xFF);
    if (has_sub) {
      e_off += snprintf(extra_slots + e_off, sizeof(extra_slots) - e_off, " (");
      bool first = true;
      if (grp.query_slots.target_temp_offset != 0xFF) {
        e_off += snprintf(extra_slots + e_off, sizeof(extra_slots) - e_off,
                          "TT:%u", grp.query_slots.target_temp_offset);
        first = false;
      }
      if (grp.query_slots.current_temp_offset != 0xFF) {
        e_off += snprintf(extra_slots + e_off, sizeof(extra_slots) - e_off,
                          "%sAT:%u", first ? "" : ", ",
                          grp.query_slots.current_temp_offset);
        first = false;
      }
      if (grp.query_slots.fan_speed_offset != 0xFF) {
        e_off += snprintf(extra_slots + e_off, sizeof(extra_slots) - e_off,
                          "%sFS:%u", first ? "" : ", ",
                          grp.query_slots.fan_speed_offset);
        first = false;
      }
      if (grp.query_slots.power_w_offset != 0xFF) {
        e_off +=
            snprintf(extra_slots + e_off, sizeof(extra_slots) - e_off, "%sW:%u",
                     first ? "" : ", ", grp.query_slots.power_w_offset);
      }
      e_off += snprintf(extra_slots + e_off, sizeof(extra_slots) - e_off, ")");
    }

    char id_str[8];
    snprintf(id_str, sizeof(id_str), "0x%02X", grp.dev_id);
    table.row({id_str, name_safe, cls_str, ctl_len_str, pwr_buf, qry_len_str,
               extra_slots});
  }

  table.end('-');
  CliFmt::PrintBoxFooter(
      out,
      "TT: Target Temp, AT: Ambient Temp, FS: Fan Speed, W: Power Wattage");
}

void wallpadPrintControlDetail(AppendBuf &out, uint8_t dev_id) {
  GroupControlTemplate grp{};
  if (!g_control_registry.findGroup(dev_id, grp)) {
    out.appendFormat(
        "[ERROR] Group 0x%02X not found in control blueprints.\r\n", dev_id);
    return;
  }

  char name_safe[17] = {0};
  strncpy(name_safe, grp.group_name, sizeof(name_safe) - 1);

  char title[80];
  snprintf(title, sizeof(title), "DEVICE CONTROL BLUEPRINT DETAIL: 0x%02X (%s)",
           grp.dev_id, name_safe);
  CliFmt::PrintBoxHeader(out, title);

  char sub_buf[78];
  snprintf(sub_buf, sizeof(sub_buf),
           "Frame: CTL %uB | QRY %uB | Sub1 Offset #%u | Sub2 Offset #%u",
           grp.frame_len, grp.query_slots.expected_len, grp.sub1_offset,
           grp.sub2_offset);
  CliFmt::PrintBoxSubtitle(out, sub_buf);

  static constexpr Column DETAIL_COLS[] = {
      {"Action / Status Slot", 25, Align::LEFT, Align::CENTER},
      {"Discovered Configuration", 48, Align::LEFT, Align::CENTER},
  };
  TableRenderer table(out, DETAIL_COLS, 2);
  table.header(false);

  char v_buf[64];
  auto row_slot = [&](const char *label, bool disc, auto fmt_val) {
    if (disc)
      fmt_val();
    else
      snprintf(v_buf, sizeof(v_buf), "None");
    table.row({label, v_buf});
  };

  row_slot("Power Control Slot", grp.power_slot.discovered, [&]() {
    snprintf(v_buf, sizeof(v_buf), "Offset #%u  [ ON: 0x%02X / OFF: 0x%02X ]",
             grp.power_slot.action_offset, grp.power_slot.on_val,
             grp.power_slot.off_val);
  });
  row_slot("Temp Control Slot", grp.temp_slot.discovered, [&]() {
    snprintf(v_buf, sizeof(v_buf), "Offset #%u  [ Range: %u ~ %u C ]",
             grp.temp_slot.action_offset, grp.temp_slot.min_val,
             grp.temp_slot.max_val);
  });
  row_slot("Fan Speed Slot", grp.speed_slot.discovered, [&]() {
    if (grp.speed_slot.level_count > 0) {
      char tok_str[48] = {0};
      for (uint8_t i = 0; i < grp.speed_slot.level_count; ++i) {
        char t_buf[16];
        snprintf(t_buf, sizeof(t_buf), "%sL%u:0x%02X", (i > 0 ? "," : ""),
                 i + 1, grp.speed_slot.level_tokens[i]);
        strncat(tok_str, t_buf, sizeof(tok_str) - strlen(tok_str) - 1);
      }
      snprintf(v_buf, sizeof(v_buf), "Offset #%u  [ %s ]",
               grp.speed_slot.action_offset, tok_str);
    } else {
      snprintf(v_buf, sizeof(v_buf), "Offset #%u  [ Range: %u ~ %u ]",
               grp.speed_slot.action_offset, grp.speed_slot.min_val,
               grp.speed_slot.max_val);
    }
  });
  row_slot("Close Action Slot", grp.close_slot.discovered, [&]() {
    snprintf(v_buf, sizeof(v_buf), "Offset #%u  [ Action: 0x%02X ]",
             grp.close_slot.action_offset, grp.close_slot.off_val);
  });

  table.separator('-');

  struct InboundDef {
    const char *label;
    uint8_t off;
  };
  const InboundDef kInbound[] = {
      {"Power Status Offset", grp.query_slots.power_offset},
      {"Target Temp Offset", grp.query_slots.target_temp_offset},
      {"Ambient Temp Offset", grp.query_slots.current_temp_offset},
      {"Fan Speed Offset", grp.query_slots.fan_speed_offset},
      {"Power Wattage Offset", grp.query_slots.power_w_offset},
      {"CTL ACK State Offset", grp.ack_slots.power_offset},
  };
  for (const auto &inb : kInbound) {
    if (inb.off != 0xFF)
      snprintf(v_buf, sizeof(v_buf), "Offset #%u", inb.off);
    else
      strcpy(v_buf, "None");
    table.row({inb.label, v_buf});
  }

  table.end('-');
  char tip_buf[78];
  snprintf(
      tip_buf, sizeof(tip_buf),
      "Tip: Use 'ctl reset 0x%02X' to re-probe or 'ctl name 0x%02X <name>'",
      grp.dev_id, grp.dev_id);
  CliFmt::PrintBoxFooter(out, tip_buf);
}

} // namespace WallpadCli

// ============================================================================
// Telemetry & Statistics Formatters (namespace Fmt)
// ============================================================================
namespace Fmt {

void FormatHwMetrics(AppendBuf &out, const HwSnapshot &hw) {
  out.append(DIV80);
  out.appendFormat("%-16s %11s  %11s  %11s  %11s  %11s\r\n", "Resource / Core",
                   "Current", "15m Avg", "15m Peak", "24h Avg", "24h Peak");
  out.append(DIV80);

  struct HwRow {
    const char *name;
    uint16_t cur, a15, p15, a24, p24;
    const char *suffix;
  };
  const HwRow rows[] = {
      {"CPU0 (Net/WiFi)", hw.cpu0_cur, hw.cpu0_15m_avg, hw.cpu0_15m_peak,
       hw.cpu0_24h_avg, hw.cpu0_24h_peak, "%"},
      {"CPU1 (RS485/IO)", hw.cpu1_cur, hw.cpu1_15m_avg, hw.cpu1_15m_peak,
       hw.cpu1_24h_avg, hw.cpu1_24h_peak, "%"},
      {"RAM Used", hw.ram_cur, hw.ram_15m_avg, hw.ram_15m_peak, hw.ram_24h_avg,
       hw.ram_24h_peak, " KB"},
      {"Temp", static_cast<uint16_t>(hw.temp_cur),
       static_cast<uint16_t>(hw.temp_15m_avg),
       static_cast<uint16_t>(hw.temp_15m_peak),
       static_cast<uint16_t>(hw.temp_24h_avg),
       static_cast<uint16_t>(hw.temp_24h_peak), " C"},
  };

  for (const auto &r : rows) {
    char c[5][16];
    snprintf(c[0], sizeof(c[0]), "%u%s", static_cast<unsigned>(r.cur),
             r.suffix);
    snprintf(c[1], sizeof(c[1]), "%u%s", static_cast<unsigned>(r.a15),
             r.suffix);
    snprintf(c[2], sizeof(c[2]), "%u%s", static_cast<unsigned>(r.p15),
             r.suffix);
    snprintf(c[3], sizeof(c[3]), "%u%s", static_cast<unsigned>(r.a24),
             r.suffix);
    snprintf(c[4], sizeof(c[4]), "%u%s", static_cast<unsigned>(r.p24),
             r.suffix);
    out.appendFormat("%-16s %11s  %11s  %11s  %11s  %11s\r\n", r.name, c[0],
                     c[1], c[2], c[3], c[4]);
  }
}

void FormatNetworkStats(AppendBuf &out, const PktSnapshot &pkt) {
  out.append(DIV80);
  out.appendFormat("%-10s %-6s %-13s %-7s %-11s %-11s %-8s %s\r\n", "Channel",
                   "Port", "Status", "Conn", "RX Pkts", "TX Pkts", "Dropped",
                   "Uncache");
  out.append(DIV80);

  bool is_conn = pkt.ch6.is_connected;
  uint32_t rx = pkt.ch6.rx_pkts;
  uint32_t tx = pkt.ch6.tx_pkts;
  const char *status_str = !is_conn               ? "Disconnected"
                           : (rx == 0 && tx == 0) ? "Idle"
                                                  : "Connected";
  out.appendFormat("%-10s %-6u %-14s %3u%12u%12u%10u%10u\r\n", "CH#6_Mgmt",
                   Config::TCP::MGMT_PORT, status_str,
                   static_cast<unsigned>(pkt.ch6.connection_count),
                   static_cast<unsigned>(rx), static_cast<unsigned>(tx),
                   static_cast<unsigned>(pkt.ch6.dropped_pkts),
                   static_cast<unsigned>(pkt.ch6.uncached_pkts));
}

void FormatRs485Stats(AppendBuf &out, const PktSnapshot &pkt) {
  out.append(DIV80);
  out.appendFormat("%-10s %10s %12s %15s %10s %9s %8s\r\n", "Channel",
                   "RX Pkts", "TX Pkts", "CRC Err", "Inv Frm", "Timeouts",
                   "Uncache");
  out.append(DIV80);

  const char *rs_n[] = {"CH#1_IoT", "CH#2_WP#1", "CH#3_WP#2", "CH#4_WP#3"};
  const ChanStats *rs_st[] = {&pkt.ch1, &pkt.ch2, &pkt.ch3, &pkt.ch4};
  for (int i = 0; i < 4; ++i) {
    uint32_t rx = rs_st[i]->rx_pkts, crc = rs_st[i]->crc_errors;
    char r_str[24];
    snprintf(r_str, sizeof(r_str), "%u (%.2f%%)", static_cast<unsigned>(crc),
             rx ? (static_cast<float>(crc) / rx) * 100.0f : 0.0f);
    out.appendFormat("%-10s %10u %12u %15s %10u %9u %8u\r\n", rs_n[i],
                     static_cast<unsigned>(rx),
                     static_cast<unsigned>(rs_st[i]->tx_pkts), r_str,
                     static_cast<unsigned>(rs_st[i]->invalid_frames),
                     static_cast<unsigned>(rs_st[i]->timeouts),
                     static_cast<unsigned>(rs_st[i]->uncached_pkts));
  }

  for (int s = 0; s < Config::TCP::MAX_EW11_SLOTS; s++) {
    auto &slot = g_hub_slots[s];
    if (!slot.enabled && strlen(slot.target_ip) == 0 && slot.target_port == 0)
      continue;

    char chan_name[16];
    snprintf(chan_name, sizeof(chan_name), "CH#5_%u",
             slot.target_port ? slot.target_port
                              : Config::TCP::EW11_SLOT_PORTS[s]);

    uint32_t drp = slot.dropped_pkts;
    char drp_str[24];
    snprintf(drp_str, sizeof(drp_str), "%u", static_cast<unsigned>(drp));

    out.appendFormat("%-10s %10u %12u %15s %10u %9u %8u\r\n", chan_name,
                     static_cast<unsigned>(slot.rx_pkts),
                     static_cast<unsigned>(slot.tx_pkts),
                     drp > 0 ? drp_str : "0 (0.00%)", 0u, 0u, 0u);
  }
}

void FormatTaskStacks(AppendBuf &out, const StackSnapshot &st,
                      const TaskWdtMonitor &wdt) {
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

  out.append(DIV80);
  out.appendFormat("%-11s %-12s %-10s %-12s %-8s %-18s\r\n", "Task Name",
                   "Min Stack", "Last Feed", "Peak Intvl", "Status",
                   "Task Scope");
  out.append(DIV80);

  uint32_t now = millis();
  for (size_t i = 0; i < 6; ++i) {
    uint32_t last_feed =
        wdt.tasks[i].last_feed_ms.load(std::memory_order_relaxed);
    uint32_t elapsed =
        (last_feed > 0 && now >= last_feed) ? (now - last_feed) : 0;
    uint32_t peak =
        wdt.tasks[i].max_interval_ms.load(std::memory_order_relaxed);

    out.appendFormat("%-11s %5u Bytes  %5u ms     %5u ms       %-7s %-18s\r\n",
                     names[i], stacks[i], static_cast<unsigned>(elapsed),
                     static_cast<unsigned>(peak), gtag(stacks[i]), scopes[i]);
  }
}

} // namespace Fmt

const CommandDef kConsoleCmds[] = {
    {"stats", "Show real-time HW metrics & traffic stats [clear]",
     SystemCli::cmdStats},
    {"devs", "Show device registry & cache [1|2|all|clear]",
     WallpadCli::cmdDevs},
    {"wifi", "Manage WiFi connection [status|scan|connect|disconnect]",
     WifiCli::cmdWifi},
    {"trace", "Packet monitoring [on|off|ctl|ack|pol|rmt|drp|ch|devid]",
     WallpadCli::cmdTrace},
    {"wallpad",
     "Wallpad protocol & auto-probing "
     "[status|list|set|save|delete|auto|reset|simulate]",
     WallpadCli::cmdWallpad},
    {"ctl", "Device control blueprints [table|<dev_id>|name|class|reset]",
     WallpadCli::cmdCtl},
    {"config", "View or modify runtime configuration [set|reset]",
     ConfigCli::cmdConfig},
    {"save", "Save current runtime configuration to NVS flash",
     ConfigCli::cmdSave},
    {"ew11", "CH5 EW11 hub sockets & FCU [list|set|frame|reset|enable|disable]",
     ConfigCli::cmdEw11},
    {"routes", "Show dynamic device ingress routing table [clear]",
     ConfigCli::cmdRoutes},
    {"logview",
     "Persistent reboot history & crash logs [list|<1-20>|last|clear]",
     SystemCli::cmdLogView},
    {"coredump", "Show crash core dump summary or erase partition [clear]",
     SystemCli::cmdCoreDump},
    {"ota", "Dual-partition OTA & rollback [status|rollback|validate|cloud]",
     SystemCli::cmdOta},
    {"reboot", "Perform hardware system reboot with safe shutdown",
     SystemCli::cmdReboot},
    {"q", "Stop active packet tracing (shortcut for 'trace off')",
     WallpadCli::cmdStop},
    {"exit", "Disconnect current Telnet CLI session", TelnetManager::cmdExit},
    {"help", "Display comprehensive command reference and usage examples",
     SystemCli::cmdHelp}};

const size_t kConsoleCmdsCount = sizeof(kConsoleCmds) / sizeof(kConsoleCmds[0]);
