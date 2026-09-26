#include "WallpadParser.h"
#include "ControlTemplate.h"
#include "Common.h"
#include "TelnetCli.h"
#include <algorithm>
#include <vector>
#include <set>
#include <map>
#include <cstring>
#include <cstdio>

AutoProbingEngine g_auto_probing_engine;

AutoProbingEngine::AutoProbingEngine() {
  reset();
}

const char *AutoProbingEngine::getAlgoName(ChecksumAlgo algo) {
  switch (algo) {
  case ChecksumAlgo::XOR_ALL:        return "XOR (0..[N-3])";
  case ChecksumAlgo::XOR_NO_STX:     return "XOR (1..[N-3])";
  case ChecksumAlgo::SUM_ALL:        return "SUM (0..[N-3])";
  case ChecksumAlgo::SUM_NO_STX:     return "SUM (1..[N-3])";
  case ChecksumAlgo::TWOS_COMPLEMENT:return "2's Complement (1..[N-3])";
  case ChecksumAlgo::ONES_COMPLEMENT:return "1's Complement (0..[N-3])";
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

    if (prof.dev_id_offset > 0 || prof.sub1_offset > 0 || prof.sub2_offset > 0) {
      _desc.dev_id_offset = prof.dev_id_offset;
      _desc.sub1_offset = prof.sub1_offset;
      _desc.sub2_offset = prof.sub2_offset;
      if (!_desc.is_swapped_addr) {
        if (_desc.gw_addr_offset == _desc.dev_id_offset || _desc.gw_addr_offset != 2) {
          _desc.gw_addr_offset = 2;
          _desc.gw_addr = (prof.gw_addr != 0) ? prof.gw_addr : 0x01;
        }
        if (_desc.sub1_offset == 2 && _desc.sub2_offset > 2) {
          _desc.sub1_offset = _desc.sub2_offset;
        }
      }
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
      int max_hdr = std::max({_desc.opcode_offset, _desc.dev_id_offset, _desc.sub1_offset, _desc.sub2_offset});
      if (_desc.gw_addr_offset > max_hdr) max_hdr = _desc.gw_addr_offset;
      if (_desc.len_offset != 0xFF && _desc.len_offset > max_hdr) max_hdr = _desc.len_offset;
      if (_desc.seq_offset != 0xFF && _desc.seq_offset > max_hdr) max_hdr = _desc.seq_offset;
      if (_desc.ack_flag_offset != 0xFF && _desc.ack_flag_offset > max_hdr) max_hdr = _desc.ack_flag_offset;
      _desc.payload_offset = static_cast<uint8_t>(std::max(max_hdr + 1, 8));

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

void AutoProbingEngine::feedControlFrame(span<const uint8_t> ctrl_frame) {
  if (ctrl_frame.size() < 5) return;

  bool should_sync = false;
  AutoProbeDescriptor desc_to_sync;

  {
    CriticalSectionLocker lock(&_mux);
    if (ctrl_frame.size() <= _desc.opcode_offset) return;

    uint8_t op = ctrl_frame[_desc.opcode_offset];
    // QRY나 ACK가 아닌 경우에만 제어 오프코드로 처리
    if (op == _desc.query_opcode || op == _desc.ack_opcode) return;

    if (!_desc.control_seen || _desc.control_opcode != op) {
      _desc.control_opcode = op;
      _desc.control_seen = true;
      _desc.opcodes_locked = true;

      uint8_t flen = static_cast<uint8_t>(ctrl_frame.size());
      bool len_found = false;
      for (uint8_t i = 0; i < _desc.ctrl_len_cnt; ++i) {
        if (_desc.learned_ctrl_lens[i] == flen) {
          len_found = true;
          break;
        }
      }
      if (!len_found && _desc.ctrl_len_cnt < sizeof(_desc.learned_ctrl_lens)) {
        _desc.learned_ctrl_lens[_desc.ctrl_len_cnt++] = flen;
      }

      if (_desc.is_locked) {
        should_sync = true;
        desc_to_sync = _desc;
      }
    } else {
      // 이미 opcode는 학습되었으나 길이가 추가되는 경우 (예: 11바이트, 13바이트 등)
      uint8_t flen = static_cast<uint8_t>(ctrl_frame.size());
      bool len_found = false;
      for (uint8_t i = 0; i < _desc.ctrl_len_cnt; ++i) {
        if (_desc.learned_ctrl_lens[i] == flen) {
          len_found = true;
          break;
        }
      }
      if (!len_found && _desc.ctrl_len_cnt < sizeof(_desc.learned_ctrl_lens)) {
        _desc.learned_ctrl_lens[_desc.ctrl_len_cnt++] = flen;
        if (_desc.is_locked) {
          should_sync = true;
          desc_to_sync = _desc;
        }
      }
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

void AutoProbingEngine::injectControlSpec(uint8_t ctrl_op, uint8_t ctrl_len) {
  bool should_sync = false;
  AutoProbeDescriptor desc_to_sync;
  {
    CriticalSectionLocker lock(&_mux);
    _desc.control_opcode = ctrl_op;
    _desc.control_seen = true;
    _desc.opcodes_locked = true;

    bool len_found = false;
    for (uint8_t i = 0; i < _desc.ctrl_len_cnt; ++i) {
      if (_desc.learned_ctrl_lens[i] == ctrl_len) {
        len_found = true;
        break;
      }
    }
    if (!len_found && _desc.ctrl_len_cnt < sizeof(_desc.learned_ctrl_lens)) {
      _desc.learned_ctrl_lens[_desc.ctrl_len_cnt++] = ctrl_len;
    }

    if (_desc.is_locked) {
      should_sync = true;
      desc_to_sync = _desc;
    }
  }

  if (should_sync) {
    ProfileRepository::syncAutoProfileToNvs(desc_to_sync);
  }
}

bool AutoProbingEngine::analyzeCacheMatrix() {
  if (g_polling_targets.ackedCount() < 2 && g_device_repo.getOnlineCount() < 2) {
    return false;
  }

  struct PktPair {
    StaticPacket q;
    StaticPacket r;
  };
  std::vector<PktPair> pairs;
  pairs.reserve(std::min<size_t>(g_polling_targets.totalCount(), 32));

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

  int seq_idx = -1;
  if (N >= 4) {
    for (size_t k = 1; k < min_common_len - 1; ++k) {
      int ik = static_cast<int>(k);
      if (ik == len_idx || ik == opcode_idx || ik == swap_i || ik == swap_j || ik == promoted_dev_idx)
        continue;
      std::set<uint8_t> distinct_vals;
      for (size_t m = 0; m < N; ++m) distinct_vals.insert(pairs[m].q.data[k]);
      if (distinct_vals.size() < 3)
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
        if (master_gw_idx < 0) {
          master_gw_idx = ik;
        }
      } else if (promoted_dev_idx >= 0) {
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

  int ack_flag_idx = -1;
  int max_known_hdr = std::max({opcode_idx, dev_type_idx, sub_cmd_idx, sub_id_idx, swap_i, swap_j, len_idx, seq_idx});
  for (size_t k = 1; k < min_common_len - 1; ++k) {
    int ik = static_cast<int>(k);
    if (ik == len_idx || ik == opcode_idx || ik == swap_i || ik == swap_j ||
        ik == seq_idx || ik == sub_cmd_idx || ik == dev_type_idx || ik == sub_id_idx) {
      continue;
    }
    if (ik > max_known_hdr) {
      continue;
    }
    std::set<uint8_t> r_vals;
    for (size_t m = 0; m < N; ++m) r_vals.insert(pairs[m].r.data[k]);
    if (r_vals.size() == 1 && (*r_vals.begin() == 0x00 || *r_vals.begin() == 0x01)) {
      ack_flag_idx = ik;
      break;
    }
  }

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
    } else if (sub_id_idx >= 0) {
      _desc.sub1_offset = static_cast<uint8_t>(sub_id_idx);
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
    if (master_gw_idx >= 0) {
      _desc.gw_addr_offset = static_cast<uint8_t>(master_gw_idx);
      _desc.gw_addr = pairs[0].q.data[master_gw_idx];
    } else if (swap_i >= 0 && master_gw_idx >= 0 && promoted_dev_idx >= 0) {
      _desc.gw_addr_offset = static_cast<uint8_t>(master_gw_idx);
      _desc.gw_addr = pairs[0].q.data[master_gw_idx];
    } else if (!_desc.is_swapped_addr && dev_type_idx >= 0) {
      _desc.gw_addr_offset = _desc.dev_id_offset;
    }

    if (min_common_len >= 5 && min_common_len <= 64) {
      _desc.learned_query_len = static_cast<uint8_t>(min_common_len);
    }

    _desc.offsets_locked = (dev_type_idx >= 0 && sub_id_idx >= 0);

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

    // Zero-Variance Padding 스캐너: max_hdr 직후 모든 샘플에서 값이 0x00 고정인 더미/패딩 열 자동 스킵
    int payload_start = max_hdr + 1;
    while (payload_start < static_cast<int>(min_common_len - 2)) {
      bool is_all_zero = true;
      for (size_t m = 0; m < N; ++m) {
        if (pairs[m].r.data[payload_start] != 0x00) {
          is_all_zero = false;
          break;
        }
      }
      if (is_all_zero) {
        payload_start++;
      } else {
        break;
      }
    }
    _desc.payload_offset = static_cast<uint8_t>(payload_start);

    snprintf(_desc.description, sizeof(_desc.description),
             "Auto: OP@%u(0x%02X/0x%02X) DEV@%u SUB1@%u SUB2@%u PL@%u",
             _desc.opcode_offset, _desc.query_opcode, _desc.ack_opcode,
             _desc.dev_id_offset, _desc.sub1_offset, _desc.sub2_offset,
             _desc.payload_offset);
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
  _desc.payload_offset = 7;
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
