// ============================================================================
// WallpadProtocol: Level 3 Wallpad Profiles, Protocol Engine & Probing Cache
// ============================================================================

#include "Protocol/WallpadProtocol.h"
#include "System/SystemDiagnostics.h"

#include "esp_log.h"
#include <Preferences.h>
#include <algorithm>
#include <array>
#include <bitset>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

static const char *TAG = "ProfileMatcher";

template <class F> static inline size_t countIf(const PollingTargetEntry *e, size_t n, F f) {
  size_t c = 0;
  for (size_t i = 0; i < n; ++i)
    c += f(e[i]) ? 1 : 0;
  return c;
}


RTC_NOINIT_ATTR RtcWarmCache rtc_warm_cache;

bool g_warm_cache_loaded = false;
uint8_t g_warm_cache_source = 0; // 0: None/Cold, 1: RTC SRAM, 2: NVS Flash
uint8_t g_warm_cache_restored_count = 0;
std::atomic<bool> g_warm_cache_dirty{false};
std::atomic<uint32_t> g_warm_cache_dirty_ms{0};

namespace {
static NvsEnvelope<RtcWarmCache> s_warm_cache_env;
} // anonymous namespace

void WarmCache_SaveToRtc() {
  memset(&rtc_warm_cache, 0, sizeof(rtc_warm_cache));
  rtc_warm_cache.magic = RTC_MAGIC_WARM_CACHE;
  rtc_warm_cache.count = static_cast<uint8_t>(
      g_polling_targets.getWarmCacheEntries(rtc_warm_cache.entries, PollingTargetRegistry::MAX_TARGETS));
  if (rtc_warm_cache.count > 0) {
    rtc_warm_cache.crc32 =
        FastCrc32(reinterpret_cast<const uint8_t *>(rtc_warm_cache.entries),
                  sizeof(RtcWarmCacheEntry) * rtc_warm_cache.count);
  }
}

void WarmCache_SaveToNvs() {
  WarmCache_SaveToRtc();
  if (rtc_warm_cache.count > 0) {
    Preferences p;
    if (p.begin("wp_wc", false)) {
      s_warm_cache_env.payload = rtc_warm_cache;
      s_warm_cache_env.seal();
      p.putBytes("wc_data", &s_warm_cache_env, sizeof(s_warm_cache_env));
      p.end();
      ::Serial.printf(
          "[WARM CACHE] Synced %u targets to NVS Flash snapshot.\r\n",
          rtc_warm_cache.count);
    }
  }
  g_warm_cache_dirty.store(false, std::memory_order_release);
}

void WarmCache_RestoreOnBoot() {
  uint32_t now = millis();
  esp_reset_reason_t reason = esp_reset_reason();

  // 1순위: RTC FAST SRAM에서 0ms 즉시 복원
  if (reason != ESP_RST_POWERON &&
      rtc_warm_cache.magic == RTC_MAGIC_WARM_CACHE &&
      rtc_warm_cache.count > 0 &&
      rtc_warm_cache.count <= PollingTargetRegistry::MAX_TARGETS) {
    uint32_t computed_crc =
        FastCrc32(reinterpret_cast<const uint8_t *>(rtc_warm_cache.entries),
                  sizeof(RtcWarmCacheEntry) * rtc_warm_cache.count);
    if (computed_crc == rtc_warm_cache.crc32) {
      g_polling_targets.loadFromWarmCache(rtc_warm_cache.entries,
                                          rtc_warm_cache.count, now);
      g_warm_cache_loaded = true;
      g_warm_cache_source = 1;
      g_warm_cache_restored_count = rtc_warm_cache.count;
      ::Serial.printf("[WARM CACHE] Restored %u targets from RTC Fast SRAM "
                      "(0ms delay)!\r\n",
                      rtc_warm_cache.count);
      return;
    }
  }

  // 2순위: NVS Flash 스냅샷에서 무결성 검증 후 복원
  Preferences p;
  if (p.begin("wp_wc", true)) {
    if (p.isKey("wc_data")) {
      size_t len = p.getBytesLength("wc_data");
      if (len == sizeof(s_warm_cache_env) &&
          p.getBytes("wc_data", &s_warm_cache_env, sizeof(s_warm_cache_env)) ==
              sizeof(s_warm_cache_env)) {
        if (s_warm_cache_env.verify() && s_warm_cache_env.payload.count > 0 &&
            s_warm_cache_env.payload.count <= PollingTargetRegistry::MAX_TARGETS) {
          uint32_t computed_crc = FastCrc32(
              reinterpret_cast<const uint8_t *>(
                  s_warm_cache_env.payload.entries),
              sizeof(RtcWarmCacheEntry) * s_warm_cache_env.payload.count);
          if (computed_crc == s_warm_cache_env.payload.crc32) {
            g_polling_targets.loadFromWarmCache(
                s_warm_cache_env.payload.entries,
                s_warm_cache_env.payload.count, now);
            g_warm_cache_loaded = true;
            g_warm_cache_source = 2;
            g_warm_cache_restored_count = s_warm_cache_env.payload.count;
            ::Serial.printf(
                "[WARM CACHE] Restored %u targets from NVS Flash snapshot!\r\n",
                s_warm_cache_env.payload.count);
            p.end();
            return;
          }
        }
      }
    }
    p.end();
  }

  g_warm_cache_loaded = false;
  g_warm_cache_source = 0;
  g_warm_cache_restored_count = 0;
  ::Serial.println(
      F("[WARM CACHE] Cold start initialized (No prior cache found)."));
}

void WarmCache_CheckNvsDebounce() {
  if (g_warm_cache_dirty.load(std::memory_order_acquire)) {
    uint32_t dirty_ms = g_warm_cache_dirty_ms.load(std::memory_order_relaxed);
    if (dirty_ms > 0 &&
        TimeUtils::isElapsed(dirty_ms,
                             Config::Timing::WARM_CACHE_NVS_DEBOUNCE_MS)) {
      WarmCache_SaveToNvs();
    }
  }
}

// ============================================================================
// 공통 헬퍼 (파일 전체에서 공유)
// ============================================================================

// ============================================================================
// PollingTargetRegistry
// ============================================================================

PollingTargetRegistry g_polling_targets;

namespace {
bool entryMatches(const PollingTargetEntry &e, uint8_t d, uint8_t s1,
                  uint8_t s2, const uint8_t *raw, size_t n) {
  const bool in_key = d || s1 || s2;
  const bool e_key = e.dev_id || e.sub1 || e.sub2;
  if (in_key && e_key)
    return e.dev_id == d && e.sub1 == s1 && e.sub2 == s2;
  if (!in_key && e_key)
    return false;
  return raw && n > 0 && e.raw_query_len == n &&
         memcmp(e.raw_query_data.data(), raw, n) == 0;
}
} // namespace

void PollingTargetRegistry::registerOrTouch(uint8_t ch, uint8_t dev_id,
                                            uint8_t sub1, uint8_t sub2,
                                            const uint8_t *raw_pkt,
                                            size_t raw_len) {
  // CH2/CH3(월패드/앱), CH5(EW11 스니핑)만 대상. 0x2A(신발장/원격검침)는 폴링
  // 제외
  if ((ch != 2 && ch != 3 && ch != 5) || dev_id == 0x2A)
    return;

  const uint32_t now = millis();
  const uint8_t ch_bit = (ch < 8) ? static_cast<uint8_t>(1 << ch) : 0;
  const bool has_raw = raw_pkt && raw_len > 0 && raw_len <= 64;
  bool is_new_entry = false;
  {
    CriticalSectionLocker lock(&_mux);

    PollingTargetEntry *hit = nullptr;
    for (size_t i = 0; i < _count; ++i) {
      if (entryMatches(_entries[i], dev_id, sub1, sub2, raw_pkt, raw_len)) {
        hit = &_entries[i];
        break;
      }
    }

    PollingTargetEntry *e = hit;
    if (hit) {
      if (hit->last_requested_ms > 0 && now > hit->last_requested_ms) {
        const uint32_t delta = now - hit->last_requested_ms;
        if (delta >= 100 && delta <= 10000) {
          hit->last_interval_ms = hit->last_interval_ms
                                      ? (hit->last_interval_ms * 3 + delta) / 4
                                      : delta;
        }
      }
      if (hit->hit_count < 65535)
        hit->hit_count++;
      if (hit->dev_id == 0 && (dev_id || sub1 || sub2)) {
        hit->dev_id = dev_id;
        hit->sub1 = sub1;
        hit->sub2 = sub2;
      }
    } else if (_count < MAX_TARGETS) {
      e = &_entries[_count++];
      *e = PollingTargetEntry{}; // 이전 슬롯의 ACK/잔여값 제거
      e->dev_id = dev_id;
      e->sub1 = sub1;
      e->sub2 = sub2;
      e->hit_count = 1;
      is_new_entry = true;
    }

    if (e) {
      e->last_requested_ms = now;
      e->source_channels |= ch_bit;
      e->is_active = true;
      e->is_verified = true;
      if (has_raw) {
        e->raw_query_len = static_cast<uint8_t>(raw_len);
        memcpy(e->raw_query_data.data(), raw_pkt, raw_len);
      }
    }
  }

  if (is_new_entry) {
    g_warm_cache_dirty.store(true, std::memory_order_release);
    g_warm_cache_dirty_ms.store(now, std::memory_order_release);
  }
}

void PollingTargetRegistry::updateResponse(const uint8_t *q, size_t ql,
                                           const uint8_t *a, size_t al) {
  if (!q || !ql || !a || !al)
    return;
  CriticalSectionLocker lock(&_mux);
  for (size_t i = 0; i < _count; ++i) {
    PollingTargetEntry &e = _entries[i];
    if (e.is_active && e.raw_query_len == ql &&
        memcmp(e.raw_query_data.data(), q, ql) == 0) {
      e.raw_ack_len = std::min<uint8_t>(al, 64);
      memcpy(e.raw_ack_data.data(), a, e.raw_ack_len);
      e.is_verified = true;
      return;
    }
  }
}

void PollingTargetRegistry::reindexWithOffsets(uint8_t dev_off, uint8_t s1_off,
                                               uint8_t s2_off) {
  CriticalSectionLocker lock(&_mux);
  auto at = [](const PollingTargetEntry &e, uint8_t off) -> uint8_t {
    return (off > 0 && e.raw_query_len > off) ? e.raw_query_data[off] : 0;
  };
  for (size_t i = 0; i < _count; ++i) {
    PollingTargetEntry &e = _entries[i];
    if (!(e.source_channels & kWallpadChMask))
      continue; // CH5 등은 재계산 제외
    if (e.raw_query_len > dev_off)
      e.dev_id = e.raw_query_data[dev_off];
    e.sub1 = at(e, s1_off);
    e.sub2 = at(e, s2_off);
  }
}

void PollingTargetRegistry::sweepExpired(uint32_t stale_timeout_ms) {
  const uint32_t now = millis();
  CriticalSectionLocker lock(&_mux);
  PollingTargetEntry *first = &_entries[0];
  for (size_t i = 0; i < _count; ++i) {
    if (now - first[i].last_requested_ms > stale_timeout_ms)
      first[i].is_active = false;
  }
  PollingTargetEntry *new_end =
      std::remove_if(first, first + _count, [&](const PollingTargetEntry &e) {
        return now - e.last_requested_ms > EXPIRED_TARGET_EVICTION_TIMEOUT_MS;
      });
  _count = static_cast<size_t>(new_end - first);
}

size_t PollingTargetRegistry::getActiveTargets(PollingTargetEntry *out,
                                               size_t max_count) {
  if (!out || !max_count)
    return 0;
  CriticalSectionLocker lock(&_mux);
  size_t w = 0;
  for (size_t i = 0; i < _count && w < max_count; ++i)
    if (_entries[i].is_active)
      out[w++] = _entries[i];
  return w;
}

size_t PollingTargetRegistry::getActiveCandidates(PollingCandidate *out,
                                                  size_t max_count) {
  if (!out || !max_count)
    return 0;
  CriticalSectionLocker lock(&_mux);
  size_t w = 0;
  for (size_t i = 0; i < _count && w < max_count; ++i) {
    const PollingTargetEntry &e = _entries[i];
    if (!e.is_active)
      continue;
    out[w].dev_id = e.dev_id;
    out[w].sub1 = e.sub1;
    out[w].sub2 = e.sub2;
    out[w].source_channels = e.source_channels;
    out[w].raw_ack_len = e.raw_ack_len;
    out[w].raw_query_len = e.raw_query_len;
    out[w].entry_idx = static_cast<uint8_t>(i);
    ++w;
  }
  return w;
}

bool PollingTargetRegistry::getQueryData(uint8_t idx, const uint8_t *&out_data,
                                         uint8_t &out_len) const {
  CriticalSectionLocker lock(&_mux);
  if (idx >= _count || !_entries[idx].is_active) {
    out_data = nullptr;
    out_len = 0;
    return false;
  }
  out_len = _entries[idx].raw_query_len;
  out_data = _entries[idx].raw_query_data.data();
  return true;
}

size_t PollingTargetRegistry::activeCount() const {
  CriticalSectionLocker lock(&_mux);
  return countIf(&_entries[0], _count,
                 [](const PollingTargetEntry &e) { return e.is_active; });
}
size_t PollingTargetRegistry::totalCount() const {
  CriticalSectionLocker lock(&_mux);
  return _count;
}
size_t PollingTargetRegistry::ackedCount() const {
  CriticalSectionLocker lock(&_mux);
  return countIf(&_entries[0], _count, [](const PollingTargetEntry &e) {
    return e.is_active && e.raw_ack_len > 0;
  });
}
size_t PollingTargetRegistry::verifiedCount() const {
  CriticalSectionLocker lock(&_mux);
  return countIf(&_entries[0], _count, [](const PollingTargetEntry &e) {
    return e.is_active && e.is_verified;
  });
}

bool PollingTargetRegistry::getEntry(size_t index,
                                     PollingTargetEntry &out) const {
  CriticalSectionLocker lock(&_mux);
  if (index >= _count)
    return false;
  out = _entries[index];
  return true;
}

void PollingTargetRegistry::resetHits() {
  CriticalSectionLocker lock(&_mux);
  for (size_t i = 0; i < _count; ++i)
    _entries[i].hit_count = 0;
}

void PollingTargetRegistry::clear() {
  CriticalSectionLocker lock(&_mux);
  _count = 0;
}

void PollingTargetRegistry::loadFromWarmCache(const RtcWarmCacheEntry *entries,
                                              size_t count, uint32_t now_ms) {
  if (!entries || !count)
    return;
  CriticalSectionLocker lock(&_mux);
  size_t loaded = 0;
  for (size_t i = 0; i < count && loaded < MAX_TARGETS; ++i) {
    const RtcWarmCacheEntry &w = entries[i];
    if (w.dev_id == 0 || w.dev_id == 0x2A)
      continue;
    if (w.source_channels != 0 && !(w.source_channels & kWallpadChMask))
      continue; // CH2/CH3 아님

    PollingTargetEntry &e = _entries[loaded++];
    e = PollingTargetEntry{};
    e.dev_id = w.dev_id;
    e.sub1 = w.sub1;
    e.sub2 = w.sub2;
    e.source_channels = w.source_channels & kWallpadChMask;
    e.raw_query_len = std::min<uint8_t>(w.raw_len, 64);
    if (e.raw_query_len)
      memcpy(e.raw_query_data.data(), w.raw_query, e.raw_query_len);
    e.last_requested_ms = now_ms;
    e.last_interval_ms = 1000;
    e.is_active = true;
    e.is_verified = false; // 실제 버스 확인 전까지 미검증
    e.restored_ms = now_ms;
  }
  _count = loaded;
}

size_t PollingTargetRegistry::getWarmCacheEntries(RtcWarmCacheEntry *out,
                                                  size_t max_count) const {
  if (!out || !max_count)
    return 0;
  CriticalSectionLocker lock(&_mux);
  size_t w = 0;
  for (size_t i = 0; i < _count && w < max_count; ++i) {
    const PollingTargetEntry &e = _entries[i];
    if (!e.is_active || !(e.source_channels & kWallpadChMask) ||
        e.dev_id == 0x2A)
      continue;
    RtcWarmCacheEntry &o = out[w++];
    o.dev_id = e.dev_id;
    o.sub1 = e.sub1;
    o.sub2 = e.sub2;
    o.source_channels = e.source_channels & kWallpadChMask;
    const size_t n = std::min<size_t>(e.raw_query_len, sizeof(o.raw_query));
    o.raw_len = static_cast<uint8_t>(n);
    memset(o.raw_query, 0, sizeof(o.raw_query));
    if (n)
      memcpy(o.raw_query, e.raw_query_data.data(), n);
  }
  return w;
}

void PollingTargetRegistry::markVerified(uint8_t dev_id, uint8_t sub1,
                                         uint8_t sub2) {
  CriticalSectionLocker lock(&_mux);
  for (size_t i = 0; i < _count; ++i) {
    if (_entries[i].dev_id == dev_id && _entries[i].sub1 == sub1 &&
        _entries[i].sub2 == sub2) {
      _entries[i].is_verified = true;
      return;
    }
  }
}

// ============================================================================
// ProfileRepository

