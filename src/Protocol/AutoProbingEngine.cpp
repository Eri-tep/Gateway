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

static AutoProbingEngine::OnlineCountFn s_online_count_fn{nullptr};
static AutoProbingEngine::DeviceAckLookupFn s_lookup_fn{nullptr};
static AutoProbingEngine::UpdateFromBusFn s_update_from_bus_fn{nullptr};


void AutoProbingEngine::setDeviceHooks(OnlineCountFn count_fn,
                                       DeviceAckLookupFn lookup_fn,
                                       UpdateFromBusFn update_fn) {
  s_online_count_fn = count_fn;
  s_lookup_fn = lookup_fn;
  s_update_from_bus_fn = update_fn;
}

AutoProbingEngine g_auto_probing_engine;

AutoProbingEngine::AutoProbingEngine() { reset(); }

const char *AutoProbingEngine::getAlgoName(ChecksumAlgo algo) {
  switch (algo) {
  case ChecksumAlgo::XOR_ALL:
    return "XOR (0..[N-3])";
  case ChecksumAlgo::XOR_NO_STX:
    return "XOR (1..[N-3])";
  case ChecksumAlgo::SUM_ALL:
    return "SUM (0..[N-3])";
  case ChecksumAlgo::SUM_NO_STX:
    return "SUM (1..[N-3])";
  case ChecksumAlgo::TWOS_COMPLEMENT:
    return "2's Complement (1..[N-3])";
  case ChecksumAlgo::ONES_COMPLEMENT:
    return "1's Complement (0..[N-3])";
  case ChecksumAlgo::CRC8_MAXIM:
    return "CRC-8 (Maxim/0x31)";
  case ChecksumAlgo::NONE:
    return "None (Pure Framing)";
  default:
    return "Learning...";
  }
}

namespace {
uint8_t crc8Poly31(const uint8_t *d, size_t n) {
  uint8_t crc = 0;
  for (size_t i = 0; i < n; ++i) {
    crc ^= d[i];
    for (int b = 0; b < 8; ++b)
      crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x31)
                         : static_cast<uint8_t>(crc << 1);
  }
  return crc;
}

// 제어 프레임 길이 학습 (신규면 true)
bool addCtrlLen(AutoProbeDescriptor &d, uint8_t len) {
  for (uint8_t i = 0; i < d.ctrl_len_cnt; ++i)
    if (d.learned_ctrl_lens[i] == len)
      return false;
  if (d.ctrl_len_cnt >= sizeof(d.learned_ctrl_lens))
    return false;
  d.learned_ctrl_lens[d.ctrl_len_cnt++] = len;
  return true;
}

template <class T> uint8_t argmax256(const T *a, uint16_t &mx) {
  uint8_t best = 0;
  mx = 0;
  for (int i = 0; i < 256; ++i)
    if (a[i] > mx) {
      mx = a[i];
      best = static_cast<uint8_t>(i);
    }
  return best;
}
} // namespace

uint8_t AutoProbingEngine::calculateChecksum(ChecksumAlgo algo,
                                             span<const uint8_t> data) const noexcept {
  const size_t len = data.size();
  if (len < 3)
    return 0;
  if (algo == ChecksumAlgo::CRC8_MAXIM)
    return crc8Poly31(data.data(), len - 2);

  const bool is_xor =
      (algo == ChecksumAlgo::XOR_ALL || algo == ChecksumAlgo::XOR_NO_STX);
  const bool is_sum =
      (algo == ChecksumAlgo::SUM_ALL || algo == ChecksumAlgo::SUM_NO_STX ||
       algo == ChecksumAlgo::TWOS_COMPLEMENT ||
       algo == ChecksumAlgo::ONES_COMPLEMENT);
  if (!is_xor && !is_sum)
    return 0;

  // STX 를 제외하는 알고리즘은 1부터 시작
  const size_t start =
      (algo == ChecksumAlgo::XOR_NO_STX || algo == ChecksumAlgo::SUM_NO_STX ||
       algo == ChecksumAlgo::TWOS_COMPLEMENT)
          ? 1
          : 0;
  uint8_t r = 0;
  for (size_t i = start; i < len - 2; ++i)
    r = is_xor ? (r ^ data[i]) : (r + data[i]);

  if (algo == ChecksumAlgo::TWOS_COMPLEMENT)
    return static_cast<uint8_t>(-r);
  if (algo == ChecksumAlgo::ONES_COMPLEMENT)
    return static_cast<uint8_t>(~r);
  return r;
}

uint8_t AutoProbingEngine::calculateChecksum(ChecksumAlgo algo,
                                             const uint8_t *data,
                                             size_t len) const {
  if (!data || len < 3)
    return 0;
  return calculateChecksum(algo, span<const uint8_t>(data, len));
}

void AutoProbingEngine::initFromNvs() {
  VendorProfileDescriptor prof;
  if (!ProfileRepository::getActiveProfile(prof))
    return;

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
      if (_desc.gw_addr_offset == _desc.dev_id_offset ||
          _desc.gw_addr_offset != 2) {
        _desc.gw_addr_offset = 2;
        _desc.gw_addr = prof.gw_addr ? prof.gw_addr : 0x01;
      }
      if (_desc.sub1_offset == 2 && _desc.sub2_offset > 2)
        _desc.sub1_offset = _desc.sub2_offset;
    }
    _desc.learned_query_len =
        (prof.learned_query_len >= 3) ? prof.learned_query_len : 11;
    _desc.len_offset = prof.len_offset;
    _desc.has_len_field = (prof.has_len_field != 0);
    _desc.seq_offset = prof.seq_offset;
    _desc.has_seq_counter = (prof.seq_offset != 0xFF);
    _desc.ack_flag_offset = prof.ack_flag_offset;
    _desc.ctrl_len_cnt = prof.ctrl_len_cnt;
    memcpy(_desc.learned_ctrl_lens, prof.learned_ctrl_lens,
           sizeof(_desc.learned_ctrl_lens));
    if (_desc.control_seen && _desc.ctrl_len_cnt == 0) {
      _desc.learned_ctrl_lens[0] = _desc.learned_query_len;
      _desc.ctrl_len_cnt = 1;
    }

    int max_hdr =
        std::max({_desc.opcode_offset, _desc.dev_id_offset, _desc.sub1_offset,
                  _desc.sub2_offset, _desc.gw_addr_offset});
    for (uint8_t o :
         {_desc.len_offset, _desc.seq_offset, _desc.ack_flag_offset})
      if (o != 0xFF && o > max_hdr)
        max_hdr = o;
    _desc.payload_offset = static_cast<uint8_t>(std::max(max_hdr + 1, 8));
    _desc.offsets_locked = true;
    _desc.opcodes_locked = true;
  }

  _desc.is_locked = true;
  _consecutive_mismatches = 0;
  snprintf(_desc.description, sizeof(_desc.description),
           "Restored: %s (STX 0x%02X ETX 0x%02X / %s)", prof.name, prof.stx,
           prof.etx, getAlgoName(prof.cs_algo));
}

void AutoProbingEngine::feedFrame(span<const uint8_t> f) {
  if (f.size() < 3 || f.size() > 64)
    return;

  bool should_sync = false;
  AutoProbeDescriptor snap;
  {
    CriticalSectionLocker lock(&_mux);
    _desc.tested_packets++;

    const uint8_t stx = f[0], etx = f[f.size() - 1],
                  actual_cs = f[f.size() - 2];
    _stx_counts[stx]++;
    _etx_counts[etx]++;

    if (_desc.is_locked) {
      const bool ok = stx == _desc.stx && etx == _desc.etx &&
                      (_desc.checksum_algo == ChecksumAlgo::NONE ||
                       calculateChecksum(_desc.checksum_algo, f.data(),
                                         f.size()) == actual_cs);
      if (ok) {
        _desc.matched_packets++;
        _consecutive_mismatches = 0;
      } else if (++_consecutive_mismatches >= 5) { // 5연속 불일치 → 재학습
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
      return;
    }

    uint16_t max_stx = 0, max_etx = 0;
    const uint8_t best_stx = argmax256(_stx_counts, max_stx);
    const uint8_t best_etx = argmax256(_etx_counts, max_etx);

    ChecksumAlgo matched = ChecksumAlgo::UNKNOWN;
    for (uint8_t a = 1; a <= 7; ++a) {
      ChecksumAlgo algo = static_cast<ChecksumAlgo>(a);
      if (calculateChecksum(algo, f.data(), f.size()) == actual_cs) {
        _algo_matches[a]++;
        matched = algo;
        break;
      }
    }
    if (matched == ChecksumAlgo::UNKNOWN)
      return;

    if (matched != _candidate_algo) {
      _candidate_algo = matched;
      _consecutive_matches = 1;
      return;
    }
    if (++_consecutive_matches >= 10 && max_stx >= 5 && max_etx >= 5) {
      _desc.is_locked = true;
      _desc.stx = best_stx;
      _desc.etx = best_etx;
      _desc.min_len = 3;
      _desc.max_len = 64;
      _desc.checksum_algo = matched;
      _desc.matched_packets = _desc.tested_packets;
      _consecutive_mismatches = 0;
      snprintf(_desc.description, sizeof(_desc.description),
               "Locked: STX 0x%02X ETX 0x%02X (%s)", best_stx, best_etx,
               getAlgoName(matched));
      should_sync = true;
      snap = _desc;
    }
  }
  if (should_sync)
    ProfileRepository::syncAutoProfileToNvs(snap);
}

void AutoProbingEngine::feedOpcodePair(span<const uint8_t>,
                                       span<const uint8_t>) {
  // 조기 opcode 잠금 제거 — analyzeCacheMatrix() 가 유일한 opcode 판정자
}

void AutoProbingEngine::feedControlFrame(span<const uint8_t> ctrl) {
  if (ctrl.size() < 5)
    return;

  bool should_sync = false;
  AutoProbeDescriptor snap;
  {
    CriticalSectionLocker lock(&_mux);
    if (ctrl.size() <= _desc.opcode_offset)
      return;
    const uint8_t op = ctrl[_desc.opcode_offset];
    if (op == _desc.query_opcode || op == _desc.ack_opcode)
      return;

    bool changed = !_desc.control_seen || _desc.control_opcode != op;
    if (changed) {
      _desc.control_opcode = op;
      _desc.control_seen = true;
      _desc.opcodes_locked = true;
    }
    changed |= addCtrlLen(_desc, static_cast<uint8_t>(ctrl.size()));
    if (changed && _desc.is_locked) {
      should_sync = true;
      snap = _desc;
    }
  }
  if (should_sync)
    ProfileRepository::syncAutoProfileToNvs(snap);
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
  AutoProbeDescriptor snap;
  {
    CriticalSectionLocker lock(&_mux);
    _desc.control_opcode = ctrl_op;
    _desc.control_seen = true;
    _desc.opcodes_locked = true;
    addCtrlLen(_desc, ctrl_len);
    if (_desc.is_locked) {
      should_sync = true;
      snap = _desc;
    }
  }
  if (should_sync)
    ProfileRepository::syncAutoProfileToNvs(snap);
}

// ----------------------------------------------------------------------------
// 전체 매트릭스 분석: 쿼리/응답 쌍의 컬럼 통계로 헤더 필드 위치를 추정
// (std::set/map 제거 → bitset/배열 사용, 반복 패턴은 람다로 공통화)
// ----------------------------------------------------------------------------
bool AutoProbingEngine::analyzeCacheMatrix() {
  const size_t online_dev_count = s_online_count_fn ? s_online_count_fn() : 0;
  if (g_polling_targets.ackedCount() < 2 && online_dev_count < 2)
    return false;

  struct PktPair {
    StaticPacket q, r;
  };
  std::vector<PktPair> pairs;
  const size_t target_count = g_polling_targets.totalCount();
  pairs.reserve(std::min<size_t>(target_count, 32));

  for (size_t i = 0; i < target_count; ++i) {
    PollingTargetEntry t;
    if (!g_polling_targets.getEntry(i, t) || !t.is_active ||
        t.raw_query_len < 4 ||
        !(t.source_channels & kWallpadChMask)) // 월패드(CH2/CH3) 유래만 분석
      continue;

    const uint8_t *ack = nullptr;
    size_t ack_len = 0;
    if (t.raw_ack_len >= 4) {
      ack = t.raw_ack_data.data();
      ack_len = t.raw_ack_len;
    } else if (s_lookup_fn) {
      const uint8_t *dev_ack = nullptr;
      size_t dev_ack_len = 0;
      if (s_lookup_fn(t.dev_id, t.sub1, t.sub2, &dev_ack, &dev_ack_len)) {
        ack = dev_ack;
        ack_len = dev_ack_len;
      }
    }
    if (!ack)
      continue;

    PktPair p;
    p.q.length = t.raw_query_len;
    memcpy(p.q.data.data(), t.raw_query_data.data(), t.raw_query_len);
    p.r.length = ack_len;
    memcpy(p.r.data.data(), ack, ack_len);
    pairs.push_back(p);
  }

  const size_t N = pairs.size();
  if (N < 2)
    return false;

  size_t min_len = 256;
  for (const auto &p : pairs)
    min_len = std::min<size_t>({min_len, p.q.length, p.r.length});
  if (min_len < 4)
    return false;
  const size_t K = min_len - 1; // 스캔 대상: 컬럼 1 .. K-1

  using Bits = std::bitset<256>;
  auto all = [&](auto &&pred) {
    for (const auto &p : pairs)
      if (!pred(p))
        return false;
    return true;
  };
  auto qBits = [&](size_t k) {
    Bits b;
    for (const auto &p : pairs)
      b.set(p.q.data[k]);
    return b;
  };
  auto rBits = [&](size_t k) {
    Bits b;
    for (const auto &p : pairs)
      b.set(p.r.data[k]);
    return b;
  };
  auto in = [](int k, std::initializer_list<int> ex) {
    for (int x : ex)
      if (x == k)
        return true;
    return false;
  };
  auto combo = [&](size_t a, size_t b) { // (a,b) 컬럼 조합의 고유 개수
    std::vector<uint16_t> v;
    v.reserve(N);
    for (const auto &p : pairs)
      v.push_back(static_cast<uint16_t>((p.q.data[a] << 8) | p.q.data[b]));
    std::sort(v.begin(), v.end());
    return static_cast<size_t>(std::unique(v.begin(), v.end()) - v.begin());
  };

  // 1) 길이 필드: 값 == 길이 - delta (delta 0 = 정확히 일치)
  int len_idx = -1;
  for (size_t k = 1; k < K && len_idx < 0; ++k) {
    for (int delta : {0, 2, 3, 4, 5}) {
      if (all([&](const PktPair &p) {
            return p.q.data[k] == int(p.q.length) - delta &&
                   p.r.data[k] == int(p.r.length) - delta;
          })) {
        len_idx = static_cast<int>(k);
        break;
      }
    }
  }

  // 2) Opcode: 쿼리/응답 값이 다르면서 모든 쌍에서 동일한 컬럼
  int opcode_idx = -1;
  uint8_t learned_q_op = 0, learned_ack_op = 0;
  for (size_t k = 1; k < K; ++k) {
    if (int(k) == len_idx)
      continue;
    const uint8_t cq = pairs[0].q.data[k], cr = pairs[0].r.data[k];
    if (cq == cr)
      continue;
    if (all([&](const PktPair &p) {
          return p.q.data[k] == cq && p.r.data[k] == cr;
        })) {
      opcode_idx = int(k);
      learned_q_op = cq;
      learned_ack_op = cr;
      break;
    }
  }

  // 3) 주소 swap (쿼리의 i,j 가 응답에서 교차)
  int swap_i = -1, swap_j = -1, promoted_dev_idx = -1, master_gw_idx = -1;
  for (size_t i = 1; i < K && swap_i < 0; ++i) {
    if (in(int(i), {len_idx, opcode_idx}))
      continue;
    for (size_t j = i + 1; j < K; ++j) {
      if (in(int(j), {len_idx, opcode_idx}))
        continue;
      if (!all([&](const PktPair &p) {
            return p.q.data[i] == p.r.data[j] && p.q.data[j] == p.r.data[i] &&
                   p.q.data[i] != p.q.data[j];
          }))
        continue;
      swap_i = int(i);
      swap_j = int(j);
      const size_t ci = qBits(i).count(), cj = qBits(j).count();
      if (ci == 1 && cj > 1) {
        master_gw_idx = int(i);
        promoted_dev_idx = int(j);
      } else if (cj == 1 && ci > 1) {
        master_gw_idx = int(j);
        promoted_dev_idx = int(i);
      } else {
        master_gw_idx = int(i);
        promoted_dev_idx = int(j);
      }
      break;
    }
  }

  // 4) 시퀀스 카운터 (+1 씩 증가하는 컬럼)
  int seq_idx = -1;
  if (N >= 4) {
    for (size_t k = 1; k < K && seq_idx < 0; ++k) {
      if (in(int(k), {len_idx, opcode_idx, swap_i, swap_j, promoted_dev_idx}))
        continue;
      if (qBits(k).count() < 3)
        continue;
      bool inc = true;
      for (size_t m = 0; m + 1 < N && inc; ++m)
        inc = static_cast<uint8_t>(pairs[m + 1].q.data[k] -
                                   pairs[m].q.data[k]) == 1;
      if (inc)
        seq_idx = int(k);
    }
  }

  // 5) 서브 커맨드 / 마스터(게이트웨이) 주소
  int sub_cmd_idx = -1;
  for (size_t k = 1; k < K; ++k) {
    if (in(int(k),
           {len_idx, opcode_idx, swap_i, swap_j, seq_idx, promoted_dev_idx}))
      continue;
    if (!all([&](const PktPair &p) { return p.q.data[k] == p.r.data[k]; }))
      continue;

    if (qBits(k).count() == 1) {
      if (master_gw_idx < 0)
        master_gw_idx = int(k);
    } else if (promoted_dev_idx >= 0) {
      int16_t dep[256];
      std::fill(std::begin(dep), std::end(dep), int16_t(-1));
      bool pure = true;
      size_t keys = 0;
      for (const auto &p : pairs) {
        const uint8_t dt = p.q.data[promoted_dev_idx], sc = p.q.data[k];
        if (dep[dt] >= 0 && dep[dt] != sc) {
          pure = false;
          break;
        }
        if (dep[dt] < 0)
          ++keys;
        dep[dt] = sc;
      }
      if (pure && keys > 1) {
        sub_cmd_idx = int(k);
        break;
      }
    }
  }

  // 6) 기기 타입 / 서브 ID 후보
  std::vector<size_t> cols;
  for (size_t k = 1; k < K; ++k) {
    if (in(int(k), {len_idx, opcode_idx, swap_i, swap_j, seq_idx, sub_cmd_idx,
                    promoted_dev_idx}))
      continue;
    if (qBits(k).count() >= 2)
      cols.push_back(k);
  }

  int dev_type_idx = promoted_dev_idx, sub_id_idx = -1;
  const size_t min_unique = std::max<size_t>(2, N * 8 / 10);

  if (dev_type_idx < 0) { // 응답 길이를 결정하는 컬럼 = 기기 타입
    for (size_t cand : cols) {
      int16_t lenOf[256];
      std::fill(std::begin(lenOf), std::end(lenOf), int16_t(-1));
      bool ok = true;
      size_t keys = 0;
      for (const auto &p : pairs) {
        const uint8_t v = p.q.data[cand];
        const int16_t rl = static_cast<int16_t>(p.r.length);
        if (lenOf[v] >= 0 && lenOf[v] != rl) {
          ok = false;
          break;
        }
        if (lenOf[v] < 0)
          ++keys;
        lenOf[v] = rl;
      }
      if (!ok || keys <= 1)
        continue;
      Bits distinct;
      for (int16_t l : lenOf)
        if (l >= 0)
          distinct.set(static_cast<size_t>(l));
      if (distinct.count() > 1) {
        dev_type_idx = int(cand);
        break;
      }
    }
  }

  if (dev_type_idx >= 0) {
    for (size_t cand : cols) {
      if (int(cand) == dev_type_idx)
        continue;
      if (combo(dev_type_idx, cand) >= min_unique) {
        sub_id_idx = int(cand);
        break;
      }
    }
  } else {
    for (size_t c1 : cols) {
      for (size_t c2 : cols) {
        if (c1 == c2 || combo(c1, c2) < min_unique)
          continue;
        const bool c1_is_dev = qBits(c1).count() <= qBits(c2).count();
        dev_type_idx = int(c1_is_dev ? c1 : c2);
        sub_id_idx = int(c1_is_dev ? c2 : c1);
        break;
      }
      if (dev_type_idx >= 0)
        break;
    }
  }

  // 7) ACK 플래그 (응답에서 항상 0x00/0x01 고정인 헤더 내부 컬럼)
  int ack_flag_idx = -1;
  const int max_known_hdr =
      std::max({opcode_idx, dev_type_idx, sub_cmd_idx, sub_id_idx, swap_i,
                swap_j, len_idx, seq_idx});
  for (size_t k = 1; k < K; ++k) {
    const int ik = int(k);
    if (in(ik, {len_idx, opcode_idx, swap_i, swap_j, seq_idx, sub_cmd_idx,
                dev_type_idx, sub_id_idx}) ||
        ik > max_known_hdr)
      continue;
    const Bits rb = rBits(k);
    if (rb.count() == 1 && (rb.test(0x00) || rb.test(0x01))) {
      ack_flag_idx = ik;
      break;
    }
  }

  // 8) 디스크립터 반영
  AutoProbeDescriptor snap;
  {
    CriticalSectionLocker lock(&_mux);
    if (opcode_idx >= 0) {
      _desc.opcode_offset = uint8_t(opcode_idx);
      _desc.query_opcode = learned_q_op;
      _desc.ack_opcode = learned_ack_op;
      _desc.opcodes_locked = true;
    }
    if (dev_type_idx >= 0)
      _desc.dev_id_offset = uint8_t(dev_type_idx);
    if (sub_cmd_idx >= 0)
      _desc.sub1_offset = uint8_t(sub_cmd_idx);
    else if (sub_id_idx >= 0)
      _desc.sub1_offset = uint8_t(sub_id_idx);
    if (sub_id_idx >= 0)
      _desc.sub2_offset = uint8_t(sub_id_idx);

    _desc.len_offset = (len_idx >= 0) ? uint8_t(len_idx) : 0xFF;
    _desc.has_len_field = (len_idx >= 0);
    _desc.seq_offset = (seq_idx >= 0) ? uint8_t(seq_idx) : 0xFF;
    _desc.has_seq_counter = (seq_idx >= 0);
    _desc.ack_flag_offset = (ack_flag_idx >= 0) ? uint8_t(ack_flag_idx) : 0xFF;
    _desc.is_swapped_addr = (swap_i >= 0);

    if (master_gw_idx >= 0) {
      _desc.gw_addr_offset = uint8_t(master_gw_idx);
      _desc.gw_addr = pairs[0].q.data[master_gw_idx];
    } else if (!_desc.is_swapped_addr && dev_type_idx >= 0) {
      _desc.gw_addr_offset = _desc.dev_id_offset;
    }

    if (min_len >= 5 && min_len <= 64)
      _desc.learned_query_len = uint8_t(min_len);
    _desc.offsets_locked = (dev_type_idx >= 0 && sub_id_idx >= 0);

    const int max_hdr =
        std::max({0, opcode_idx, dev_type_idx, sub_cmd_idx, sub_id_idx, swap_i,
                  swap_j, len_idx, seq_idx, ack_flag_idx});

    // 제로 분산 패딩 스킵: max_hdr 직후 모든 응답에서 0x00 고정인 열
    int payload_start = max_hdr + 1;
    while (payload_start < int(min_len) - 2 && all([&](const PktPair &p) {
             return p.r.data[payload_start] == 0x00;
           }))
      ++payload_start;
    _desc.payload_offset = uint8_t(payload_start);

    snprintf(_desc.description, sizeof(_desc.description),
             "Auto: OP@%u(0x%02X/0x%02X) DEV@%u SUB1@%u SUB2@%u PL@%u",
             _desc.opcode_offset, _desc.query_opcode, _desc.ack_opcode,
             _desc.dev_id_offset, _desc.sub1_offset, _desc.sub2_offset,
             _desc.payload_offset);
    snap = _desc;
  }

  ProfileRepository::syncAutoProfileToNvs(snap);

  if (snap.offsets_locked) {
    g_polling_targets.reindexWithOffsets(snap.dev_id_offset, snap.sub1_offset,
                                         snap.sub2_offset);
    for (size_t i = 0; i < target_count; ++i) {
      PollingTargetEntry t;
      if (g_polling_targets.getEntry(i, t) && t.is_active &&
          t.raw_ack_len >= 4) {
        StaticPacket ack_pkt;
        ack_pkt.channel_id = 1;
        ack_pkt.length = t.raw_ack_len;
        memcpy(ack_pkt.data.data(), t.raw_ack_data.data(), t.raw_ack_len);
        if (s_update_from_bus_fn) {
          s_update_from_bus_fn(ack_pkt);
        }
      }
    }
  }

  System_TraceMessage("[AUTO PROBE] ★ Full-Matrix Cache Analysis Complete! "
                      "Offsets locked & saved to NVS.\r\n");
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
  _desc.ack_opcode = 0x04;
  _desc.len_offset = 0xFF;
  _desc.seq_offset = 0xFF;
  _desc.ack_flag_offset = 0xFF;
  _desc.payload_offset = 7;
  _consecutive_mismatches = 0;
  strncpy(_desc.description, "Probing bus traffic...",
          sizeof(_desc.description) - 1);
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
// Control: DeviceRouteRegistry
// ============================================================================

