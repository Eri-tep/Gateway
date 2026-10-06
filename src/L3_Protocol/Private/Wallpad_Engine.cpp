// ============================================================================
// Wallpad_Protocol.cpp — L3 Protocol & Routing Engine
// Wallpad Business Logic Orchestration & Domain Engine Implementation
// Canonical 4+1 Layer: L3 Routing/Protocol layer
// ============================================================================

#include "L3_Protocol/Private/Wallpad_Engine.h"
#include "L3_Protocol/Public/Device_Registry.h"
#include "L3_Protocol/Public/Packet_Router.h"
#include "L3_Protocol/Private/Control_Registry.h"
#include "L0_Foundation/System_Buffer.h"
#include "L0_Foundation/System_Config.h"
#include "L0_Foundation/System_Platform.h"

#include <Preferences.h>
#include <cstring>
#include <esp_timer.h>

namespace {

int Wallpad_ScoreCandidate(const PollingTargetRegistry::PollingCandidate &tgt,
                           const DeviceStateEntry *cached_dev) noexcept {
  // CH5 (EW11 TCP) 소속 타겟(FCU 모드버스, 엘리베이터 등)은 CH1 물리 버스 폴링에서 원천 배제
  if ((tgt.source_channels != 0 && !(tgt.source_channels & kWallpadChMask)) ||
      (tgt.source_channels & (1 << 5)) ||
      tgt.dev_id == Config::FCU::DEV_ID) {
    return 999;
  }
  RouteEndpoint ep;
  if (Router_LookupRoute(tgt.dev_id, tgt.sub1, tgt.sub2, ep) &&
      ep.channel_id == 5) {
    return 999;
  }
  if (!cached_dev) {
    return 1;
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
static std::atomic<uint32_t> s_stale_poll_cnt{0};

} // namespace

bool Wallpad_BuildNextPollPacket(StaticPacket &out_pkt, uint8_t &poll_dev_id,
                                 uint8_t &poll_sub1, uint8_t &poll_sub2) noexcept {
  if (Wallpad_TakeRelearnRequest()) {
    Wallpad_CheckConvergence(true);
    System_TraceMessage("[AUTO PROBE] Convergence state reset. Re-learning "
                        "bus offsets...\r\n");
  }

  if (!s_convergence_done) {
    Wallpad_CheckConvergence(false);
  }

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
      DeviceStateEntry cached_dev_snap{};
      bool has_cached = Device_FindCopy(tgt.dev_id, tgt.sub1, tgt.sub2, cached_dev_snap);
      int score = Wallpad_ScoreCandidate(tgt, has_cached ? &cached_dev_snap : nullptr);

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
        s_stale_poll_cnt.fetch_add(1, std::memory_order_relaxed);
      }
      s_current_dev_idx = (chosen_idx + 1) % active_cnt;
      target_selected = true;
    }
  }

  if (!target_selected) {
    size_t dev_cnt = Device_GetCount();
    if (dev_cnt > 0) {
      size_t idx = s_current_dev_idx % dev_cnt;
      DeviceStateEntry dev_snap{};
      bool has_dev = Device_GetAtCopy(idx, dev_snap);
      s_current_dev_idx = (idx + 1) % dev_cnt;
      if (has_dev && dev_snap.dev_id != Config::FCU::DEV_ID &&
          (dev_snap.is_online || dev_snap.last_updated_ms == 0 ||
           TimeUtils::isElapsed(dev_snap.last_stale_poll_ms,
                                Config::Timing::CH1_STALE_POLL_INTERVAL_MS))) {
        RouteEndpoint ep;
        if (!Router_LookupRoute(dev_snap.dev_id, dev_snap.sub1, dev_snap.sub2, ep) ||
            ep.channel_id != 5) {
          poll_dev_id = dev_snap.dev_id;
          poll_sub1 = dev_snap.sub1;
          poll_sub2 = dev_snap.sub2;
          if (!dev_snap.is_online)
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
                                      bool &out_unidir) noexcept {
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

  out_unidir = (has_grp && grp.isUnidirectional()) || dev_id == 0x34;
  return ControlAction::TRANSMIT_LOCAL;
}

uint32_t Wallpad_GetPollIntervalMs() noexcept {
  size_t active_tgts = g_polling_targets.activeCount();
  return (s_convergence_done || active_tgts == 0)
             ? TimingConfig_Get().ch1_poll_interval_ms
             : 20;
}

static std::atomic<bool> s_relearn_requested{false};

void Wallpad_RequestRelearn() noexcept {
  s_relearn_requested.store(true, std::memory_order_release);
}

bool Wallpad_TakeRelearnRequest() noexcept {
  return s_relearn_requested.exchange(false, std::memory_order_acq_rel);
}

bool Wallpad_CheckConvergence(bool reset) noexcept {
  if (reset) {
    s_convergence_done = false;
    s_stable_start_ms = 0;
    s_last_active_tgts = 0;
    if (g_system_event_group) {
      xEventGroupClearBits(g_system_event_group, SYS_EVT_CACHE_READY);
    }
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
      g_polling_targets.resetHits();
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

uint32_t Wallpad_GetStalePollCount() noexcept {
  return s_stale_poll_cnt.load(std::memory_order_relaxed);
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

// ── FramingTracker Implementation (L3) ───────────────────────────────────────

void FramingTracker::setFixedLock(uint8_t stx, uint8_t etx, uint8_t len) noexcept {
  candidate_stx.store(stx, std::memory_order_relaxed);
  candidate_etx.store(etx, std::memory_order_relaxed);
  candidate_len.store(len, std::memory_order_relaxed);
  consecutive_matches.store(10, std::memory_order_relaxed);
  consecutive_mismatches.store(0, std::memory_order_relaxed);
  is_custom_fixed.store(true, std::memory_order_relaxed);
  status.store(FramingStatus::LOCKED, std::memory_order_relaxed);
}

void FramingTracker::reset() noexcept {
  is_custom_fixed.store(false, std::memory_order_relaxed);
  candidate_stx.store(0, std::memory_order_relaxed);
  candidate_etx.store(0, std::memory_order_relaxed);
  candidate_len.store(0, std::memory_order_relaxed);
  consecutive_matches.store(0, std::memory_order_relaxed);
  consecutive_mismatches.store(0, std::memory_order_relaxed);
  status.store(FramingStatus::WAITING, std::memory_order_relaxed);
}

void FramingTracker::clearNvs(const char *nvs_ns, const char *tag) noexcept {
  reset();
  Preferences prefs;
  if (prefs.begin(nvs_ns, false)) {
    prefs.clear();
    prefs.end();
    ::Serial.printf("[%s] Cleared framing NVS storage (%s).\r\n", tag, nvs_ns);
  }
}

void FramingTracker::processFrame(uint8_t stx, uint8_t etx, uint8_t len,
                                  const char *nvs_ns,
                                  const char *tag) noexcept {
  if (is_custom_fixed.load(std::memory_order_relaxed)) {
    return;
  }

  FramingStatus cur = status.load(std::memory_order_relaxed);

  if (stx == 0x7F && etx == 0xEE && (len == 0 || len == 5)) {
    setFixedLock(0x7F, 0xEE, 5);
    saveToNvs(nvs_ns, tag);
    return;
  }

  if (cur == FramingStatus::WAITING) {
    candidate_stx.store(stx, std::memory_order_relaxed);
    candidate_etx.store(etx, std::memory_order_relaxed);
    if (len > 0)
      candidate_len.store(len, std::memory_order_relaxed);
    consecutive_matches.store(1, std::memory_order_relaxed);
    consecutive_mismatches.store(0, std::memory_order_relaxed);
    status.store(FramingStatus::LEARNING, std::memory_order_relaxed);
    return;
  }

  uint8_t cand_s = candidate_stx.load(std::memory_order_relaxed);
  uint8_t cand_e = candidate_etx.load(std::memory_order_relaxed);

  if (stx == cand_s && etx == cand_e) {
    if (len > 0)
      candidate_len.store(len, std::memory_order_relaxed);
    consecutive_mismatches.store(0, std::memory_order_relaxed);
    uint8_t m = consecutive_matches.fetch_add(1, std::memory_order_relaxed) + 1;
    if (m >= 3) {
      status.store(FramingStatus::LOCKED, std::memory_order_relaxed);
      saveToNvs(nvs_ns, tag);
    } else {
      status.store(FramingStatus::LEARNING, std::memory_order_relaxed);
    }
  } else {
    consecutive_matches.store(0, std::memory_order_relaxed);
    uint8_t m =
        consecutive_mismatches.fetch_add(1, std::memory_order_relaxed) + 1;
    if (cur == FramingStatus::LOCKED) {
      if (m >= 10) {
        status.store(FramingStatus::WAITING, std::memory_order_relaxed);
        consecutive_mismatches.store(0, std::memory_order_relaxed);
      }
    } else {
      if (m >= 5) {
        candidate_stx.store(stx, std::memory_order_relaxed);
        candidate_etx.store(etx, std::memory_order_relaxed);
        if (len > 0)
          candidate_len.store(len, std::memory_order_relaxed);
        consecutive_matches.store(1, std::memory_order_relaxed);
        consecutive_mismatches.store(0, std::memory_order_relaxed);
        status.store(FramingStatus::LEARNING, std::memory_order_relaxed);
      }
    }
  }
}

void FramingTracker::restoreFromNvs(const char *nvs_ns,
                                    const char *tag) noexcept {
  if (!nvs_ns)
    nvs_ns = "dp_frame_p0";

  Preferences prefs;
  if (prefs.begin(nvs_ns, true)) {
    uint8_t s = prefs.getUChar("stx", 0);
    uint8_t e = prefs.getUChar("etx", 0);
    uint8_t l = prefs.getUChar("len", 0);
    bool locked = prefs.getBool("locked", false);
    bool fixed = prefs.getBool("fixed", false);
    prefs.end();

    if (locked && s != 0 && e != 0) {
      candidate_stx.store(s, std::memory_order_relaxed);
      candidate_etx.store(e, std::memory_order_relaxed);
      candidate_len.store(l, std::memory_order_relaxed);
      status.store(FramingStatus::LOCKED, std::memory_order_relaxed);
      is_custom_fixed.store(fixed, std::memory_order_relaxed);
      ::Serial.printf("[%s] Restored valid framing from NVS (%s): STX=0x%02X, "
                      "ETX=0x%02X, LEN=%u\r\n",
                      tag, nvs_ns, s, e, l);
    }
  }
}

void FramingTracker::saveToNvs(const char *nvs_ns, const char *tag) noexcept {
  if (!nvs_ns)
    nvs_ns = "dp_frame_p0";

  Preferences prefs;
  if (prefs.begin(nvs_ns, false)) {
    uint8_t s = candidate_stx.load(std::memory_order_relaxed);
    uint8_t e = candidate_etx.load(std::memory_order_relaxed);
    uint8_t l = candidate_len.load(std::memory_order_relaxed);
    bool is_locked =
        (status.load(std::memory_order_relaxed) == FramingStatus::LOCKED);
    bool fixed = is_custom_fixed.load(std::memory_order_relaxed);

    prefs.putUChar("stx", s);
    prefs.putUChar("etx", e);
    prefs.putUChar("len", l);
    prefs.putBool("locked", is_locked);
    prefs.putBool("fixed", fixed);
    prefs.end();

    ::Serial.printf("[%s] Persisted framing to NVS (%s): STX=0x%02X, "
                    "ETX=0x%02X, LEN=%u%s\r\n",
                    tag, nvs_ns, s, e, l, fixed ? " [FIXED]" : "");
  }
}

bool FramingTracker::isConsistent(uint8_t stx, uint8_t etx) const noexcept {
  const FramingStatus cur = status.load(std::memory_order_relaxed);
  if (cur != FramingStatus::LOCKED)
    return true;
  return (stx == candidate_stx.load(std::memory_order_relaxed) &&
          etx == candidate_etx.load(std::memory_order_relaxed));
}

// ── Doorphone Protocol & FSM Sealed State ─────────────────────────────────────

namespace {

struct DoorphoneState {
  std::atomic<bool> front_bell{false};
  std::atomic<bool> lobby_bell{false};
  std::atomic<uint32_t> last_bell_ms{0};
};

static DoorphoneState s_doorphone_state{};
static FramingTracker s_doorphone_tracker{};
static DoorphoneTxHandler s_dp_tx_handler = nullptr;

class DoorphoneController {
public:
  enum class Step : uint8_t { IDLE = 0, CALL_SENT, OPEN_SENT };

private:
  std::atomic<Step> _step{Step::IDLE};
  std::atomic<uint8_t> _c_stx{0x7F};
  std::atomic<uint8_t> _c_etx{0xEE};
  std::atomic<uint8_t> _op_open{0};
  std::atomic<uint8_t> _op_end{0};
  esp_timer_handle_t _timer{nullptr};

public:
  void init() {
    if (!_timer) {
      esp_timer_create_args_t timer_args{};
      timer_args.callback = onTimerCallback;
      timer_args.name = "dp_fsm_timer";
      esp_timer_create(&timer_args, &_timer);
    }
  }

  bool startSequence(uint8_t stx, uint8_t etx, uint8_t op_call, uint8_t op_open,
                     uint8_t op_end) {
    Step expected = Step::IDLE;
    if (!_step.compare_exchange_strong(expected, Step::CALL_SENT)) {
      return false;
    }

    _c_stx.store(stx, std::memory_order_release);
    _c_etx.store(etx, std::memory_order_release);
    _op_open.store(op_open, std::memory_order_release);
    _op_end.store(op_end, std::memory_order_release);

    sendDpPacket(stx, op_call, etx);

    if (_timer) {
      esp_timer_start_once(_timer, 350000); // 350ms 후 문열림 패킷 전송 (골든 타임)
    }
    return true;
  }

  void cancel() {
    if (_timer) {
      esp_timer_stop(_timer);
    }
    _step.store(Step::IDLE, std::memory_order_release);
  }

  [[nodiscard]] bool isBusy() const noexcept {
    return _step.load(std::memory_order_acquire) != Step::IDLE;
  }

  [[nodiscard]] Step getStep() const noexcept {
    return _step.load(std::memory_order_acquire);
  }

  static void sendDpPacket(uint8_t stx, uint8_t op, uint8_t etx) {
    StaticPacket pkt{4, 5};
    pkt.data[0] = stx;
    pkt.data[1] = op;
    pkt.data[2] = 0x00;
    pkt.data[3] = 0x00;
    pkt.data[4] = etx;
    (void)Router_EnqueueDownlink(4, pkt);
  }

  static void onTimerCallback(void *arg);
};

static DoorphoneController s_doorphone_controller;

void DoorphoneController::onTimerCallback(void * /*arg*/) {
  const Step cur = s_doorphone_controller._step.load(std::memory_order_acquire);
  switch (cur) {
  case Step::CALL_SENT:
    sendDpPacket(
        s_doorphone_controller._c_stx.load(std::memory_order_acquire),
        s_doorphone_controller._op_open.load(std::memory_order_acquire),
        s_doorphone_controller._c_etx.load(std::memory_order_acquire));
    s_doorphone_controller._step.store(Step::OPEN_SENT,
                                       std::memory_order_release);
    if (s_doorphone_controller._timer) {
      esp_timer_start_once(s_doorphone_controller._timer,
                           750000); // 750ms 후 종료 패킷
    }
    break;

  case Step::OPEN_SENT:
    sendDpPacket(s_doorphone_controller._c_stx.load(std::memory_order_acquire),
                 s_doorphone_controller._op_end.load(std::memory_order_acquire),
                 s_doorphone_controller._c_etx.load(std::memory_order_acquire));
    s_doorphone_state.front_bell.store(false, std::memory_order_release);
    s_doorphone_state.lobby_bell.store(false, std::memory_order_release);
    s_doorphone_controller._step.store(Step::IDLE, std::memory_order_release);
    break;

  default:
    break;
  }
}

} // namespace

void Wallpad_InitDecoupledHooks() noexcept {
  Device_RegisterParserHooks(
    [](std::span<const uint8_t> frame) noexcept -> bool {
      auto *parser = WallpadParserFactory::getActiveParser();
      return parser ? parser->isAckPacket(frame) : false;
    },
    [](std::span<const uint8_t> frame, uint8_t &dev_id, uint8_t &sub1, uint8_t &sub2) noexcept -> bool {
      auto *parser = WallpadParserFactory::getActiveParser();
      return parser ? parser->extractDeviceKey(frame, dev_id, sub1, sub2) : false;
    }
  );

  Device_RegisterStateDecoder(ControlTemplate_DecodeByDevId);
  Device_RegisterNormSub1Hook(ControlTemplate_NormSub1);

  ControlTemplateRegistry::setDeviceUnitCountProvider([](uint8_t dev_id) -> size_t {
    size_t units = 0;
    for (size_t i = 0; i < Device_GetCount() && units < 2; ++i) {
      DeviceStateEntry snap{};
      if (Device_GetSnapshot(i, snap) && snap.dev_id == dev_id)
        ++units;
    }
    return units;
  });

  AutoProbingEngine::setDeviceHooks(
    []() -> size_t {
      return Device_GetOnlineCount();
    },
    [](uint8_t dev_id, uint8_t sub1, uint8_t sub2,
       uint8_t *out_buf, size_t max_len, size_t *out_len) -> bool {
      if (!out_buf || !out_len || max_len == 0) return false;
      DeviceStateEntry snap{};
      if (Device_FindCopy(dev_id, sub1, sub2, snap) && snap.is_online && snap.last_ack_len >= 4) {
        size_t c_len = std::min(static_cast<size_t>(snap.last_ack_len), max_len);
        memcpy(out_buf, snap.last_ack_data.data(), c_len);
        *out_len = c_len;
        return true;
      }
      return false;
    },
    [](StaticPacket &ack) {
      Device_ProcessBusPacket(ack);
    }
  );
}

bool Wallpad_DoorphoneOpen(bool is_lobby) noexcept {
  FramingStatus dp_status = FramingStatus::WAITING;
  uint8_t dp_stx = 0;
  uint8_t dp_etx = 0;
  uint8_t dp_len = 0;
  Wallpad_DoorphoneGetFraming(dp_status, dp_stx, dp_etx, dp_len);

  if (dp_stx == 0)
    dp_stx = 0x7F;
  if (dp_etx == 0)
    dp_etx = 0xEE;

  const DoorphoneSpec *dp_prof =
      ProfileMatcher::matchDoorphone(dp_stx, dp_etx, dp_len);

  uint8_t op_call = (!is_lobby) ? (dp_prof ? dp_prof->call_front : 0xB9)
                                : (dp_prof ? dp_prof->call_lobby : 0x5F);
  uint8_t op_open = (!is_lobby) ? (dp_prof ? dp_prof->open_front : 0xB4)
                                : (dp_prof ? dp_prof->open_lobby : 0x61);
  uint8_t op_end = (!is_lobby) ? (dp_prof ? dp_prof->end_front : 0xB8)
                               : (dp_prof ? dp_prof->end_lobby : 0x60);

  // 50ms Pre-Guard Time: 벨 수신 직후 3840 bps 반이중 버스 충돌 방지용 Line Silent 대기
  uint32_t last_bell = s_doorphone_state.last_bell_ms.load(std::memory_order_relaxed);
  if (last_bell > 0) {
    uint32_t now_ms = millis();
    constexpr uint32_t kDpPreGuardMs = 50;
    if (now_ms - last_bell < kDpPreGuardMs) {
      uint32_t rem_ms = kDpPreGuardMs - (now_ms - last_bell);
      if (rem_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(rem_ms) > 0 ? pdMS_TO_TICKS(rem_ms) : 1);
      }
    }
  }

  return Wallpad_DoorphoneStartSequence(dp_stx, dp_etx, op_call, op_open, op_end);
}

void Wallpad_DoorphoneInit() noexcept {
  s_doorphone_controller.init();
  Device_RegisterDoorphoneOpenHandler(Wallpad_DoorphoneOpen);
  Wallpad_InitDecoupledHooks();
  ProfileRepository::addProfileChangeListener(Wallpad_DoorphoneOnProfileChanged);
  g_control_registry.init();
}

bool Wallpad_DoorphoneStartSequence(uint8_t stx, uint8_t etx, uint8_t op_call,
                                    uint8_t op_open, uint8_t op_end) noexcept {
  return s_doorphone_controller.startSequence(stx, etx, op_call, op_open, op_end);
}

void Wallpad_DoorphoneCancel() noexcept {
  s_doorphone_controller.cancel();
}

bool Wallpad_DoorphoneIsBusy() noexcept {
  return s_doorphone_controller.isBusy();
}

void Wallpad_DoorphoneGetState(bool &out_front_bell, bool &out_lobby_bell,
                              uint32_t &out_last_bell_ms) noexcept {
  out_front_bell = s_doorphone_state.front_bell.load(std::memory_order_relaxed);
  out_lobby_bell = s_doorphone_state.lobby_bell.load(std::memory_order_relaxed);
  out_last_bell_ms = s_doorphone_state.last_bell_ms.load(std::memory_order_relaxed);
}

void Wallpad_DoorphoneGetFraming(FramingStatus &out_status, uint8_t &out_stx,
                                uint8_t &out_etx, uint8_t &out_len) noexcept {
  out_status = s_doorphone_tracker.status.load(std::memory_order_relaxed);
  out_stx = s_doorphone_tracker.candidate_stx.load(std::memory_order_relaxed);
  out_etx = s_doorphone_tracker.candidate_etx.load(std::memory_order_relaxed);
  out_len = s_doorphone_tracker.candidate_len.load(std::memory_order_relaxed);
}

bool Wallpad_DoorphoneGetLockedFraming(uint8_t &stx, uint8_t &etx, uint8_t &len) noexcept {
  if (s_doorphone_tracker.status.load(std::memory_order_relaxed) == FramingStatus::LOCKED) {
    stx = s_doorphone_tracker.candidate_stx.load(std::memory_order_relaxed);
    etx = s_doorphone_tracker.candidate_etx.load(std::memory_order_relaxed);
    len = s_doorphone_tracker.candidate_len.load(std::memory_order_relaxed);
    return true;
  }
  return false;
}

void Wallpad_DoorphoneFrameDetected(uint8_t stx, uint8_t etx, uint8_t len) noexcept {
  char cur_dp_ns[16];
  FramingTracker::getNvsNamespace(Config_GetWallpadProfile(), cur_dp_ns, sizeof(cur_dp_ns));

  uint8_t fixed_len = 0;
  if (Wallpad_MatchDoorphoneLock(stx, etx, len, fixed_len) && len >= 5) {
    if (s_doorphone_tracker.status.load(std::memory_order_relaxed) != FramingStatus::LOCKED) {
      s_doorphone_tracker.setFixedLock(stx, etx, fixed_len);
      s_doorphone_tracker.saveToNvs(cur_dp_ns);
    }
  } else {
    s_doorphone_tracker.processFrame(stx, etx, len, cur_dp_ns);
  }
}

void Wallpad_DoorphoneCheckBellTimeout() noexcept {
  const uint32_t last_bell = s_doorphone_state.last_bell_ms.load(std::memory_order_relaxed);
  if (last_bell > 0 && TimeUtils::isElapsed(last_bell, Config::Timing::DOORPHONE_BELL_TIMEOUT_MS)) {
    Wallpad_ResetDoorphoneBellState();
  }
}

void Wallpad_DoorphoneClearNvs(const char *nvs_ns) noexcept {
  s_doorphone_tracker.clearNvs(nvs_ns, "DOORPHONE");
}

void Wallpad_DoorphoneRestoreNvs(const char *nvs_ns) noexcept {
  s_doorphone_tracker.restoreFromNvs(nvs_ns, "DOORPHONE");
}

void Wallpad_DoorphoneSaveNvs(const char *nvs_ns) noexcept {
  s_doorphone_tracker.saveToNvs(nvs_ns, "DOORPHONE");
}

void Wallpad_DoorphoneOnProfileChanged(uint8_t old_idx, uint8_t new_idx) noexcept {
  if (old_idx != new_idx) {
    char old_ns[16], new_ns[16];
    FramingTracker::getNvsNamespace(old_idx, old_ns, sizeof(old_ns));
    FramingTracker::getNvsNamespace(new_idx, new_ns, sizeof(new_ns));
    s_doorphone_tracker.saveToNvs(old_ns, "DOORPHONE");
    s_doorphone_tracker.reset();
    s_doorphone_tracker.restoreFromNvs(new_ns, "DOORPHONE");
  }
}

void Wallpad_DoorphoneRegisterTxHandler(DoorphoneTxHandler handler) noexcept {
  s_dp_tx_handler = handler;
}

const DoorphoneSpec *Wallpad_MatchDoorphone(uint8_t stx, uint8_t etx, uint8_t len) noexcept {
  return ProfileMatcher::matchDoorphone(stx, etx, len);
}

void Wallpad_ResetDoorphoneBellState() noexcept {
  s_doorphone_state.front_bell.store(false, std::memory_order_release);
  s_doorphone_state.lobby_bell.store(false, std::memory_order_release);
  s_doorphone_state.last_bell_ms.store(0, std::memory_order_release);
  Device_NotifyDoorphoneEvent(false, false);
}

void Wallpad_HandleDoorphonePacket(const StaticPacket &packet) noexcept {
  Wallpad_DoorphoneCheckBellTimeout();
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
    s_doorphone_state.front_bell.store(true, std::memory_order_release);
    s_doorphone_state.last_bell_ms.store(now, std::memory_order_release);
    state_changed = true;
    break;
  case DoorphoneEvent::END_FRONT:
    s_doorphone_state.front_bell.store(false, std::memory_order_release);
    state_changed = true;
    break;
  case DoorphoneEvent::BELL_LOBBY:
    s_doorphone_state.lobby_bell.store(true, std::memory_order_release);
    s_doorphone_state.last_bell_ms.store(now, std::memory_order_release);
    state_changed = true;
    break;
  case DoorphoneEvent::END_LOBBY:
    s_doorphone_state.lobby_bell.store(false, std::memory_order_release);
    state_changed = true;
    break;
  case DoorphoneEvent::NONE:
  default:
    break;
  }

  if (state_changed) {
    bool f = s_doorphone_state.front_bell.load(std::memory_order_relaxed);
    bool l = s_doorphone_state.lobby_bell.load(std::memory_order_relaxed);
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
