// ============================================================================
// WallpadProtocol: Level 3 Wallpad Profiles, Protocol Engine & Probing Cache
// Implementation
// ============================================================================

#include "L3_Protocol/Private/Wallpad_Engine.h"
#include "L3_Protocol/Public/Protocol_Device.h"
#include "L3_Protocol/Private/Routing_Engine.h"
#include "L0_Foundation/System_Buffer.h"
#include "L0_Foundation/System_Platform.h"

#include <span>
#include <Arduino.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <esp_timer.h>
#include "L3_Protocol/Public/Protocol_Facade.h"

// ============================================================================

// ============================================================================
// PART 2: WALLPAD PROTOCOL ORCHESTRATION & FSM ENGINE
// ============================================================================
// ============================================================================
// Wallpad_Protocol.cpp — L3 Protocol & Routing Engine
// Wallpad Business Logic Orchestration & Domain Engine Implementation
// Canonical 4+1 Layer: L3 Routing/Protocol layer
// ============================================================================



namespace {

int Wallpad_ScoreCandidate(const PollingTargetRegistry::PollingCandidate &tgt,
                           const DeviceMetadata *cached_meta) noexcept {
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
  if (!cached_meta) {
    return 1;
  }
  if (cached_meta->last_updated_ms == 0) {
    return 1;
  }
  if (cached_meta->is_online) {
    return 2;
  }
  if (TimeUtils::isElapsed(cached_meta->last_stale_poll_ms,
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
  // Deferred NVS flush is strictly isolated to Core 0 background maintenance tick (Pillar 1 Non-Interference)
  if (Wallpad_TakeRelearnRequest()) {
    Wallpad_CheckConvergence(true);
    System_TraceMessage("[AUTO PROBE] Convergence state reset. Re-learning "
                        "bus offsets...\r\n");
  }

  if (!s_convergence_done) {
    Wallpad_CheckConvergence(false);
  }

  Polling_GetRegistry().sweepExpired(Config::Timing::STALE_DEVICE_THRESHOLD_MS);

  PollingTargetRegistry::PollingCandidate candidates[PollingTargetRegistry::MAX_TARGETS];
  size_t active_cnt = Polling_GetRegistry().getActiveCandidates(
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
      DeviceMetadata cached_meta{};
      bool has_cached = Device_GetMetadata(tgt.dev_id, tgt.sub1, tgt.sub2, cached_meta);
      int score = Wallpad_ScoreCandidate(tgt, has_cached ? &cached_meta : nullptr);

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
        Polling_GetRegistry().getQueryData(tgt.entry_idx, poll_raw_ptr, poll_raw_len);
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
      DeviceMetadata dev_meta{};
      bool has_dev = Device_GetAtMetadata(idx, dev_meta);
      s_current_dev_idx = (idx + 1) % dev_cnt;
      if (has_dev && dev_meta.dev_id != Config::FCU::DEV_ID &&
          (dev_meta.is_online || dev_meta.last_updated_ms == 0 ||
           TimeUtils::isElapsed(dev_meta.last_stale_poll_ms,
                                Config::Timing::CH1_STALE_POLL_INTERVAL_MS))) {
        RouteEndpoint ep;
        if (!Router_LookupRoute(dev_meta.dev_id, dev_meta.sub1, dev_meta.sub2, ep) ||
            ep.channel_id != 5) {
          poll_dev_id = dev_meta.dev_id;
          poll_sub1 = dev_meta.sub1;
          poll_sub2 = dev_meta.sub2;
          if (!dev_meta.is_online)
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
    const size_t copy_len =
        std::min(static_cast<size_t>(poll_raw_len), out_pkt.data.size());
    out_pkt.length = static_cast<uint8_t>(copy_len);
    memcpy(out_pkt.data.data(), poll_raw_ptr, copy_len);
  } else {
    if (!Universal_GetEngine().buildQueryPacket(poll_dev_id, poll_sub1, poll_sub2, out_pkt)) {
      return false;
    }
  }
  return true;
}

void Wallpad_HandleBusPacket(uint8_t channel_id, const StaticPacket &ack_pkt,
                             const StaticPacket *matching_query) noexcept {
  StaticPacket ack = ack_pkt;
  ack.channel_id = channel_id;

  if (matching_query && matching_query->length > 0) {
    Polling_GetRegistry().updateResponse(matching_query->data.data(), matching_query->length,
                                     ack.data.data(), ack.length);
    AutoProbe_GetEngine().feedOpcodePair(
        std::span<const uint8_t>(matching_query->data.data(), matching_query->length),
        std::span<const uint8_t>(ack.data.data(), ack.length));
  }

  Device_ProcessBusPacket(ack);

  uint8_t dev_id = 0, sub1 = 0, sub2 = 0;
  std::span<const uint8_t> ack_span(ack.data.data(), ack.length);
  if (Universal_GetEngine().extractDeviceKey(ack_span, dev_id, sub1, sub2)) {
    if (channel_id == 1 && (!matching_query || matching_query->length == 0)) {
      Polling_GetRegistry().markVerified(dev_id, sub1, sub2);
    }
    Router_RecordRoute(channel_id, -1, dev_id, sub1, sub2);
  }
}

ControlAction Wallpad_EvaluateControl(StaticPacket &req, StaticPacket &virtual_ack_out,
                                      bool &out_unidir) noexcept {
  out_unidir = false;

  if (UNLIKELY(req.length < 5))
    return ControlAction::DROP;
  auto &engine = Universal_GetEngine();
  std::span<const uint8_t> frame(req.data.data(), req.length);

  uint8_t dev_id = 0, sub1 = 0, sub2 = 0;
  const bool has_key = engine.extractDeviceKey(frame, dev_id, sub1, sub2);

  if (engine.isQueryPacket(frame)) {
    virtual_ack_out.channel_id = req.channel_id;
    if (has_key && Device_CopyVirtualAck(dev_id, sub1, sub2, virtual_ack_out)) {
      return ControlAction::VIRTUAL_ACK_IMMEDIATE;
    }
    return ControlAction::DROP;
  }

  GroupControlTemplate grp{};
  const bool has_grp =
      (has_key && dev_id != 0) && Control_GetRegistry().findGroup(dev_id, grp);

  bool is_ctl = engine.isControlPacket(frame);
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
  size_t active_tgts = Polling_GetRegistry().activeCount();
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
    System_ClearEventBits(SYS_EVT_CACHE_READY);
    return false;
  }

  if (s_convergence_done) {
    return true;
  }

  size_t active_tgts = Polling_GetRegistry().activeCount();
  size_t online_devs = Device_GetOnlineCount();

  if (active_tgts != s_last_active_tgts) {
    s_last_active_tgts = active_tgts;
    s_stable_start_ms = millis();
  }

  bool is_all_online = (online_devs >= active_tgts);
  auto &engine = Universal_GetEngine();
  if (engine.isAutoMode() &&
      !AutoProbe_GetEngine().isOffsetsLocked()) {
    is_all_online = (Polling_GetRegistry().verifiedCount() >= active_tgts);
  }

  if (active_tgts > 0 && is_all_online) {
    if (s_stable_start_ms == 0) {
      s_stable_start_ms = millis();
      return false;
    }
    if (TimeUtils::isElapsed(s_stable_start_ms,
                             Config::Timing::CACHE_CONVERGENCE_STABLE_MS)) {
      s_convergence_done = true;
      System_SetEventBits(SYS_EVT_CACHE_READY);
      if (engine.isAutoMode() &&
          !AutoProbe_GetEngine().isOffsetsLocked()) {
        AutoProbe_GetEngine().analyzeCacheMatrix();
      }
      Polling_GetRegistry().resetHits();
      System_TraceMessage(
          "[SYSTEM MSG]  ★ 2nd-Tier Cache Converged (Zero Offline). "
          "Runtime metrics synchronized.\r\n");
      Control_GetRegistry().synthesizeFromConvergedCache();
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

void Wallpad_ResetStalePollCount() noexcept {
  s_stale_poll_cnt.store(0, std::memory_order_relaxed);
}

uint8_t Wallpad_GetStx() noexcept {
  return Universal_GetEngine().getStx();
}

ExtractedFrameResult Wallpad_ExtractAndValidateFast(const uint8_t *stream, size_t stream_len, size_t stx_idx) noexcept {
  return Universal_GetEngine().extractPacketLengthAndValidate(stream, stream_len, stx_idx);
}

int Wallpad_ExtractLength(const uint8_t *stream, size_t stream_len, size_t stx_idx) noexcept {
  return Wallpad_ExtractAndValidateFast(stream, stream_len, stx_idx).length;
}

bool Wallpad_ValidatePacket(std::span<const uint8_t> frame) noexcept {
  return Universal_GetEngine().validatePacket(frame);
}

bool Wallpad_HandleSubBusQuery(uint8_t channel_id, const StaticPacket &req,
                               StaticPacket &virtual_ack_out) noexcept {
  auto &engine = Universal_GetEngine();
  std::span<const uint8_t> frame(req.data.data(), req.length);
  if (!engine.isQueryPacket(frame)) {
    return false;
  }
  uint8_t dev_id = 0, sub1 = 0, sub2 = 0;
  engine.extractDeviceKey(frame, dev_id, sub1, sub2);
  Polling_GetRegistry().registerOrTouch(channel_id, dev_id, sub1, sub2,
                                    req.data.data(), req.length);
  virtual_ack_out.channel_id = channel_id;
  return Device_CopyVirtualAck(dev_id, sub1, sub2, virtual_ack_out);
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
  if (nvsClearNs(nvs_ns)) {
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
    requestNvsSave(nvs_ns);
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
      requestNvsSave(nvs_ns);
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

void FramingTracker::init() noexcept {
  if (!commit_mutex) {
    commit_mutex = xSemaphoreCreateMutexStatic(&commit_mutex_storage);
  }
}

void FramingTracker::restoreFromNvs(const char *nvs_ns,
                                    const char *tag) noexcept {
  if (!nvs_ns)
    nvs_ns = "dp_frame_p0";

  SystemNvs prefs;
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

      struct FramingSnapshot {
        uint8_t s, e, l;
        bool locked, fixed;
        char ns[16];
      } snap{s, e, l, locked, fixed, {0}};
      strncpy(snap.ns, nvs_ns, sizeof(snap.ns));
      last_committed_crc32 =
          FastCrc32(reinterpret_cast<const uint8_t *>(&snap), sizeof(snap));
      has_committed_crc = true;

      ::Serial.printf("[%s] Restored valid framing from NVS (%s): STX=0x%02X, "
                      "ETX=0x%02X, LEN=%u\r\n",
                      tag, nvs_ns, s, e, l);
    }
  }
}

void FramingTracker::requestNvsSave(const char *nvs_ns) noexcept {
  if (nvs_ns && nvs_ns[0]) {
    strncpy(pending_nvs_ns, nvs_ns, sizeof(pending_nvs_ns) - 1);
    pending_nvs_ns[sizeof(pending_nvs_ns) - 1] = '\0';
  }
  const uint32_t now = millis();
  bool expected = false;
  if (nvs_dirty.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    first_request_ms.store(now, std::memory_order_release);
    has_first_request.store(true, std::memory_order_release);
  }
  last_request_ms.store(now, std::memory_order_release);
}

bool FramingTracker::commitNvsIfPending() noexcept {
  if (!nvs_dirty.load(std::memory_order_acquire)) {
    return false;
  }

  const uint32_t now = millis();
  if (next_retry_ms > 0 && static_cast<int32_t>(now - next_retry_ms) < 0) {
    return false;
  }

  const bool has_first = has_first_request.load(std::memory_order_relaxed);
  const uint32_t first_req = first_request_ms.load(std::memory_order_relaxed);
  const uint32_t last_req = last_request_ms.load(std::memory_order_relaxed);
  const bool starvation_triggered =
      (has_first &&
       static_cast<int32_t>(now - (first_req + Config::Timing::NVS_COMMIT_STARVATION_MS)) >= 0);

  if (!starvation_triggered) {
    // 1. Debounce (quiet window since last request)
    if (last_req > 0 &&
        static_cast<int32_t>(now - (last_req + Config::Timing::NVS_COMMIT_GUARD_MS)) < 0) {
      return false;
    }
    // 2. Realistic bus quiet (100ms idle on CH1 inter-packet gap)
    const uint32_t last_ch1_rx = ProtocolDiag_GetLastCh1RxMs();
    if (last_ch1_rx > 0 &&
        static_cast<int32_t>(now - (last_ch1_rx + Config::Timing::NVS_COMMIT_BUS_QUIET_MS)) < 0) {
      return false;
    }
  }

  if (!commit_mutex || xSemaphoreTake(commit_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
    return false;
  }

  nvs_dirty.exchange(false, std::memory_order_acq_rel);

  const uint8_t s = candidate_stx.load(std::memory_order_relaxed);
  const uint8_t e = candidate_etx.load(std::memory_order_relaxed);
  const uint8_t l = candidate_len.load(std::memory_order_relaxed);
  const bool is_locked = (status.load(std::memory_order_relaxed) == FramingStatus::LOCKED);
  const bool fixed = is_custom_fixed.load(std::memory_order_relaxed);

  struct FramingSnapshot {
    uint8_t s{0}, e{0}, l{0};
    bool locked{false}, fixed{false};
    char ns[16]{0};
  };
  static_assert(std::is_trivially_copyable_v<FramingSnapshot>);
  FramingSnapshot snap;
  memset(&snap, 0, sizeof(snap));
  snap.s = s;
  snap.e = e;
  snap.l = l;
  snap.locked = is_locked;
  snap.fixed = fixed;
  strncpy(snap.ns, pending_nvs_ns, sizeof(snap.ns) - 1);

  const uint32_t current_crc =
      FastCrc32(reinterpret_cast<const uint8_t *>(&snap), sizeof(snap));

  if (has_committed_crc && current_crc == last_committed_crc32) {
    failure_count = 0;
    next_retry_ms = 0;
    has_first_request.store(false, std::memory_order_release);
    xSemaphoreGive(commit_mutex);
    return true; // Wear guard: identical data
  }

  System_TaskWdtResetSelf();

  System_SetNvsBusy(true);
  SystemNvs prefs;
  bool success = false;
  if (prefs.begin(pending_nvs_ns, false)) {
    prefs.putUChar("stx", s);
    prefs.putUChar("etx", e);
    prefs.putUChar("len", l);
    prefs.putBool("locked", is_locked);
    prefs.putBool("fixed", fixed);
    prefs.end();
    success = true;
  }
  System_SetNvsBusy(false);

  if (success) {
    last_committed_crc32 = current_crc;
    has_committed_crc = true;
    failure_count = 0;
    next_retry_ms = 0;
    has_first_request.store(false, std::memory_order_release);
    ESP_LOGI("FRAMING", "[NVS] Decoupled commit succeeded (%s): STX=0x%02X ETX=0x%02X L=%u",
             pending_nvs_ns, s, e, l);
  } else {
    nvs_dirty.store(true, std::memory_order_release);
    failure_count = std::min<uint8_t>(failure_count + 1, 5);
    const uint32_t backoff_ms =
        std::min<uint32_t>(3000u << std::min<uint8_t>(failure_count - 1, 4), 60000u);
    next_retry_ms = millis() + backoff_ms;
    ESP_LOGW("FRAMING", "[NVS] Commit failed (attempt %u), backoff %u ms",
             failure_count, backoff_ms);
  }

  xSemaphoreGive(commit_mutex);
  return success;
}

bool FramingTracker::forceFlush(uint32_t timeout_ms) noexcept {
  if (!nvs_dirty.load(std::memory_order_acquire)) {
    return true;
  }

  if (!commit_mutex || xSemaphoreTake(commit_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
    ESP_LOGW("FRAMING", "[WARN] forceFlush lock timeout (%u ms), skipping to prevent hang", timeout_ms);
    return false;
  }

  nvs_dirty.exchange(false, std::memory_order_acq_rel);

  const uint8_t s = candidate_stx.load(std::memory_order_relaxed);
  const uint8_t e = candidate_etx.load(std::memory_order_relaxed);
  const uint8_t l = candidate_len.load(std::memory_order_relaxed);
  const bool is_locked = (status.load(std::memory_order_relaxed) == FramingStatus::LOCKED);
  const bool fixed = is_custom_fixed.load(std::memory_order_relaxed);

  struct FramingSnapshot {
    uint8_t s{0}, e{0}, l{0};
    bool locked{false}, fixed{false};
    char ns[16]{0};
  };
  FramingSnapshot snap;
  memset(&snap, 0, sizeof(snap));
  snap.s = s;
  snap.e = e;
  snap.l = l;
  snap.locked = is_locked;
  snap.fixed = fixed;
  strncpy(snap.ns, pending_nvs_ns, sizeof(snap.ns) - 1);

  const uint32_t current_crc =
      FastCrc32(reinterpret_cast<const uint8_t *>(&snap), sizeof(snap));

  if (has_committed_crc && current_crc == last_committed_crc32) {
    has_first_request.store(false, std::memory_order_release);
    xSemaphoreGive(commit_mutex);
    return true;
  }

  System_TaskWdtResetSelf();

  System_SetNvsBusy(true);
  SystemNvs prefs;
  bool success = false;
  if (prefs.begin(pending_nvs_ns, false)) {
    prefs.putUChar("stx", s);
    prefs.putUChar("etx", e);
    prefs.putUChar("len", l);
    prefs.putBool("locked", is_locked);
    prefs.putBool("fixed", fixed);
    prefs.end();
    success = true;
  }
  System_SetNvsBusy(false);

  if (success) {
    last_committed_crc32 = current_crc;
    has_committed_crc = true;
    failure_count = 0;
    next_retry_ms = 0;
    has_first_request.store(false, std::memory_order_release);
  } else {
    nvs_dirty.store(true, std::memory_order_release);
  }

  xSemaphoreGive(commit_mutex);
  return success;
}

void FramingTracker::saveToNvs(const char *nvs_ns, const char * /*tag*/) noexcept {
  if (nvs_ns) {
    strncpy(pending_nvs_ns, nvs_ns, sizeof(pending_nvs_ns) - 1);
    pending_nvs_ns[sizeof(pending_nvs_ns) - 1] = '\0';
  }
  forceFlush(100);
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
        System_DelayMs(rem_ms);
      }
    }
  }

  return Wallpad_DoorphoneStartSequence(dp_stx, dp_etx, op_call, op_open, op_end);
}

void Wallpad_DoorphoneInit() noexcept {
  s_doorphone_tracker.init();
  s_doorphone_controller.init();
  ProfileRepository::addProfileChangeListener(Wallpad_DoorphoneOnProfileChanged);
  Control_GetRegistry().init();
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
  const WallpadProfile *p = ProfileMatcher::getActiveProfile();
  if (p && p->doorphone.stx != 0 && p->doorphone.etx != 0 && p->doorphone.len != 0) {
    stx = p->doorphone.stx;
    etx = p->doorphone.etx;
    len = p->doorphone.len;
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
      s_doorphone_tracker.requestNvsSave(cur_dp_ns);
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

bool Wallpad_DoorphoneIsFramingNvsDirty() noexcept {
  return s_doorphone_tracker.isNvsDirty();
}

bool Wallpad_DoorphoneCommitNvsIfPending() noexcept {
  return s_doorphone_tracker.commitNvsIfPending();
}

void Wallpad_DoorphoneForceFlushNvs(uint32_t timeout_ms) noexcept {
  s_doorphone_tracker.forceFlush(timeout_ms);
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

bool Wallpad_IsQueryPacket(std::span<const uint8_t> frame) noexcept {
  return Universal_GetEngine().isQueryPacket(frame);
}
