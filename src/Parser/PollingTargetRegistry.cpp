#include "WallpadParser.h"
#include "Common.h"
#include <cstring>

namespace {
constexpr uint32_t EXPIRED_TARGET_EVICTION_TIMEOUT_MS = 600000;
} // namespace

PollingTargetRegistry g_polling_targets;

void PollingTargetRegistry::registerOrTouch(uint8_t ch, uint8_t dev_id,
                                            uint8_t sub1, uint8_t sub2,
                                            const uint8_t *raw_pkt,
                                            size_t raw_len) {
  // CH2 and CH3 (Wallpad/App) and CH5 (EW11 sniffing) are allowed target sources
  if (ch != 2 && ch != 3 && ch != 5) {
    return;
  }

  // 0x2A (신발장 서브 패널 / 원격검침)는 폴링 대상이 아니므로 타겟 레지스트리 등록 원천 차단
  if (dev_id == 0x2A) {
    return;
  }

  uint32_t now = millis();
  bool is_new_entry = false;
  {
    CriticalSectionLocker lock(&_mux);

    for (size_t i = 0; i < _count; ++i) {
      bool match = false;
      if (dev_id != 0 || sub1 != 0 || sub2 != 0) {
        match = (_entries[i].dev_id == dev_id && _entries[i].sub1 == sub1 &&
                 _entries[i].sub2 == sub2);
      }
      if (!match && raw_pkt && raw_len > 0 && _entries[i].raw_query_len == raw_len) {
        match = (memcmp(_entries[i].raw_query_data.data(), raw_pkt, raw_len) == 0);
      }
      if (match) {
        if (_entries[i].last_requested_ms > 0 && now > _entries[i].last_requested_ms) {
          uint32_t delta = now - _entries[i].last_requested_ms;
          if (delta >= 100 && delta <= 10000) {
            if (_entries[i].last_interval_ms == 0) {
              _entries[i].last_interval_ms = delta;
            } else {
              _entries[i].last_interval_ms = (_entries[i].last_interval_ms * 3 + delta) / 4;
            }
          }
        }
        _entries[i].last_requested_ms = now;
        if (ch < 8)
          _entries[i].source_channels |= (1 << ch);
        if (_entries[i].hit_count < 65535)
          _entries[i].hit_count++;
        _entries[i].is_active = true;
        _entries[i].is_verified = true;
        if (raw_pkt && raw_len > 0 && raw_len <= 64) {
          _entries[i].raw_query_len = static_cast<uint8_t>(raw_len);
          memcpy(_entries[i].raw_query_data.data(), raw_pkt, raw_len);
        }
        return;
      }
    }

    if (_count < MAX_TARGETS) {
      _entries[_count].dev_id = dev_id;
      _entries[_count].sub1 = sub1;
      _entries[_count].sub2 = sub2;
      _entries[_count].last_requested_ms = now;
      _entries[_count].last_interval_ms = 0;
      _entries[_count].source_channels = (ch < 8) ? (1 << ch) : 0;
      _entries[_count].hit_count = 1;
      _entries[_count].is_active = true;
      _entries[_count].is_verified = true;
      _entries[_count].restored_ms = 0;
      if (raw_pkt && raw_len > 0 && raw_len <= 64) {
        _entries[_count].raw_query_len = static_cast<uint8_t>(raw_len);
        memcpy(_entries[_count].raw_query_data.data(), raw_pkt, raw_len);
      } else {
        _entries[_count].raw_query_len = 0;
      }
      _count++;
      is_new_entry = true;
    }
  }

  if (is_new_entry) {
    g_warm_cache_dirty.store(true, std::memory_order_release);
    g_warm_cache_dirty_ms.store(now, std::memory_order_release);
  }
}

void PollingTargetRegistry::updateResponse(const uint8_t *query_pkt, size_t query_len,
                                           const uint8_t *ack_pkt, size_t ack_len) {
  if (!query_pkt || query_len == 0 || !ack_pkt || ack_len == 0)
    return;
  CriticalSectionLocker lock(&_mux);
  for (size_t i = 0; i < _count; ++i) {
    if (_entries[i].is_active && _entries[i].raw_query_len == query_len &&
        memcmp(_entries[i].raw_query_data.data(), query_pkt, query_len) == 0) {
      _entries[i].raw_ack_len = std::min<uint8_t>(ack_len, 64);
      memcpy(_entries[i].raw_ack_data.data(), ack_pkt, _entries[i].raw_ack_len);
      _entries[i].is_verified = true;
      return;
    }
  }
}

void PollingTargetRegistry::reindexWithOffsets(uint8_t dev_id_offset, uint8_t sub1_offset,
                                               uint8_t sub2_offset) {
  CriticalSectionLocker lock(&_mux);
  for (size_t i = 0; i < _count; ++i) {
    if (_entries[i].raw_query_len > dev_id_offset) {
      _entries[i].dev_id = _entries[i].raw_query_data[dev_id_offset];
    }
    if (sub1_offset > 0 && _entries[i].raw_query_len > sub1_offset) {
      _entries[i].sub1 = _entries[i].raw_query_data[sub1_offset];
    } else {
      _entries[i].sub1 = 0;
    }
    if (sub2_offset > 0 && _entries[i].raw_query_len > sub2_offset) {
      _entries[i].sub2 = _entries[i].raw_query_data[sub2_offset];
    } else {
      _entries[i].sub2 = 0;
    }
  }
}

void PollingTargetRegistry::sweepExpired(uint32_t stale_timeout_ms) {
  uint32_t now = millis();
  CriticalSectionLocker lock(&_mux);
  size_t write_idx = 0;
  for (size_t i = 0; i < _count; ++i) {
    uint32_t elapsed = now - _entries[i].last_requested_ms;
    if (elapsed > stale_timeout_ms) {
      _entries[i].is_active = false;
    }
    if (elapsed <= EXPIRED_TARGET_EVICTION_TIMEOUT_MS) {
      if (write_idx != i) {
        _entries[write_idx] = _entries[i];
      }
      write_idx++;
    }
  }
  _count = write_idx;
}

size_t PollingTargetRegistry::getActiveTargets(PollingTargetEntry *out_targets,
                                              size_t max_count) {
  if (!out_targets || max_count == 0)
    return 0;
  CriticalSectionLocker lock(&_mux);
  size_t written = 0;
  for (size_t i = 0; i < _count && written < max_count; ++i) {
    if (_entries[i].is_active) {
      out_targets[written++] = _entries[i];
    }
  }
  return written;
}

size_t PollingTargetRegistry::activeCount() const {
  CriticalSectionLocker lock(&_mux);
  size_t active = 0;
  for (size_t i = 0; i < _count; ++i) {
    if (_entries[i].is_active)
      active++;
  }
  return active;
}

size_t PollingTargetRegistry::totalCount() const {
  CriticalSectionLocker lock(&_mux);
  return _count;
}

size_t PollingTargetRegistry::ackedCount() const {
  CriticalSectionLocker lock(&_mux);
  size_t acked = 0;
  for (size_t i = 0; i < _count; ++i) {
    if (_entries[i].is_active && _entries[i].raw_ack_len > 0)
      acked++;
  }
  return acked;
}

bool PollingTargetRegistry::getEntry(size_t index, PollingTargetEntry &out) const {
  CriticalSectionLocker lock(&_mux);
  if (index >= _count)
    return false;
  out = _entries[index];
  return true;
}

void PollingTargetRegistry::resetHits() {
  CriticalSectionLocker lock(&_mux);
  for (size_t i = 0; i < _count; ++i) {
    _entries[i].hit_count = 0;
  }
}

void PollingTargetRegistry::clear() {
  CriticalSectionLocker lock(&_mux);
  _count = 0;
}

void PollingTargetRegistry::loadFromWarmCache(const RtcWarmCacheEntry *entries, size_t count, uint32_t now_ms) {
  if (!entries || count == 0)
    return;
  CriticalSectionLocker lock(&_mux);
  size_t loaded = 0;
  constexpr uint8_t CH23_MASK = (1 << 2) | (1 << 3);

  for (size_t i = 0; i < count && loaded < MAX_TARGETS; ++i) {
    // Exclude invalid entries, CH5-only entries, or EW11/remote dev_ids (0x2A)
    if (entries[i].dev_id == 0 || entries[i].dev_id == 0x2A) continue;
    if (entries[i].source_channels != 0 && (entries[i].source_channels & CH23_MASK) == 0) {
      continue; // Not from CH2 or CH3
    }

    _entries[loaded].dev_id = entries[i].dev_id;
    _entries[loaded].sub1 = entries[i].sub1;
    _entries[loaded].sub2 = entries[i].sub2;
    _entries[loaded].source_channels = entries[i].source_channels & CH23_MASK;
    _entries[loaded].raw_query_len = std::min<uint8_t>(entries[i].raw_len, 64);
    if (_entries[loaded].raw_query_len > 0) {
      memcpy(_entries[loaded].raw_query_data.data(), entries[i].raw_query, _entries[loaded].raw_query_len);
    }
    _entries[loaded].last_requested_ms = now_ms;
    _entries[loaded].last_interval_ms = 1000;
    _entries[loaded].hit_count = 0;
    _entries[loaded].is_active = true;
    _entries[loaded].is_verified = false; // Initially unverified until live bus confirmation
    _entries[loaded].restored_ms = now_ms;
    loaded++;
  }
  _count = loaded;
}

size_t PollingTargetRegistry::getWarmCacheEntries(RtcWarmCacheEntry *out_entries, size_t max_count) const {
  if (!out_entries || max_count == 0)
    return 0;
  CriticalSectionLocker lock(&_mux);
  size_t written = 0;
  constexpr uint8_t CH23_MASK = (1 << 2) | (1 << 3);

  for (size_t i = 0; i < _count && written < max_count; ++i) {
    if (_entries[i].is_active && (_entries[i].source_channels & CH23_MASK) != 0 && _entries[i].dev_id != 0x2A) {
      out_entries[written].dev_id = _entries[i].dev_id;
      out_entries[written].sub1 = _entries[i].sub1;
      out_entries[written].sub2 = _entries[i].sub2;
      out_entries[written].source_channels = _entries[i].source_channels & CH23_MASK;
      size_t copy_sz = std::min<size_t>(_entries[i].raw_query_len, sizeof(out_entries[written].raw_query));
      out_entries[written].raw_len = static_cast<uint8_t>(copy_sz);
      if (copy_sz > 0) {
        memcpy(out_entries[written].raw_query, _entries[i].raw_query_data.data(), copy_sz);
      } else {
        memset(out_entries[written].raw_query, 0, sizeof(out_entries[written].raw_query));
      }
      written++;
    }
  }
  return written;
}

void PollingTargetRegistry::markVerified(uint8_t dev_id, uint8_t sub1, uint8_t sub2) {
  CriticalSectionLocker lock(&_mux);
  for (size_t i = 0; i < _count; ++i) {
    if (_entries[i].dev_id == dev_id && _entries[i].sub1 == sub1 && _entries[i].sub2 == sub2) {
      _entries[i].is_verified = true;
      return;
    }
  }
}

size_t PollingTargetRegistry::verifiedCount() const {
  CriticalSectionLocker lock(&_mux);
  size_t v = 0;
  for (size_t i = 0; i < _count; ++i) {
    if (_entries[i].is_active && _entries[i].is_verified)
      v++;
  }
  return v;
}

