#include "L4_Services/CLI_Commands.h"
#include "L0_Foundation/System_Platform.h"
#include "L3_Protocol/Public/Protocol_Facade.h"
#include <WiFi.h>
#include <algorithm>
#include <esp_core_dump.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>

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

  char *scratch = Cli_GetScratchBuffer();
  size_t scratch_sz = Cli_GetScratchBufferSize();
  scratch[0] = '\0';
  AppendBuf out{scratch, scratch_sz};

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
      FixedBuf<8> no_buf;
      no_buf.appendFormat("%d", i + 1);
      int rssi = WiFi.RSSI(i);
      FixedBuf<16> sig_buf;
      sig_buf.appendFormat("%d dBm (%d%%)", rssi,
                           std::min(std::max(2 * (rssi + 100), 0), 100));
      FixedBuf<8> ch_buf;
      ch_buf.appendFormat("%d", WiFi.channel(i));
      table.row({no_buf.c_str(), WiFi.SSID(i).c_str(), sig_buf.c_str(), ch_buf.c_str(), encType});
    }
  }
  table.end('-');
  CliFmt::PrintBoxFooter(out,
                         "Use 'wifi connect <ssid> <password>' to switch AP");
  out.append("\r\n");
  WiFi.scanDelete();

  CLI_GetTelnetManager().sendScanResult(req, out.buf);
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
           FixedBuf<16> rssi_b;
           rssi_b.appendFormat("%d dBm", WiFi.RSSI());

           const auto &cfg = Config_Get();
           table.row({"Station (STA)", "SSID",
                      sta_ok ? WiFi.SSID().c_str() : cfg.wifi_ssid,
                      sta_ok ? "[CONNECTED]" : "[DISCONNECTED]"});
           table.row({"", "IP Address",
                      sta_ok ? WiFi.localIP().toString().c_str() : "0.0.0.0",
                      sta_ok ? "[ACTIVE]" : "[IDLE]"});
           table.row({"", "Signal (RSSI)", sta_ok ? rssi_b.c_str() : "N/A",
                      sta_ok ? "[STABLE]" : "[IDLE]"});
           table.separator('-');

           bool ap_active = (WiFi.getMode() == WIFI_MODE_AP ||
                             WiFi.getMode() == WIFI_MODE_APSTA);
           table.row({"SoftAP (AP)", "SSID", cfg.ap_ssid,
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
         auto &scan_req = CLI_GetWifiScanReq();
         scan_req.clientIp = IPAddress(0, 0, 0, 0);
         scan_req.sessionId = 0;
         xTaskCreatePinnedToCore(AsyncWifiScanTask, "WifiScanWorker", 4096,
                                 &scan_req, 2, NULL, 0);
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
         RuntimeConfig staged_cfg = Config_Get();
         strncpy(staged_cfg.wifi_ssid, ssid_arg,
                 sizeof(staged_cfg.wifi_ssid) - 1);
         staged_cfg.wifi_ssid[sizeof(staged_cfg.wifi_ssid) - 1] = '\0';
         strncpy(staged_cfg.wifi_password, pass_arg,
                 sizeof(staged_cfg.wifi_password) - 1);
         staged_cfg.wifi_password[sizeof(staged_cfg.wifi_password) - 1] = '\0';
         Config_SaveStaged(staged_cfg, TimingConfig_Get());
         sendTelnetMsgf(s, "[WIFI] Saved SSID '%s' to NVS. Connecting...\r\n",
                        ssid_arg);
         WiFi.disconnect(false);
         vTaskDelay(pdMS_TO_TICKS(100));
         WiFi.begin(ssid_arg, pass_arg);
       }},
      {"disconnect", "disconnect", "Disconnect from current Wi-Fi AP",
       [](int s, int, const Args &) {
         WiFi.disconnect(false);
         sendTelnetMsg(s, "[WIFI] Disconnected from Wi-Fi AP.\r\n");
       }},
  };

  if (CliFmt::DispatchSubCmd(subCmd, sock, count, ctx.args, kWifiDefs))
    return;

  CliFmt::PrintSubCmdHelp(sock, "WIFI COMMAND REFERENCE", kWifiDefs,
                          "Tip: Configuration persists to NVS flash memory");
}

} // namespace WifiCli

// ============================================================================
// From src/CLI/CliSystem.cpp
// ============================================================================

namespace SystemCli {

void printSystemOverview(AppendBuf &out, const SysSnapshot &sys) {
  uint32_t ts = millis() / 1000;
  time_t now = time(nullptr);
  struct tm timeinfo;
  FixedBuf<64> time_str;
  const char *time_src = "System RTC";
  if (now > 1672531200) {
    localtime_r(&now, &timeinfo);
    strftime(time_str.storage, sizeof(time_str.storage), "%Y-%m-%d %H:%M:%S", &timeinfo);
    time_str.offset = strlen(time_str.storage);
    time_src = "NTP Synced";
  } else {
    time_str.appendFormat("Uptime: %ud %02uh %02um %02us",
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

  uint32_t free_heap = sys.free_heap / 1024;
  uint32_t min_free_heap = sys.min_free_heap / 1024;
  uint32_t total_heap = sys.total_heap / 1024;
  uint32_t heap_free_pct =
      (total_heap > 0) ? (free_heap * 100 / total_heap) : 0;

  uint32_t sketch_size = ESP.getSketchSize() / 1024;
  uint32_t free_sketch = ESP.getFreeSketchSpace() / 1024;
  uint32_t app_slot_size = sketch_size + free_sketch;
  uint32_t flash_chip_mb = (ESP.getFlashChipSize() / 1024) / 1024;
  uint32_t app_used_pct =
      (app_slot_size > 0) ? (sketch_size * 100 / app_slot_size) : 0;

  char heap_bar[16], flash_bar[16];
  make_ascii_bar(heap_bar, sizeof(heap_bar), heap_free_pct);
  make_ascii_bar(flash_bar, sizeof(flash_bar), app_used_pct);

  bool wifi_conn = sys.wifi_connected;
  int wifi_rssi = sys.wifi_rssi;
  const char *wifi_qual = !wifi_conn           ? "[DISCONNECTED]"
                          : (wifi_rssi >= -65) ? "[EXCELLENT]"
                          : (wifi_rssi >= -75) ? "[GOOD]"
                          : (wifi_rssi >= -85) ? "[FAIR]"
                                               : "[POOR]";

  FixedBuf<80> wp_status_buf;
  char profile_summary_buf[80] = "Unknown";
  ProtocolDiag_GetProfileSummary(profile_summary_buf, sizeof(profile_summary_buf));
  wp_status_buf.append(profile_summary_buf);

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
      "WiFi Connection : %s (%d dBm, IP: %s) %s\r\n"
      "Heap Memory     : %s %3u%% Free (Free %uKB / %uKB, Min %uKB)\r\n"
      "Flash (OTA App) : %s %3u%% Used (%uKB / %uKB, Chip %uMB)\r\n",
      Config::FIRMWARE_VERSION, wp_status_buf.c_str(), time_str.c_str(), time_src, ts / 86400,
      (ts % 86400) / 3600, (ts % 3600) / 60, ts % 60,
      wifi_conn ? "Connected" : "Disconnected", wifi_rssi,
      sys.wifi_ip, wifi_qual, heap_bar, heap_free_pct,
      free_heap, total_heap, min_free_heap, flash_bar, app_used_pct,
      sketch_size, app_slot_size, flash_chip_mb);
}

void printStats(int sock) {
  static std::atomic<bool> s_busy{false};
  if (s_busy.exchange(true, std::memory_order_acquire)) {
    sendTelnetMsg(sock, "[BUSY] Stats is being generated.\r\n");
    return;
  }

  System_FeedWdt(5);
  SysSnapshot sys_snap;
  HwSnapshot hw_snap;
  StackSnapshot stack_snap;
  PktSnapshot pkt_snap;
  System_TakeSnapshot(sys_snap, hw_snap, stack_snap, pkt_snap);

  char *scratch = Cli_GetScratchBuffer();
  size_t scratch_sz = Cli_GetScratchBufferSize();
  scratch[0] = '\0';
  AppendBuf out{scratch, scratch_sz};

  printSystemOverview(out, sys_snap);

  Fmt::FormatHwMetrics(out, hw_snap);
  Fmt::FormatNetworkStats(out, pkt_snap);
  Fmt::FormatRs485Stats(out, pkt_snap);

  out.append(Fmt::DIV80);
  out.appendFormat("%-55s %24s\r\n", "Metric / Event",
                   "Value / Counter / Status");
  out.append(Fmt::DIV80);

  uint32_t poll_cnt = 0, vip_cnt = 0, norm_cnt = 0;
  System_GetCh1Metrics(poll_cnt, vip_cnt, norm_cnt);

  out.appendFormat(
      "%-55s %24u\r\n"
      "%-55s %24u\r\n"
      "%-55s %24u\r\n"
      "%-55s %24u\r\n",
      "Total Device Polls",
      static_cast<unsigned>(poll_cnt),
      "VIP Controls (SmartThings App)",
      static_cast<unsigned>(vip_cnt),
      "Normal Controls (Wallpad)",
      static_cast<unsigned>(norm_cnt),
      "Stale Emerg Polls",
      static_cast<unsigned>(ProtocolDiag_GetStalePollCount()));

  System_FormatTaskStacks(out, stack_snap);

  LatencySnapshot lat_snap;
  System_GetCh1Latency(lat_snap);
  Fmt::FormatCh1Latency(out, lat_snap);

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
      System_ResetTrafficStats();
      ProtocolDiag_ResetBridgeStats();
      ProtocolDiag_PollingResetHits();
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
  CLI_RequestRestart("Telnet Command");
}

static void FormatRebootLogEntry(AppendBuf &out, const LogEntry &e, size_t idx, size_t count) {
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

  out.appendFormat("\r\n%s", Fmt::DIV80EQ);
  out.appendFormat("                   GATEWAY BRIDGE REBOOT SNAPSHOT MONITOR                     \r\n");
  out.appendFormat("%s", Fmt::DIV80EQ);
  out.appendFormat("Log Index       : #%zu / %zu\r\n", idx + 1, count);
  out.appendFormat("Reboot Reason   : %s\r\n", e.reason);
  out.appendFormat("Firmware        : %s\r\n", Config::FIRMWARE_VERSION);
  out.appendFormat("Log Time        : %s (%s)\r\n", t_buf, t_src);
  out.appendFormat("Uptime          : %ud %02uh %02um %02us\r\n", s / 86400,
                   (s % 86400) / 3600, (s % 3600) / 60, s % 60);
  out.appendFormat("WiFi Connection : %s\r\n", w_str);
  out.appendFormat(
      "Heap Memory     : Free %u KB / Min Free %u KB / Total %u KB\r\n",
      static_cast<unsigned>(e.stats_snapshot.free_heap / 1024),
      static_cast<unsigned>(e.stats_snapshot.min_free_heap / 1024),
      static_cast<unsigned>(e.stats_snapshot.total_heap / 1024));
  out.appendFormat("Flash Storage   : Sketch %u KB / Total Flash %u KB\r\n\r\n",
                   static_cast<unsigned>(e.stats_snapshot.sketch_size_kb),
                   static_cast<unsigned>(e.stats_snapshot.flash_total_kb));

  Fmt::FormatHwMetrics(out, e.hw_snapshot);
  Fmt::FormatNetworkStats(out, e.packet_stats_snapshot);
  Fmt::FormatRs485Stats(out, e.packet_stats_snapshot);
  System_FormatTaskStacks(out, e.stack_snapshot);
  out.appendFormat("%s\r\n", Fmt::DIV80EQ);
}

void cmdLogView(CliContext &ctx) {
  int client = ctx.sock;
  const char *sub_cmd = (ctx.args.count() > 0) ? ctx.args.get(1) : "list";

  if (strcasecmp(sub_cmd, "clear") == 0) {
    System_ClearRebootLog();
    sendTelnetMsg(client, "Reboot log history CLEARED from NVS flash.\r\n");
    return;
  }

  size_t count = System_GetRebootLogCount();
  if (count == 0) {
    sendTelnetMsg(client,
                  "\r\n[LOGVIEW] No persistent reboot logs found in NVS.\r\n");
    return;
  }

  if (strcasecmp(sub_cmd, "list") == 0) {
    withScratchBuf(client, [count](AppendBuf &out) {
      CliFmt::PrintBoxHeader(out, "PERSISTENT REBOOT LOG HISTORY");
      CliFmt::PrintBoxSubtitlef(
          out, "Total Stored: %u / 20 Logs | Non-Volatile RTC/NVS",
          static_cast<unsigned>(count));

      static constexpr Column REBOOT_COLS[] = {
          {"No", 3, Align::CENTER, Align::CENTER},
          {"Timestamp", 19, Align::CENTER, Align::CENTER},
          {"Reboot Reason", 33, Align::LEFT, Align::CENTER},
          {"Uptime", 12, Align::CENTER, Align::CENTER},
      };
      TableRenderer table(out, REBOOT_COLS, 4);
      table.header(false);

      FixedBuf<32> time_buf;
      FixedBuf<16> up_buf;
      FixedBuf<8> no_buf;

      for (size_t i = 0; i < count; i++) {
        LogEntry entry;
        if (System_GetRebootLogEntry(i, entry)) {
          time_buf.reset();
          if (entry.timestamp > 0) {
            struct tm timeinfo;
            time_t sec = static_cast<time_t>(entry.timestamp);
            localtime_r(&sec, &timeinfo);
            if (timeinfo.tm_year >= 124) {
              strftime(time_buf.storage, sizeof(time_buf.storage), "%Y-%m-%d %H:%M:%S",
                       &timeinfo);
              time_buf.offset = strlen(time_buf.storage);
            } else {
              time_buf.appendFormat("%04d-%02d-%02d %02d:%02d:%02d",
                                    timeinfo.tm_year + 1900, timeinfo.tm_mon + 1,
                                    timeinfo.tm_mday, timeinfo.tm_hour,
                                    timeinfo.tm_min, timeinfo.tm_sec);
            }
          } else {
            time_buf.append("N/A");
          }
          uint32_t sec = entry.stats_snapshot.uptime_ms / 1000;
          up_buf.reset();
          up_buf.appendFormat("%02uh %02um %02us", sec / 3600,
                              (sec % 3600) / 60, sec % 60);

          no_buf.reset();
          no_buf.appendFormat("#%u", static_cast<unsigned>(i + 1));
          table.row({no_buf.c_str(), time_buf.c_str(), entry.reason, up_buf.c_str()});
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

  char *scratch = Cli_GetScratchBuffer();
  size_t scratch_sz = Cli_GetScratchBufferSize();
  scratch[0] = '\0';
  AppendBuf out{scratch, scratch_sz};
  LogEntry e;
  if (System_GetRebootLogEntry(target_idx, e)) {
    FormatRebootLogEntry(out, e, target_idx, count);
    sendTelnetMsgLen(client, out.buf, out.offset);
  } else {
    sendTelnetMsg(client, "\r\n[LOGVIEW] Failed to read reboot log entry.\r\n");
  }
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
    out.appendFormat("| Exception VAddr : 0x%08X                               "
                     "                  |\r\n",
                     static_cast<unsigned>(summary.ex_info.exc_vaddr));
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

  FixedBuf<36> run_val, next_val, timer_val, crash_val;
  run_val.appendFormat("%s (0x%06X, %u KB)",
                       running ? running->label : "app0",
                       running ? static_cast<unsigned>(running->address) : 0x10000,
                       running ? static_cast<unsigned>(running->size / 1024) : 3712);
  next_val.appendFormat("%s (0x%06X, %u KB)",
                        next ? next->label : "app1",
                        next ? static_cast<unsigned>(next->address) : 0x3B0000,
                        next ? static_cast<unsigned>(next->size / 1024) : 3712);

  bool val_done = TimeUtils::isElapsed(
      System_GetBootTimeMs(), Config::Timing::OTA_VALIDATION_PERIOD_MS);
  timer_val.append(val_done ? "120s Passed" : "Evaluating (<120s)");
  crash_val.appendFormat("%u Consecutive Crashes",
                         static_cast<unsigned>(System_GetCrashCounter()));
  bool is_rescue = System_IsRescueMode();

  CliFmt::PrintBoxHeader(out, "DUAL-PARTITION OTA & ROLLBACK MONITOR");
  static constexpr Column OTA_COLS[] = {
      {"Category", 13, Align::LEFT, Align::CENTER},
      {"Parameter", 13, Align::LEFT, Align::CENTER},
      {"Value / Target", 27, Align::LEFT, Align::CENTER},
      {"Status", 14, Align::LEFT, Align::CENTER},
  };
  TableRenderer table(out, OTA_COLS, 4);
  table.header(false);

  table.row({"Running App", "Partition", run_val.c_str(), "[ACTIVE]"});
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

  table.row({"Backup Target", "Partition", next_val.c_str(), "[STANDBY]"});
  table.row({"", "Rollback", next_desc, next_status});
  table.separator('-');

  table.row({"Safety Guard", "Health Timer", timer_val.c_str(),
             val_done ? "[STABLE]" : "[TESTING]"});
  table.row({"", "Crash Loop", crash_val.c_str(),
             System_GetCrashCounter() == 0 ? "[STABLE]" : "[WARNING]"});
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
    sendTelnetMsgf(sock,
                   "[ERROR] Rollback failed (No rollback partition available, "
                   "err=0x%x)\r\n",
                   err);
  }
}

void otaValidate(int sock) {
  esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  if (err == ESP_OK) {
    sendTelnetMsg(sock, "[OTA] Current firmware manually confirmed as VALID. "
                        "Auto-rollback cancelled.\r\n");
  } else {
    sendTelnetMsgf(sock, "[ERROR] Failed to mark app valid: 0x%x\r\n", err);
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
         System_StartHttpOta(url);
       }},
  };

  if (CliFmt::DispatchSubCmd(subCmd, sock, count, ctx.args, kOtaDefs))
    return;

  CliFmt::PrintSubCmdHelp(
      sock, "OTA COMMAND REFERENCE", kOtaDefs,
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
    char s_cur[16], s_a15[16], s_p15[16], s_a24[16], s_p24[16];
    AppendBuf{s_cur, sizeof(s_cur)}.appendFormat("%u%s", static_cast<unsigned>(r.cur), r.suffix);
    AppendBuf{s_a15, sizeof(s_a15)}.appendFormat("%u%s", static_cast<unsigned>(r.a15), r.suffix);
    AppendBuf{s_p15, sizeof(s_p15)}.appendFormat("%u%s", static_cast<unsigned>(r.p15), r.suffix);
    AppendBuf{s_a24, sizeof(s_a24)}.appendFormat("%u%s", static_cast<unsigned>(r.a24), r.suffix);
    AppendBuf{s_p24, sizeof(s_p24)}.appendFormat("%u%s", static_cast<unsigned>(r.p24), r.suffix);
    out.appendFormat("%-16s %11s  %11s  %11s  %11s  %11s\r\n", r.name, s_cur,
                     s_a15, s_p15, s_a24, s_p24);
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
  FixedBuf<24> r_str;
  for (int i = 0; i < 4; ++i) {
    uint32_t rx = rs_st[i]->rx_pkts, crc = rs_st[i]->crc_errors;
    r_str.reset();
    r_str.appendFormat("%u (%.2f%%)", static_cast<unsigned>(crc),
                       rx ? (static_cast<float>(crc) / rx) * 100.0f : 0.0f);
    out.appendFormat("%-10s %10u %12u %15s %10u %9u %8u\r\n", rs_n[i],
                     static_cast<unsigned>(rx),
                     static_cast<unsigned>(rs_st[i]->tx_pkts), r_str.c_str(),
                     static_cast<unsigned>(rs_st[i]->invalid_frames),
                     static_cast<unsigned>(rs_st[i]->timeouts),
                     static_cast<unsigned>(rs_st[i]->uncached_pkts));
  }

  FixedBuf<16> chan_name;
  for (int s = 0; s < Config::TCP::MAX_EW11_SLOTS; s++) {
    HubClientSlotSnapshot slot;
    System_GetBridgeSlotSnapshot(static_cast<uint8_t>(s), slot);
    if (!slot.enabled && strlen(slot.target_ip) == 0 && slot.target_port == 0)
      continue;

    chan_name.reset();
    chan_name.appendFormat("CH#5_%u",
                           slot.target_port ? slot.target_port
                                            : Config::TCP::EW11_SLOT_PORTS[s]);

    uint32_t rx = slot.rx_pkts, crc = slot.crc_errors;
    r_str.reset();
    r_str.appendFormat("%u (%.2f%%)", static_cast<unsigned>(crc),
                       rx ? (static_cast<float>(crc) / rx) * 100.0f : 0.0f);

    out.appendFormat("%-10s %10u %12u %15s %10u %9u %8u\r\n", chan_name.c_str(),
                     static_cast<unsigned>(slot.rx_pkts),
                     static_cast<unsigned>(slot.tx_pkts),
                     r_str.c_str(),
                     static_cast<unsigned>(slot.invalid_frames),
                     static_cast<unsigned>(slot.timeouts),
                     static_cast<unsigned>(slot.uncached_pkts));
  }
}

void FormatCh1Latency(AppendBuf &out, const LatencySnapshot &lat) {
  out.append(DIV80);

  const uint32_t mhz = std::max<uint32_t>(getCpuFrequencyMhz(), 1);
  auto us100 = [mhz](uint32_t c) {
    return static_cast<uint32_t>(static_cast<uint64_t>(c) * 100u / mhz);
  };

  const uint32_t cnt = lat.count;
  const uint32_t mx = lat.max_cycles;
  const uint32_t mxUs = us100(mx);

  // 1. Title Row: 47 chars left + 33 chars right = 80 chars
  char r_buf[34];
  if (mx == 0) {
    snprintf(r_buf, sizeof(r_buf), "Count: %u | Max: 0 us", static_cast<unsigned>(cnt));
  } else if (mxUs >= 100000) {
    snprintf(r_buf, sizeof(r_buf), "Count: %u | Max: %u.%02u ms",
             static_cast<unsigned>(cnt),
             static_cast<unsigned>(mxUs / 100000),
             static_cast<unsigned>((mxUs % 100000) / 1000));
  } else {
    snprintf(r_buf, sizeof(r_buf), "Count: %u | Max: %u.%01u us",
             static_cast<unsigned>(cnt),
             static_cast<unsigned>(mxUs / 100),
             static_cast<unsigned>((mxUs % 100) / 10));
  }
  out.appendFormat("%-47s%33s\r\n", "Hot-Path Real-Time Latency (Core 1 / Task_Ch1)", r_buf);

  // 2. Divider
  out.append(DIV80);

  // 3. Table Header: 24 + 1 + 17 + 1 + 10 + 1 + 10 + 2 + 14 = 80 chars
  out.appendFormat("%-24s %-17s %10s %10s  %-14s\r\n",
                   "Latency Range (Cycles)", "Wall-Clock Time", "Hits", "Ratio", "  Distribution");

  // 4. Data Rows: 24 + 1 + 17 + 1 + 10 + 1 + 10 + 2 + 14 = 80 chars
  for (unsigned b = 0; b < LatencySnapshot::kBuckets; ++b) {
    const uint32_t h = lat.hist[b];
    if (h == 0) continue;

    char range_buf[25];
    char time_buf[18];

    if (b == LatencySnapshot::kBuckets - 1) {
      const uint32_t lo = 1u << (b - 1);
      const uint32_t u = us100(lo);
      snprintf(range_buf, sizeof(range_buf), ">= %7u cyc (Spike)", static_cast<unsigned>(lo));
      snprintf(time_buf, sizeof(time_buf), ">= %5u.%01u us",
               static_cast<unsigned>(u / 100), static_cast<unsigned>((u % 100) / 10));
    } else {
      const uint32_t hi = 1u << b;
      const uint32_t u = us100(hi);
      snprintf(range_buf, sizeof(range_buf), "<  %7u cyc", static_cast<unsigned>(hi));
      snprintf(time_buf, sizeof(time_buf), "<  %5u.%01u us",
               static_cast<unsigned>(u / 100), static_cast<unsigned>((u % 100) / 10));
    }

    const uint32_t ratio_x10 = cnt > 0
        ? static_cast<uint32_t>((static_cast<uint64_t>(h) * 1000u + (cnt / 2)) / cnt)
        : 0;

    char rat_buf[11];
    snprintf(rat_buf, sizeof(rat_buf), "%u.%01u%%",
             static_cast<unsigned>(ratio_x10 / 10), static_cast<unsigned>(ratio_x10 % 10));

    unsigned hashes = std::min<unsigned>(10u, (ratio_x10 + 50) / 100);
    if (h > 0 && hashes == 0) hashes = 1;

    char bar[15];
    bar[0] = ' ';
    bar[1] = ' ';
    bar[2] = '[';
    for (unsigned i = 0; i < 10; ++i) {
      bar[3 + i] = (i < hashes) ? '#' : ' ';
    }
    bar[13] = ']';
    bar[14] = '\0';

    out.appendFormat("%-24s %-17s %10u %10s  %-14s\r\n",
                     range_buf, time_buf, static_cast<unsigned>(h), rat_buf, bar);
  }

  // 5. Divider
  out.append(DIV80);

  // 6. Peak WCET Summary Row: 45 chars left + 35 chars right = 80 chars
  char peak_buf[46];
  char ago_buf[36];

  snprintf(peak_buf, sizeof(peak_buf), "Peak WCET: %u cyc (%u.%02u us)",
           static_cast<unsigned>(mx),
           static_cast<unsigned>(mxUs / 100),
           static_cast<unsigned>(mxUs % 100));

  if (mx > 0) {
    const uint32_t ageMs = lat.max_age_ms;
    snprintf(ago_buf, sizeof(ago_buf), "Last Event: %u.%01us ago",
             static_cast<unsigned>(ageMs / 1000),
             static_cast<unsigned>((ageMs % 1000) / 100));
  } else {
    ago_buf[0] = '\0';
  }

  out.appendFormat("%-45s%35s\r\n", peak_buf, ago_buf);
}

} // namespace Fmt
