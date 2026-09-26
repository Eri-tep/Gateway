#include "CliCommon.h"

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

  constexpr uint8_t ALLOWED_MASK = (1 << 2) | (1 << 3) | (1 << 5);

  if (tgt_total == 0) {
    out.append("  (No polling targets registered yet. Waiting for Wallpad/App queries...)\r\n");
  } else {
    unsigned int display_idx = 1;
    for (size_t i = 0; i < tgt_total; ++i) {
      PollingTargetEntry tgt;
      if (!g_polling_targets.getEntry(i, tgt))
        continue;

      // Display entries sourced from CH2, CH3, or CH5
      if (tgt.source_channels != 0 && (tgt.source_channels & ALLOWED_MASK) == 0) {
        continue;
      }

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
          display_idx++, status_str, last_req_str,
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

  if (total_count == 0) {
    out.append("  (No physical devices discovered on RS-485 bus yet)\r\n");
  } else {
    for (size_t i = 0; i < total_count; ++i) {
      DeviceStateEntry dev;
      if (!g_device_repo.getSnapshot(i, dev) || dev.dev_id == 0)
        continue;

      char ack_hex[96] = {0};
      if (dev.last_ack_len > 0) {
        Fmt::FormatHex(dev.last_ack_data.data(), dev.last_ack_len, ack_hex, sizeof(ack_hex));
      } else {
        snprintf(ack_hex, sizeof(ack_hex), "(No ACK received from bus yet)");
      }

      char updated_str[16] = "-";
      if (dev.last_updated_ms > 0) {
        Fmt::FormatElapsed(now, dev.last_updated_ms, updated_str, sizeof(updated_str));
      }

      const char *status_str = dev.is_online ? "ONLINE" : "OFFLINE";

      out.appendFormat("%02u  %-7s  %-5s  %s\r\n",
                       static_cast<unsigned int>(i + 1), status_str, updated_str, ack_hex);
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

  g_cli_scratch_buf[0] = '\0';
  AppendBuf out{g_cli_scratch_buf, sizeof(g_cli_scratch_buf)};

  if (show_tier1) {
    devsPrintTier1Targets(out, now);
  }
  if (show_tier2) {
    devsPrintTier2Cache(out, now);
  }

  sendTelnetMsgLen(client, out.buf, out.offset);
}



void cmdWallpad(EmbeddedCli *cli, char *args, void *context) {
  int sock = getSock(context);
  int argc = embeddedCliGetTokenCount(args);
  const char *sub = (argc > 0) ? embeddedCliGetToken(args, 1) : "status";

  if (argc == 0 || strcasecmp(sub, "status") == 0) {
    g_cli_scratch_buf[0] = '\0';
    AppendBuf out{g_cli_scratch_buf, sizeof(g_cli_scratch_buf)};
    wallpadPrintStatus(out);
    sendTelnetMsgLen(sock, out.buf, out.offset);
    return;
  }

  if (strcasecmp(sub, "list") == 0) {
    g_cli_scratch_buf[0] = '\0';
    AppendBuf out{g_cli_scratch_buf, sizeof(g_cli_scratch_buf)};
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
    g_probe_convergence_reset.store(true, std::memory_order_release);
  } else if (strcasecmp(sub, "simulate") == 0) {
    if (argc < 2) {
      sendTelnetMsg(sock, "[ERROR] Usage: wallpad simulate <hex_bytes...> (e.g. wallpad simulate F7 0E 01 19 01 40 11 01 00 B6 EE)\r\n");
      return;
    }
    uint8_t sim_buf[64]{0};
    size_t sim_len = 0;
    for (int i = 2; i <= argc && sim_len < sizeof(sim_buf); ++i) {
      const char *tok = embeddedCliGetToken(args, i);
      if (!tok) break;
      char *endp = nullptr;
      unsigned long val = strtoul(tok, &endp, 16);
      if (endp != tok) {
        sim_buf[sim_len++] = static_cast<uint8_t>(val);
      }
    }
    if (sim_len < 3) {
      sendTelnetMsg(sock, "[ERROR] Simulated packet must be at least 3 bytes.\r\n");
      return;
    }
    g_auto_probing_engine.feedFrame(span<const uint8_t>(sim_buf, sim_len));
    sendTelnetMsgf(sock, "[OK] Fed %u simulated bytes into Auto-Probing Engine.\r\n", sim_len);
  } else if (strcasecmp(sub, "help") == 0 || strcasecmp(sub, "?") == 0) {
    g_cli_scratch_buf[0] = '\0';
    AppendBuf out{g_cli_scratch_buf, sizeof(g_cli_scratch_buf)};
    out.append("\r\n");
    out.append(Fmt::DIV80EQ);
    out.append("                    WALLPAD & PROTOCOL COMMAND REFERENCE                      \r\n");
    out.append(Fmt::DIV80EQ);
    out.append("Command                           Description\r\n");
    out.append(Fmt::DIV80);
    out.append("  wallpad [status]                Show auto-probing status, timings & locked profile\r\n");
    out.append("  wallpad list                    List available vendor & saved NVS custom profiles\r\n");
    out.append("  wallpad set <key|id>            Manually switch active wallpad vendor profile\r\n");
    out.append("  wallpad save <name>             Save current auto-learned profile to NVS slot\r\n");
    out.append("  wallpad delete <id>             Reset a saved custom profile slot in NVS\r\n");
    out.append("  wallpad auto                    Switch to Universal Auto-Probing mode\r\n");
    out.append("  wallpad reset                   Reset auto-probing engine and re-learn bus traffic\r\n");
    out.append("  wallpad simulate <hex...>       Inject raw hex packet into auto-probing engine\r\n");
    out.append(Fmt::DIV80EQ);
    out.append("Tip: Use 'ctl' for device control blueprints & learned slots.\r\n");
    out.append(Fmt::DIV80EQ);
    out.append("\r\n");
    sendTelnetMsgLen(sock, out.buf, out.offset);
  } else {
    sendTelnetMsg(sock, "Usage: wallpad [status | list | set <key|id> | save <name> | delete <id> | auto | reset | simulate <hex...> | help]\r\n");
  }
}

void cmdCtl(EmbeddedCli *cli, char *args, void *context) {
  auto *session = getSession(context);
  int sock = session ? session->sock : -1;
  if (sock < 0) return;

  if (session) {
    if (session->txLen > 0) {
      sendTelnetMsgLen(session->sock, session->txBuf, session->txLen);
      session->txLen = 0;
      session->needsSend = false;
    }
  }

  int argc = embeddedCliGetTokenCount(args);

  if (argc == 0) {
    g_cli_scratch_buf[0] = '\0';
    AppendBuf out{g_cli_scratch_buf, sizeof(g_cli_scratch_buf)};
    wallpadPrintControlTable(out);
    sendTelnetMsgLen(sock, out.buf, out.offset);
    if (session) {
      session->txLen = 0;
      session->needsSend = false;
    }
    return;
  }

  const char *sub = embeddedCliGetToken(args, 1);

  if (strcasecmp(sub, "table") == 0 || strcasecmp(sub, "list") == 0 || strcasecmp(sub, "view") == 0) {
    g_cli_scratch_buf[0] = '\0';
    AppendBuf out{g_cli_scratch_buf, sizeof(g_cli_scratch_buf)};
    wallpadPrintControlTable(out);
    sendTelnetMsgLen(sock, out.buf, out.offset);
  } else if (strcasecmp(sub, "reset") == 0) {
    if (argc < 2) {
      sendTelnetMsg(sock, "[ERROR] Usage: ctl reset <all | dev_id> (e.g. ctl reset all, ctl reset 0x18)\r\n");
      return;
    }
    const char *arg = embeddedCliGetToken(args, 2);
    if (strcasecmp(arg, "all") == 0) {
      g_control_registry.resetGroup(0, true);
      sendTelnetMsg(sock, "[OK] All control blueprints reset & re-synthesized from catalog specs.\r\n");
    } else {
      char *endp = nullptr;
      uint8_t dev_id = static_cast<uint8_t>(strtoul(arg, &endp, 0));
      if (endp == arg || dev_id == 0) {
        sendTelnetMsgf(sock, "[ERROR] Invalid target '%s'. Use 'ctl reset all' or 'ctl reset <dev_id>'.\r\n", arg);
        return;
      }
      g_control_registry.resetGroup(dev_id, false);
      sendTelnetMsgf(sock, "[OK] Control template for DevID 0x%02X action slots reset completed.\r\n", dev_id);
    }
  } else if (strcasecmp(sub, "name") == 0 || strcasecmp(sub, "setname") == 0) {
    if (argc >= 3) {
      uint8_t dev_id = static_cast<uint8_t>(strtoul(embeddedCliGetToken(args, 2), nullptr, 0));
      const char *name = embeddedCliGetToken(args, 3);
      if (dev_id == 0 || !name || strlen(name) == 0) {
        sendTelnetMsg(sock, "[ERROR] Usage: ctl name <dev_id> <custom_name> (e.g. ctl name 0x1B Gas)\r\n");
      } else {
        if (g_control_registry.setGroupName(dev_id, name)) {
          sendTelnetMsgf(sock, "[OK] DevID 0x%02X group name set to '%s' and saved to NVS flash.\r\n", dev_id, name);
        } else {
          sendTelnetMsgf(sock, "[ERROR] DevID 0x%02X not found in blueprint registry.\r\n", dev_id);
        }
      }
    } else {
      sendTelnetMsg(sock, "[ERROR] Usage: ctl name <dev_id> <custom_name> (e.g. ctl name 0x1B Gas)\r\n");
    }
  } else if (strcasecmp(sub, "class") == 0 || strcasecmp(sub, "setclass") == 0) {
    if (argc >= 3) {
      uint8_t dev_id = static_cast<uint8_t>(strtoul(embeddedCliGetToken(args, 2), nullptr, 0));
      const char *cls_str = embeddedCliGetToken(args, 3);
      const char *custom_name = (argc >= 4) ? embeddedCliGetToken(args, 4) : nullptr;
      DeviceClass cls = DeviceClass::UNKNOWN;
      const char *def_name = cls_str;

      if (strcasecmp(cls_str, "light") == 0 || strcasecmp(cls_str, "switch") == 0) {
        cls = DeviceClass::SWITCH;
        def_name = "Light";
      } else if (strcasecmp(cls_str, "outlet") == 0) {
        cls = DeviceClass::SWITCH;
        def_name = "Outlet";
      } else if (strcasecmp(cls_str, "vent") == 0 || strcasecmp(cls_str, "fan") == 0) {
        cls = DeviceClass::VENT;
        def_name = "Vent";
      } else if (strcasecmp(cls_str, "thermo") == 0 || strcasecmp(cls_str, "thermostat") == 0 || strcasecmp(cls_str, "heat") == 0) {
        cls = DeviceClass::THERMOSTAT;
        def_name = "Thermo";
      } else if (strcasecmp(cls_str, "gas") == 0) {
        cls = DeviceClass::GAS;
        def_name = "Gas";
      } else if (strcasecmp(cls_str, "aircon") == 0 || strcasecmp(cls_str, "ac") == 0) {
        cls = DeviceClass::AIRCON;
        def_name = "Aircon";
      } else if (strcasecmp(cls_str, "ev") == 0 || strcasecmp(cls_str, "elevator") == 0) {
        cls = DeviceClass::MOMENTARY;
        def_name = "Elevator";
      }

      if (dev_id == 0 || cls == DeviceClass::UNKNOWN) {
        sendTelnetMsg(sock, "[ERROR] Usage: ctl class <dev_id> <light|outlet|vent|thermo|gas|aircon|ev> [name]\r\n");
      } else {
        const char *final_name = (custom_name && strlen(custom_name) > 0) ? custom_name : def_name;
        if (g_control_registry.setGroupClass(dev_id, cls, final_name)) {
          sendTelnetMsgf(sock, "[OK] DevID 0x%02X class set to %s ('%s') and saved to NVS flash.\r\n", dev_id, cls_str, final_name);
        } else {
          sendTelnetMsgf(sock, "[ERROR] DevID 0x%02X not found in blueprint registry.\r\n", dev_id);
        }
      }
    } else {
      sendTelnetMsg(sock, "[ERROR] Usage: ctl class <dev_id> <light|outlet|vent|thermo|gas|aircon|ev> [name]\r\n");
    }
  } else if (strcasecmp(sub, "help") == 0 || strcasecmp(sub, "?") == 0) {
    g_cli_scratch_buf[0] = '\0';
    AppendBuf out{g_cli_scratch_buf, sizeof(g_cli_scratch_buf)};
    out.append("\r\n");
    out.append(Fmt::DIV80EQ);
    out.append("             DEVICE GROUP CONTROL BLUEPRINT COMMANDS                          \r\n");
    out.append(Fmt::DIV80EQ);
    out.append("Command                           Description\r\n");
    out.append("  ctl                             Display control blueprint table [view|list]\r\n");
    out.append("  ctl <dev_id>                    Inspect packet blueprint & slots (e.g. ctl 0x18)\r\n");
    out.append("  ctl name <dev_id> <name>        Set custom group name (e.g. Gas, Elevator)\r\n");
    out.append("  ctl reset <dev_id>              Reset action slots for specific device\r\n");
    out.append("  ctl reset all                   Factory wipe & re-inject blueprints from catalog\r\n");
    out.append(Fmt::DIV80EQ);
    out.append("\r\n");
    sendTelnetMsgLen(sock, out.buf, out.offset);
  } else {
    char *endp = nullptr;
    uint8_t dev_id = static_cast<uint8_t>(strtoul(sub, &endp, 0));
    if (endp != sub && dev_id != 0) {
      g_cli_scratch_buf[0] = '\0';
      AppendBuf out{g_cli_scratch_buf, sizeof(g_cli_scratch_buf)};
      wallpadPrintControlDetail(out, dev_id);
      sendTelnetMsgLen(sock, out.buf, out.offset);
    } else {
      sendTelnetMsg(sock, "Usage: ctl [<dev_id> | name <id> <name> | reset <all|id> | help]\r\n");
    }
  }
}

void wallpadPrintControlTable(AppendBuf &out) {
  out.append("\r\n");
  out.append(Fmt::DIV80EQ);
  out.append("                 DEVICE CONTROL BLUEPRINTS & ACTION SLOTS                     \r\n");
  out.append(Fmt::DIV80EQ);

  size_t count = g_control_registry.getGroupCount();
  out.appendFormat("  Registered Blueprints: %zu Groups | Auto-Mapped & NVS Persisted               \r\n", count);
  out.append(Fmt::DIV80);
  out.append("DevID  Name        Class      CTL_Len  Power Action Slot   QRY_Len  Status Offsets\r\n");
  out.append(Fmt::DIV80);

  if (count == 0) {
    out.append("  (No control blueprints registered yet. Waiting for profile or learning...)\r\n");
    out.append(Fmt::DIV80);
    out.append(Fmt::DIV80EQ);
    out.append("\r\n");
    return;
  }

  for (size_t i = 0; i < count; ++i) {
    GroupControlTemplate grp{};
    if (!g_control_registry.getGroupByIndex(i, grp) || grp.dev_id == 0) continue;

    const char *cls_str = "UNKNOWN";
    switch (grp.coverage.dev_class) {
      case DeviceClass::SWITCH:     cls_str = "SWITCH"; break;
      case DeviceClass::OUTLET:     cls_str = "OUTLET"; break;
      case DeviceClass::GAS:        cls_str = "GAS"; break;
      case DeviceClass::MOMENTARY:  cls_str = "MOMENT"; break;
      case DeviceClass::THERMOSTAT: cls_str = "THERMO"; break;
      case DeviceClass::VENT:       cls_str = "VENT"; break;
      case DeviceClass::AIRCON:     cls_str = "AIRCON"; break;
      default: break;
    }

    char name_safe[17] = {0};
    strncpy(name_safe, grp.group_name, sizeof(name_safe) - 1);

    char pwr_buf[24] = {0};
    if (grp.power_slot.discovered) {
      snprintf(pwr_buf, sizeof(pwr_buf), "#%u [0x%02X/0x%02X]",
               grp.power_slot.action_offset, grp.power_slot.on_val, grp.power_slot.off_val);
    } else {
      snprintf(pwr_buf, sizeof(pwr_buf), "-");
    }

    char ctl_len_str[12] = {0};
    if (grp.frame_len > 0) snprintf(ctl_len_str, sizeof(ctl_len_str), "%u Byte", grp.frame_len);
    else snprintf(ctl_len_str, sizeof(ctl_len_str), "-");

    char qry_len_str[12] = {0};
    if (grp.query_slots.expected_len > 0) snprintf(qry_len_str, sizeof(qry_len_str), "%u Byte", grp.query_slots.expected_len);
    else snprintf(qry_len_str, sizeof(qry_len_str), "-");

    char extra_slots[40] = {0};
    size_t e_off = 0;
    if (grp.query_slots.power_offset != 0xFF) {
      e_off += snprintf(extra_slots + e_off, sizeof(extra_slots) - e_off, "#%u", grp.query_slots.power_offset);
    } else {
      e_off += snprintf(extra_slots + e_off, sizeof(extra_slots) - e_off, "-");
    }

    if (grp.query_slots.target_temp_offset != 0xFF) {
      e_off += snprintf(extra_slots + e_off, sizeof(extra_slots) - e_off, " (TT:#%u)", grp.query_slots.target_temp_offset);
    }
    if (grp.query_slots.current_temp_offset != 0xFF) {
      e_off += snprintf(extra_slots + e_off, sizeof(extra_slots) - e_off, " (AT:#%u)", grp.query_slots.current_temp_offset);
    }
    if (grp.query_slots.fan_speed_offset != 0xFF) {
      e_off += snprintf(extra_slots + e_off, sizeof(extra_slots) - e_off, " (FS:#%u)", grp.query_slots.fan_speed_offset);
    }
    if (grp.query_slots.power_w_offset != 0xFF) {
      e_off += snprintf(extra_slots + e_off, sizeof(extra_slots) - e_off, " (W:#%u)", grp.query_slots.power_w_offset);
    }

    out.appendFormat("0x%02X   %-11s %-10s %-8s %-19s %-8s %s\r\n",
                     grp.dev_id, name_safe, cls_str,
                     ctl_len_str, pwr_buf,
                     qry_len_str, extra_slots);
  }

  out.append(Fmt::DIV80);
  out.append("  * TT: Target Temp, AT: Ambient Temp, FS: Fan Speed, W: Power Wattage\r\n");
  out.append(Fmt::DIV80EQ);
  out.append("\r\n");
}

void wallpadPrintControlDetail(AppendBuf &out, uint8_t dev_id) {
  const GroupControlTemplate *grp = g_control_registry.findGroup(dev_id);
  if (!grp) {
    out.appendFormat("[ERROR] Group 0x%02X not found in control blueprints.\r\n", dev_id);
    return;
  }

  char name_safe[17] = {0};
  strncpy(name_safe, grp->group_name, sizeof(name_safe) - 1);

  out.append("\r\n");
  out.append(Fmt::DIV80EQ);
  out.appendFormat("               DEVICE CONTROL BLUEPRINT DETAIL: 0x%02X (%s)                \r\n",
                   grp->dev_id, name_safe);
  out.append(Fmt::DIV80EQ);
  out.appendFormat("  Frame Specs     : CTL Length = %u Bytes | QRY Response Length = %u Bytes\r\n",
                   grp->frame_len, grp->query_slots.expected_len);
  out.appendFormat("  Addressing      : Sub1 = Offset #%u | Sub2 = Offset #%u (Override = 0x%02X)\r\n",
                   grp->sub1_offset, grp->sub2_offset, grp->ctl_sub1_override);
  out.append(Fmt::DIV80);

  out.append("[Outbound Action Slots]\r\n");
  if (grp->power_slot.discovered) {
    out.appendFormat("  Power Control : Offset #%u  [ ON: 0x%02X / OFF: 0x%02X ]\r\n",
                     grp->power_slot.action_offset, grp->power_slot.on_val, grp->power_slot.off_val);
  } else {
    out.append("  Power Control : None\r\n");
  }

  if (grp->temp_slot.discovered) {
    out.appendFormat("  Temp Control  : Offset #%u  [ Range: %u ~ %u C ]\r\n",
                     grp->temp_slot.action_offset, grp->temp_slot.min_val, grp->temp_slot.max_val);
  } else {
    out.append("  Temp Control  : None\r\n");
  }

  if (grp->speed_slot.discovered) {
    if (grp->speed_slot.level_count > 0) {
      char tok_str[64] = {0};
      for (uint8_t i = 0; i < grp->speed_slot.level_count; ++i) {
        char t_buf[16] = {0};
        snprintf(t_buf, sizeof(t_buf), "%sL%u:0x%02X", (i > 0 ? ", " : ""), i + 1, grp->speed_slot.level_tokens[i]);
        strncat(tok_str, t_buf, sizeof(tok_str) - strlen(tok_str) - 1);
      }
      out.appendFormat("  Speed Control : Offset #%u  [ Levels: %s ]\r\n",
                       grp->speed_slot.action_offset, tok_str);
    } else {
      out.appendFormat("  Speed Control : Offset #%u  [ Range: %u ~ %u ]\r\n",
                       grp->speed_slot.action_offset, grp->speed_slot.min_val, grp->speed_slot.max_val);
    }
  } else {
    out.append("  Speed Control : None\r\n");
  }

  if (grp->close_slot.discovered) {
    out.appendFormat("  Close Control : Offset #%u  [ Action: 0x%02X ]\r\n",
                     grp->close_slot.action_offset, grp->close_slot.off_val);
  } else {
    out.append("  Close Control : None\r\n");
  }

  out.append("\r\n[Inbound Status Slots]\r\n");
  auto format_slot = [](AppendBuf &b, const char *label, uint8_t off) {
    if (off != 0xFF) {
      b.appendFormat("  %-13s : Offset #%u\r\n", label, off);
    } else {
      b.appendFormat("  %-13s : None\r\n", label);
    }
  };

  format_slot(out, "Power State", grp->query_slots.power_offset);
  format_slot(out, "Target Temp", grp->query_slots.target_temp_offset);
  format_slot(out, "Ambient Temp", grp->query_slots.current_temp_offset);
  format_slot(out, "Fan Speed", grp->query_slots.fan_speed_offset);
  format_slot(out, "Power Wattage", grp->query_slots.power_w_offset);
  format_slot(out, "CTL ACK State", grp->ack_slots.power_offset);

  out.append(Fmt::DIV80EQ);
  out.append("\r\n");
}

} // namespace WallpadCli


