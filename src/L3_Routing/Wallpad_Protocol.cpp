// ============================================================================
// Wallpad_Protocol.cpp — L3 Protocol & Routing Engine
// Wallpad Business Logic Orchestration & Domain Engine Implementation
// Canonical 4+1 Layer: L3 Routing/Protocol layer
// ============================================================================

#include "L3_Routing/Wallpad_Protocol.h"
#include "L3_Routing/Device_Registry.h"
#include "L3_Routing/Packet_Router.h"
#include "L3_Routing/ControlTemplate.h"
#include "L2_Channels/RS485_CH.h"
#include "L1_Drivers/Diagnostics_Driver.h"
#include "L1_Drivers/RTOS_Driver.h"
#include "L0_Base/System_Buffer.h"
#include "L0_Base/System_Config.h"
#include "L0_Base/System_Platform.h"

#include <algorithm>
#include <cstring>
#include <esp_timer.h>

namespace {

int Wallpad_ScoreCandidate(const PollingTargetRegistry::PollingCandidate &tgt,
                           const DeviceStateEntry *cached_dev) noexcept {
  if (!cached_dev) {
    return 1;
  }
  RouteEndpoint ep;
  if (Router_LookupRoute(tgt.dev_id, tgt.sub1, tgt.sub2, ep) &&
      ep.channel_id == 5) {
    return 999;
  }
  if (cached_dev->last_updated_ms == 0) {
    return 1;
  }
  if (cached_dev->is_online) {
    return 2;
  }
  if (TimeUtils::isElapsed(cached_dev->last_stale_poll_ms,
                           Config::Timing::CH1_STALE_POLL_INTERVAL_MS)) {
    return 3;
  }
  return 999;
}

static size_t s_current_dev_idx = 0;
static uint32_t s_stable_start_ms = 0;
static size_t s_last_active_tgts = 0;
static bool s_convergence_done = false;

} // namespace

bool Wallpad_BuildNextPollPacket(StaticPacket &out_pkt, uint8_t &poll_dev_id,
                                 uint8_t &poll_sub1, uint8_t &poll_sub2) noexcept {
  g_polling_targets.sweepExpired(Config::Timing::STALE_DEVICE_THRESHOLD_MS);

  PollingTargetRegistry::PollingCandidate candidates[PollingTargetRegistry::MAX_TARGETS];
  size_t active_cnt = g_polling_targets.getActiveCandidates(
      candidates, PollingTargetRegistry::MAX_TARGETS);

  poll_dev_id = 0;
  poll_sub1 = 0;
  poll_sub2 = 0;
  const uint8_t *poll_raw_ptr = nullptr;
  uint8_t poll_raw_len = 0;
  bool target_selected = false;
  uint32_t now = millis();

  if (active_cnt > 0) {
    size_t chosen_idx = active_cnt;
    int chosen_score = 999;

    for (size_t i = 0; i < active_cnt; i++) {
      size_t idx = (s_current_dev_idx + i) % active_cnt;
      const auto &tgt = candidates[idx];
      const auto *cached_dev = Device_Find(tgt.dev_id, tgt.sub1, tgt.sub2);
      int score = Wallpad_ScoreCandidate(tgt, cached_dev);

      if (score <= 3) {
        chosen_idx = idx;
        chosen_score = score;
        break;
      }
    }

    if (chosen_idx < active_cnt) {
      const auto &tgt = candidates[chosen_idx];
      poll_dev_id = tgt.dev_id;
      poll_sub1 = tgt.sub1;
      poll_sub2 = tgt.sub2;
      poll_raw_len = tgt.raw_query_len;
      if (poll_raw_len > 0) {
        g_polling_targets.getQueryData(tgt.entry_idx, poll_raw_ptr, poll_raw_len);
      }
      if (chosen_score == 3) {
        Device_SetLastStalePollMs(tgt.dev_id, tgt.sub1, tgt.sub2, now);
        g_ch1_state_metrics.stale_poll_cnt.fetch_add(1, std::memory_order_relaxed);
      }
      s_current_dev_idx = (chosen_idx + 1) % active_cnt;
      target_selected = true;
    }
  }

  if (!target_selected) {
    size_t dev_cnt = Device_GetCount();
    if (dev_cnt > 0) {
      size_t idx = s_current_dev_idx % dev_cnt;
      auto *dev = Device_GetAt(idx);
      s_current_dev_idx = (idx + 1) % dev_cnt;
      if (dev &&
          (dev->is_online || dev->last_updated_ms == 0 ||
           TimeUtils::isElapsed(dev->last_stale_poll_ms,
                                Config::Timing::CH1_STALE_POLL_INTERVAL_MS))) {
        RouteEndpoint ep;
        if (!Router_LookupRoute(dev->dev_id, dev->sub1, dev->sub2, ep) ||
            ep.channel_id != 5) {
          poll_dev_id = dev->dev_id;
          poll_sub1 = dev->sub1;
          poll_sub2 = dev->sub2;
          if (!dev->is_online)
            Device_SetLastStalePollMsByIndex(idx, now);
          target_selected = true;
        }
      }
    }
  }

  if (!target_selected) {
    return false;
  }

  out_pkt.channel_id = 1;
  if (poll_raw_len > 0 && poll_raw_ptr) {
    out_pkt.length = poll_raw_len;
    memcpy(out_pkt.data.data(), poll_raw_ptr, poll_raw_len);
  } else {
    auto *parser = WallpadParserFactory::getActiveParser();
    if (parser) {
      parser->buildQueryPacket(poll_dev_id, poll_sub1, poll_sub2, out_pkt);
    }
  }
  return true;
}

void Wallpad_HandleBusPacket(uint8_t channel_id, const StaticPacket &ack_pkt,
                             const StaticPacket *matching_query) noexcept {
  StaticPacket ack = ack_pkt;
  ack.channel_id = channel_id;

  if (matching_query && matching_query->length > 0) {
    g_polling_targets.updateResponse(matching_query->data.data(), matching_query->length,
                                     ack.data.data(), ack.length);
    g_auto_probing_engine.feedOpcodePair(
        span<const uint8_t>(matching_query->data.data(), matching_query->length),
        span<const uint8_t>(ack.data.data(), ack.length));
  }

  Device_ProcessBusPacket(ack);

  auto *parser = WallpadParserFactory::getActiveParser();
  if (parser) {
    uint8_t dev_id = 0, sub1 = 0, sub2 = 0;
    span<const uint8_t> ack_span(ack.data.data(), ack.length);
    if (parser->extractDeviceKey(ack_span, dev_id, sub1, sub2)) {
      if (channel_id == 1) {
        g_polling_targets.markVerified(dev_id, sub1, sub2);
      }
      Router_RecordRoute(channel_id, -1, dev_id, sub1, sub2);
    }
  }
}

void Wallpad_HandlePollTimeout(uint8_t poll_dev_id, uint8_t poll_sub1,
                               uint8_t poll_sub2) noexcept {
  Device_HandlePollingTimeout(poll_dev_id, poll_sub1, poll_sub2);
}

ControlAction Wallpad_EvaluateControl(StaticPacket &req, StaticPacket &virtual_ack_out,
                                      uint8_t &out_ch5_slot, bool &out_unidir) noexcept {
  out_ch5_slot = 0;
  out_unidir = false;

  if (UNLIKELY(req.length < 5))
    return ControlAction::DROP;
  auto *parser = WallpadParserFactory::getActiveParser();
  if (!parser)
    return ControlAction::DROP;
  span<const uint8_t> frame(req.data.data(), req.length);

  uint8_t dev_id = 0, sub1 = 0, sub2 = 0;
  const bool has_key = parser->extractDeviceKey(frame, dev_id, sub1, sub2);

  if (parser->isQueryPacket(frame)) {
    virtual_ack_out.channel_id = req.channel_id;
    if (has_key && Device_CopyVirtualAck(dev_id, sub1, sub2, virtual_ack_out)) {
      return ControlAction::VIRTUAL_ACK_IMMEDIATE;
    }
    return ControlAction::DROP;
  }

  GroupControlTemplate grp{};
  const bool has_grp =
      (has_key && dev_id != 0) && g_control_registry.findGroup(dev_id, grp);

  bool is_ctl = parser->isControlPacket(frame);
  if (!is_ctl && has_grp && grp.frame_len > 4 &&
      frame.size() >= grp.frame_len) {
    VendorProfileDescriptor desc;
    ProfileRepository::getActiveProfile(desc);
    const uint8_t op_off =
        (desc.opcode_offset < frame.size()) ? desc.opcode_offset : 4;
    is_ctl = (frame[op_off] == grp.raw_template[op_off]);
  }
  if (!is_ctl)
    return ControlAction::DROP;

  if (has_grp) {
    // 가스: 원격 '열림' 차단 (닫힘 값만 허용)
    if (grp.coverage.dev_class == DeviceClass::GAS &&
        grp.close_slot.discovered &&
        grp.close_slot.action_offset < req.length &&
        req.data[grp.close_slot.action_offset] != grp.close_slot.off_val) {
      System_TracePacket(req.channel_id, false, TraceType::DRP, req);
      return ControlAction::DROP;
    }

    // 난방: 온도 설정 범위 검증
    const auto &ts = grp.temp_slot;
    if (grp.coverage.dev_class == DeviceClass::THERMOSTAT && ts.discovered &&
        ts.action_offset < req.length && ts.category_offset != 0xFF &&
        ts.category_offset < req.length &&
        req.data[ts.category_offset] == ts.category_val) {
      const uint8_t t = req.data[ts.action_offset];
      if (t < 5 || t > 35) {
        System_TracePacket(req.channel_id, false, TraceType::DRP, req);
        return ControlAction::DROP;
      }
    }
  }

  RouteEndpoint ep{1, -1, 0};
  const bool route_known =
      has_key && Router_LookupRoute(dev_id, sub1, sub2, ep);

  if (route_known && ep.channel_id == 5 && ep.slot_idx >= 0 &&
      ep.slot_idx < Config::TCP::MAX_EW11_SLOTS) {
    out_ch5_slot = static_cast<uint8_t>(ep.slot_idx);
    out_unidir = (has_grp && grp.isUnidirectional()) || dev_id == 0x34;
    Router_ForwardToCh5(out_ch5_slot, req, out_unidir);
    return ControlAction::FORWARD_CH5;
  }

  return ControlAction::TRANSMIT_LOCAL;
}

uint32_t Wallpad_GetPollIntervalMs() noexcept {
  size_t active_tgts = g_polling_targets.activeCount();
  return (s_convergence_done || active_tgts == 0)
             ? g_timing_config.ch1_poll_interval_ms
             : 20;
}

bool Wallpad_CheckConvergence(bool reset) noexcept {
  if (reset) {
    s_convergence_done = false;
    s_stable_start_ms = 0;
    s_last_active_tgts = 0;
    return false;
  }

  if (s_convergence_done) {
    return true;
  }

  size_t active_tgts = g_polling_targets.activeCount();
  size_t online_devs = Device_GetOnlineCount();

  if (active_tgts != s_last_active_tgts) {
    s_last_active_tgts = active_tgts;
    s_stable_start_ms = millis();
  }

  bool is_all_online = (online_devs >= active_tgts);
  auto *parser = WallpadParserFactory::getActiveParser();
  if (parser && parser->isAutoMode() &&
      !g_auto_probing_engine.isOffsetsLocked()) {
    is_all_online = (g_polling_targets.verifiedCount() >= active_tgts);
  }

  if (active_tgts > 0 && is_all_online) {
    if (s_stable_start_ms == 0) {
      s_stable_start_ms = millis();
    } else if (TimeUtils::isElapsed(s_stable_start_ms,
                                    Config::Timing::CACHE_CONVERGENCE_STABLE_MS)) {
      s_convergence_done = true;
      if (g_system_event_group) {
        xEventGroupSetBits(g_system_event_group, SYS_EVT_CACHE_READY);
      }
      if (parser && parser->isAutoMode() &&
          !g_auto_probing_engine.isOffsetsLocked()) {
        g_auto_probing_engine.analyzeCacheMatrix();
      }
      g_pkt_stats.resetAll();
      g_polling_targets.resetHits();
      g_metrics.reset();
      g_ch1_state_metrics.normal_cnt.store(0, std::memory_order_relaxed);
      g_ch1_state_metrics.vip_cnt.store(0, std::memory_order_relaxed);
      System_TraceMessage(
          "[SYSTEM MSG]  ★ 2nd-Tier Cache Converged (Zero Offline). "
          "Runtime metrics synchronized.\r\n");
      g_control_registry.synthesizeFromConvergedCache();
      System_TraceMessage(
          "[CTL] Control template synthesis triggered.\r\n");
      return true;
    }
  } else {
    s_stable_start_ms = 0;
  }
  return false;
}

uint8_t Wallpad_GetStx() noexcept {
  auto *parser = WallpadParserFactory::getActiveParser();
  return parser ? parser->getStx() : PKT_STX;
}

bool Wallpad_IsAutoUnlocked() noexcept {
  auto *parser = WallpadParserFactory::getActiveParser();
  return parser && parser->isAutoMode() && !parser->isLocked();
}

void Wallpad_FeedAutoFrame(span<const uint8_t> frame) noexcept {
  g_auto_probing_engine.feedFrame(frame);
}

int Wallpad_ExtractLength(const uint8_t *stream, size_t stream_len, size_t stx_idx) noexcept {
  auto *parser = WallpadParserFactory::getActiveParser();
  return parser ? parser->extractPacketLength(stream, stream_len, stx_idx) : -1;
}

bool Wallpad_ValidatePacket(span<const uint8_t> frame) noexcept {
  auto *parser = WallpadParserFactory::getActiveParser();
  return parser ? parser->validatePacket(frame) : false;
}

bool Wallpad_HandleSubBusQuery(uint8_t channel_id, const StaticPacket &req,
                               StaticPacket &virtual_ack_out) noexcept {
  auto *parser = WallpadParserFactory::getActiveParser();
  if (!parser) return false;
  span<const uint8_t> frame(req.data.data(), req.length);
  if (!parser->isQueryPacket(frame)) {
    return false;
  }
  uint8_t dev_id = 0, sub1 = 0, sub2 = 0;
  parser->extractDeviceKey(frame, dev_id, sub1, sub2);
  g_polling_targets.registerOrTouch(channel_id, dev_id, sub1, sub2,
                                    req.data.data(), req.length);
  virtual_ack_out.channel_id = channel_id;
  return Device_CopyVirtualAck(dev_id, sub1, sub2, virtual_ack_out);
}

void Wallpad_FeedControlFrame(span<const uint8_t> frame) noexcept {
  g_auto_probing_engine.feedControlFrame(frame);
}

const DoorphoneSpec *Wallpad_MatchDoorphone(uint8_t stx, uint8_t etx, uint8_t len) noexcept {
  return ProfileMatcher::matchDoorphone(stx, etx, len);
}

void Wallpad_ResetDoorphoneBellState() noexcept {
  g_doorphone_state.front_bell.store(false, std::memory_order_release);
  g_doorphone_state.lobby_bell.store(false, std::memory_order_release);
  g_doorphone_state.last_bell_ms.store(0, std::memory_order_release);
  Device_NotifyDoorphoneEvent(false, false);
}

void Wallpad_HandleDoorphonePacket(const StaticPacket &packet) noexcept {
  if (packet.length < 3) return;
  uint8_t opcode = packet.data[1];
  uint32_t now = millis();
  bool state_changed = false;
  uint8_t pkt_stx = packet.data[0];
  uint8_t pkt_etx = packet.data[packet.length - 1];
  const DoorphoneSpec *dp_prof =
      ProfileMatcher::matchDoorphone(pkt_stx, pkt_etx, packet.length);

  enum class DoorphoneEvent : uint8_t {
    NONE = 0,
    BELL_FRONT,
    END_FRONT,
    BELL_LOBBY,
    END_LOBBY
  };

  struct DoorphoneActionTable {
    static constexpr DoorphoneEvent resolve(uint8_t op,
                                            const DoorphoneSpec *spec) noexcept {
      if (spec) {
        if (op == spec->bell_front) return DoorphoneEvent::BELL_FRONT;
        if (op == spec->end_front)  return DoorphoneEvent::END_FRONT;
        if (op == spec->bell_lobby) return DoorphoneEvent::BELL_LOBBY;
        if (op == spec->end_lobby)  return DoorphoneEvent::END_LOBBY;
      }
      switch (op) {
      case 0xB5: return DoorphoneEvent::BELL_FRONT;
      case 0xB6: [[fallthrough]];
      case 0xB8: return DoorphoneEvent::END_FRONT;
      case 0x5A: [[fallthrough]];
      case 0x5F: return DoorphoneEvent::BELL_LOBBY;
      case 0x60: return DoorphoneEvent::END_LOBBY;
      default:   return DoorphoneEvent::NONE;
      }
    }
  };

  const DoorphoneEvent ev = DoorphoneActionTable::resolve(opcode, dp_prof);
  switch (ev) {
  case DoorphoneEvent::BELL_FRONT:
    g_doorphone_state.front_bell.store(true, std::memory_order_release);
    g_doorphone_state.last_bell_ms.store(now, std::memory_order_release);
    state_changed = true;
    break;
  case DoorphoneEvent::END_FRONT:
    g_doorphone_state.front_bell.store(false, std::memory_order_release);
    state_changed = true;
    break;
  case DoorphoneEvent::BELL_LOBBY:
    g_doorphone_state.lobby_bell.store(true, std::memory_order_release);
    g_doorphone_state.last_bell_ms.store(now, std::memory_order_release);
    state_changed = true;
    break;
  case DoorphoneEvent::END_LOBBY:
    g_doorphone_state.lobby_bell.store(false, std::memory_order_release);
    state_changed = true;
    break;
  case DoorphoneEvent::NONE:
  default:
    break;
  }

  if (state_changed) {
    bool f = g_doorphone_state.front_bell.load(std::memory_order_relaxed);
    bool l = g_doorphone_state.lobby_bell.load(std::memory_order_relaxed);
    Device_NotifyDoorphoneEvent(f, l);
  }
}

bool Wallpad_MatchDoorphoneLock(uint8_t stx, uint8_t etx, uint8_t len,
                                uint8_t &out_fixed_len) noexcept {
  const DoorphoneSpec *dp_spec = ProfileMatcher::matchDoorphone(stx, etx, len);
  if (dp_spec && stx == dp_spec->stx && etx == dp_spec->etx) {
    out_fixed_len = dp_spec->len;
    return true;
  }
  return false;
}

bool Wallpad_IsQueryPacket(span<const uint8_t> frame) noexcept {
  auto *parser = WallpadParserFactory::getActiveParser();
  return parser ? parser->isQueryPacket(frame) : false;
}
