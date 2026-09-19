#include "WallpadParser.h"
#include "Common.h"
#include <cstring>

PollingTargetRegistry g_polling_targets;

void PollingTargetRegistry::registerOrTouch(uint8_t ch, uint8_t dev_id,
                                            uint8_t sub1, uint8_t sub2,
                                            const uint8_t *raw_pkt,
                                            size_t raw_len) {
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
      _entries[i].raw_ack_len = static_cast<uint8_t>(std::min<size_t>(ack_len, 64));
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

void PollingTargetRegistry::sweepExpired(uint32_t ttl_ms) {
  uint32_t now = millis();
  CriticalSectionLocker lock(&_mux);

  bool list_changed = false;
  size_t write_idx = 0;

  for (size_t i = 0; i < _count; ++i) {
    if (_entries[i].is_active) {
      if (!_entries[i].is_verified &&
          TimeUtils::isElapsed(_entries[i].restored_ms, Config::Timing::WARM_CACHE_VERIFY_TIMEOUT_MS)) {
        _entries[i].is_active = false;
      } else if (TimeUtils::isElapsed(_entries[i].last_requested_ms, ttl_ms)) {
        _entries[i].is_active = false;
      }
    }

    if (!_entries[i].is_active &&
        TimeUtils::isElapsed(_entries[i].last_requested_ms, Config::Timing::EXPIRED_TARGET_EVICTION_TIMEOUT_MS)) {
      list_changed = true;
      continue;
    }

    if (write_idx != i) {
      _entries[write_idx] = _entries[i];
    }
    write_idx++;
  }

  if (write_idx != _count) {
    _count = write_idx;
  }

  if (list_changed) {
    g_warm_cache_dirty.store(true, std::memory_order_release);
    g_warm_cache_dirty_ms.store(now, std::memory_order_release);
  }
}

size_t PollingTargetRegistry::getActiveTargets(PollingTargetEntry *out_buf,
                                               size_t max_count) {
  if (!out_buf || max_count == 0)
    return 0;
  CriticalSectionLocker lock(&_mux);
  size_t written = 0;
  for (size_t i = 0; i < _count && written < max_count; ++i) {
    if (_entries[i].is_active) {
      out_buf[written++] = _entries[i];
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
    if (_entries[i].is_active && _entries[i].raw_ack_len >= 4) {
      acked++;
    }
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
  size_t to_load = std::min(count, MAX_TARGETS);
  _count = to_load;
  for (size_t i = 0; i < to_load; ++i) {
    _entries[i].dev_id = entries[i].dev_id;
    _entries[i].sub1 = entries[i].sub1;
    _entries[i].sub2 = entries[i].sub2;
    _entries[i].source_channels = entries[i].source_channels;
    _entries[i].raw_query_len = std::min<uint8_t>(entries[i].raw_len, 64);
    if (_entries[i].raw_query_len > 0) {
      memcpy(_entries[i].raw_query_data.data(), entries[i].raw_query, _entries[i].raw_query_len);
    }
    _entries[i].last_requested_ms = now_ms;
    _entries[i].last_interval_ms = 1000;
    _entries[i].hit_count = 0;
    _entries[i].is_active = true;
    _entries[i].is_verified = false; // Initially unverified until live bus confirmation
    _entries[i].restored_ms = now_ms;
  }
}

size_t PollingTargetRegistry::getWarmCacheEntries(RtcWarmCacheEntry *out_entries, size_t max_count) const {
  if (!out_entries || max_count == 0)
    return 0;
  CriticalSectionLocker lock(&_mux);
  size_t written = 0;
  for (size_t i = 0; i < _count && written < max_count; ++i) {
    if (_entries[i].is_active) {
      out_entries[written].dev_id = _entries[i].dev_id;
      out_entries[written].sub1 = _entries[i].sub1;
      out_entries[written].sub2 = _entries[i].sub2;
      out_entries[written].source_channels = _entries[i].source_channels;
      out_entries[written].raw_len = _entries[i].raw_query_len;
      if (_entries[i].raw_query_len > 0) {
        memcpy(out_entries[written].raw_query, _entries[i].raw_query_data.data(), _entries[i].raw_query_len);
      } else {
        memset(out_entries[written].raw_query, 0, 64);
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

