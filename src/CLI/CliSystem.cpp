#include "CliCommon.h"

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
    g_cli_scratch_buf[0] = '\0';
    AppendBuf out{g_cli_scratch_buf, sizeof(g_cli_scratch_buf)};

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

  LogManager::readRebootLog(g_cli_scratch_buf, sizeof(g_cli_scratch_buf), target_idx);
  sendTelnetMsg(client, g_cli_scratch_buf);
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
    g_cli_scratch_buf[0] = '\0';
    AppendBuf out{g_cli_scratch_buf, sizeof(g_cli_scratch_buf)};
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

  g_cli_scratch_buf[0] = '\0';
  AppendBuf out{g_cli_scratch_buf, sizeof(g_cli_scratch_buf)};

  out.append("\r\n");
  out.append(Fmt::DIV80EQ);
  out.append("                       GATEWAY BRIDGE COMMAND REFERENCE                       \r\n");
  out.append(Fmt::DIV80EQ);
  out.append("Command / Parameter               Description & Usage Example\r\n");
  out.append(Fmt::DIV80);

  out.append(" [ SYSTEM & DIAGNOSTICS ]\r\n");
  out.append("  stats [clear]                   Show real-time HW metrics & traffic stats (or reset)\r\n");
  out.append("  devs [1|2|clear]                Show device registry & cache (1: T1 Targets, 2: T2 Cache)\r\n");
  out.append("  logview [list|<1-20>|last|clear] View persistent reboot log history from NVS\r\n");
  out.append("  coredump [clear]                View crash core dump summary & backtrace\r\n");
  out.append("  ota [status|rollback|validate]  Dual-partition OTA & auto-rollback management\r\n");
  out.append("  reboot                          Safely commit buffers and reboot gateway hardware\r\n");
  out.append(Fmt::DIV80);

  out.append(" [ PROTOCOL & PROBING ]\r\n");
  out.append("  wallpad [status]                Show wallpad profile learning status & parameters\r\n");
  out.append("  wallpad list                    List available vendor & saved NVS custom profiles\r\n");
  out.append("  wallpad set <key|id>            Manually switch active wallpad profile\r\n");
  out.append("  wallpad save <name>             Save current auto-learned profile to NVS slot\r\n");
  out.append("  wallpad delete <id>             Reset a saved custom profile slot in NVS\r\n");
  out.append("  wallpad auto                    Switch to Universal Auto-Probing mode\r\n");
  out.append("  wallpad reset                   Reset auto-probing engine and re-learn bus traffic\r\n");
  out.append("  wallpad simulate <hex...>       Inject raw hex packet into auto-probing engine\r\n");
  out.append(Fmt::DIV80);
  out.append(" [ CONTROL BLUEPRINT & LEARNING ]\r\n");
  out.append("  ctl [table|list]                Display learned control blueprint table\r\n");
  out.append("  ctl <dev_id>                    Dump detailed packet blueprint & action slots (e.g. ctl 0x18)\r\n");
  out.append("  ctl learn [id|all]              Active probing session (all groups if omitted)\r\n");
  out.append("  ctl lock [dev_id|all]           Lock blueprint(s) into immutable state\r\n");
  out.append("  ctl unlock [dev_id|all]         Unlock blueprint(s) for continuous learning\r\n");
  out.append("  ctl name <dev_id> <name>        Assign custom group name (e.g. ctl name 0x1B Gas)\r\n");
  out.append("  ctl class <dev_id> <class>      Assign device class (light|outlet|vent|thermo|gas|ev)\r\n");
  out.append("  ctl status                      Show active probing real-time progress\r\n");
  out.append("  ctl reset [id|all]              Reset blueprint(s) and wipe from NVS flash\r\n");
  out.append("  trace [on|off|ctl|ack|pol|...]  Live packet stream monitoring with filters\r\n");
  out.append("  q                               Shortcut to stop live tracing immediately\r\n");
  out.append(Fmt::DIV80);

  out.append(" [ NETWORK & CONFIG ]\r\n");
  out.append("  wifi [status]                   Show Wi-Fi STA connection status & signal strength\r\n");
  out.append("  wifi scan                       Scan surrounding 2.4GHz Wi-Fi AP networks\r\n");
  out.append("  wifi connect <ssid> [password]  Connect to target Wi-Fi AP network\r\n");
  out.append("  wifi disconnect                 Disconnect current Wi-Fi station\r\n");
  out.append("  ew11 [list|status]              Show CH5 EW11 multi-client slots & framing status\r\n");
  out.append("  ew11 set <slot> <ip> [port]     Configure EW11 slot IP & port (Saved to NVS)\r\n");
  out.append("  ew11 frame <slot> <stx> <etx>   Manually fix slot packet framing in NVS\r\n");
  out.append("  ew11 reset <slot>               Reset slot framing tracker to autonomous auto-probing\r\n");
  out.append("  ew11 enable/disable <slot>      Enable or disable target EW11 client slot\r\n");
  out.append("  routes [clear]                  Show dynamic device ingress routing table\r\n");
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
