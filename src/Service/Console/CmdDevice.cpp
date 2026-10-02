#include "Service/BridgeService.h"
#include "Service/Console/ConsoleFmt.h"
#include "Service/ConsoleCli.h"
#include "Service/ConsoleCommands.h"
#include "Service/EngineTask.h"
#include "Service/RemoteService.h"
#include <WiFi.h>
#include <esp_ota_ops.h>

namespace WallpadCli {
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
