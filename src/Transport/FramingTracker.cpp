// ============================================================================
// FramingTracker: Level 2 Generic Serial Packet Framing Implementation
// ============================================================================

#include "Transport/FramingTracker.h"
#include <Arduino.h>
#include <Preferences.h>

void FramingTracker::setFixedLock(uint8_t stx, uint8_t etx,
                                  uint8_t len) noexcept {
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
