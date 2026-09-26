#include "CliCommon.h"

namespace WallpadCli {

void wallpadPrintStatus(AppendBuf &out) {
  auto *active = WallpadParserFactory::getActiveParser();
  auto desc = g_auto_probing_engine.getDescriptor();
  VendorProfileDescriptor active_prof;
  bool is_manual_prof = false;
  if (ProfileRepository::getActiveProfile(active_prof) && strcasecmp(active_prof.key, "auto") != 0) {
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
    desc.payload_offset = std::max({active_prof.opcode_offset, active_prof.dev_id_offset,
                                    active_prof.sub1_offset, active_prof.sub2_offset}) + 1;
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
  const auto *matched_p = ProfileMatcher::getActiveProfile();
  if (matched_p) {
    out.appendFormat("Catalog Match   : %s (%u Devices Spec Injected)\r\n",
                     matched_p->vendor_name, static_cast<unsigned>(matched_p->device_count));
  } else {
    out.append("Catalog Match   : None (Generic Framing Only)\r\n");
  }
  out.appendFormat("Devices Tracked : %u Active / %u Online (%u Offline) [%s]\r\n",
                   static_cast<unsigned>(active_targets), static_cast<unsigned>(online_devs),
                   static_cast<unsigned>(g_device_repo.count() >= online_devs ? (g_device_repo.count() - online_devs) : 0),
                   (online_devs >= active_targets && active_targets > 0) ? "100% Synced" : "Syncing");
  out.append(Fmt::DIV80);
  out.append("Packet Field    Parameter       Value / Layout Rule                       Status\r\n");
  out.append(Fmt::DIV80);

  char stx_buf[16], etx_buf[16], len_buf[32], pkt_len_buf[32];
  snprintf(stx_buf, sizeof(stx_buf), "%02X", active ? active->getStx() : 0xF7);
  snprintf(etx_buf, sizeof(etx_buf), "%02X", active ? active->getEtx() : 0xEE);
  snprintf(len_buf, sizeof(len_buf), "Byte #1");

  size_t total_tgts = g_polling_targets.totalCount();

  uint8_t q_lens[8], ack_lens[8];
  size_t q_len_cnt = 0, ack_len_cnt = 0;

  auto add_unique_len = [](uint8_t *arr, size_t &cnt, uint8_t len) {
    if (len >= 3 && len <= 64 && cnt < 8) {
      if (std::find(arr, arr + cnt, len) == arr + cnt) {
        arr[cnt++] = len;
      }
    }
  };

  for (size_t i = 0; i < total_tgts; ++i) {
    PollingTargetEntry entry;
    if (g_polling_targets.getEntry(i, entry)) {
      if (entry.raw_query_len > 0) add_unique_len(q_lens, q_len_cnt, entry.raw_query_len);
      if (entry.raw_ack_len > 0) add_unique_len(ack_lens, ack_len_cnt, entry.raw_ack_len);
    }
  }
  std::sort(q_lens, q_lens + q_len_cnt);
  std::sort(ack_lens, ack_lens + ack_len_cnt);

  auto format_lens = [](const uint8_t *arr, size_t cnt, char *out, size_t out_sz, const char *fallback) {
    if (cnt == 0) {
      snprintf(out, out_sz, "%s", fallback);
      return;
    }
    size_t off = 0;
    for (size_t i = 0; i < cnt; ++i) {
      off += snprintf(out + off, out_sz - off, "%s%u", (i == 0 ? "" : ", "), arr[i]);
    }
    snprintf(out + off, out_sz - off, " Byte");
  };

  char def_len_buf[16];
  if (desc.learned_query_len > 0) {
    snprintf(def_len_buf, sizeof(def_len_buf), "%u Byte", desc.learned_query_len);
  } else {
    snprintf(def_len_buf, sizeof(def_len_buf), "Waiting");
  }

  char q_str[32], ack_str[32];
  format_lens(q_lens, q_len_cnt, q_str, sizeof(q_str), def_len_buf);
  format_lens(ack_lens, ack_len_cnt, ack_str, sizeof(ack_str), def_len_buf);

  char len_prefix[16];
  if (desc.has_len_field) {
    snprintf(len_prefix, sizeof(len_prefix), "Byte #%u", desc.len_offset);
  } else {
    snprintf(len_prefix, sizeof(len_prefix), desc.is_locked ? "Fixed" : "Waiting");
  }

  char q_val[48], cmd_val[48], ack_val[48];
  snprintf(q_val, sizeof(q_val), "%s : %s", len_prefix, q_str);
  if (desc.ctrl_len_cnt > 0) {
    char cmd_lens_str[32];
    format_lens(desc.learned_ctrl_lens, desc.ctrl_len_cnt, cmd_lens_str, sizeof(cmd_lens_str), def_len_buf);
    snprintf(cmd_val, sizeof(cmd_val), "%s : %s", len_prefix, cmd_lens_str);
  } else {
    snprintf(cmd_val, sizeof(cmd_val), "%s : Waiting", len_prefix);
  }
  snprintf(ack_val, sizeof(ack_val), "%s : %s", len_prefix, ack_str);

  const char *len_status = desc.is_locked ? "[LOCKED]" : "[LEARNING]";
  const char *cmd_status = (desc.control_seen && desc.ctrl_len_cnt > 0) ? "[LOCKED]" : "[WAITING]";

  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "Header", "[ST] STX", stx_buf, len_status);
  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "", "[LN] Query", q_val, len_status);
  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "", "[LN] Command", cmd_val, cmd_status);
  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "", "[LN] Response", ack_val, len_status);
  out.append(Fmt::DIV80);

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
    snprintf(out, out_sz, "%s : %s", prefix, hex_str);
  };

  char addr_mode_buf[48];
  if (desc.offsets_locked) {
    if (desc.is_swapped_addr) {
      snprintf(addr_mode_buf, sizeof(addr_mode_buf), "Swapped (GW:Byte#%u <-> ID:Byte#%u)",
               desc.gw_addr_offset, desc.dev_id_offset);
    } else {
      snprintf(addr_mode_buf, sizeof(addr_mode_buf), "Direct (Single Address)");
    }
  } else {
    snprintf(addr_mode_buf, sizeof(addr_mode_buf), "%s", desc.is_locked ? "Probing..." : "Waiting");
  }
  const char *addr_status = desc.offsets_locked ? "[LOCKED]" : (desc.is_locked ? "[LEARNING]" : "[WAITING]");

  char gw_val_buf[48];
  if (desc.offsets_locked) {
    if (desc.gw_addr_offset != 0xFF) {
      snprintf(gw_val_buf, sizeof(gw_val_buf), "Byte #%u : %02X", desc.gw_addr_offset, desc.gw_addr);
    } else {
      snprintf(gw_val_buf, sizeof(gw_val_buf), "Val: %02X", desc.gw_addr);
    }
  } else {
    snprintf(gw_val_buf, sizeof(gw_val_buf), "-");
  }

  char dev_off_label[32], sub1_off_label[32], sub2_off_label[32];
  if (desc.offsets_locked) {
    snprintf(dev_off_label, sizeof(dev_off_label), "Byte #%u", desc.dev_id_offset);
    snprintf(sub1_off_label, sizeof(sub1_off_label), "Byte #%u", desc.sub1_offset);
    snprintf(sub2_off_label, sizeof(sub2_off_label), "Byte #%u", desc.sub2_offset);
  } else {
    snprintf(dev_off_label, sizeof(dev_off_label), "Probing...");
    snprintf(sub1_off_label, sizeof(sub1_off_label), "Probing...");
    snprintf(sub2_off_label, sizeof(sub2_off_label), "Probing...");
  }

  char dev_list_buf[64], sub1_list_buf[64], sub2_list_buf[64];
  format_hex_list(dev_ids, dev_id_cnt, dev_off_label, dev_list_buf, sizeof(dev_list_buf));
  format_hex_list(sub1_ids, sub1_cnt, sub1_off_label, sub1_list_buf, sizeof(sub1_list_buf));
  format_hex_list(sub2_ids, sub2_cnt, sub2_off_label, sub2_list_buf, sizeof(sub2_list_buf));

  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "Addressing", "Addr Mode", addr_mode_buf, addr_status);
  if (desc.offsets_locked) {
    out.appendFormat("%-16s%-16s%-38s%10s\r\n", "", "[GW] Master", gw_val_buf, addr_status);
  }
  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "", "[ID] Device", dev_list_buf, addr_status);
  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "", "[S1] Sub Addr", sub1_list_buf, addr_status);
  if (desc.sub2_offset != 0xFF && desc.sub2_offset != desc.sub1_offset && sub2_cnt > 0) {
    out.appendFormat("%-16s%-16s%-38s%10s\r\n", "", "[S2] Sub Addr", sub2_list_buf, addr_status);
  }
  out.append(Fmt::DIV80);

  char op_line_buf[48];
  char ctl_hex[8];
  if (desc.control_seen && desc.control_opcode != 0) {
    snprintf(ctl_hex, sizeof(ctl_hex), "%02X", desc.control_opcode);
  } else {
    snprintf(ctl_hex, sizeof(ctl_hex), "??");
  }
  snprintf(op_line_buf, sizeof(op_line_buf), "Byte #%u : QRY:%02X, CTL:%s, ACK:%02X",
           desc.opcode_offset, desc.query_opcode, ctl_hex, desc.ack_opcode);

  const char *opcode_status;
  if (!desc.opcodes_locked) {
    opcode_status = "[LEARNING]";
  } else if (!desc.control_seen || desc.control_opcode == 0) {
    opcode_status = "[WAITING]";        // QRY+ACK 확정, CTL은 아직 미관측(대기)
  } else {
    opcode_status = "[LOCKED]";         // QRY+CTL+ACK 모두 확정
  }

  char seq_line_buf[32];
  if (desc.has_seq_counter) {
    snprintf(seq_line_buf, sizeof(seq_line_buf), "Byte #%u : +1 Counter", desc.seq_offset);
  } else {
    snprintf(seq_line_buf, sizeof(seq_line_buf), desc.offsets_locked ? "-" : "None");
  }
  const char *seq_status = desc.offsets_locked ? (desc.has_seq_counter ? "[LOCKED]" : "[UNUSED]") : "[WAITING]";

  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "Command", "[OP] Opcode", op_line_buf, opcode_status);
  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "", "Sequence", seq_line_buf, seq_status);
  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "", "[CX] Context", sub1_list_buf, addr_status);
  out.append(Fmt::DIV80);

  char payload_range_buf[48];
  char payload_len_buf[32];
  if (desc.offsets_locked) {
    snprintf(payload_range_buf, sizeof(payload_range_buf), "Byte #%u ~ #[N-3]", desc.payload_offset);
    snprintf(payload_len_buf, sizeof(payload_len_buf), "Data = [LEN - %u] Byte", desc.payload_offset + 2);
  } else {
    snprintf(payload_range_buf, sizeof(payload_range_buf), "Byte #7 ~ #[N-3] : Est");
    snprintf(payload_len_buf, sizeof(payload_len_buf), "Data = [LEN - 9] Byte : Est");
  }
  const char *payload_status = desc.offsets_locked ? "[LOCKED]" : "[ESTIMATE]";
  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "Payload", "[PL] Data Range", payload_range_buf, payload_status);
  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "", "[PL] Length", payload_len_buf, payload_status);
  out.append(Fmt::DIV80);

  char cs_algo_buf[48];
  snprintf(cs_algo_buf, sizeof(cs_algo_buf), "Byte #[N-2] : %s", AutoProbingEngine::getAlgoName(desc.checksum_algo));
  char etx_line_buf[32];
  snprintf(etx_line_buf, sizeof(etx_line_buf), "Byte #[N-1] : %s", etx_buf);

  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "Tail", "[CS] Checksum", cs_algo_buf, desc.is_locked ? "[LOCKED]" : "[LEARNING]");
  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "", "[ET] ETX", etx_line_buf, desc.is_locked ? "[LOCKED]" : "[LEARNING]");
  out.append(Fmt::DIV80);

  uint32_t b1 = g_config.uart_baud_rate;
  uint32_t b2 = g_config.ch2_baud_rate;
  uint32_t b3 = g_config.ch3_baud_rate;

  char baud_buf[48];
  if (b1 == b2 && b2 == b3) {
    snprintf(baud_buf, sizeof(baud_buf), "%u bps : CH1~3", static_cast<unsigned>(b1));
    out.appendFormat("%-16s%-16s%-38s%10s\r\n", "Bus Physical", "Baudrate", baud_buf, "[CONFIG]");
  } else {
    snprintf(baud_buf, sizeof(baud_buf), "CH1:%u, CH2:%u, CH3:%u",
             static_cast<unsigned>(b1), static_cast<unsigned>(b2), static_cast<unsigned>(b3));
    out.appendFormat("%-16s%-16s%-38s%10s\r\n", "Bus Physical", "Baudrate", baud_buf, "[CONFIG]");
  }
  char ipg_silence_buf[48];
  snprintf(ipg_silence_buf, sizeof(ipg_silence_buf), "%u ms : CH1~3",
           static_cast<unsigned>(Config::Timing::WALLPAD_AUTO_IPG_MS));
  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "", "IPG Silence", ipg_silence_buf, "[CONFIG]");
  out.append(Fmt::DIV80);

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

  char dp_baud_buf[32];
  snprintf(dp_baud_buf, sizeof(dp_baud_buf), "%u bps", static_cast<unsigned>(g_config.doorphone_baud_rate));
  char dp_ipg_buf[32];
  snprintf(dp_ipg_buf, sizeof(dp_ipg_buf), "%u ms", static_cast<unsigned>(Config::Timing::DOORPHONE_IPG_MS));
  char dp_debounce_buf[32];
  snprintf(dp_debounce_buf, sizeof(dp_debounce_buf), "%u ms", static_cast<unsigned>(Config::Timing::DOORPHONE_DEBOUNCE_MS));

  uint8_t cur_dp_stx = g_doorphone_tracker.candidate_stx.load(std::memory_order_relaxed);
  uint8_t cur_dp_etx = g_doorphone_tracker.candidate_etx.load(std::memory_order_relaxed);
  uint8_t cur_dp_len = g_doorphone_tracker.candidate_len.load(std::memory_order_relaxed);
  const DoorphoneSpec *dp_prof =
      ProfileMatcher::matchDoorphone(cur_dp_stx, cur_dp_etx, cur_dp_len);

  char dp_match_buf[48];
  char dp_ops_f_buf[64];
  char dp_ops_l_buf[64];
  const char *dp_match_status = "[WAITING]";
  const char *dp_ops_status = "[WAITING]";

  if (dp_prof) {
    snprintf(dp_match_buf, sizeof(dp_match_buf), "%s", dp_prof->desc);
    dp_match_status = (dp_status == Config::Doorphone::FramingStatus::LOCKED) ? "[LOCKED]" : "[LEARNING]";
    dp_ops_status = dp_match_status;
    snprintf(dp_ops_f_buf, sizeof(dp_ops_f_buf), "Bell:%02X, Call:%02X, Open:%02X, End:%02X",
             dp_prof->bell_front, dp_prof->call_front, dp_prof->open_front, dp_prof->end_front);
    snprintf(dp_ops_l_buf, sizeof(dp_ops_l_buf), "Bell:%02X, Call:%02X, Open:%02X, End:%02X",
             dp_prof->bell_lobby, dp_prof->call_lobby, dp_prof->open_lobby, dp_prof->end_lobby);
  } else if (dp_status == Config::Doorphone::FramingStatus::WAITING) {
    snprintf(dp_match_buf, sizeof(dp_match_buf), "Waiting for traffic...");
    snprintf(dp_ops_f_buf, sizeof(dp_ops_f_buf), "Waiting...");
    snprintf(dp_ops_l_buf, sizeof(dp_ops_l_buf), "Waiting...");
    dp_match_status = "[WAITING]";
    dp_ops_status = "[WAITING]";
  } else {
    snprintf(dp_match_buf, sizeof(dp_match_buf), "No Catalog Match");
    snprintf(dp_ops_f_buf, sizeof(dp_ops_f_buf), "Bell:B5, Call:B9, Open:B4, End:B8");
    snprintf(dp_ops_l_buf, sizeof(dp_ops_l_buf), "Bell:5A, Call:5F, Open:61, End:60");
    dp_match_status = "[UNKNOWN]";
    dp_ops_status = "[UNKNOWN]";
  }

  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "Doorphone (CH4)", "Framing", dp_frame_buf, dp_status_str);
  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "", "Catalog Match", dp_match_buf, dp_match_status);
  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "", "Opcodes(F)", dp_ops_f_buf, dp_ops_status);
  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "", "Opcodes(L)", dp_ops_l_buf, dp_ops_status);
  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "", "Baudrate", dp_baud_buf, "[CONFIG]");
  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "", "Time-gap", dp_ipg_buf, "[CONFIG]");
  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "", "Debounce", dp_debounce_buf, "[CONFIG]");
  out.append(Fmt::DIV80);

  {
    MutexLocker lock(g_ch5_mutex);
    bool first_ew11 = true;
    for (int s = 0; s < Config::TCP::MAX_EW11_SLOTS; s++) {
      auto &slot = g_hub_slots[s];
      uint16_t listen_port = slot.target_port ? slot.target_port : Config::TCP::EW11_SLOT_PORTS[s];
      char port_title[24];
      snprintf(port_title, sizeof(port_title), "%u", listen_port);

      char slot_detail[48];
      const char *slot_status_str = "[UNUSED]";

      if (!slot.enabled && !slot.is_connected && slot.target_ip[0] == '\0') {
        snprintf(slot_detail, sizeof(slot_detail), "Disabled");
        slot_status_str = "[UNUSED]";
      } else {
        const char *ip_str = slot.target_ip[0] ? slot.target_ip : "-";
        if (slot.is_connected) {
          snprintf(slot_detail, sizeof(slot_detail), "Connect: %s", ip_str);
          slot_status_str = "[ACTIVE]";
        } else {
          snprintf(slot_detail, sizeof(slot_detail), "Listening");
          slot_status_str = "[WAITING]";
        }
      }

      out.appendFormat("%-16s%-16s%-38s%10s\r\n",
                       first_ew11 ? "EW11 (CH5)" : "",
                       port_title, slot_detail, slot_status_str);
      first_ew11 = false;
    }
  }
  out.append(Fmt::DIV80);

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
  auto format_compact = [](char *buf, size_t sz, uint32_t count) {
    if (count >= 1000000) {
      snprintf(buf, sz, "%.1fM", count / 1000000.0);
    } else if (count >= 1000) {
      snprintf(buf, sz, "%.1fk", count / 1000.0);
    } else {
      snprintf(buf, sz, "%u", static_cast<unsigned>(count));
    }
  };
  char m_str[16], t_str[16];
  format_compact(m_str, sizeof(m_str), desc.matched_packets);
  format_compact(t_str, sizeof(t_str), desc.tested_packets);

  snprintf(cs_rate_buf, sizeof(cs_rate_buf), "%s / %s Packets (%u%%)",
           m_str, t_str, static_cast<unsigned>(cs_pct));
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

} // namespace WallpadCli
