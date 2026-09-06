#include "WallpadParser.h"
#include "Common.h"
#include "TelnetCli.h"
#include <Preferences.h>
#include <algorithm>
#include <vector>
#include <set>
#include <map>

// ============================================================================
// 1ST TIER CACHE: POLLING TARGET REGISTRY
// ============================================================================

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

// ============================================================================
// UNIVERSAL AUTO-PROBING PROTOCOL ENGINE
// ============================================================================

AutoProbingEngine g_auto_probing_engine;

AutoProbingEngine::AutoProbingEngine() {
  reset();
}

const char *AutoProbingEngine::getAlgoName(ChecksumAlgo algo) {
  switch (algo) {
  case ChecksumAlgo::XOR_ALL:        return "XOR [0..N-3]";
  case ChecksumAlgo::XOR_NO_STX:     return "XOR [1..N-3]";
  case ChecksumAlgo::SUM_ALL:        return "SUM [0..N-3]";
  case ChecksumAlgo::SUM_NO_STX:     return "SUM [1..N-3]";
  case ChecksumAlgo::TWOS_COMPLEMENT:return "2's Complement [1..N-3]";
  case ChecksumAlgo::ONES_COMPLEMENT:return "1's Complement [0..N-3]";
  case ChecksumAlgo::CRC8_MAXIM:     return "CRC-8 (Maxim/0x31)";
  case ChecksumAlgo::NONE:           return "None (Pure Framing)";
  default:                           return "Learning...";
  }
}

uint8_t AutoProbingEngine::calculateChecksum(ChecksumAlgo algo, const uint8_t *data, size_t len) const {
  if (!data || len < 3)
    return 0;
  switch (algo) {
  case ChecksumAlgo::XOR_ALL: {
    uint8_t cs = 0;
    for (size_t i = 0; i < len - 2; ++i)
      cs ^= data[i];
    return cs;
  }
  case ChecksumAlgo::XOR_NO_STX: {
    uint8_t cs = 0;
    for (size_t i = 1; i < len - 2; ++i)
      cs ^= data[i];
    return cs;
  }
  case ChecksumAlgo::SUM_ALL: {
    uint8_t cs = 0;
    for (size_t i = 0; i < len - 2; ++i)
      cs += data[i];
    return cs;
  }
  case ChecksumAlgo::SUM_NO_STX: {
    uint8_t cs = 0;
    for (size_t i = 1; i < len - 2; ++i)
      cs += data[i];
    return cs;
  }
  case ChecksumAlgo::TWOS_COMPLEMENT: {
    uint8_t sum = 0;
    for (size_t i = 1; i < len - 2; ++i)
      sum += data[i];
    return (0x100 - sum) & 0xFF;
  }
  case ChecksumAlgo::ONES_COMPLEMENT: {
    uint8_t sum = 0;
    for (size_t i = 0; i < len - 2; ++i)
      sum += data[i];
    return (~sum) & 0xFF;
  }
  case ChecksumAlgo::CRC8_MAXIM: {
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len - 2; ++i) {
      crc ^= data[i];
      for (int b = 0; b < 8; ++b) {
        if (crc & 0x80)
          crc = (crc << 1) ^ 0x31;
        else
          crc <<= 1;
      }
    }
    return crc;
  }
  case ChecksumAlgo::NONE:
  default:
    return 0;
  }
}

void AutoProbingEngine::initFromNvs() {
  VendorProfileDescriptor prof;
  if (ProfileRepository::getActiveProfile(prof)) {
    CriticalSectionLocker lock(&_mux);
    _desc.stx = prof.stx;
    _desc.etx = prof.etx;
    _desc.min_len = prof.min_len;
    _desc.max_len = prof.max_len;
    _desc.checksum_algo = prof.cs_algo;
    _desc.opcode_offset = prof.opcode_offset;
    _desc.query_opcode = prof.query_op;
    _desc.control_opcode = prof.ctrl_op;
    _desc.ack_opcode = prof.ack_op;
    _desc.control_seen = (prof.ctrl_op != 0);

    // ★ 오프셋 및 주소 모드 복원
    if (prof.dev_id_offset > 0 || prof.sub1_offset > 0 || prof.sub2_offset > 0) {
      _desc.dev_id_offset = prof.dev_id_offset;
      _desc.sub1_offset = prof.sub1_offset;
      _desc.sub2_offset = prof.sub2_offset;
      _desc.is_swapped_addr = (prof.is_swapped_addr != 0);
      _desc.gw_addr_offset = (prof.gw_addr_offset > 0) ? prof.gw_addr_offset : 2;
      _desc.gw_addr = (prof.gw_addr != 0) ? prof.gw_addr : 0x01;
      _desc.learned_query_len = (prof.learned_query_len >= 3) ? prof.learned_query_len : 11;
      _desc.len_offset = prof.len_offset;
      _desc.has_len_field = (prof.has_len_field != 0);
      _desc.seq_offset = prof.seq_offset;
      _desc.has_seq_counter = (prof.seq_offset != 0xFF);
      _desc.ack_flag_offset = prof.ack_flag_offset;
      _desc.ctrl_len_cnt = prof.ctrl_len_cnt;
      memcpy(_desc.learned_ctrl_lens, prof.learned_ctrl_lens, sizeof(_desc.learned_ctrl_lens));
      if (_desc.control_seen && _desc.ctrl_len_cnt == 0) {
        _desc.learned_ctrl_lens[0] = _desc.learned_query_len;
        _desc.ctrl_len_cnt = 1;
      }
      _desc.offsets_locked = true;
      _desc.opcodes_locked = true;
    }

    _desc.is_locked = true;
    _consecutive_mismatches = 0;
    snprintf(_desc.description, sizeof(_desc.description),
             "Restored: %s (STX 0x%02X ETX 0x%02X / %s)",
             prof.name, prof.stx, prof.etx, getAlgoName(prof.cs_algo));
  }
}

void AutoProbingEngine::feedFrame(span<const uint8_t> raw_frame) {
  if (raw_frame.size() < 3 || raw_frame.size() > 64)
    return;

  bool should_sync = false;
  AutoProbeDescriptor desc_to_sync;

  {
    CriticalSectionLocker lock(&_mux);
    _desc.tested_packets++;

    uint8_t stx = raw_frame[0];
    uint8_t etx = raw_frame[raw_frame.size() - 1];
    uint8_t actual_cs = raw_frame[raw_frame.size() - 2];

    _stx_counts[stx]++;
    _etx_counts[etx]++;

    if (_desc.is_locked) {
      bool ok = false;
      if (stx == _desc.stx && etx == _desc.etx) {
        if (_desc.checksum_algo == ChecksumAlgo::NONE ||
            calculateChecksum(_desc.checksum_algo, raw_frame.data(), raw_frame.size()) == actual_cs) {
          _desc.matched_packets++;
          _consecutive_mismatches = 0;
          ok = true;
        }
      }
      if (!ok) {
        _consecutive_mismatches++;
        if (_consecutive_mismatches >= 5) {
          // Auto-unlock! Physical bus does not match restored/locked state!
          _desc.is_locked = false;
          _desc.opcodes_locked = false;
          _desc.matched_packets = 0;
          _consecutive_matches = 0;
          _consecutive_mismatches = 0;
          _candidate_algo = ChecksumAlgo::UNKNOWN;
          memset(_stx_counts, 0, sizeof(_stx_counts));
          memset(_etx_counts, 0, sizeof(_etx_counts));
          memset(_algo_matches, 0, sizeof(_algo_matches));
          memset(_diff_idx_counts, 0, sizeof(_diff_idx_counts));
          snprintf(_desc.description, sizeof(_desc.description),
                   "Auto-unlocked (Mismatch detected, re-probing...)");
        }
      }
      return; // ★ lock이 여기서 소멸 → 스핀락 해제 후 return
    }

    // Dominant STX / ETX
    uint8_t best_stx = 0xF7, best_etx = 0xEE;
    uint16_t max_stx_cnt = 0, max_etx_cnt = 0;
    for (int i = 0; i < 256; ++i) {
      if (_stx_counts[i] > max_stx_cnt) {
        max_stx_cnt = _stx_counts[i];
        best_stx = static_cast<uint8_t>(i);
      }
      if (_etx_counts[i] > max_etx_cnt) {
        max_etx_cnt = _etx_counts[i];
        best_etx = static_cast<uint8_t>(i);
      }
    }

    // Evaluate candidate checksum algorithms 1..7
    ChecksumAlgo matched_this_frame = ChecksumAlgo::UNKNOWN;
    for (uint8_t a = 1; a <= 7; ++a) {
      ChecksumAlgo algo = static_cast<ChecksumAlgo>(a);
      if (calculateChecksum(algo, raw_frame.data(), raw_frame.size()) == actual_cs) {
        _algo_matches[a]++;
        matched_this_frame = algo;
        break;
      }
    }

    if (matched_this_frame != ChecksumAlgo::UNKNOWN) {
      if (matched_this_frame == _candidate_algo) {
        _consecutive_matches++;
        if (_consecutive_matches >= 10 && max_stx_cnt >= 5 && max_etx_cnt >= 5) {
          _desc.is_locked = true;
          _desc.stx = best_stx;
          _desc.etx = best_etx;
          _desc.min_len = 3;
          _desc.max_len = 64;
          _desc.checksum_algo = matched_this_frame;
          _desc.matched_packets = _desc.tested_packets;
          _consecutive_mismatches = 0;
          snprintf(_desc.description, sizeof(_desc.description),
                   "Locked: STX 0x%02X ETX 0x%02X (%s)", best_stx, best_etx,
                   getAlgoName(matched_this_frame));
          should_sync = true;
          desc_to_sync = _desc;
        }
      } else {
        _candidate_algo = matched_this_frame;
        _consecutive_matches = 1;
      }
    }
  } // ★ CriticalSectionLocker 소멸 → 스핀락 완전 해제

  // ★ 스핀락 완전 해제 후 안전하게 Flash NVS 저장
  if (should_sync) {
    ProfileRepository::syncAutoProfileToNvs(desc_to_sync);
  }
}

void AutoProbingEngine::feedOpcodePair(span<const uint8_t> req, span<const uint8_t> ack) {
  if (req.size() < 5 || ack.size() < 5)
    return;

  bool should_sync = false;
  AutoProbeDescriptor desc_to_sync;

  {
    CriticalSectionLocker lock(&_mux);
    if (_desc.opcodes_locked)
      return;

    size_t min_len = std::min(req.size(), ack.size());
    for (size_t i = 1; i < min_len - 1; ++i) {
      if (req[i] != ack[i]) {
        _diff_idx_counts[i < 16 ? i : 15]++;
        if (_diff_idx_counts[i < 16 ? i : 15] >= 10) {
          _desc.opcode_offset = static_cast<uint8_t>(i);
          _desc.query_opcode = req[i];
          _desc.ack_opcode = ack[i];
          _desc.opcodes_locked = true;
          if (_desc.is_locked) {
            should_sync = true;
            desc_to_sync = _desc;
          }
          break;
        }
      }
    }
  }

  if (should_sync) {
    ProfileRepository::syncAutoProfileToNvs(desc_to_sync);
  }
}

void AutoProbingEngine::feedControlPair(span<const uint8_t> ctrl_req, span<const uint8_t> ack_res) {
  if (ctrl_req.size() < 5 || ack_res.size() < 5)
    return;

  bool should_sync = false;
  AutoProbeDescriptor desc_to_sync;

  {
    CriticalSectionLocker lock(&_mux);
    if (_desc.opcode_offset >= ctrl_req.size() - 2 || _desc.opcode_offset >= ack_res.size() - 2)
      return;

    uint8_t ctrl_op = ctrl_req[_desc.opcode_offset];
    uint8_t ack_op = ack_res[_desc.opcode_offset];

    // 1) ctrl_op는 query 및 ack 코드와 달라야 함
    if (ctrl_op == _desc.query_opcode || ctrl_op == _desc.ack_opcode)
      return;

    // 2) ack_res의 opcode는 ack_opcode와 일치해야 함 (or zero if probing)
    if (_desc.ack_opcode != 0 && ack_op != _desc.ack_opcode)
      return;

    // 3) 송신 제어 패킷과 수신 응답 패킷의 장치 ID 매칭 검증
    if (_desc.dev_id_offset < ctrl_req.size() && _desc.dev_id_offset < ack_res.size()) {
      if (ctrl_req[_desc.dev_id_offset] != ack_res[_desc.dev_id_offset])
        return; // 타깃 장치 ID 불일치 시 확정 보류
    }

    uint8_t prev_cnt = _desc.ctrl_len_cnt;
    auto add_ctrl_len = [&](uint8_t len) {
      if (len >= 3 && len <= 64) {
        for (uint8_t i = 0; i < _desc.ctrl_len_cnt; ++i) {
          if (_desc.learned_ctrl_lens[i] == len) return;
        }
        if (_desc.ctrl_len_cnt < 4) {
          _desc.learned_ctrl_lens[_desc.ctrl_len_cnt++] = len;
          std::sort(_desc.learned_ctrl_lens, _desc.learned_ctrl_lens + _desc.ctrl_len_cnt);
        }
      }
    };

    // 4) 제어 패킷 길이 상시 수집
    add_ctrl_len(static_cast<uint8_t>(ctrl_req.size()));
    if (_desc.ctrl_len_cnt > prev_cnt && _desc.is_locked) {
      should_sync = true;
      desc_to_sync = _desc;
    }

    // 5) ★ 3회 연속 폐루프 실물 응답 검증 시 제어 코드 확정!
    if (_candidate_ctrl_op == ctrl_op) {
      _control_matches++;
      if (_control_matches >= 3) {
        _desc.control_opcode = ctrl_op;
        _desc.control_seen = true;
        if (_desc.is_locked) {
          should_sync = true;
          desc_to_sync = _desc;
        }
      }
    } else {
      _candidate_ctrl_op = ctrl_op;
      _control_matches = 1;
    }
  }

  if (should_sync) {
    ProfileRepository::syncAutoProfileToNvs(desc_to_sync);
  }
}

bool AutoProbingEngine::isLocked() const {
  CriticalSectionLocker lock(&_mux);
  return _desc.is_locked;
}

bool AutoProbingEngine::isOffsetsLocked() const {
  CriticalSectionLocker lock(&_mux);
  return _desc.offsets_locked;
}

bool AutoProbingEngine::analyzeCacheMatrix() {
  struct PktPair {
    StaticPacket q;
    StaticPacket r;
  };
  std::vector<PktPair> pairs;
  pairs.reserve(64);

  size_t target_count = g_polling_targets.totalCount();
  for (size_t i = 0; i < target_count; ++i) {
    PollingTargetEntry target;
    if (!g_polling_targets.getEntry(i, target))
      continue;
    if (!target.is_active || target.raw_query_len < 4)
      continue;

    if (target.raw_ack_len >= 4) {
      PktPair pair;
      pair.q.length = target.raw_query_len;
      memcpy(pair.q.data.data(), target.raw_query_data.data(), target.raw_query_len);
      pair.r.length = target.raw_ack_len;
      memcpy(pair.r.data.data(), target.raw_ack_data.data(), target.raw_ack_len);
      pairs.push_back(pair);
    } else {
      const DeviceStateEntry *dev = g_device_repo.find(target.dev_id, target.sub1, target.sub2);
      if (dev && dev->is_online && dev->last_ack_len >= 4) {
        PktPair pair;
        pair.q.length = target.raw_query_len;
        memcpy(pair.q.data.data(), target.raw_query_data.data(), target.raw_query_len);
        pair.r.length = dev->last_ack_len;
        memcpy(pair.r.data.data(), dev->last_ack_data.data(), dev->last_ack_len);
        pairs.push_back(pair);
      }
    }
  }

  if (pairs.size() < 2)
    return false;

  size_t N = pairs.size();
  size_t min_common_len = 256;
  for (const auto &p : pairs) {
    if (p.q.length < min_common_len) min_common_len = p.q.length;
    if (p.r.length < min_common_len) min_common_len = p.r.length;
  }
  if (min_common_len < 4)
    return false;

  // ==========================================================================
  // MASTER 10-STEP DETERMINISTIC ELIMINATION PIPELINE
  // ==========================================================================

  // [Step 2: Length (LEN) Early Detection & Elimination]
  int len_idx = -1;
  for (size_t k = 1; k < min_common_len - 1; ++k) {
    bool exact_match = true;
    for (size_t m = 0; m < N; ++m) {
      if (pairs[m].q.data[k] != pairs[m].q.length || pairs[m].r.data[k] != pairs[m].r.length) {
        exact_match = false;
        break;
      }
    }
    if (exact_match) {
      len_idx = static_cast<int>(k);
      break;
    }
    // Check Payload Length (LEN - Delta)
    for (uint8_t delta : {2, 3, 4, 5}) {
      bool delta_match = true;
      for (size_t m = 0; m < N; ++m) {
        if (pairs[m].q.data[k] != (pairs[m].q.length - delta) ||
            pairs[m].r.data[k] != (pairs[m].r.length - delta)) {
          delta_match = false;
          break;
        }
      }
      if (delta_match) {
        len_idx = static_cast<int>(k);
        break;
      }
    }
    if (len_idx >= 0) break;
  }

  // [Step 3: Opcode (Command) Determination & Elimination]
  int opcode_idx = -1;
  uint8_t learned_q_op = 0, learned_ack_op = 0;
  for (size_t k = 1; k < min_common_len - 1; ++k) {
    if (static_cast<int>(k) == len_idx) continue;
    uint8_t cq = pairs[0].q.data[k];
    uint8_t cr = pairs[0].r.data[k];
    if (cq == cr) continue;
    bool all_match = true;
    for (size_t m = 1; m < N; ++m) {
      if (pairs[m].q.data[k] != cq || pairs[m].r.data[k] != cr) {
        all_match = false;
        break;
      }
    }
    if (all_match) {
      opcode_idx = static_cast<int>(k);
      learned_q_op = cq;
      learned_ack_op = cr;
      break;
    }
  }

  // [Step 4: Dual Address (SA/DA) Swap & Device Type Promotion]
  int swap_i = -1, swap_j = -1;
  int promoted_dev_idx = -1;
  int master_gw_idx = -1;
  for (size_t i = 1; i < min_common_len - 1; ++i) {
    if (static_cast<int>(i) == len_idx || static_cast<int>(i) == opcode_idx) continue;
    for (size_t j = i + 1; j < min_common_len - 1; ++j) {
      if (static_cast<int>(j) == len_idx || static_cast<int>(j) == opcode_idx) continue;
      bool is_cross = true;
      for (size_t m = 0; m < N; ++m) {
        if (pairs[m].q.data[i] != pairs[m].r.data[j] ||
            pairs[m].q.data[j] != pairs[m].r.data[i] ||
            pairs[m].q.data[i] == pairs[m].q.data[j]) {
          is_cross = false;
          break;
        }
      }
      if (is_cross) {
        swap_i = static_cast<int>(i);
        swap_j = static_cast<int>(j);
        std::set<uint8_t> set_i, set_j;
        for (size_t m = 0; m < N; ++m) {
          set_i.insert(pairs[m].q.data[i]);
          set_j.insert(pairs[m].q.data[j]);
        }
        if (set_i.size() == 1 && set_j.size() > 1) {
          master_gw_idx = static_cast<int>(i);
          promoted_dev_idx = static_cast<int>(j);
        } else if (set_j.size() == 1 && set_i.size() > 1) {
          master_gw_idx = static_cast<int>(j);
          promoted_dev_idx = static_cast<int>(i);
        } else {
          master_gw_idx = static_cast<int>(i);
          promoted_dev_idx = static_cast<int>(j);
        }
        break;
      }
    }
    if (swap_i >= 0) break;
  }

  // [Step 5: Sequence Counter Monotonic Increment Detection & Elimination]
  int seq_idx = -1;
  if (N >= 3) {
    for (size_t k = 1; k < min_common_len - 1; ++k) {
      int ik = static_cast<int>(k);
      if (ik == len_idx || ik == opcode_idx || ik == swap_i || ik == swap_j || ik == promoted_dev_idx)
        continue;
      bool is_inc = true;
      for (size_t m = 0; m < N - 1; ++m) {
        uint8_t diff = static_cast<uint8_t>((pairs[m + 1].q.data[k] - pairs[m].q.data[k]) & 0xFF);
        if (diff != 1) {
          is_inc = false;
          break;
        }
      }
      if (is_inc) {
        seq_idx = ik;
        break;
      }
    }
  }

  // [Step 6: Sub-Command / Service ID Detection & Elimination]
  int sub_cmd_idx = -1;
  for (size_t k = 1; k < min_common_len - 1; ++k) {
    int ik = static_cast<int>(k);
    if (ik == len_idx || ik == opcode_idx || ik == swap_i || ik == swap_j || ik == seq_idx || ik == promoted_dev_idx)
      continue;
    bool eq = true;
    for (size_t m = 0; m < N; ++m) {
      if (pairs[m].q.data[k] != pairs[m].r.data[k]) {
        eq = false;
        break;
      }
    }
    if (eq) {
      std::set<uint8_t> vals;
      for (size_t m = 0; m < N; ++m) vals.insert(pairs[m].q.data[k]);
      if (vals.size() == 1) {
        sub_cmd_idx = ik;
        break;
      }
      if (promoted_dev_idx >= 0) {
        std::map<uint8_t, uint8_t> dep_map;
        bool pure_func = true;
        for (size_t m = 0; m < N; ++m) {
          uint8_t dt = pairs[m].q.data[promoted_dev_idx];
          uint8_t sc = pairs[m].q.data[k];
          if (dep_map.count(dt) && dep_map[dt] != sc) {
            pure_func = false;
            break;
          }
          dep_map[dt] = sc;
        }
        if (pure_func && dep_map.size() > 1) {
          sub_cmd_idx = ik;
          break;
        }
      }
    }
  }

  // [Step 7: Device Type & Sub-ID Determination]
  std::vector<size_t> candidate_cols;
  for (size_t k = 1; k < min_common_len - 1; ++k) {
    int ik = static_cast<int>(k);
    if (ik == len_idx || ik == opcode_idx || ik == swap_i || ik == swap_j ||
        ik == seq_idx || ik == sub_cmd_idx || ik == promoted_dev_idx) {
      continue;
    }
    std::set<uint8_t> vals;
    for (size_t m = 0; m < N; ++m) vals.insert(pairs[m].q.data[k]);
    if (vals.size() >= 2) candidate_cols.push_back(k);
  }

  int dev_type_idx = promoted_dev_idx;
  int sub_id_idx = -1;
  size_t min_unique = std::max<size_t>(2, N * 8 / 10);

  if (dev_type_idx >= 0) {
    // Swapped Dual Address: DevType is already promoted from Swap Target!
    for (size_t cand : candidate_cols) {
      std::set<uint16_t> combo_keys;
      for (size_t m = 0; m < N; ++m) {
        uint16_t key = (static_cast<uint16_t>(pairs[m].q.data[dev_type_idx]) << 8) | pairs[m].q.data[cand];
        combo_keys.insert(key);
      }
      if (combo_keys.size() >= min_unique) {
        sub_id_idx = static_cast<int>(cand);
        break;
      }
    }
  } else {
    // Stationary Direct Mode:
    // Filter 1: Length Correlation: Length = f(Candidate)
    for (size_t cand : candidate_cols) {
      std::map<uint8_t, size_t> len_map;
      bool consistent = true;
      for (size_t m = 0; m < N; ++m) {
        uint8_t val = pairs[m].q.data[cand];
        size_t r_len = pairs[m].r.length;
        if (len_map.count(val) && len_map[val] != r_len) {
          consistent = false;
          break;
        }
        len_map[val] = r_len;
      }
      if (consistent && len_map.size() > 1) {
        std::set<size_t> distinct_lens;
        for (auto &kv : len_map) distinct_lens.insert(kv.second);
        if (distinct_lens.size() > 1) {
          dev_type_idx = static_cast<int>(cand);
          break;
        }
      }
    }

    if (dev_type_idx >= 0) {
      for (size_t cand : candidate_cols) {
        if (static_cast<int>(cand) == dev_type_idx) continue;
        std::set<uint16_t> combo_keys;
        for (size_t m = 0; m < N; ++m) {
          uint16_t key = (static_cast<uint16_t>(pairs[m].q.data[dev_type_idx]) << 8) | pairs[m].q.data[cand];
          combo_keys.insert(key);
        }
        if (combo_keys.size() >= min_unique) {
          sub_id_idx = static_cast<int>(cand);
          break;
        }
      }
    } else {
      // Fixed length protocols: cluster by cardinalities
      for (size_t cand1 : candidate_cols) {
        for (size_t cand2 : candidate_cols) {
          if (cand1 == cand2) continue;
          std::set<uint16_t> combo_keys;
          for (size_t m = 0; m < N; ++m) {
            uint16_t key = (static_cast<uint16_t>(pairs[m].q.data[cand1]) << 8) | pairs[m].q.data[cand2];
            combo_keys.insert(key);
          }
          if (combo_keys.size() >= min_unique) {
            std::set<uint8_t> set1, set2;
            for (size_t m = 0; m < N; ++m) {
              set1.insert(pairs[m].q.data[cand1]);
              set2.insert(pairs[m].q.data[cand2]);
            }
            if (set1.size() <= set2.size()) {
              dev_type_idx = static_cast<int>(cand1);
              sub_id_idx = static_cast<int>(cand2);
            } else {
              dev_type_idx = static_cast<int>(cand2);
              sub_id_idx = static_cast<int>(cand1);
            }
            break;
          }
        }
        if (dev_type_idx >= 0) break;
      }
    }
  }

  // [Step 8: ACK / Status Flag Detection & Elimination]
  int ack_flag_idx = -1;
  for (size_t k = 1; k < min_common_len - 1; ++k) {
    int ik = static_cast<int>(k);
    if (ik == len_idx || ik == opcode_idx || ik == swap_i || ik == swap_j ||
        ik == seq_idx || ik == sub_cmd_idx || ik == dev_type_idx || ik == sub_id_idx) {
      continue;
    }
    std::set<uint8_t> r_vals;
    for (size_t m = 0; m < N; ++m) r_vals.insert(pairs[m].r.data[k]);
    if (r_vals.size() == 1 && (*r_vals.begin() == 0x00 || *r_vals.begin() == 0x01)) {
      ack_flag_idx = ik;
      break;
    }
  }

  // [Step 10: Finalize Profile & Update State]
  AutoProbeDescriptor desc_to_sync;
  {
    CriticalSectionLocker lock(&_mux);
    if (opcode_idx >= 0) {
      _desc.opcode_offset = static_cast<uint8_t>(opcode_idx);
      _desc.query_opcode = learned_q_op;
      _desc.ack_opcode = learned_ack_op;
      _desc.opcodes_locked = true;
    }
    if (dev_type_idx >= 0) {
      _desc.dev_id_offset = static_cast<uint8_t>(dev_type_idx);
    }
    if (sub_cmd_idx >= 0) {
      _desc.sub1_offset = static_cast<uint8_t>(sub_cmd_idx);
    }
    if (sub_id_idx >= 0) {
      _desc.sub2_offset = static_cast<uint8_t>(sub_id_idx);
    }
    _desc.len_offset = (len_idx >= 0) ? static_cast<uint8_t>(len_idx) : 0xFF;
    _desc.has_len_field = (len_idx >= 0);
    _desc.seq_offset = (seq_idx >= 0) ? static_cast<uint8_t>(seq_idx) : 0xFF;
    _desc.has_seq_counter = (seq_idx >= 0);
    _desc.ack_flag_offset = (ack_flag_idx >= 0) ? static_cast<uint8_t>(ack_flag_idx) : 0xFF;

    _desc.is_swapped_addr = (swap_i >= 0);
    if (swap_i >= 0 && master_gw_idx >= 0 && promoted_dev_idx >= 0) {
      _desc.gw_addr_offset = static_cast<uint8_t>(master_gw_idx);
      _desc.gw_addr = pairs[0].q.data[master_gw_idx];
    } else if (!_desc.is_swapped_addr && dev_type_idx >= 0) {
      _desc.gw_addr_offset = _desc.dev_id_offset;
    }

    if (min_common_len >= 5 && min_common_len <= 64) {
      _desc.learned_query_len = static_cast<uint8_t>(min_common_len);
    }

    _desc.offsets_locked = (dev_type_idx >= 0 && sub_id_idx >= 0);

    // Calculate Payload Boundary
    int max_hdr = 0;
    if (opcode_idx > max_hdr) max_hdr = opcode_idx;
    if (dev_type_idx > max_hdr) max_hdr = dev_type_idx;
    if (sub_cmd_idx > max_hdr) max_hdr = sub_cmd_idx;
    if (sub_id_idx > max_hdr) max_hdr = sub_id_idx;
    if (swap_i > max_hdr) max_hdr = swap_i;
    if (swap_j > max_hdr) max_hdr = swap_j;
    if (len_idx > max_hdr) max_hdr = len_idx;
    if (seq_idx > max_hdr) max_hdr = seq_idx;
    if (ack_flag_idx > max_hdr) max_hdr = ack_flag_idx;
    _desc.payload_offset = static_cast<uint8_t>(max_hdr + 1);

    snprintf(_desc.description, sizeof(_desc.description),
             "Auto: OP@%u(0x%02X/0x%02X) DEV@%u SUB1@%u SUB2@%u",
             _desc.opcode_offset, _desc.query_opcode, _desc.ack_opcode,
             _desc.dev_id_offset, _desc.sub1_offset, _desc.sub2_offset);
    desc_to_sync = _desc;
  }

  ProfileRepository::syncAutoProfileToNvs(desc_to_sync);

  if (desc_to_sync.offsets_locked) {
    g_polling_targets.reindexWithOffsets(desc_to_sync.dev_id_offset,
                                         desc_to_sync.sub1_offset,
                                         desc_to_sync.sub2_offset);
    for (size_t i = 0; i < target_count; ++i) {
      PollingTargetEntry target;
      if (g_polling_targets.getEntry(i, target) && target.is_active && target.raw_ack_len >= 4) {
        StaticPacket ack_pkt;
        ack_pkt.channel_id = 1;
        ack_pkt.length = target.raw_ack_len;
        memcpy(ack_pkt.data.data(), target.raw_ack_data.data(), target.raw_ack_len);
        g_device_repo.updateFromBus(ack_pkt);
      }
    }
  }

  g_telnet_tracer.trace("[AUTO PROBE] ★ Full-Matrix Cache Analysis Complete! Offsets locked & saved to NVS.\r\n");
  return true;
}

AutoProbeDescriptor AutoProbingEngine::getDescriptor() const {
  CriticalSectionLocker lock(&_mux);
  return _desc;
}

void AutoProbingEngine::reset() {
  CriticalSectionLocker lock(&_mux);
  memset(&_desc, 0, sizeof(_desc));
  _desc.stx = 0xF7;
  _desc.etx = 0xEE;
  _desc.min_len = 3;
  _desc.max_len = 64;
  _desc.checksum_algo = ChecksumAlgo::XOR_ALL;
  _desc.opcode_offset = 4;
  _desc.query_opcode = 0x01;
  _desc.control_opcode = 0x00;
  _desc.ack_opcode = 0x04;
  _desc.opcodes_locked = false;
  _desc.control_seen = false;
  _desc.len_offset = 0xFF;
  _desc.has_len_field = false;
  _desc.seq_offset = 0xFF;
  _desc.has_seq_counter = false;
  _desc.ack_flag_offset = 0xFF;
  _desc.is_locked = false;
  _consecutive_mismatches = 0;
  strncpy(_desc.description, "Probing bus traffic...", sizeof(_desc.description) - 1);
  memset(_stx_counts, 0, sizeof(_stx_counts));
  memset(_etx_counts, 0, sizeof(_etx_counts));
  memset(_algo_matches, 0, sizeof(_algo_matches));
  memset(_diff_idx_counts, 0, sizeof(_diff_idx_counts));
  _consecutive_matches = 0;
  _candidate_algo = ChecksumAlgo::UNKNOWN;
  _control_matches = 0;
  _candidate_ctrl_op = 0;
}

// ============================================================================
// PROFILE REPOSITORY (NVS-BACKED DATA PROFILES)
// ============================================================================

static const VendorProfileDescriptor s_default_profiles[ProfileRepository::MAX_PROFILES] = {
    // 0: Universal Auto-Probing
    {"Auto", "Universal Auto-Probing", 0xF7, 0xEE, 3, 64, ChecksumAlgo::XOR_ALL, 4, 0x01, 0x00, 0x04, 3, 5, 6, 0, 0, 0, 0, 2, 0x01, 11, 0xFF, 0, 0xFF, 0xFF, {0}, 0},
    // 1: User Slot 1
    {"Custom1", "[Empty Custom Slot]", 0xF7, 0xEE, 3, 64, ChecksumAlgo::XOR_ALL, 4, 0x01, 0x00, 0x04, 3, 5, 6, 0, 0, 0, 0, 2, 0x01, 11, 0xFF, 0, 0xFF, 0xFF, {0}, 0},
    // 2: User Slot 2
    {"Custom2", "[Empty Custom Slot]", 0xF7, 0xEE, 3, 64, ChecksumAlgo::XOR_ALL, 4, 0x01, 0x00, 0x04, 3, 5, 6, 0, 0, 0, 0, 2, 0x01, 11, 0xFF, 0, 0xFF, 0xFF, {0}, 0},
    // 3: User Slot 3
    {"Custom3", "[Empty Custom Slot]", 0xF7, 0xEE, 3, 64, ChecksumAlgo::XOR_ALL, 4, 0x01, 0x00, 0x04, 3, 5, 6, 0, 0, 0, 0, 2, 0x01, 11, 0xFF, 0, 0xFF, 0xFF, {0}, 0}
};

static VendorProfileDescriptor s_active_profiles[ProfileRepository::MAX_PROFILES];
static bool s_profiles_initialized = false;
static portMUX_TYPE s_prof_mux = portMUX_INITIALIZER_UNLOCKED;

void ProfileRepository::init() {
  if (s_profiles_initialized)
    return;

  VendorProfileDescriptor loaded_profiles[MAX_PROFILES];
  memcpy(loaded_profiles, s_default_profiles, sizeof(s_default_profiles));

  Preferences prefs;
  if (prefs.begin("wp_profiles", true)) {
    for (size_t i = 0; i < MAX_PROFILES; ++i) {
      char pkey[16];
      snprintf(pkey, sizeof(pkey), "p_%u", static_cast<unsigned>(i));
      if (!prefs.isKey(pkey))
        continue;
      NvsEnvelope<VendorProfileDescriptor> env;
      size_t len = prefs.getBytesLength(pkey);
      if (len == sizeof(env)) {
        if (prefs.getBytes(pkey, &env, sizeof(env)) == sizeof(env) && env.verify()) {
          loaded_profiles[i] = env.payload;
        }
      }
    }
    prefs.end();
  }

  {
    CriticalSectionLocker lock(&s_prof_mux);
    if (!s_profiles_initialized) {
      memcpy(s_active_profiles, loaded_profiles, sizeof(loaded_profiles));
      s_profiles_initialized = true;
    }
  }

  g_auto_probing_engine.initFromNvs();
}

size_t ProfileRepository::getProfileCount() {
  init();
  return MAX_PROFILES;
}

bool ProfileRepository::getProfile(size_t index, VendorProfileDescriptor &out) {
  init();
  CriticalSectionLocker lock(&s_prof_mux);
  if (index >= MAX_PROFILES)
    return false;
  out = s_active_profiles[index];
  return true;
}

bool ProfileRepository::getProfileByKey(const char *key, VendorProfileDescriptor &out) {
  if (!key)
    return false;
  init();
  CriticalSectionLocker lock(&s_prof_mux);
  for (size_t i = 0; i < MAX_PROFILES; ++i) {
    if (strcasecmp(key, s_active_profiles[i].key) == 0) {
      out = s_active_profiles[i];
      return true;
    }
  }
  return false;
}

bool ProfileRepository::getActiveProfile(VendorProfileDescriptor &out) {
  init();
  uint8_t idx = g_config.wallpad_profile;
  return getProfile((idx < MAX_PROFILES) ? idx : 0, out);
}

bool ProfileRepository::setActiveProfileIndex(size_t index) {
  if (index >= MAX_PROFILES)
    return false;
  {
    CriticalSectionLocker lock(&g_config_mux);
    g_config.wallpad_profile = static_cast<uint8_t>(index);
    g_config_dirty.store(true, std::memory_order_release);
  }
  Config_Save();

  if (index != 0) {
    VendorProfileDescriptor desc;
    if (getProfile(index, desc) && desc.door_stx != 0 && desc.door_etx != 0) {
      g_doorphone_tracker.setFixedLock(desc.door_stx, desc.door_etx, desc.door_len);
      g_doorphone_tracker.saveToNvs();
    }
  } else {
    // 0번(Auto): 도어폰도 자동 언락/적응형 학습 모드로 전환
    g_doorphone_tracker.is_custom_fixed.store(false, std::memory_order_relaxed);
    g_doorphone_tracker.saveToNvs();
  }
  return true;
}

bool ProfileRepository::setActiveProfileByKey(const char *key) {
  if (!key)
    return false;
  init();
  for (size_t i = 0; i < MAX_PROFILES; ++i) {
    if (strcasecmp(key, s_active_profiles[i].key) == 0) {
      return setActiveProfileIndex(i);
    }
  }
  return false;
}

bool ProfileRepository::saveCustomProfile(size_t index, const VendorProfileDescriptor &profile) {
  if (index >= MAX_PROFILES)
    return false;
  init();
  {
    CriticalSectionLocker lock(&s_prof_mux);
    s_active_profiles[index] = profile;
  }
  Preferences prefs;
  if (prefs.begin("wp_profiles", false)) {
    char pkey[16];
    snprintf(pkey, sizeof(pkey), "p_%u", static_cast<unsigned>(index));
    NvsEnvelope<VendorProfileDescriptor> env;
    env.payload = profile;
    env.seal();
    prefs.putBytes(pkey, &env, sizeof(env));
    prefs.end();
  }

  if (g_config.wallpad_profile == index && profile.door_stx != 0 && profile.door_etx != 0) {
    g_doorphone_tracker.setFixedLock(profile.door_stx, profile.door_etx, profile.door_len);
    g_doorphone_tracker.saveToNvs();
  }
  return true;
}

namespace {
const char *getChecksumShortName(ChecksumAlgo algo) noexcept {
  switch (algo) {
  case ChecksumAlgo::XOR_ALL:
  case ChecksumAlgo::XOR_NO_STX:      return "XOR";
  case ChecksumAlgo::SUM_ALL:
  case ChecksumAlgo::SUM_NO_STX:      return "SUM";
  case ChecksumAlgo::TWOS_COMPLEMENT: return "2'sComp";
  case ChecksumAlgo::ONES_COMPLEMENT: return "1'sComp";
  case ChecksumAlgo::CRC8_MAXIM:      return "CRC8";
  case ChecksumAlgo::NONE:            return "None";
  default:                            return "CS";
  }
}
} // namespace

void ProfileRepository::inferVendorDescription(const AutoProbeDescriptor &ad, char *out_desc, size_t max_len) {
  if (!out_desc || max_len == 0)
    return;

  uint8_t stx = (ad.stx > 0) ? ad.stx : 0xF7;
  uint8_t etx = (ad.etx > 0) ? ad.etx : 0xEE;

  // 1. 길이 표현 포맷팅
  char len_buf[16];
  if (ad.min_len == ad.max_len && ad.min_len >= 3) {
    snprintf(len_buf, sizeof(len_buf), "%uB", ad.min_len);
  } else if (ad.min_len >= 3 && ad.max_len <= 64 && ad.max_len > ad.min_len) {
    snprintf(len_buf, sizeof(len_buf), "%u-%uB", ad.min_len, ad.max_len);
  } else {
    snprintf(len_buf, sizeof(len_buf), "Var");
  }

  // 2. 일관된 순수 기술 스펙 포맷팅: Profile (STX..ETX, 길이, 체크섬)
  snprintf(out_desc, max_len, "Profile (%02X..%02X, %s, %s)",
           stx, etx, len_buf, getChecksumShortName(ad.checksum_algo));
}

bool ProfileRepository::saveCurrentAutoAs(const char *name, size_t &saved_idx) {
  if (!name || strlen(name) == 0)
    return false;
  init();

  AutoProbeDescriptor ad = g_auto_probing_engine.getDescriptor();

  // 1. [UI 전용] 관측된 실제 패킷 길이 수집 (화면 표시/제조사 유추 전용)
  size_t total_tgts = g_polling_targets.totalCount();
  uint8_t obs_min_len = 255, obs_max_len = 0;
  for (size_t i = 0; i < total_tgts; ++i) {
    PollingTargetEntry entry;
    if (g_polling_targets.getEntry(i, entry) && entry.raw_query_len > 0) {
      if (entry.raw_query_len < obs_min_len) obs_min_len = entry.raw_query_len;
      if (entry.raw_query_len > obs_max_len) obs_max_len = entry.raw_query_len;
    }
  }

  AutoProbeDescriptor ui_desc = ad;
  if (obs_min_len <= obs_max_len && obs_min_len >= 3) {
    ui_desc.min_len = obs_min_len;
    ui_desc.max_len = obs_max_len;
  }

  // 2. [파서 런타임 전용] 실제 통신 엔진이 동작할 안전한 프레이밍 바운드 (3~64B 가변 허용)
  VendorProfileDescriptor new_prof;
  memset(&new_prof, 0, sizeof(new_prof));
  strncpy(new_prof.key, name, sizeof(new_prof.key) - 1);
  
  // UI 텍스트 설명 생성 (순수 텍스트 생성용 독립 호출)
  inferVendorDescription(ui_desc, new_prof.name, sizeof(new_prof.name));

  // 파서 런타임 바운드: 어떤 장비(11B, 13B, 18B, 32B 등)가 오더라도 100% 수용 가능한 가변 바운드
  new_prof.stx = (ad.stx > 0) ? ad.stx : 0xF7;
  new_prof.etx = (ad.etx > 0) ? ad.etx : 0xEE;
  new_prof.min_len = 3;  // 최소 3바이트 이상 모든 프레임 수용
  new_prof.max_len = 64; // 최대 64바이트 이하 모든 프레임 수용 (타임아웃 원천 차단)
  new_prof.cs_algo = ad.checksum_algo;
  new_prof.opcode_offset = (ad.opcode_offset > 0 && ad.opcode_offset < 10) ? ad.opcode_offset : 4;
  new_prof.query_op = (ad.query_opcode > 0) ? ad.query_opcode : 0x01;
  new_prof.ctrl_op = ad.control_seen ? ad.control_opcode : 0x02;
  new_prof.ack_op = (ad.ack_opcode > 0) ? ad.ack_opcode : 0x04;
  new_prof.dev_id_offset = ad.offsets_locked ? ad.dev_id_offset : 3;
  new_prof.sub1_offset = ad.offsets_locked ? ad.sub1_offset : 5;
  new_prof.sub2_offset = ad.offsets_locked ? ad.sub2_offset : 6;
  new_prof.is_swapped_addr = ad.offsets_locked ? (ad.is_swapped_addr ? 1 : 0) : 0;
  new_prof.gw_addr_offset = ad.offsets_locked ? ad.gw_addr_offset : 2;
  new_prof.gw_addr = ad.offsets_locked ? ad.gw_addr : 0x01;
  new_prof.learned_query_len = (ad.offsets_locked && ad.learned_query_len >= 3) ? ad.learned_query_len : 11;

  // 도어폰의 현재 학습된 프레이밍도 함께 프로파일에 저장
  uint8_t dp_s = g_doorphone_tracker.candidate_stx.load(std::memory_order_relaxed);
  uint8_t dp_e = g_doorphone_tracker.candidate_etx.load(std::memory_order_relaxed);
  uint8_t dp_l = g_doorphone_tracker.candidate_len.load(std::memory_order_relaxed);
  new_prof.door_stx = (dp_s > 0) ? dp_s : 0x7F;
  new_prof.door_etx = (dp_e > 0) ? dp_e : 0xEE;
  new_prof.door_len = (dp_l >= 3) ? dp_l : 9;

  size_t target_slot = 1;
  bool found_match = false;
  for (size_t i = 1; i < MAX_PROFILES; ++i) {
    if (strcasecmp(s_active_profiles[i].key, name) == 0) {
      target_slot = i;
      found_match = true;
      break;
    }
  }
  if (!found_match) {
    for (size_t i = 1; i < MAX_PROFILES; ++i) {
      if (strncasecmp(s_active_profiles[i].name, "[Empty", 6) == 0 ||
          strncasecmp(s_active_profiles[i].key, "Custom", 6) == 0) {
        target_slot = i;
        break;
      }
    }
  }

  saveCustomProfile(target_slot, new_prof);
  setActiveProfileIndex(target_slot);
  saved_idx = target_slot;
  return true;
}

bool ProfileRepository::deleteProfile(size_t index) {
  if (index == 0 || index >= MAX_PROFILES)
    return false;
  init();
  VendorProfileDescriptor empty_prof = s_default_profiles[index];
  saveCustomProfile(index, empty_prof);
  if (g_config.wallpad_profile == index) {
    setActiveProfileIndex(0);
  }
  return true;
}

void ProfileRepository::syncAutoProfileToNvs(const AutoProbeDescriptor &auto_desc) {
  init();
  VendorProfileDescriptor desc;
  {
    CriticalSectionLocker lock(&s_prof_mux);
    desc = s_active_profiles[0]; // 0 is 'auto'
    desc.stx = auto_desc.stx;
    desc.etx = auto_desc.etx;
    desc.min_len = auto_desc.min_len;
    desc.max_len = auto_desc.max_len;
    desc.cs_algo = auto_desc.checksum_algo;
    desc.opcode_offset = auto_desc.opcode_offset;
    desc.query_op = auto_desc.query_opcode;
    if (auto_desc.control_seen) {
      desc.ctrl_op = auto_desc.control_opcode;
    }
    desc.ack_op = auto_desc.ack_opcode;
    if (auto_desc.offsets_locked) {
      desc.dev_id_offset = auto_desc.dev_id_offset;
      desc.sub1_offset = auto_desc.sub1_offset;
      desc.sub2_offset = auto_desc.sub2_offset;
      desc.is_swapped_addr = auto_desc.is_swapped_addr ? 1 : 0;
      desc.gw_addr_offset = auto_desc.gw_addr_offset;
      desc.gw_addr = auto_desc.gw_addr;
      desc.learned_query_len = auto_desc.learned_query_len;
      desc.len_offset = auto_desc.len_offset;
      desc.has_len_field = auto_desc.has_len_field ? 1 : 0;
      desc.seq_offset = auto_desc.seq_offset;
      desc.ack_flag_offset = auto_desc.ack_flag_offset;
      desc.ctrl_len_cnt = auto_desc.ctrl_len_cnt;
      memcpy(desc.learned_ctrl_lens, auto_desc.learned_ctrl_lens, sizeof(desc.learned_ctrl_lens));
    }
    s_active_profiles[0] = desc;
  }
  Preferences prefs;
  if (prefs.begin("wp_profiles", false)) {
    NvsEnvelope<VendorProfileDescriptor> env;
    env.payload = desc;
    env.seal();
    prefs.putBytes("p_0", &env, sizeof(env));
    prefs.end();
  }
}

void ProfileRepository::resetAllToDefaults() {
  init();
  {
    CriticalSectionLocker lock(&s_prof_mux);
    memcpy(s_active_profiles, s_default_profiles, sizeof(s_default_profiles));
  }
  Preferences prefs;
  if (prefs.begin("wp_profiles", false)) {
    prefs.clear();
    prefs.end();
  }
}

// ============================================================================
// DATA-DRIVEN UNIVERSAL PROTOCOL ENGINE IMPLEMENTATION
// ============================================================================

static UniversalProtocolEngine s_universal_engine;

const char *UniversalProtocolEngine::getVendorName() const {
  VendorProfileDescriptor desc;
  ProfileRepository::getActiveProfile(desc);
  if (strcasecmp(desc.key, "auto") == 0) {
    auto ad = g_auto_probing_engine.getDescriptor();
    if (ad.is_locked) {
      static char buf[64];
      snprintf(buf, sizeof(buf), "Auto [STX 0x%02X ETX 0x%02X / %s]",
               ad.stx, ad.etx, AutoProbingEngine::getAlgoName(ad.checksum_algo));
      return buf;
    }
    return "Auto (Learning...)";
  }
  static char buf[32];
  snprintf(buf, sizeof(buf), "%s", desc.name);
  return buf;
}

const char *UniversalProtocolEngine::getProfileKey() const {
  static char buf[16];
  VendorProfileDescriptor desc;
  ProfileRepository::getActiveProfile(desc);
  snprintf(buf, sizeof(buf), "%s", desc.key);
  return buf;
}

uint8_t UniversalProtocolEngine::getVendorId() const {
  return g_config.wallpad_profile;
}

static inline bool checkFramingPure(span<const uint8_t> frame, uint8_t stx,
                                    uint8_t etx, uint8_t min_len,
                                    uint8_t max_len, ChecksumAlgo algo) {
  if (frame.size() < min_len || frame.size() > max_len || frame.size() < 3)
    return false;
  if (frame[0] != stx || frame[frame.size() - 1] != etx)
    return false;
  if (algo == ChecksumAlgo::NONE)
    return true;
  uint8_t cs = g_auto_probing_engine.calculateChecksum(algo, frame.data(),
                                                       frame.size());
  return cs == frame[frame.size() - 2];
}

bool UniversalProtocolEngine::validatePacket(span<const uint8_t> frame) const {
  if (frame.size() < 3 || frame.size() > 64)
    return false;

  VendorProfileDescriptor desc = activeProfile();

  if (isAutoProfile(desc)) {
    g_auto_probing_engine.feedFrame(frame);
    auto ad = g_auto_probing_engine.getDescriptor();
    return checkFramingPure(frame, ad.stx, ad.etx, ad.min_len, ad.max_len,
                            ad.checksum_algo);
  }

  return checkFramingPure(frame, desc.stx, desc.etx, desc.min_len, desc.max_len,
                          desc.cs_algo);
}

bool UniversalProtocolEngine::isQueryPacket(span<const uint8_t> frame) const {
  VendorProfileDescriptor desc = activeProfile();
  if (isAutoProfile(desc)) {
    auto ad = g_auto_probing_engine.getDescriptor();
    if (frame.size() <= ad.opcode_offset)
      return false;
    return frame[ad.opcode_offset] == ad.query_opcode;
  }
  if (frame.size() <= desc.opcode_offset)
    return false;
  return frame[desc.opcode_offset] == desc.query_op;
}

bool UniversalProtocolEngine::isControlPacket(span<const uint8_t> frame) const {
  VendorProfileDescriptor desc = activeProfile();
  if (isAutoProfile(desc)) {
    auto ad = g_auto_probing_engine.getDescriptor();
    if (frame.size() <= ad.opcode_offset)
      return false;
    if (ad.control_seen && ad.control_opcode != 0) {
      return frame[ad.opcode_offset] == ad.control_opcode;
    }
    return (frame[ad.opcode_offset] != ad.query_opcode && frame[ad.opcode_offset] != ad.ack_opcode);
  }
  if (frame.size() <= desc.opcode_offset)
    return false;
  return frame[desc.opcode_offset] == desc.ctrl_op;
}

bool UniversalProtocolEngine::isAckPacket(span<const uint8_t> frame) const {
  VendorProfileDescriptor desc = activeProfile();
  if (isAutoProfile(desc)) {
    auto ad = g_auto_probing_engine.getDescriptor();
    if (frame.size() <= ad.opcode_offset)
      return false;
    return (frame[ad.opcode_offset] == ad.ack_opcode || frame[ad.opcode_offset] == ad.query_opcode);
  }
  if (frame.size() <= desc.opcode_offset)
    return false;
  return (frame[desc.opcode_offset] == desc.ack_op || frame[desc.opcode_offset] == desc.query_op);
}

bool UniversalProtocolEngine::extractDeviceKey(span<const uint8_t> frame,
                                               uint8_t &dev_id, uint8_t &sub1,
                                               uint8_t &sub2) const {
  VendorProfileDescriptor desc = activeProfile();
  uint8_t d_off = desc.dev_id_offset;
  uint8_t s1_off = desc.sub1_offset;
  uint8_t s2_off = desc.sub2_offset;

  bool is_swapped = (desc.is_swapped_addr != 0);
  uint8_t gw_addr_off = is_swapped ? desc.gw_addr_offset : d_off;

  if (isAutoProfile(desc)) {
    auto ad = g_auto_probing_engine.getDescriptor();
    if (ad.offsets_locked) {
      d_off = ad.dev_id_offset;
      s1_off = ad.sub1_offset;
      s2_off = ad.sub2_offset;
      is_swapped = ad.is_swapped_addr;
      gw_addr_off = ad.gw_addr_offset;
    }
  }

  if (frame.size() <= d_off)
    return false;

  // ★ swap 구조 보완: ACK 패킷에서 DevType은 gw_addr_offset 위치에 있음
  // (QUERY: dev_id_offset=swap_i=DevType, gw_addr_offset=swap_j=GW주소)
  // (ACK:   dev_id_offset=swap_i=GW주소, gw_addr_offset=swap_j=DevType) ← 교차!
  if (is_swapped && isAckPacket(frame)) {
    if (frame.size() <= gw_addr_off)
      return false;
    dev_id = frame[gw_addr_off];  // ACK에서 DevType = gw_addr_offset 위치
  } else {
    dev_id = frame[d_off];        // QUERY 또는 swap 없는 ACK: dev_id_offset 위치
  }

  sub1 = (s1_off < frame.size()) ? frame[s1_off] : 0;
  sub2 = (s2_off < frame.size()) ? frame[s2_off] : 0;
  return true;
}

bool UniversalProtocolEngine::buildQueryPacket(uint8_t dev_id, uint8_t sub1,
                                               uint8_t sub2,
                                               StaticPacket &out) const {
  VendorProfileDescriptor desc = activeProfile();

  uint8_t stx = desc.stx;
  uint8_t etx = desc.etx;
  ChecksumAlgo algo = desc.cs_algo;
  uint8_t op_off = desc.opcode_offset;
  uint8_t q_op = desc.query_op;
  uint8_t d_off = desc.dev_id_offset;
  uint8_t s1_off = desc.sub1_offset;
  uint8_t s2_off = desc.sub2_offset;

  if (isAutoProfile(desc)) {
    auto ad = g_auto_probing_engine.getDescriptor();
    stx = ad.stx;
    etx = ad.etx;
    algo = ad.checksum_algo;
    op_off = ad.opcode_offset;
    q_op = ad.query_opcode;
    if (ad.offsets_locked) {
      d_off = ad.dev_id_offset;
      s1_off = ad.sub1_offset;
      s2_off = ad.sub2_offset;
    }

    // ★ 버스 관측 기반 동적 패킷 길이 (기본: learned_query_len, 최소 11)
    uint8_t pkt_len = (ad.offsets_locked && ad.learned_query_len >= 5)
                          ? ad.learned_query_len : 11;
    out.channel_id = 1;
    out.length = pkt_len;
    out.data.fill(0);
    out.data[0] = stx;
    out.data[1] = pkt_len;  // LEN 필드

    // ★ GW 주소: 버스에서 관측된 ad.gw_addr를 ad.gw_addr_offset 위치에 기입
    // (기존: data[2] = 0x01 하드코딩 → 현재: 관측값 사용)
    if (ad.gw_addr_offset < pkt_len) {
      out.data[ad.gw_addr_offset] = ad.gw_addr;
    }

    if (d_off < pkt_len) out.data[d_off] = dev_id;
    if (op_off < pkt_len) out.data[op_off] = q_op;
    if (s1_off > 0 && s1_off < pkt_len) out.data[s1_off] = sub1;
    if (s2_off > 0 && s2_off < pkt_len) out.data[s2_off] = sub2;

    // CS = Byte #(N-2), ETX = Byte #(N-1)
    if (pkt_len >= 3) {
      out.data[pkt_len - 2] = g_auto_probing_engine.calculateChecksum(algo, out.data.data(), pkt_len);
      out.data[pkt_len - 1] = etx;
    }
    return true;
  }

  // ── Non-Auto 프로파일: 저장된 프로파일의 길이와 GW 주소 규칙 사용 ──
  uint8_t pkt_len = (desc.learned_query_len >= 3 && desc.learned_query_len <= 64)
                        ? desc.learned_query_len : 11;
  out.channel_id = 1;
  out.length = pkt_len;
  out.data.fill(0);
  out.data[0] = stx;
  out.data[1] = pkt_len;

  if (desc.gw_addr_offset < pkt_len) {
    out.data[desc.gw_addr_offset] = (desc.gw_addr != 0) ? desc.gw_addr : 0x01;
  }

  if (d_off < pkt_len) out.data[d_off] = dev_id;
  if (op_off < pkt_len) out.data[op_off] = q_op;
  if (s1_off > 0 && s1_off < pkt_len) out.data[s1_off] = sub1;
  if (s2_off > 0 && s2_off < pkt_len) out.data[s2_off] = sub2;

  if (pkt_len >= 3) {
    out.data[pkt_len - 2] = g_auto_probing_engine.calculateChecksum(algo, out.data.data(), pkt_len);
    out.data[pkt_len - 1] = etx;
  }
  return true;
}

uint8_t UniversalProtocolEngine::calculateChecksum(const uint8_t *data, size_t len) const {
  VendorProfileDescriptor desc = activeProfile();
  ChecksumAlgo algo = isAutoProfile(desc)
                          ? g_auto_probing_engine.getDescriptor().checksum_algo
                          : desc.cs_algo;
  return g_auto_probing_engine.calculateChecksum(algo, data, len);
}

uint8_t UniversalProtocolEngine::getStx() const {
  VendorProfileDescriptor desc = activeProfile();
  return isAutoProfile(desc) ? g_auto_probing_engine.getDescriptor().stx : desc.stx;
}

uint8_t UniversalProtocolEngine::getEtx() const {
  VendorProfileDescriptor desc = activeProfile();
  return isAutoProfile(desc) ? g_auto_probing_engine.getDescriptor().etx : desc.etx;
}

uint8_t UniversalProtocolEngine::getMinPacketLen() const {
  VendorProfileDescriptor desc = activeProfile();
  return isAutoProfile(desc) ? g_auto_probing_engine.getDescriptor().min_len : desc.min_len;
}

uint8_t UniversalProtocolEngine::getMaxPacketLen() const {
  VendorProfileDescriptor desc = activeProfile();
  return isAutoProfile(desc) ? g_auto_probing_engine.getDescriptor().max_len : desc.max_len;
}

int UniversalProtocolEngine::extractPacketLength(const uint8_t *stream, size_t stream_len, size_t stx_idx) const {
  if (stx_idx >= stream_len)
    return -1;

  // 진입 시 단 1회만 메타데이터 스냅샷
  VendorProfileDescriptor desc = activeProfile();
  bool is_auto = isAutoProfile(desc);
  AutoProbeDescriptor ad = is_auto ? g_auto_probing_engine.getDescriptor() : AutoProbeDescriptor{};

  uint8_t stx = is_auto ? ad.stx : desc.stx;
  uint8_t etx = is_auto ? ad.etx : desc.etx;
  uint8_t min_len = is_auto ? ad.min_len : desc.min_len;
  uint8_t max_len = is_auto ? ad.max_len : desc.max_len;
  ChecksumAlgo algo = is_auto ? ad.checksum_algo : desc.cs_algo;

  if (stream[stx_idx] != stx)
    return -1;

  uint8_t safe_min = std::max<uint8_t>(min_len, 3);
  uint8_t safe_max = (max_len >= safe_min && max_len <= 64) ? max_len : 64;

  for (size_t l = safe_min; l <= safe_max; ++l) {
    if (stx_idx + l > stream_len) {
      return 0; // 아직 패킷 바이트가 덜 들어옴 (추가 수신 대기)
    }
    if (stream[stx_idx + l - 1] == etx) {
      span<const uint8_t> cand(&stream[stx_idx], l);
      if (checkFramingPure(cand, stx, etx, safe_min, safe_max, algo)) {
        return static_cast<int>(l); // STX + ETX + Checksum 3박자 통과!
      }
    }
  }

  if (stx_idx + safe_max <= stream_len) {
    return -1; // safe_max까지 유효한 프레임 없음 -> 다음 바이트로 이동
  }
  return 0; // 추가 수신 대기
}

// ============================================================================
// WALLPAD PARSER FACTORY (COMPATIBILITY FACADE)
// ============================================================================

void WallpadParserFactory::init() {
  ProfileRepository::init();
}

IWallpadParser *WallpadParserFactory::getActiveParser() {
  return &s_universal_engine;
}

bool WallpadParserFactory::setProfile(uint8_t index) {
  return ProfileRepository::setActiveProfileIndex(index);
}

bool WallpadParserFactory::setProfileByKey(const char *key) {
  return ProfileRepository::setActiveProfileByKey(key);
}

size_t WallpadParserFactory::getParserCount() {
  return ProfileRepository::getProfileCount();
}
