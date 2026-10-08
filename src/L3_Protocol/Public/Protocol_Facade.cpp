// ============================================================================
// ProtocolDiagnostics: Level 3 Public Diagnostics & Engine Facade Implementation
// ============================================================================

#include "L3_Protocol/Public/Protocol_Facade.h"
#include "L3_Protocol/Private/Routing_Engine.h"
#include "L2_Transport/RS485_CH.h"
#include "L2_Transport/TCP_CH.h"
#include "L2_Transport/Bridge_CH.h"
#include "L3_Protocol/Private/Wallpad_Engine.h"
#include "L3_Protocol/Private/Fcu_Engine.h"
#include <algorithm>
#include <atomic>
#include <span>

void ProtocolDiag_WarmCacheSaveToNvs() noexcept {
  WarmCache_SaveToNvs();
}

void ProtocolDiag_WarmCacheCheckNvsDebounce() noexcept {
  // NVS write (WarmCache_CheckNvsDebounce) is intentionally NOT called here.
  // Network task (Core 0) must never block on Preferences NVS write.
  // Actual NVS flush is performed inside Wallpad_BuildNextPollPacket()
  // which runs on Task_Ch1 (Core 1) every polling cycle.
  Wallpad_DoorphoneCheckBellTimeout();
}


void ProtocolDiag_GetWarmCacheStatus(uint8_t &out_source, uint8_t &out_restored_count) noexcept {
  auto st = WarmCache_GetStatus();
  out_source = st.source;
  out_restored_count = st.restored_count;
}

void ProtocolDiag_PollingResetHits() noexcept {
  Polling_GetRegistry().resetHits();
}

void ProtocolDiag_PollingClear() noexcept {
  Polling_GetRegistry().clear();
}

void ProtocolDiag_PollingSweepExpired(uint32_t threshold_ms) noexcept {
  Polling_GetRegistry().sweepExpired(threshold_ms);
}

size_t ProtocolDiag_GetPollingTargetCount() noexcept {
  return Polling_GetRegistry().totalCount();
}

bool ProtocolDiag_GetPollingEntry(size_t index, PollingEntrySnapshot &snap) noexcept {
  PollingTargetEntry tgt;
  if (!Polling_GetRegistry().getEntry(index, tgt))
    return false;
  snap.dev_id = tgt.dev_id;
  snap.sub1 = tgt.sub1;
  snap.sub2 = tgt.sub2;
  snap.source_channels = tgt.source_channels;
  snap.hits = tgt.hit_count;
  snap.last_seen_ms = tgt.last_requested_ms;
  snap.is_active = tgt.is_active;
  snap.verified = tgt.is_verified;
  snap.pkt_len = tgt.raw_query_len;
  if (tgt.raw_query_len > 0) {
    size_t copy_len = std::min(static_cast<size_t>(tgt.raw_query_len), snap.pkt_data.size());
    std::copy(tgt.raw_query_data.begin(), tgt.raw_query_data.begin() + copy_len, snap.pkt_data.begin());
  }
  return true;
}

void ProtocolDiag_PollingRegisterOrTouch(uint8_t ch, uint8_t dev_id, uint8_t sub1,
                                         uint8_t sub2, const uint8_t *pkt_data,
                                         size_t pkt_len) noexcept {
  Polling_GetRegistry().registerOrTouch(ch, dev_id, sub1, sub2, pkt_data, pkt_len);
}

void ProtocolDiag_AutoProbingReset() noexcept {
  AutoProbe_GetEngine().reset();
}

void ProtocolDiag_AutoProbingFeedFrame(const uint8_t *data, size_t len) noexcept {
  if (data && len > 0) {
    AutoProbe_GetEngine().feedFrame(std::span<const uint8_t>(data, len));
  }
}

bool ProtocolDiag_GetAutoProbingDescriptor(AutoProbingDescriptorSnapshot &out) noexcept {
  auto desc = AutoProbe_GetEngine().getDescriptor();
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

  out.stx = desc.stx;
  out.etx = desc.etx;
  snprintf(out.checksum_algo_name, sizeof(out.checksum_algo_name), "%s",
           AutoProbingEngine::getAlgoName(desc.checksum_algo));
  out.opcode_offset = desc.opcode_offset;
  out.query_opcode = desc.query_opcode;
  out.control_opcode = desc.control_opcode;
  out.ack_opcode = desc.ack_opcode;
  out.control_seen = desc.control_seen;
  out.dev_id_offset = desc.dev_id_offset;
  out.sub1_offset = desc.sub1_offset;
  out.sub2_offset = desc.sub2_offset;
  out.gw_addr_offset = desc.gw_addr_offset;
  out.gw_addr = desc.gw_addr;
  out.is_swapped_addr = desc.is_swapped_addr;
  out.has_seq_counter = desc.has_seq_counter;
  out.seq_offset = desc.seq_offset;
  out.is_locked = desc.is_locked;
  out.opcodes_locked = desc.opcodes_locked;
  out.offsets_locked = desc.offsets_locked;
  out.payload_offset = desc.payload_offset;
  out.is_manual = is_manual_prof;
  out.matched_packets = desc.matched_packets;
  out.tested_packets = desc.tested_packets;
  return true;
}

void ProtocolDiag_GetPollingStats(size_t &active, size_t &verified, size_t &total) noexcept {
  active = Polling_GetRegistry().activeCount();
  verified = Polling_GetRegistry().verifiedCount();
  total = Polling_GetRegistry().totalCount();
}

void ProtocolDiag_GetActiveAddresses(uint8_t *dev_ids, size_t &dev_cnt,
                                     uint8_t *sub1_ids, size_t &sub1_cnt,
                                     uint8_t *sub2_ids, size_t &sub2_cnt,
                                     size_t max_items) noexcept {
  dev_cnt = 0;
  sub1_cnt = 0;
  sub2_cnt = 0;
  constexpr uint8_t CH23_MASK = (1 << 2) | (1 << 3);
  size_t total_tgts = Polling_GetRegistry().totalCount();
  for (size_t i = 0; i < total_tgts; ++i) {
    PollingTargetEntry entry;
    if (Polling_GetRegistry().getEntry(i, entry)) {
      if ((entry.source_channels & CH23_MASK) == 0)
        continue;
      if (dev_cnt < max_items && std::find(dev_ids, dev_ids + dev_cnt, entry.dev_id) == dev_ids + dev_cnt) {
        dev_ids[dev_cnt++] = entry.dev_id;
      }
      if (sub1_cnt < max_items && std::find(sub1_ids, sub1_ids + sub1_cnt, entry.sub1) == sub1_ids + sub1_cnt) {
        sub1_ids[sub1_cnt++] = entry.sub1;
      }
      if (sub2_cnt < max_items && std::find(sub2_ids, sub2_ids + sub2_cnt, entry.sub2) == sub2_ids + sub2_cnt) {
        sub2_ids[sub2_cnt++] = entry.sub2;
      }
    }
  }
  std::sort(dev_ids, dev_ids + dev_cnt);
  std::sort(sub1_ids, sub1_ids + sub1_cnt);
  std::sort(sub2_ids, sub2_ids + sub2_cnt);
}

uint32_t ProtocolDiag_GetStalePollCount() noexcept {
  return Wallpad_GetStalePollCount();
}

void ProtocolDiag_GetFramingNamespace(uint8_t profile_idx, char *out_buf,
                                      size_t buf_len) noexcept {
  if (out_buf && buf_len > 0) {
    snprintf(out_buf, buf_len, "dp_frame_p%u",
             static_cast<unsigned int>(profile_idx & 0x03));
  }
}

bool ProtocolDiag_ExtractDeviceKey(std::span<const uint8_t> frame,
                                   uint8_t &out_dev_id, uint8_t &out_sub1,
                                   uint8_t &out_sub2) noexcept {
  if (frame.size() < 5)
    return false;
  return Universal_GetEngine().extractDeviceKey(frame, out_dev_id, out_sub1, out_sub2);
}

bool ProtocolDiag_ExtractDeviceKey(const uint8_t *data, size_t len,
                                   uint8_t &out_dev_id, uint8_t &out_sub1,
                                   uint8_t &out_sub2) noexcept {
  if (!data)
    return false;
  return ProtocolDiag_ExtractDeviceKey(std::span<const uint8_t>(data, len),
                                       out_dev_id, out_sub1, out_sub2);
}

void ProtocolDiag_GetActiveVendorName(char *out_buf, size_t max_len) noexcept {
  if (!out_buf || max_len == 0)
    return;
  Universal_GetEngine().getVendorName(out_buf, max_len);
}

void ProtocolDiag_GetProfileSummary(char *out_buf, size_t max_len) noexcept {
  if (!out_buf || max_len == 0)
    return;

  auto &active = Universal_GetEngine();
  auto desc = AutoProbe_GetEngine().getDescriptor();
  char vendor_name_buf[64] = "Unknown";
  active.getVendorName(vendor_name_buf, sizeof(vendor_name_buf));
  const char *catalog_vendor = vendor_name_buf;

  if (Config_GetWallpadProfile() == 0) {
    snprintf(out_buf, max_len,
             desc.is_locked ? "Auto Detect (%s)" : "Auto Detect (Learning...)",
             catalog_vendor);
  } else {
    VendorProfileDescriptor cur_p;
    const char *p_name = ProfileRepository::getActiveProfile(cur_p)
                             ? (cur_p.name[0] ? cur_p.name : cur_p.key)
                             : nullptr;
    if (p_name && strcmp(catalog_vendor, "Unknown") != 0 &&
        strcmp(p_name, catalog_vendor) != 0) {
      snprintf(out_buf, max_len, "%s (%s)", p_name, catalog_vendor);
    } else if (p_name) {
      snprintf(out_buf, max_len, "%s", p_name);
    } else {
      snprintf(out_buf, max_len, "%s", catalog_vendor);
    }
  }
}

void ProtocolDiag_GetActiveProfileKey(char *out_buf, size_t max_len) noexcept {
  if (!out_buf || max_len == 0)
    return;
  Universal_GetEngine().getActiveProfileKey(out_buf, max_len);
}

void ProtocolDiag_GetSnapshot(ProtocolDiagnosticSnapshot &out) noexcept {
  ProtocolDiag_GetProfileSummary(out.profile_summary, sizeof(out.profile_summary));
  Universal_GetEngine().getVendorName(out.vendor_name, sizeof(out.vendor_name));
  Universal_GetEngine().getActiveProfileKey(out.profile_key, sizeof(out.profile_key));
  out.stale_poll_count = Wallpad_GetStalePollCount();
  out.group_count = Control_GetRegistry().getGroupCount();
  out.polling_active = Polling_GetRegistry().activeCount();
  out.polling_verified = Polling_GetRegistry().verifiedCount();
  out.polling_total = Polling_GetRegistry().totalCount();
  auto st = WarmCache_GetStatus();
  out.wc_source = st.source;
  out.wc_restored_count = st.restored_count;
}

bool ProtocolDiag_GetCatalogMatch(char *vendor_buf, size_t v_len, size_t &device_count) noexcept {
  const auto *matched_p = ProfileMatcher::getActiveProfile();
  if (matched_p) {
    if (vendor_buf && v_len > 0) {
      snprintf(vendor_buf, v_len, "%s", matched_p->vendor_name);
    }
    device_count = matched_p->device_count;
    return true;
  }
  device_count = 0;
  return false;
}

size_t ProtocolDiag_GetProfileCount() noexcept {
  return ProfileRepository::getProfileCount();
}

bool ProtocolDiag_GetProfileInfo(size_t idx, ProfileInfoSnapshot &out) noexcept {
  VendorProfileDescriptor p_desc;
  if (!ProfileRepository::getProfile(idx, p_desc)) {
    return false;
  }
  snprintf(out.key, sizeof(out.key), "%s", p_desc.key);
  snprintf(out.name, sizeof(out.name), "%s", p_desc.name);
  out.stx = p_desc.stx;
  out.etx = p_desc.etx;
  out.query_op = p_desc.query_op;
  out.ctrl_op = p_desc.ctrl_op;
  out.ack_op = p_desc.ack_op;
  out.is_active = (Config_GetWallpadProfile() == idx);
  return true;
}

bool ProtocolDiag_SetActiveProfile(size_t slot) noexcept {
  if (slot < ProfileRepository::MAX_PROFILES) {
    ProfileRepository::setActiveProfileIndex(slot);
    return true;
  }
  return false;
}

bool ProtocolDiag_SetActiveProfileByKey(const char *key) noexcept {
  if (!key)
    return false;
  return ProfileRepository::setActiveProfileByKey(key);
}

bool ProtocolDiag_SaveCurrentProfileAs(const char *name, size_t &saved_slot) noexcept {
  return ProfileRepository::saveCurrentAutoAs(name, saved_slot);
}

bool ProtocolDiag_DeleteProfile(size_t idx) noexcept {
  if (idx >= 1 && idx < ProfileRepository::getProfileCount()) {
    ProfileRepository::deleteProfile(idx);
    return true;
  }
  return false;
}

size_t ProtocolDiag_GetMaxProfiles() noexcept {
  return ProfileRepository::MAX_PROFILES;
}

void ProtocolDiag_WallpadReset() noexcept {
  char wp_ns[16] = {0};
  char dp_ns[16] = {0};
  ProtocolDiag_GetFramingNamespace(0, wp_ns, sizeof(wp_ns));
  ProtocolDiag_GetFramingNamespace(0, dp_ns, sizeof(dp_ns));
  AutoProbe_GetEngine().reset();
  Wallpad_DoorphoneClearNvs(dp_ns);
}

void ProtocolDiag_RequestRelearn() noexcept {
  Wallpad_RequestRelearn();
}

bool ProtocolDiag_CommitAutoProfileNvsIfPending() noexcept {
  return ProfileRepository::commitAutoProfileNvsIfPending();
}

void ProtocolDiag_DoorphoneClearNvs(const char *nvs_ns) noexcept {
  Wallpad_DoorphoneClearNvs(nvs_ns);
}

void ProtocolDiag_DoorphoneGetFraming(FramingStatus &out_status, uint8_t &out_stx,
                                      uint8_t &out_etx, uint8_t &out_len) noexcept {
  Wallpad_DoorphoneGetFraming(out_status, out_stx, out_etx, out_len);
}

bool ProtocolDiag_GetDoorphoneMatch(DoorphoneMatchSnapshot &out) noexcept {
  FramingStatus dp_status = FramingStatus::WAITING;
  uint8_t cur_dp_stx = 0;
  uint8_t cur_dp_etx = 0;
  uint8_t cur_dp_len = 0;
  Wallpad_DoorphoneGetFraming(dp_status, cur_dp_stx, cur_dp_etx, cur_dp_len);

  const DoorphoneSpec *dp_prof =
      ProfileMatcher::matchDoorphone(cur_dp_stx, cur_dp_etx, cur_dp_len);
  if (dp_prof) {
    out.matched = true;
    snprintf(out.desc, sizeof(out.desc), "%s", dp_prof->desc);
    out.bell_front = dp_prof->bell_front;
    out.call_front = dp_prof->call_front;
    out.open_front = dp_prof->open_front;
    out.end_front = dp_prof->end_front;
    out.bell_lobby = dp_prof->bell_lobby;
    out.call_lobby = dp_prof->call_lobby;
    out.open_lobby = dp_prof->open_lobby;
    out.end_lobby = dp_prof->end_lobby;
    return true;
  }
  out.matched = false;
  return false;
}

bool ProtocolDiag_BuildControlPacket(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                                     ControlActionType act, int val,
                                     StaticPacket &out_req) noexcept {
  return Control_GetRegistry().buildControlPacket(dev_id, sub1, sub2, act, val, out_req);
}

size_t ProtocolDiag_GetGroupCount() noexcept {
  return Control_GetRegistry().getGroupCount();
}

const char *ProtocolDiag_GetGroupName(uint8_t dev_id) noexcept {
  GroupControlTemplate grp{};
  if (Control_GetRegistry().findGroup(dev_id, grp)) {
    static char s_buf[16];
    snprintf(s_buf, sizeof(s_buf), "%s", grp.group_name);
    return s_buf;
  }
  return "Unknown";
}

static void CopyTemplateToSnapshot(const GroupControlTemplate &grp, BlueprintSnapshot &out) {
  out.dev_id = grp.dev_id;
  snprintf(out.group_name, sizeof(out.group_name), "%s", grp.group_name);
  out.dev_class = grp.coverage.dev_class;
  out.frame_len = grp.frame_len;
  out.sub1_offset = grp.sub1_offset;
  out.sub2_offset = grp.sub2_offset;

  out.power_discovered = grp.power_slot.discovered;
  out.power_offset = grp.power_slot.action_offset;
  out.power_on = grp.power_slot.on_val;
  out.power_off = grp.power_slot.off_val;

  out.temp_discovered = grp.temp_slot.discovered;
  out.temp_offset = grp.temp_slot.action_offset;
  out.temp_min = grp.temp_slot.min_val;
  out.temp_max = grp.temp_slot.max_val;

  out.speed_discovered = grp.speed_slot.discovered;
  out.speed_offset = grp.speed_slot.action_offset;
  out.speed_levels = grp.speed_slot.level_count;
  for (size_t l = 0; l < 4 && l < grp.speed_slot.level_count; ++l) {
    out.speed_tokens[l] = grp.speed_slot.level_tokens[l];
  }
  out.speed_min = grp.speed_slot.min_val;
  out.speed_max = grp.speed_slot.max_val;

  out.close_discovered = grp.close_slot.discovered;
  out.close_offset = grp.close_slot.action_offset;
  out.close_val = grp.close_slot.off_val;

  out.mode_discovered = grp.mode_slot.discovered;
  out.mode_offset = grp.mode_slot.action_offset;

  out.ack_discovered = grp.ack_slots.discovered;
  out.ack_pwr_offset = grp.ack_slots.power_offset;
  out.ack_target_temp_offset = grp.ack_slots.target_temp_offset;
  out.ack_curr_temp_offset = grp.ack_slots.current_temp_offset;
  out.ack_fan_speed_offset = grp.ack_slots.fan_speed_offset;
  out.ack_valve_offset = grp.ack_slots.valve_state_offset;
  out.ack_wattage_offset = 0xFF;

  out.qry_len = grp.query_slots.expected_len;
  out.qry_pwr_offset = grp.query_slots.power_offset;
  out.qry_target_temp_offset = grp.query_slots.target_temp_offset;
  out.qry_curr_temp_offset = grp.query_slots.current_temp_offset;
  out.qry_fan_speed_offset = grp.query_slots.fan_speed_offset;
  out.qry_valve_offset = grp.query_slots.valve_state_offset;
  out.qry_wattage_offset = grp.query_slots.power_w_offset;
}

bool ProtocolDiag_GetBlueprintAt(size_t index, BlueprintSnapshot &out) noexcept {
  GroupControlTemplate grp{};
  if (!Control_GetRegistry().getGroupByIndex(index, grp))
    return false;
  CopyTemplateToSnapshot(grp, out);
  return true;
}

bool ProtocolDiag_GetBlueprint(uint8_t dev_id, BlueprintSnapshot &out) noexcept {
  GroupControlTemplate grp{};
  if (!Control_GetRegistry().findGroup(dev_id, grp))
    return false;
  CopyTemplateToSnapshot(grp, out);
  return true;
}

bool ProtocolDiag_SetGroupName(uint8_t dev_id, const char *name) noexcept {
  return Control_GetRegistry().setGroupName(dev_id, name);
}

bool ProtocolDiag_SetGroupClass(uint8_t dev_id, DeviceClass cls, const char *name) noexcept {
  return Control_GetRegistry().setGroupClass(dev_id, cls, name);
}

void ProtocolDiag_ResetGroup(uint8_t dev_id, bool all) noexcept {
  Control_GetRegistry().resetGroup(dev_id, all);
}

uint16_t ProtocolDiag_CalculateChecksum(const uint8_t *data, size_t len) noexcept {
  return Universal_GetEngine().calculateChecksum(data, len);
}

// ── Bridge Transport Slot Control API (L4 → L3 Gateway) ──────────────────────
bool ProtocolDiag_SetBridgeSlotEnabled(uint8_t slot_idx, bool enabled) noexcept {
  return Bridge_SetSlotEnabled(slot_idx, enabled);
}

bool ProtocolDiag_SetBridgeSlotConfig(uint8_t slot_idx, bool enabled, const char *ip,
                                      uint16_t port, const char *name) noexcept {
  return Bridge_SetSlot(slot_idx, enabled, ip, port, name);
}

bool ProtocolDiag_SetBridgeFramingLock(uint8_t slot_idx, uint8_t stx, uint8_t etx, uint8_t len) noexcept {
  return Bridge_SetFramingLock(slot_idx, stx, etx, len);
}

bool ProtocolDiag_ResetBridgeFraming(uint8_t slot_idx) noexcept {
  return Bridge_ResetFramingTracker(slot_idx);
}

void ProtocolDiag_ResetBridgeStats() noexcept {
  Bridge_ResetStats();
  Fcu_ResetStats();
  Wallpad_ResetStalePollCount();
}

// ── L2 RS485 Dispatcher SPI Binding & Lifecycle ─────────────────────────────
void Protocol_BindDispatcher(RS485_PacketDispatcher &dispatcher) noexcept {
  dispatcher.onBuildPoll = Wallpad_BuildNextPollPacket;
  dispatcher.onBusPacket = Wallpad_HandleBusPacket;
  dispatcher.onTimeout = Device_HandlePollingTimeout;
  dispatcher.onDispatchControl = Router_DispatchControl;
  dispatcher.onGetPollIntervalMs = Wallpad_GetPollIntervalMs;
  dispatcher.onGetStx = []() noexcept { return Universal_GetEngine().getStx(); };
  dispatcher.onIsAutoUnlocked = []() noexcept {
    auto &e = Universal_GetEngine();
    return e.isAutoMode() && !e.isLocked();
  };
  dispatcher.onFeedAutoFrame = [](std::span<const uint8_t> f) noexcept {
    AutoProbe_GetEngine().feedFrame(f);
  };
  dispatcher.onExtractLength = [](const uint8_t *s, size_t len, size_t idx) noexcept {
    return Universal_GetEngine().extractPacketLength(s, len, idx);
  };
  dispatcher.onValidatePacket = [](std::span<const uint8_t> f) noexcept {
    return Universal_GetEngine().validatePacket(f);
  };
  dispatcher.onHandleSubBusQuery = Wallpad_HandleSubBusQuery;
  dispatcher.onFeedControlFrame = [](std::span<const uint8_t> f) noexcept {
    AutoProbe_GetEngine().feedControlFrame(f);
  };
  dispatcher.onDoorphonePacket = Wallpad_HandleDoorphonePacket;
  dispatcher.onDoorphoneReset = Wallpad_ResetDoorphoneBellState;
  dispatcher.onMatchDoorphoneLock = Wallpad_MatchDoorphoneLock;
  dispatcher.onDoorphoneGetLockedFraming = Wallpad_DoorphoneGetLockedFraming;
  dispatcher.onDoorphoneFrameDetected = Wallpad_DoorphoneFrameDetected;
  dispatcher.onIsQueryPacket = [](std::span<const uint8_t> f) noexcept {
    return Universal_GetEngine().isQueryPacket(f);
  };
}

void Protocol_DoorphoneInit() noexcept {
  Wallpad_DoorphoneInit();
}

void Protocol_DoorphoneRestoreNvs(const char *dp_ns) noexcept {
  Wallpad_DoorphoneRestoreNvs(dp_ns);
}

void Protocol_WarmCacheRestoreOnBoot() noexcept {
  WarmCache_RestoreOnBoot();
}

static void Protocol_OnBridgePacketReceived(uint8_t slot_idx, const StaticPacket &pkt) noexcept {
  uint8_t dev_id = 0, sub1 = 0, sub2 = 0;
  uint8_t stx = Wallpad_GetStx();
  uint8_t etx = PKT_ETX;
  if (ProtocolDiag_ExtractDeviceKey(pkt.data.data(), pkt.length, dev_id, sub1, sub2) &&
      dev_id != 0 && dev_id != stx && dev_id != etx && dev_id != 0xFF) {
    Router_RecordRoute(5, static_cast<int8_t>(slot_idx), dev_id, sub1, sub2);
    if (dev_id == 0x34) {
      if (pkt.length == 11 && pkt.data[4] == 0x04) {
        static std::atomic<uint8_t> s_last_elev_pwr{0xFF};
        uint8_t new_pwr = (pkt.data[8] == 0x06) ? 1 : 0;
        uint8_t prev = s_last_elev_pwr.exchange(new_pwr, std::memory_order_acq_rel);
        if (prev != new_pwr) {
          ESP_LOGI("ProtocolDiag", "[CH5] Elevator State Changed -> Power: %u", new_pwr);
          Device_NotifyElevatorEvent(sub1, sub2, 15, 0, new_pwr, false);
        }
      } else if (pkt.length == 13 && pkt.data[4] == 0x01 && pkt.data[8] == 0x01) {
        uint8_t floor = pkt.data[9];
        uint8_t ho = pkt.data[10];
        ESP_LOGI("ProtocolDiag", "[CH5] Elevator Arrived -> Floor: %u, Car: %u", floor, ho);
        Device_NotifyElevatorEvent(sub1, sub2, floor, ho, 0, true);
      }
    }
    if (dev_id != 0x2A) {
      bool is_arrival = (dev_id == 0x34 && pkt.length == 13 && pkt.data[4] == 0x01 && pkt.data[8] == 0x01);
      bool is_query = Wallpad_IsQueryPacket(std::span<const uint8_t>(pkt.data.data(), pkt.length)) && !is_arrival;
      bool is_ack = (pkt.length >= 5 && pkt.data[4] == 0x04);
      if (is_query) {
        ProtocolDiag_PollingRegisterOrTouch(5, dev_id, sub1, sub2, pkt.data.data(), pkt.length);
      }
      if (is_ack) {
        Device_ProcessBusPacket(const_cast<StaticPacket &>(pkt));
      }
    }
  }
}

void Protocol_BindBridgeDispatcher(Bridge_PacketDispatcher &dispatcher) noexcept {
  dispatcher.onGetStx = []() noexcept { return Universal_GetEngine().getStx(); };
  dispatcher.onExtractLength = [](const uint8_t *s, size_t len, size_t idx) noexcept {
    return Universal_GetEngine().extractPacketLength(s, len, idx);
  };
  dispatcher.onValidatePacket = [](const uint8_t *data, size_t len) noexcept {
    if (!data || len == 0) return false;
    return Universal_GetEngine().validatePacket(std::span<const uint8_t>(data, len));
  };
  dispatcher.onPacketReceived = Protocol_OnBridgePacketReceived;
}

bool ProtocolDiag_RegisterTcpParticipant(const ProtocolTcpParticipant &p) noexcept {
  Transport::ReactorParticipant rp;
  rp.name = p.name;
  rp.populateFds = p.populateFds;
  rp.processEvents = p.processEvents;
  rp.tick = p.tick;
  return Transport::TcpReactor::registerParticipant(rp);
}

uint8_t Protocol_LookupDeviceChannel(uint8_t dev_id, uint8_t sub1, uint8_t sub2) noexcept {
  RouteEndpoint ep{1, -1, 0};
  if (Router_LookupRoute(dev_id, sub1, sub2, ep)) {
    return ep.channel_id;
  }
  return 1;
}

void Protocol_ClearRoutes() noexcept {
  Router_ClearRoutes();
}

size_t Protocol_GetRoutes(DeviceRouteSnapshot *out_buf, size_t max_count) noexcept {
  if (!out_buf || max_count == 0) return 0;
  static DeviceRouteEntry tmp_entries[64];
  const size_t cap = std::min(max_count, sizeof(tmp_entries) / sizeof(tmp_entries[0]));
  const size_t count = Router_GetRoutes(std::span<DeviceRouteEntry>(tmp_entries, cap));
  for (size_t i = 0; i < count; ++i) {
    out_buf[i].dev_id = tmp_entries[i].dev_id;
    out_buf[i].sub1 = tmp_entries[i].sub1;
    out_buf[i].sub2 = tmp_entries[i].sub2;
    out_buf[i].channel_id = tmp_entries[i].endpoint.channel_id;
    out_buf[i].slot_idx = tmp_entries[i].endpoint.slot_idx;
    out_buf[i].last_seen_ms = tmp_entries[i].endpoint.last_seen_ms;
  }
  return count;
}

bool Protocol_DispatchControl(StaticPacket &req, StaticPacket &virtual_ack_out) noexcept {
  return Router_DispatchControl(req, virtual_ack_out);
}

