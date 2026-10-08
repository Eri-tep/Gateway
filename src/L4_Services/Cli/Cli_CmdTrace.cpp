#include "L4_Services/CLI_Commands.h"
#include "L3_Protocol/Public/Protocol_Facade.h"
#include "L3_Protocol/Public/Protocol_Device.h"
#include "L0_Foundation/System_Platform.h"
#include <WiFi.h>

namespace WallpadCli {

void wallpadPrintStatus(AppendBuf &out) {
  AutoProbingDescriptorSnapshot desc{};
  ProtocolDiag_GetAutoProbingDescriptor(desc);

  size_t active_targets = 0, verified_targets = 0, total_targets = 0;
  ProtocolDiag_GetPollingStats(active_targets, verified_targets, total_targets);
  size_t online_devs = Device_GetOnlineCount();

  const char *phase_str = "Phase 1/3 (Framing Probing)";
  if (desc.is_manual || desc.offsets_locked) {
    phase_str = "Phase 3/3: Fully Locked";
  } else if (desc.is_locked) {
    phase_str = "Phase 2/3: Cache Syncing";
  }

  CliFmt::PrintBoxHeader(out, "WALLPAD PROTOCOL AUTO-PROBING ENGINE STATUS");
  char prof_key_buf[16] = "Standard";
  ProtocolDiag_GetActiveProfileKey(prof_key_buf, sizeof(prof_key_buf));

  auto print_meta = [&](const char *fmt, ...) {
    FixedBuf<128> buf;
    va_list va;
    va_start(va, fmt);
    buf.appendFormatV(fmt, va);
    va_end(va);
    out.appendFormat("| %-76.76s |\r\n", buf.c_str());
  };

  const uint8_t active_prof = Config_GetWallpadProfile();
  print_meta("Active Profile  : %s (ID: %u)", prof_key_buf,
             static_cast<unsigned>(active_prof));
  if (active_prof == 0) {
    print_meta("Profile Mode    : Auto Adaptive [%s]", phase_str);
  } else {
    print_meta("Profile Mode    : Manual Fixed");
  }
  char cat_vendor[64] = {0};
  size_t cat_dev_cnt = 0;
  if (ProtocolDiag_GetCatalogMatch(cat_vendor, sizeof(cat_vendor), cat_dev_cnt)) {
    print_meta("Catalog Match   : %s (%u Devices Spec Injected)",
               cat_vendor, static_cast<unsigned>(cat_dev_cnt));
  } else {
    print_meta("Catalog Match   : None (Generic Framing Only)");
  }
  print_meta(
      "Devices Tracked : %u Active / %u Online (%u Offline) [%s]",
      static_cast<unsigned>(active_targets), static_cast<unsigned>(online_devs),
      static_cast<unsigned>(Device_GetCount() >= online_devs
                                ? (Device_GetCount() - online_devs)
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
    FixedBuf<64> v;
    va_list args;
    va_start(args, fmt);
    v.appendFormatV(fmt, args);
    va_end(args);
    print_row(f, p, v.c_str(), s);
  };

  uint8_t dev_ids[16], sub1_ids[16], sub2_ids[16];
  size_t dev_id_cnt = 0, sub1_cnt = 0, sub2_cnt = 0;
  ProtocolDiag_GetActiveAddresses(dev_ids, dev_id_cnt, sub1_ids, sub1_cnt,
                                  sub2_ids, sub2_cnt, 16);

  auto format_hex_list = [](const uint8_t *arr, size_t cnt, const char *prefix,
                            char *out, size_t out_sz) {
    AppendBuf ob{out, out_sz};
    ob.append(prefix);
    if (cnt == 0)
      return;
    ob.append(" : ");
    char hex_str[64] = {0};
    AppendBuf hb{hex_str, sizeof(hex_str)};
    for (size_t d = 0; d < cnt; ++d) {
      if (hb.offset + 5 >= 24) {
        hb.append(", ..");
        break;
      }
      hb.appendFormat("%s%02X", (d == 0 ? "" : ", "), arr[d]);
    }
    ob.append(hex_str);
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
                              char *saved_buf = nullptr,
                              size_t saved_buf_len = 0) {
    FixedBuf<32> label;
    char list_buf[64];
    if (desc.offsets_locked)
      label.appendFormat("Byte #%u", off);
    else
      label.append("Probing...");
    format_hex_list(ids, cnt, label.c_str(), list_buf, sizeof(list_buf));
    if (saved_buf && saved_buf_len > 0) {
      strncpy(saved_buf, list_buf, saved_buf_len - 1);
      saved_buf[saved_buf_len - 1] = '\0';
    }
    print_row("", param, list_buf, addr_status);
  };
  print_addr_field("[ID] Device", desc.dev_id_offset, dev_ids, dev_id_cnt);
  print_addr_field("[S1] Sub Addr", desc.sub1_offset, sub1_ids, sub1_cnt,
                   sub1_list_buf, sizeof(sub1_list_buf));
  if (desc.sub2_offset != 0xFF && desc.sub2_offset != desc.sub1_offset &&
      sub2_cnt > 0) {
    print_addr_field("[S2] Sub Addr", desc.sub2_offset, sub2_ids, sub2_cnt);
  }
  table.separator('-');

  FixedBuf<8> ctl_hex;
  if (desc.control_seen && desc.control_opcode != 0) {
    ctl_hex.appendFormat("%02X", desc.control_opcode);
  } else {
    ctl_hex.append("??");
  }

  const char *opcode_status = !desc.opcodes_locked ? "[LEARNING]"
                              : (!desc.control_seen || desc.control_opcode == 0)
                                  ? "[WAITING]"
                                  : "[LOCKED]";
  rowf("Command", "[OP] Opcode", opcode_status,
       "Byte #%u : QRY:%02X, CTL:%s, ACK:%02X", desc.opcode_offset,
       desc.query_opcode, ctl_hex.c_str(), desc.ack_opcode);

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
  FixedBuf<32> pl_r, pl_l;
  if (desc.offsets_locked) {
    pl_r.appendFormat("Byte #%u ~ #[N-3]", desc.payload_offset);
    pl_l.appendFormat("Data = [LEN - %u] Byte",
                      desc.payload_offset + 2);
  } else {
    pl_r.append("Byte #7 ~ #[N-3] : Est");
    pl_l.append("Data = [LEN - 9] Byte : Est");
  }
  print_row("Payload", "[PL] Data Range", pl_r.c_str(), payload_status);
  print_row("", "[PL] Length", pl_l.c_str(), payload_status);
  table.separator('-');

  const char *tail_status = desc.is_locked ? "[LOCKED]" : "[LEARNING]";
  rowf("Tail", "[CS] Checksum", tail_status, "Byte #[N-2] : %s",
       desc.checksum_algo_name);
  rowf("", "[ET] ETX", tail_status, "Byte #[N-1] : %02X",
       desc.etx);
  table.separator('-');

  const auto &cfg = Config_Get();
  uint32_t b1 = cfg.uart_baud_rate, b2 = cfg.ch2_baud_rate,
           b3 = cfg.ch3_baud_rate;
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

  FramingStatus dp_status = FramingStatus::WAITING;
  uint8_t cur_dp_stx = 0;
  uint8_t cur_dp_etx = 0;
  uint8_t cur_dp_len = 0;
  ProtocolDiag_DoorphoneGetFraming(dp_status, cur_dp_stx, cur_dp_etx, cur_dp_len);

  const char *dp_status_str =
      (dp_status == FramingStatus::LOCKED)     ? "[LOCKED]"
      : (dp_status == FramingStatus::LEARNING) ? "[LEARNING]"
      : (dp_status == FramingStatus::NOISY)    ? "[NOISY]"
                                               : "[WAITING]";

  if (dp_status == FramingStatus::WAITING) {
    print_row("Doorphone (CH4)", "Framing", "-- .. --", dp_status_str);
  } else if (cur_dp_len > 0) {
    rowf("Doorphone (CH4)", "Framing", dp_status_str, "%02X .. %02X (%u Bytes)",
         cur_dp_stx, cur_dp_etx, cur_dp_len);
  } else {
    rowf("Doorphone (CH4)", "Framing", dp_status_str, "%02X .. %02X",
         cur_dp_stx, cur_dp_etx);
  }

  DoorphoneMatchSnapshot dp_match{};
  bool has_match = ProtocolDiag_GetDoorphoneMatch(dp_match);
  const char *dp_m_st =
      has_match ? ((dp_status == FramingStatus::LOCKED)
                     ? "[LOCKED]"
                     : "[LEARNING]")
                : ((dp_status == FramingStatus::WAITING)
                     ? "[WAITING]"
                     : "[UNKNOWN]");
  FixedBuf<48> op_f, op_l;
  const char *dp_desc = nullptr;
  if (has_match) {
    dp_desc = dp_match.desc;
    op_f.appendFormat("Bell:%02X, Call:%02X, Open:%02X, End:%02X",
                      dp_match.bell_front, dp_match.call_front, dp_match.open_front,
                      dp_match.end_front);
    op_l.appendFormat("Bell:%02X, Call:%02X, Open:%02X, End:%02X",
                      dp_match.bell_lobby, dp_match.call_lobby, dp_match.open_lobby,
                      dp_match.end_lobby);
  } else if (dp_status == FramingStatus::WAITING) {
    dp_desc = "Waiting for traffic...";
    op_f.append("Waiting...");
    op_l.append("Waiting...");
  } else {
    dp_desc = "No Catalog Match";
    op_f.append("Bell:B5, Call:B9, Open:B4, End:B8");
    op_l.append("Bell:5A, Call:5F, Open:61, End:60");
  }
  print_row("", "Catalog Match", dp_desc, dp_m_st);
  print_row("", "Opcodes(F)", op_f.c_str(), dp_m_st);
  print_row("", "Opcodes(L)", op_l.c_str(), dp_m_st);

  rowf("", "Baudrate", "[CONFIG]", "%u bps",
       static_cast<unsigned>(Config_Get().doorphone_baud_rate));
  rowf("", "Time-gap", "[CONFIG]", "%u ms",
       static_cast<unsigned>(Config::Timing::DOORPHONE_IPG_MS));
  rowf("", "Debounce", "[CONFIG]", "%u ms",
       static_cast<unsigned>(Config::Timing::DOORPHONE_DEBOUNCE_MS));
  table.separator('-');

  {
    FixedBuf<16> p_buf;
    FixedBuf<32> val_buf;
    for (int s = 0; s < Config::TCP::MAX_EW11_SLOTS; s++) {
      HubClientSlotSnapshot slot;
      System_GetBridgeSlotSnapshot(static_cast<uint8_t>(s), slot);
      p_buf.reset();
      val_buf.reset();
      p_buf.appendFormat("%u",
                         slot.target_port ? slot.target_port
                                          : Config::TCP::EW11_SLOT_PORTS[s]);
      const char *f_label = (s == 0) ? "EW11 (CH5)" : "";
      const char *st = "[WAITING]";
      if (!slot.enabled && !slot.is_connected && slot.target_ip[0] == '\0') {
        val_buf.append("Disabled");
        st = "[UNUSED]";
      } else if (slot.is_connected) {
        val_buf.appendFormat("Connect: %s",
                             slot.target_ip[0] ? slot.target_ip : "-");
        st = "[ACTIVE]";
      } else {
        val_buf.append("Listening");
      }
      print_row(f_label, p_buf.c_str(), val_buf.c_str(), st);
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
  auto format_compact = [](FixedBuf<16> &buf, uint32_t count) {
    if (count >= 1000000)
      buf.appendFormat("%.1fM", count / 1000000.0);
    else if (count >= 1000)
      buf.appendFormat("%.1fk", count / 1000.0);
    else
      buf.appendFormat("%u", static_cast<unsigned>(count));
  };
  FixedBuf<16> m_str, t_str;
  format_compact(m_str, desc.matched_packets);
  format_compact(t_str, desc.tested_packets);
  const char *cs_status =
      (cs_pct >= 95) ? "[STABLE]" : (cs_pct >= 80 ? "[NOISY]" : "[ERROR]");
  rowf("", "CS Validation", cs_status, "%s / %s Packets (%u%%)", m_str.c_str(), t_str.c_str(),
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

  FixedBuf<8> id_buf;
  size_t prof_cnt = ProtocolDiag_GetProfileCount();
  const uint8_t cur_prof = Config_GetWallpadProfile();
  for (size_t i = 0; i < prof_cnt; ++i) {
    ProfileInfoSnapshot p_desc;
    if (ProtocolDiag_GetProfileInfo(i, p_desc)) {
      bool is_current = (cur_prof == i);
      bool is_empty = (i > 0 && strncmp(p_desc.name, "[Empty", 6) == 0);
      const char *status_str = is_current
                                   ? ">> ACTIVE <<"
                                   : (is_empty ? "Available" : "Saved (NVS)");
      id_buf.reset();
      id_buf.appendFormat("%2u", static_cast<unsigned>(i));
      table.row({id_buf.c_str(), p_desc.key, p_desc.name, status_str});
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
  if (ProtocolDiag_SaveCurrentProfileAs(name, saved_slot)) {
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
  size_t prof_cnt = ProtocolDiag_GetProfileCount();
  if (endp != target && *endp == '\0' && val >= 1 &&
      val < static_cast<long>(prof_cnt)) {
    idx = static_cast<size_t>(val);
  } else {
    ProfileInfoSnapshot pd;
    for (size_t i = 1; i < prof_cnt; ++i) {
      if (ProtocolDiag_GetProfileInfo(i, pd) &&
          strcasecmp(pd.key, target) == 0) {
        idx = i;
        break;
      }
    }
  }
  if (idx >= 1 && idx < prof_cnt && ProtocolDiag_DeleteProfile(idx)) {
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
      val < static_cast<long>(ProtocolDiag_GetProfileCount())) {
    ok = ProtocolDiag_SetActiveProfile(static_cast<size_t>(val));
  } else {
    ok = ProtocolDiag_SetActiveProfileByKey(key);
  }

  if (ok) {
    char v_name[32] = {0};
    char p_key[16] = {0};
    ProtocolDiag_GetActiveVendorName(v_name, sizeof(v_name));
    ProtocolDiag_GetActiveProfileKey(p_key, sizeof(p_key));
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
        "Tip: Use 'q' shortcut to quickly stop active tracing");
    return;
  }

  auto &tracer = CLI_GetTracer();

  if (strcasecmp(sub, "off") == 0) {
    tracer.setTrace(false);
    sendTelnetMsg(sock, "Packet trace DISABLED.\r\n");
    return;
  }

  tracer.setClient(sock);
  tracer.setTrace(true);

  switch (Hash::fnv1a32_ci_rt(sub)) {
  case Hash::fnv1a32_ci("on"):
    tracer.setFilter(TraceType::ALL);
    sendTelnetMsg(sock, "Packet trace ENABLED: ALL packets.\r\n");
    return;
  case Hash::fnv1a32_ci("ctl"):
    tracer.setFilter(TraceType::CTL);
    sendTelnetMsg(sock, "Packet trace ENABLED: CONTROL packets only.\r\n");
    return;
  case Hash::fnv1a32_ci("ack"):
    tracer.setFilter(TraceType::ACK);
    sendTelnetMsg(sock, "Packet trace ENABLED: ACK/Response packets only.\r\n");
    return;
  case Hash::fnv1a32_ci("pol"):
    tracer.setFilter(TraceType::QRY);
    sendTelnetMsg(sock, "Packet trace ENABLED: Polling queries only.\r\n");
    return;
  case Hash::fnv1a32_ci("rmt"):
    tracer.setFilter(TraceType::RMT);
    sendTelnetMsg(sock, "Packet trace ENABLED: Doorphone packets only.\r\n");
    return;
  case Hash::fnv1a32_ci("drp"):
    tracer.setFilter(TraceType::DRP);
    sendTelnetMsg(sock, "Packet trace ENABLED: Dropped packets only.\r\n");
    return;
  default:
    break;
  }

  std::string_view sub_sv(sub);

  // Channel filter: "ch <1-6>" or "ch<1-6>"
  bool is_ch_prefix = (sub_sv.rfind("ch", 0) == 0 || sub_sv.rfind("CH", 0) == 0);
  if (is_ch_prefix) {
    int ch_val = 0;
    bool ch_ok = false;
    if (sub_sv.size() == 2 && token_count >= 2) {
      ch_ok = CliFmt::ParseInt(ctx.args.get(2), ch_val, 1, 6);
    } else if (sub_sv.size() == 3 && isdigit(static_cast<unsigned char>(sub[2]))) {
      ch_val = sub[2] - '0';
      ch_ok = (ch_val >= 1 && ch_val <= 6);
    }

    if (ch_ok) {
      tracer.setFilter(TraceType::CH, static_cast<uint8_t>(ch_val));
      sendTelnetMsgf(sock, "Packet trace ENABLED: Channel %u only.\r\n",
                     static_cast<unsigned>(ch_val));
    } else {
      sendTelnetMsg(sock, "[ERROR] Invalid channel: trace ch <1-6>\r\n");
    }
    return;
  }

  // Device ID filter: "devid <hex>" or "0x<hex>"
  bool is_devid_cmd = (strcasecmp(sub, "devid") == 0);
  bool is_hex_prefix = (sub_sv.rfind("0x", 0) == 0 || sub_sv.rfind("0X", 0) == 0);
  if (is_devid_cmd || is_hex_prefix) {
    uint8_t id = 0;
    if (is_devid_cmd && token_count >= 2) {
      id = static_cast<uint8_t>(strtol(ctx.args.get(2), nullptr, 16));
    } else if (is_hex_prefix) {
      id = static_cast<uint8_t>(strtol(sub, nullptr, 16));
    }
    tracer.setFilter(TraceType::DEVID, id);
    sendTelnetMsgf(sock, "Packet trace ENABLED: Device ID 0x%02X only.\r\n", id);
    return;
  }

  // If unrecognized, show reference help
  CliFmt::PrintSubCmdHelp(
      sock, "TRACE COMMAND REFERENCE", kTraceHelp,
      "Tip: Use 'q' shortcut to quickly stop active tracing");
}

void cmdStop(CliContext &ctx) {
  int sock = ctx.sock;
  CLI_GetTracer().setTrace(false);
  sendTelnetMsg(sock, "Packet trace DISABLED.\r\n");
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
         ProtocolDiag_SetActiveProfile(0);
         ProtocolDiag_WallpadReset();
         sendTelnetMsg(s, "[OK] Switched to Universal Auto-Probing mode "
                          "(Wallpad & Doorphone framing reset).\r\n");
       }},
      {"reset", "reset", "Reset auto-probing engine and re-learn",
       [](int s, int, const Args &) {
         ProtocolDiag_WallpadReset();
         ProtocolDiag_RequestRelearn();
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
         ProtocolDiag_AutoProbingFeedFrame(sim_buf, sim_len);
         sendTelnetMsgf(
             s, "[OK] Fed %u simulated bytes into Auto-Probing Engine.\r\n",
             sim_len);
       }},
  };

  if (CliFmt::IsHelp(sub)) {
    CliFmt::PrintSubCmdHelp(
        sock, "WALLPAD COMMAND REFERENCE", kWallpadDefs,
        "Tip: Use 'ctl' for device control blueprints & slots");
    return;
  }

  if (CliFmt::DispatchSubCmd(sub, sock, argc, ctx.args, kWallpadDefs))
    return;

  CliFmt::PrintSubCmdHelp(
      sock, "WALLPAD COMMAND REFERENCE", kWallpadDefs,
      "Tip: Use 'ctl' for device control blueprints & slots");
}

} // namespace WallpadCli
