// ============================================================================
// WallpadRegistry: Level 3 Wallpad Device Learning, Polling & Control SSOT Domain
// Implementation
// ============================================================================

#include "L3_Protocol/Private/Wallpad_Learning.h"
#include "L3_Protocol/Private/Wallpad_Engine.h"
#include "L0_Foundation/System_Buffer.h"

#include <Arduino.h>
#include <Preferences.h>
#include <algorithm>
#include <array>
#include <bitset>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <span>
#include <utility>

// ============================================================================
// PART 1: AUTO-PROBING ENGINE IMPLEMENTATION
// ============================================================================
// ============================================================================
// WallpadProtocol: Level 3 Wallpad Profiles, Protocol Engine & Probing Cache
// ============================================================================



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

static AutoProbingEngine s_auto_probing_engine;

AutoProbingEngine &AutoProbe_GetEngine() noexcept {
  return s_auto_probing_engine;
}

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

using ChecksumFunc = uint8_t (*)(const uint8_t *p, size_t end) noexcept;

inline uint8_t calcXorAll(const uint8_t *p, size_t end) noexcept {
  uint8_t r = 0;
  for (size_t i = 0; i < end; ++i) r ^= p[i];
  return r;
}

inline uint8_t calcXorNoStx(const uint8_t *p, size_t end) noexcept {
  uint8_t r = 0;
  for (size_t i = 1; i < end; ++i) r ^= p[i];
  return r;
}

inline uint8_t calcSumAll(const uint8_t *p, size_t end) noexcept {
  uint8_t r = 0;
  for (size_t i = 0; i < end; ++i) r += p[i];
  return r;
}

inline uint8_t calcSumNoStx(const uint8_t *p, size_t end) noexcept {
  uint8_t r = 0;
  for (size_t i = 1; i < end; ++i) r += p[i];
  return r;
}

inline uint8_t calcTwosComp(const uint8_t *p, size_t end) noexcept {
  return static_cast<uint8_t>(-calcSumNoStx(p, end));
}

inline uint8_t calcOnesComp(const uint8_t *p, size_t end) noexcept {
  return static_cast<uint8_t>(~calcSumAll(p, end));
}

inline uint8_t calcCrc8Maxim(const uint8_t *p, size_t end) noexcept {
  return crc8Poly31(p, end);
}

constexpr ChecksumFunc CHECKSUM_DISPATCH_TABLE[] = {
    nullptr,        // 0: UNKNOWN
    calcXorAll,     // 1: XOR_ALL
    calcXorNoStx,   // 2: XOR_NO_STX
    calcSumAll,     // 3: SUM_ALL
    calcSumNoStx,   // 4: SUM_NO_STX
    calcTwosComp,   // 5: TWOS_COMPLEMENT
    calcOnesComp,   // 6: ONES_COMPLEMENT
    calcCrc8Maxim,  // 7: CRC8_MAXIM
    nullptr         // 8: NONE
};

} // namespace

uint8_t AutoProbingEngine::calculateChecksum(ChecksumAlgo algo,
                                             std::span<const uint8_t> data) const noexcept {
  const size_t len = data.size();
  if (UNLIKELY(len < 3))
    return 0;

  const size_t idx = static_cast<size_t>(algo);
  if (LIKELY(idx < std::size(CHECKSUM_DISPATCH_TABLE) && CHECKSUM_DISPATCH_TABLE[idx])) {
    return CHECKSUM_DISPATCH_TABLE[idx](data.data(), len - 2);
  }
  return 0;
}

uint8_t AutoProbingEngine::calculateChecksum(ChecksumAlgo algo,
                                             const uint8_t *data,
                                             size_t len) const {
  if (!data || len < 3)
    return 0;
  return calculateChecksum(algo, std::span<const uint8_t>(data, len));
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

void AutoProbingEngine::feedFrame(std::span<const uint8_t> f) {
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
                       calculateChecksum(_desc.checksum_algo, f) == actual_cs);
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

    const size_t payload_len = f.size() - 2;
    uint8_t xor_all = 0;
    uint8_t sum_all = 0;
    for (size_t i = 0; i < payload_len; ++i) {
      xor_all ^= f[i];
      sum_all += f[i];
    }
    const uint8_t sum_no_stx = static_cast<uint8_t>(sum_all - f[0]);

    const uint8_t candidates[] = {
        xor_all,                              // 1: XOR_ALL
        static_cast<uint8_t>(xor_all ^ f[0]), // 2: XOR_NO_STX
        sum_all,                              // 3: SUM_ALL
        sum_no_stx,                           // 4: SUM_NO_STX
        static_cast<uint8_t>(-sum_no_stx),    // 5: TWOS_COMPLEMENT
        static_cast<uint8_t>(~sum_all),       // 6: ONES_COMPLEMENT
    };

    ChecksumAlgo matched = ChecksumAlgo::UNKNOWN;
    for (size_t i = 0; i < sizeof(candidates); ++i) {
      if (candidates[i] == actual_cs) {
        matched = static_cast<ChecksumAlgo>(i + 1);
        break;
      }
    }

    if (matched == ChecksumAlgo::UNKNOWN &&
        crc8Poly31(f.data(), payload_len) == actual_cs) {
      matched = ChecksumAlgo::CRC8_MAXIM;
    }

    if (matched == ChecksumAlgo::UNKNOWN)
      return;

    _algo_matches[std::to_underlying(matched)]++;

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

void AutoProbingEngine::feedOpcodePair(std::span<const uint8_t>,
                                       std::span<const uint8_t>) {
  // 조기 opcode 잠금 제거 — analyzeCacheMatrix() 가 유일한 opcode 판정자
}

void AutoProbingEngine::feedControlFrame(std::span<const uint8_t> ctrl) {
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
namespace {
// ── Matrix Analysis Structures ──────────────────────────────────────────────
// Header analysis only inspects first 32 bytes (min_len <= 32).
struct SlimPacket {
  uint8_t length{0};
  std::array<uint8_t, 32> data{};
};

struct SlimPktPair {
  SlimPacket q;
  SlimPacket r;
};

// Strict: Task_Ch1 exclusive path (Single-caller invariant).
// Static allocation in BSS prevents Task_Ch1 stack overflow (~5KB saved).
constexpr size_t MAX_MATRIX_PAIRS = 32;
static SlimPktPair s_matrix_pairs[MAX_MATRIX_PAIRS];
static int16_t s_work_map[256];
} // namespace

// ----------------------------------------------------------------------------
// 전체 매트릭스 분석: 쿼리/응답 쌍의 컬럼 통계로 헤더 필드 위치를 추정
// (std::set/map 제거 → bitset/배열 사용, 반복 패턴은 람다로 공통화)
// Strict: Task_Ch1 exclusive path.
// ----------------------------------------------------------------------------
bool AutoProbingEngine::analyzeCacheMatrix() {
  const size_t online_dev_count = s_online_count_fn ? s_online_count_fn() : 0;
  if (Polling_GetRegistry().ackedCount() < 2 && online_dev_count < 2)
    return false;

  // Re-initialize memory on every entry to guarantee zero residue from previous runs.
  memset(s_matrix_pairs, 0, sizeof(s_matrix_pairs));
  std::fill(std::begin(s_work_map), std::end(s_work_map), int16_t(-1));
  size_t pair_count = 0;
  const size_t target_count = Polling_GetRegistry().totalCount();

  for (size_t i = 0; i < target_count && pair_count < MAX_MATRIX_PAIRS; ++i) {
    PollingTargetEntry t;
    if (!Polling_GetRegistry().getEntry(i, t) || !t.is_active ||
        t.raw_query_len < 4 ||
        !(t.source_channels & kWallpadChMask)) // 월패드(CH2/CH3) 유래만 분석
      continue;

    const uint8_t *ack = nullptr;
    size_t ack_len = 0;
    if (t.raw_ack_len >= 4) {
      ack = t.raw_ack_data.data();
      ack_len = t.raw_ack_len;
    } else if (s_lookup_fn) {
      size_t dev_ack_len = 0;
      if (s_lookup_fn(t.dev_id, t.sub1, t.sub2,
                      s_matrix_pairs[pair_count].r.data.data(),
                      s_matrix_pairs[pair_count].r.data.size(),
                      &dev_ack_len)) {
        ack = s_matrix_pairs[pair_count].r.data.data();
        ack_len = dev_ack_len;
      }
    }
    if (!ack)
      continue;

    SlimPktPair &p = s_matrix_pairs[pair_count++];
    p.q.length = static_cast<uint8_t>(std::min<size_t>(t.raw_query_len, 32));
    memcpy(p.q.data.data(), t.raw_query_data.data(), p.q.length);
    p.r.length = static_cast<uint8_t>(std::min<size_t>(ack_len, 32));
    memcpy(p.r.data.data(), ack, p.r.length);
  }

  const size_t N = pair_count;
  if (N < 2)
    return false;

  auto pairs_span = std::span<const SlimPktPair>(s_matrix_pairs, N);

  size_t min_len = 256;
  for (const auto &p : pairs_span)
    min_len = std::min<size_t>({min_len, p.q.length, p.r.length});
  if (min_len < 4)
    return false;
  const size_t K = min_len - 1; // 스캔 대상: 컬럼 1 .. K-1

  using Bits = std::bitset<256>;
  auto all = [&](auto &&pred) {
    for (const auto &p : pairs_span)
      if (!pred(p))
        return false;
    return true;
  };
  auto qBits = [&](size_t k) {
    Bits b;
    for (const auto &p : pairs_span)
      b.set(p.q.data[k]);
    return b;
  };
  auto rBits = [&](size_t k) {
    Bits b;
    for (const auto &p : pairs_span)
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
    std::array<uint16_t, MAX_MATRIX_PAIRS> v{};
    for (size_t i = 0; i < N; ++i)
      v[i] = static_cast<uint16_t>((s_matrix_pairs[i].q.data[a] << 8) | s_matrix_pairs[i].q.data[b]);
    std::sort(v.begin(), v.begin() + N);
    return static_cast<size_t>(std::unique(v.begin(), v.begin() + N) - v.begin());
  };

  // 1) 길이 필드: 값 == 길이 - delta (delta 0 = 정확히 일치)
  int len_idx = -1;
  for (size_t k = 1; k < K && len_idx < 0; ++k) {
    for (int delta : {0, 2, 3, 4, 5}) {
      if (all([&](const SlimPktPair &p) {
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
    const uint8_t cq = s_matrix_pairs[0].q.data[k], cr = s_matrix_pairs[0].r.data[k];
    if (cq == cr)
      continue;
    if (all([&](const SlimPktPair &p) {
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
      if (!all([&](const SlimPktPair &p) {
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
        inc = static_cast<uint8_t>(s_matrix_pairs[m + 1].q.data[k] -
                                   s_matrix_pairs[m].q.data[k]) == 1;
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
    if (!all([&](const SlimPktPair &p) { return p.q.data[k] == p.r.data[k]; }))
      continue;

    if (qBits(k).count() == 1) {
      if (master_gw_idx < 0)
        master_gw_idx = int(k);
    } else if (promoted_dev_idx >= 0) {
      std::fill(std::begin(s_work_map), std::end(s_work_map), int16_t(-1));
      bool pure = true;
      size_t keys = 0;
      for (const auto &p : pairs_span) {
        const uint8_t dt = p.q.data[promoted_dev_idx], sc = p.q.data[k];
        if (s_work_map[dt] >= 0 && s_work_map[dt] != sc) {
          pure = false;
          break;
        }
        if (s_work_map[dt] < 0)
          ++keys;
        s_work_map[dt] = sc;
      }
      if (pure && keys > 1) {
        sub_cmd_idx = int(k);
        break;
      }
    }
  }

  // 6) 기기 타입 / 서브 ID 후보
  std::array<size_t, 32> cols{};
  size_t col_count = 0;
  for (size_t k = 1; k < K && col_count < cols.size(); ++k) {
    if (in(int(k), {len_idx, opcode_idx, swap_i, swap_j, seq_idx, sub_cmd_idx,
                    promoted_dev_idx}))
      continue;
    if (qBits(k).count() >= 2)
      cols[col_count++] = k;
  }
  auto cols_span = std::span<const size_t>(cols.data(), col_count);

  int dev_type_idx = promoted_dev_idx, sub_id_idx = -1;
  const size_t min_unique = std::max<size_t>(2, N * 8 / 10);

  if (dev_type_idx < 0) { // 응답 길이를 결정하는 컬럼 = 기기 타입
    for (size_t cand : cols_span) {
      std::fill(std::begin(s_work_map), std::end(s_work_map), int16_t(-1));
      bool ok = true;
      size_t keys = 0;
      for (const auto &p : pairs_span) {
        const uint8_t v = p.q.data[cand];
        const int16_t rl = static_cast<int16_t>(p.r.length);
        if (s_work_map[v] >= 0 && s_work_map[v] != rl) {
          ok = false;
          break;
        }
        if (s_work_map[v] < 0)
          ++keys;
        s_work_map[v] = rl;
      }
      if (!ok || keys <= 1)
        continue;
      Bits distinct;
      for (int16_t l : s_work_map)
        if (l >= 0)
          distinct.set(static_cast<size_t>(l));
      if (distinct.count() > 1) {
        dev_type_idx = int(cand);
        break;
      }
    }
  }

  if (dev_type_idx >= 0) {
    for (size_t cand : cols_span) {
      if (int(cand) == dev_type_idx)
        continue;
      if (combo(dev_type_idx, cand) >= min_unique) {
        sub_id_idx = int(cand);
        break;
      }
    }
  } else {
    for (size_t c1 : cols_span) {
      for (size_t c2 : cols_span) {
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
      _desc.gw_addr = s_matrix_pairs[0].q.data[master_gw_idx];
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
    while (payload_start < int(min_len) - 2 && all([&](const SlimPktPair &p) {
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
    Polling_GetRegistry().reindexWithOffsets(snap.dev_id_offset, snap.sub1_offset,
                                         snap.sub2_offset);
    for (size_t i = 0; i < target_count; ++i) {
      PollingTargetEntry t;
      if (Polling_GetRegistry().getEntry(i, t) && t.is_active &&
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
  _desc.description[sizeof(_desc.description) - 1] = '\0';
  memset(_stx_counts, 0, sizeof(_stx_counts));
  memset(_etx_counts, 0, sizeof(_etx_counts));
  memset(_algo_matches, 0, sizeof(_algo_matches));
  memset(_diff_idx_counts, 0, sizeof(_diff_idx_counts));
  _consecutive_matches = 0;
  _candidate_algo = ChecksumAlgo::UNKNOWN;
  _control_matches = 0;
  _candidate_ctrl_op = 0;
  Wallpad_InvalidateProfileCache();
}

// ============================================================================
// Control: DeviceRouteRegistry
// ============================================================================


// ============================================================================
// PART 2: POLLING TARGET REGISTRY & WARM-START CACHE IMPLEMENTATION
// ============================================================================
// ============================================================================
// WallpadProtocol: Level 3 Wallpad Profiles, Protocol Engine & Probing Cache
// ============================================================================



template <class F> static inline size_t countIf(const PollingTargetEntry *e, size_t n, F f) {
  size_t c = 0;
  for (size_t i = 0; i < n; ++i)
    c += f(e[i]) ? 1 : 0;
  return c;
}


RTC_NOINIT_ATTR RtcWarmCache rtc_warm_cache;

static bool s_warm_cache_loaded = false;
static uint8_t s_warm_cache_source = 0; // 0: None/Cold, 1: RTC SRAM, 2: NVS Flash
static uint8_t s_warm_cache_restored_count = 0;
static std::atomic<bool> s_warm_cache_dirty{false};
static std::atomic<uint32_t> s_warm_cache_dirty_ms{0};

WarmCacheStatus WarmCache_GetStatus() noexcept {
  return WarmCacheStatus{
      .loaded = s_warm_cache_loaded,
      .source = s_warm_cache_source,
      .restored_count = s_warm_cache_restored_count,
      .dirty = s_warm_cache_dirty.load(std::memory_order_relaxed),
      .dirty_ms = s_warm_cache_dirty_ms.load(std::memory_order_relaxed),
  };
}

namespace {
static NvsEnvelope<RtcWarmCache> s_warm_cache_env;
} // anonymous namespace

void WarmCache_SaveToRtc() {
  memset(&rtc_warm_cache, 0, sizeof(rtc_warm_cache));
  rtc_warm_cache.magic = RTC_MAGIC_WARM_CACHE;
  rtc_warm_cache.count = static_cast<uint8_t>(
      Polling_GetRegistry().getWarmCacheEntries(rtc_warm_cache.entries, PollingTargetRegistry::MAX_TARGETS));
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
  s_warm_cache_dirty.store(false, std::memory_order_release);
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
      Polling_GetRegistry().loadFromWarmCache(rtc_warm_cache.entries,
                                          rtc_warm_cache.count, now);
      s_warm_cache_loaded = true;
      s_warm_cache_source = 1;
      s_warm_cache_restored_count = rtc_warm_cache.count;
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
            Polling_GetRegistry().loadFromWarmCache(
                s_warm_cache_env.payload.entries,
                s_warm_cache_env.payload.count, now);
            s_warm_cache_loaded = true;
            s_warm_cache_source = 2;
            s_warm_cache_restored_count = s_warm_cache_env.payload.count;
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

  s_warm_cache_loaded = false;
  s_warm_cache_source = 0;
  s_warm_cache_restored_count = 0;
  ::Serial.println(
      F("[WARM CACHE] Cold start initialized (No prior cache found)."));
}

void WarmCache_CheckNvsDebounce() {
  if (s_warm_cache_dirty.load(std::memory_order_acquire)) {
    uint32_t dirty_ms = s_warm_cache_dirty_ms.load(std::memory_order_relaxed);
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

static PollingTargetRegistry s_polling_targets;

PollingTargetRegistry &Polling_GetRegistry() noexcept {
  return s_polling_targets;
}

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
    s_warm_cache_dirty.store(true, std::memory_order_release);
    s_warm_cache_dirty_ms.store(now, std::memory_order_release);
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


// ============================================================================
// PART 3: GROUP CONTROL TEMPLATE REGISTRY IMPLEMENTATION
// ============================================================================
// ============================================================================
// ControlTemplate: Level 3 Device Capability Blueprint Implementation
// ============================================================================



using namespace ControlTemplateUtils;

// ============================================================================
// Control: ControlTemplateRegistry
// ============================================================================

static ControlTemplateRegistry s_control_registry;

ControlTemplateRegistry &Control_GetRegistry() noexcept {
  return s_control_registry;
}

static GroupControlTemplate
    s_nvs_transfer_buf[ControlTemplateRegistry::MAX_GROUPS];

namespace {

// 슬롯에 category/값을 기록하는 공통 로직 (모든 액션 빌더가 공유)
bool applySlot(const ActionSlot &s, uint8_t val, const GroupControlTemplate &g,
               StaticPacket &out) {
  if (!s.discovered)
    return false;
  if (s.category_offset < g.frame_len)
    out.data[s.category_offset] = s.category_val;
  if (s.action_offset < g.frame_len)
    out.data[s.action_offset] = val;
  return true;
}

bool buildActionPower(const GroupControlTemplate &g, int value,
                      StaticPacket &out) {
  if (value > 0 && g.coverage.dev_class == DeviceClass::GAS)
    return false; // 가스 원격 열림 방지
  const uint8_t val = (value == 2 && g.away_mode_token != 0) ? g.away_mode_token
                      : (value > 0) ? g.power_slot.on_val
                                    : g.power_slot.off_val;
  return applySlot(g.power_slot, val, g, out);
}

bool buildActionSetTemp(const GroupControlTemplate &g, int value,
                        StaticPacket &out) {
  return applySlot(g.temp_slot, static_cast<uint8_t>(constrain(value, 5, 35)),
                   g, out);
}

bool buildActionFanSpeed(const GroupControlTemplate &g, int value,
                         StaticPacket &out) {
  const ActionSlot &s = g.speed_slot;
  uint8_t token;
  if (s.level_count > 0) {
    token = s.level_tokens[constrain(value - 1, 0, s.level_count - 1)];
  } else {
    const uint8_t mn = s.min_val ? s.min_val : 1,
                  mx = s.max_val ? s.max_val : 3;
    token = static_cast<uint8_t>(constrain(value, mn, mx));
  }
  return applySlot(s, token, g, out);
}

bool buildActionValveClose(const GroupControlTemplate &g, int,
                           StaticPacket &out) {
  return applySlot(g.close_slot, g.close_slot.off_val, g, out);
}

bool buildActionVentMode(const GroupControlTemplate &g, int value,
                         StaticPacket &out) {
  const uint8_t mx = g.mode_slot.max_val ? g.mode_slot.max_val : 5;
  return applySlot(g.mode_slot, static_cast<uint8_t>(constrain(value, 1, mx)),
                   g, out);
}

using ActionBuilderFn = bool (*)(const GroupControlTemplate &, int,
                                 StaticPacket &);
constexpr ActionBuilderFn kActionBuilders[] = {
    buildActionPower,      // POWER
    buildActionSetTemp,    // SET_TEMP
    buildActionFanSpeed,   // FAN_SPEED
    buildActionValveClose, // VALVE_CLOSE
    buildActionPower,      // MOMENTARY_TRIGGER
    buildActionVentMode    // VENT_MODE
};
static_assert(std::size(kActionBuilders) == 6,
              "kActionBuilders size must match ControlActionType count");

void clearActionSlots(GroupControlTemplate &g) {
  g.power_slot = ActionSlot{};
  g.temp_slot = ActionSlot{};
  g.speed_slot = ActionSlot{};
  g.close_slot = ActionSlot{};
  g.mode_slot = ActionSlot{};
}

// dev_id 오름차순 정렬 삽입 (registerOrTouch / NVS 로드 공통)
GroupControlTemplate *insertSorted(GroupControlTemplate *arr, size_t &count,
                                   size_t cap, const GroupControlTemplate &g) {
  if (count >= cap)
    return nullptr;
  GroupControlTemplate *pos =
      std::find_if(arr, arr + count, [&](const GroupControlTemplate &x) {
        return x.dev_id > g.dev_id;
      });
  std::move_backward(pos, arr + count, arr + count + 1);
  *pos = g;
  ++count;
  return pos;
}

} // namespace

static ControlTemplateRegistry::DeviceUnitCountFn s_device_unit_count_fn{nullptr};

void ControlTemplateRegistry::setDeviceUnitCountProvider(DeviceUnitCountFn fn) {
  s_device_unit_count_fn = fn;
}

ControlTemplateRegistry::ControlTemplateRegistry() {
  _mutex = xSemaphoreCreateMutexStatic(&_mutex_storage);
  _nvs_mutex = xSemaphoreCreateMutexStatic(&_nvs_mutex_storage);
  clear();
}

void ControlTemplateRegistry::init() {
  ProfileRepository::setProfileChangeListener([](uint8_t old_idx, uint8_t new_idx) {
    s_control_registry.onProfileChanged(old_idx, new_idx);
  });
  loadFromNvs();
}

namespace {
struct NormSub1Rule {
  uint8_t dev_id{0};
  uint8_t temp_sub1{0};
  uint8_t speed_sub1{0};
  uint8_t power_sub1{0};
};

static NormSub1Rule s_norm_rules[ControlTemplateRegistry::MAX_GROUPS]{};
static size_t s_norm_rule_count{0};
static portMUX_TYPE s_norm_rules_mux = portMUX_INITIALIZER_UNLOCKED;
} // namespace

void ControlTemplateRegistry::rebuildNormSub1LutLocked() noexcept {
  NormSub1Rule next_rules[MAX_GROUPS]{};
  size_t next_count = 0;
  for (size_t i = 0; i < _group_count && next_count < MAX_GROUPS; ++i) {
    const auto &grp = _groups[i];
    if (grp.power_slot.category_val != 0 && grp.power_slot.category_val != 0xFF) {
      if ((grp.temp_slot.category_val != 0 && grp.temp_slot.category_val != 0xFF) ||
          (grp.speed_slot.category_val != 0 && grp.speed_slot.category_val != 0xFF)) {
        next_rules[next_count++] = NormSub1Rule{
            .dev_id = grp.dev_id,
            .temp_sub1 = grp.temp_slot.category_val,
            .speed_sub1 = grp.speed_slot.category_val,
            .power_sub1 = grp.power_slot.category_val,
        };
      }
    }
  }
  taskENTER_CRITICAL(&s_norm_rules_mux);
  for (size_t i = 0; i < next_count; ++i) {
    s_norm_rules[i] = next_rules[i];
  }
  s_norm_rule_count = next_count;
  taskEXIT_CRITICAL(&s_norm_rules_mux);
}

void ControlTemplateRegistry::clear() {
  MutexLocker lock(_mutex, kManageLockTimeout);
  if (!lock.isLocked())
    return;
  std::fill(std::begin(_groups), std::end(_groups), GroupControlTemplate{});
  _group_count = 0;
  rebuildNormSub1LutLocked();
}

void ControlTemplateRegistry::autoAssignGroupName(GroupControlTemplate &group) {
  static const char *const kPlaceholders[] = {"-",    "Gas",    "Thermo",
                                              "Vent", "Aircon", "Elevator"};
  const char *n = group.group_name;
  bool placeholder = (*n == '\0') || strncmp(n, "Unknown", 7) == 0 ||
                     strncmp(n, "Dev_0x", 6) == 0;
  for (const char *p : kPlaceholders)
    placeholder |= (strcmp(n, p) == 0);
  if (!placeholder)
    return; // 사용자 지정 이름 유지

  setStr(group.group_name, group.coverage.dev_class == DeviceClass::UNKNOWN
                               ? "-"
                               : DeviceClassToName(group.coverage.dev_class));
}

bool ControlTemplateRegistry::setGroupName(uint8_t dev_id, const char *name) {
  if (dev_id == 0 || !name || !*name)
    return false;
  {
    MutexLocker lock(_mutex, kManageLockTimeout);
    if (!lock.isLocked())
      return false;
    GroupControlTemplate *g = std::find_if(
        &_groups[0], &_groups[0] + _group_count,
        [&](const GroupControlTemplate &x) { return x.dev_id == dev_id; });
    if (g == &_groups[0] + _group_count)
      return false;
    setStr(g->group_name, name);
    if (strcasecmp(name, "Elevator") == 0 || strcasecmp(name, "EV") == 0)
      g->coverage.dev_class = DeviceClass::MOMENTARY;
    else if (strcasestr(name, "Outlet"))
      g->coverage.dev_class = DeviceClass::OUTLET;
  }
  saveToNvs();
  return true;
}

bool ControlTemplateRegistry::setGroupClass(uint8_t dev_id, DeviceClass cls,
                                            const char *name) {
  if (dev_id == 0)
    return false;
  {
    MutexLocker lock(_mutex, kManageLockTimeout);
    if (!lock.isLocked())
      return false;
    GroupControlTemplate *g = std::find_if(
        &_groups[0], &_groups[0] + _group_count,
        [&](const GroupControlTemplate &x) { return x.dev_id == dev_id; });
    if (g == &_groups[0] + _group_count)
      return false;

    if (g->coverage.dev_class != cls &&
        g->coverage.dev_class != DeviceClass::UNKNOWN) {
      g->coverage = SlotCoverage{}; // 클래스 변경 → 기존 슬롯 무효화
      clearActionSlots(*g);
    }
    g->coverage.dev_class = cls;
    if (name && *name)
      setStr(g->group_name, name);
    else
      autoAssignGroupName(*g);
  }
  saveToNvs();
  return true;
}

bool ControlTemplateRegistry::findGroup(uint8_t dev_id,
                                        GroupControlTemplate &out,
                                        TickType_t timeout) const {
  if (dev_id == 0)
    return false;
  MutexLocker lock(_mutex, timeout);
  if (!lock.isLocked())
    return false;
  for (size_t i = 0; i < _group_count; ++i) {
    if (_groups[i].dev_id == dev_id) {
      out = _groups[i];
      return true;
    }
  }
  return false;
}

size_t ControlTemplateRegistry::getGroupsSnapshot(GroupControlTemplate *out_buf,
                                                  size_t max_count,
                                                  TickType_t timeout) const {
  if (!out_buf || !max_count)
    return 0;
  MutexLocker lock(_mutex, timeout);
  if (!lock.isLocked())
    return 0;
  const size_t n = std::min(_group_count, max_count);
  std::copy(&_groups[0], &_groups[0] + n, out_buf);
  return n;
}

GroupControlTemplate *
ControlTemplateRegistry::registerOrTouchUnlocked(uint8_t dev_id,
                                                 const char *name) {
  if (dev_id == 0)
    return nullptr;

  for (size_t i = 0; i < _group_count; ++i) {
    if (_groups[i].dev_id == dev_id) {
      if (name && *name)
        setStr(_groups[i].group_name, name);
      return &_groups[i];
    }
  }

  GroupControlTemplate fresh{};
  fresh.dev_id = dev_id;
  if (name && *name)
    setStr(fresh.group_name, name);
  else
    autoAssignGroupName(fresh);
  return insertSorted(&_groups[0], _group_count, MAX_GROUPS, fresh);
}

size_t ControlTemplateRegistry::getGroupCount() const {
  MutexLocker lock(_mutex, kQueryLockTimeout);
  return lock.isLocked() ? _group_count : 0;
}

bool ControlTemplateRegistry::getGroupByIndex(size_t index,
                                              GroupControlTemplate &out) const {
  MutexLocker lock(_mutex, kQueryLockTimeout);
  if (!lock.isLocked() || index >= _group_count)
    return false;
  out = _groups[index];
  return true;
}

bool ControlTemplateRegistry::resetGroup(uint8_t dev_id, bool full_reset) {
  bool modified = false;
  {
    MutexLocker lock(_mutex, kManageLockTimeout);
    if (!lock.isLocked())
      return false;

    GroupControlTemplate *begin = &_groups[0], *end = begin + _group_count;
    if (full_reset) {
      if (dev_id == 0) {
        std::fill(std::begin(_groups), std::end(_groups),
                  GroupControlTemplate{});
        _group_count = 0;
        modified = true;
      } else if (GroupControlTemplate *g =
                     std::find_if(begin, end,
                                  [&](const GroupControlTemplate &x) {
                                    return x.dev_id == dev_id;
                                  });
                 g != end) {
        std::move(g + 1, end, g);
        *(end - 1) = GroupControlTemplate{};
        --_group_count;
        modified = true;
      }
    } else {
      for (GroupControlTemplate *g = begin; g != end; ++g) {
        if (dev_id != 0 && g->dev_id != dev_id)
          continue;
        clearActionSlots(*g);
        g->ack_slots = AckStateSlots{};
        g->query_slots = QueryStateSlots{};
        const DeviceClass cls = g->coverage.dev_class; // 클래스는 유지
        g->coverage = SlotCoverage{};
        g->coverage.dev_class = cls;
        modified = true;
        if (dev_id != 0)
          break;
      }
    }
    if (modified) {
      rebuildNormSub1LutLocked();
    }
  }
  if (!modified)
    return false;

  if (full_reset && dev_id == 0) {
    {
      MutexLocker nvs_lock(_nvs_mutex, kManageLockTimeout);
      if (nvs_lock.isLocked()) {
        char ns[16];
        getControlNamespace(ns, sizeof(ns), getCurrentProfileIndex());
        Preferences prefs;
        if (prefs.begin(ns, false)) {
          prefs.clear();
          prefs.end();
        }
      }
    }
    synthesizeFromConvergedCache();
  } else {
    saveToNvs();
  }
  return true;
}

// 슬롯 한 개를 한 줄로 설정 (category_offset 은 현대통신 공통 5)
static void setSlot(ActionSlot &s, uint8_t cat, uint8_t act, uint8_t ack,
                    uint8_t mn = 0, uint8_t mx = 0) {
  s.discovered = true;
  s.category_offset = 5;
  s.category_val = cat;
  s.action_offset = act;
  s.ack_state_offset = ack;
  if (mx) {
    s.min_val = mn;
    s.max_val = mx;
  }
}

static void setLevels(ActionSlot &s, std::initializer_list<uint8_t> tokens) {
  s.level_count = static_cast<decltype(s.level_count)>(tokens.size());
  size_t i = 0;
  for (uint8_t t : tokens)
    s.level_tokens[i++] = t;
}

void ControlTemplateRegistry::applyProfile(const WallpadProfile *profile) {
  if (!profile || !profile->devices || profile->device_count == 0)
    return;

  ESP_LOGI("ControlTemplate", "Applying profile: %s (%u devices)", profile->vendor_name,
           (unsigned)profile->device_count);

  for (size_t i = 0; i < profile->device_count; ++i) {
    const DeviceSpec &spec = profile->devices[i];
    modifyOrCreateGroup(
        spec.dev_id,
        [&](GroupControlTemplate &grp) {
          grp.coverage.dev_class = spec.dev_class;
          setStr(grp.group_name, spec.name);
          grp.frame_len = spec.ctl_len;
          grp.sub1_offset = profile->sub1_offset;
          grp.sub2_offset = profile->sub2_offset;

          // 0x34 엘리베이터: 월패드 쿼리가 없으므로 기본 제어 골격 주입
          if (spec.dev_id == 0x34 && spec.ctl_len == 11) {
            static const uint8_t ev_proto[11] = {0xF7, 0x0B, 0x01, 0x34,
                                                 0x02, 0x41, 0x10, 0x06,
                                                 0x00, 0x9C, 0xEE};
            std::copy(ev_proto, ev_proto + 11, grp.raw_template);
            grp.ctl_sub1_override = 0x10;
          }

          // 전원 슬롯
          grp.power_slot.discovered = true;
          grp.power_slot.action_offset = spec.ctl_payload_offset;
          grp.power_slot.on_val = spec.pwr_on_val;
          grp.power_slot.off_val = spec.pwr_off_val;
          grp.power_slot.ack_state_offset = spec.ctl_ack_state_offset;
          if (spec.pwr_away_val != 0xFF)
            grp.away_mode_token = spec.pwr_away_val;

          const uint8_t act = spec.ctl_payload_offset,
                        ack = spec.ctl_ack_state_offset;
          switch (spec.dev_class) {
          case DeviceClass::THERMOSTAT:
            setSlot(grp.temp_slot, 0x45, act, ack, 14, 36);
            grp.temp_slot.ack_target_offset = spec.ctl_ack_echo_offset;
            grp.temp_slot.ack_telemetry_offset = spec.ctl_ack_ambtemp_offset;
            break;
          case DeviceClass::VENT:
            setSlot(grp.speed_slot, 0x42, act, ack, 1, 3);
            setLevels(grp.speed_slot, {0x01, 0x03, 0x07}); // 약/중/강
            setSlot(grp.mode_slot, 0x43, act, ack, 1,
                    4); // 일반/바이패스/자동/공기청정
            break;
          case DeviceClass::GAS:
            setSlot(grp.close_slot, 0x43, act, ack);
            grp.close_slot.off_val = spec.pwr_off_val;
            break;
          case DeviceClass::AIRCON:
            setSlot(grp.temp_slot, 0x45, act, ack, 18, 30);
            grp.temp_slot.ack_target_offset = spec.ctl_ack_echo_offset;
            setSlot(grp.speed_slot, 0x42, act, ack, 1, 3);
            setLevels(grp.speed_slot, {0x01, 0x02, 0x03}); // 미풍/약풍/강풍
            setSlot(grp.mode_slot, 0x41, act, ack, 1,
                    5); // 냉방/제습/송풍/자동/난방
            break;
          default:
            break;
          }

          // 제어 응답(ack) 슬롯
          grp.ack_slots.discovered = true;
          grp.ack_slots.power_offset = spec.ctl_ack_state_offset;
          if (spec.dev_class == DeviceClass::THERMOSTAT) {
            grp.ack_slots.target_temp_offset = spec.ctl_ack_echo_offset;
            grp.ack_slots.current_temp_offset = spec.ctl_ack_ambtemp_offset;
          }

          // 쿼리 응답 슬롯
          auto &q = grp.query_slots;
          q.discovered = true;
          q.expected_len = spec.qry_ack_len;
          q.power_offset = spec.qry_power_offset;
          q.target_temp_offset = spec.qry_settemp_offset;
          q.current_temp_offset = spec.qry_ambtemp_offset;
          q.fan_speed_offset = spec.qry_fanspeed_offset;
          q.valve_state_offset = spec.qry_valve_offset;
          q.power_w_offset = spec.qry_watt_h_offset;
        },
        spec.name);

    ESP_LOGI("ControlTemplate",
             "Injected Dev 0x%02X (%s): CTL len=%u, QRY len=%u, StateOff=#%u",
             spec.dev_id, spec.name, spec.ctl_len, spec.qry_ack_len,
             spec.qry_power_offset);
  }
}

void ControlTemplateRegistry::matchAndInject(const AutoProbeDescriptor &ad) {
  if (const WallpadProfile *profile = ProfileMatcher::matchProfile(ad)) {
    applyProfile(profile);
    AutoProbe_GetEngine().injectControlSpec(0x02, 11);
  } else {
    ESP_LOGW("ControlTemplate",
             "No matching wallpad profile found. Fallback to default framing.");
  }
}

void ControlTemplateRegistry::synthesizeFromConvergedCache() {
  const auto ad = AutoProbe_GetEngine().getDescriptor();
  if (!ad.offsets_locked)
    return;

  const uint8_t ctrl_opcode = ad.control_opcode ? ad.control_opcode : 0x02;
  const size_t total = Polling_GetRegistry().totalCount();
  for (size_t i = 0; i < total; ++i) {
    PollingTargetEntry entry{};
    if (!Polling_GetRegistry().getEntry(i, entry) || !entry.is_active ||
        entry.raw_query_len < 5)
      continue;

    const uint8_t d_id = entry.dev_id
                             ? entry.dev_id
                             : (ad.dev_id_offset < entry.raw_query_len
                                    ? entry.raw_query_data[ad.dev_id_offset]
                                    : 0);
    if (d_id == 0 || d_id == 0x34)
      continue;
    if (entry.source_channels == (1 << 5))
      continue; // CH5 단독 유래 제외

    modifyOrCreateGroup(d_id, [&](GroupControlTemplate &grp) {
      if (grp.frame_len != 0)
        return;
      grp.frame_len = entry.raw_query_len;
      std::copy_n(entry.raw_query_data.begin(),
                  std::min<size_t>(entry.raw_query_len, 32), grp.raw_template);
      if (ad.opcode_offset < grp.frame_len)
        grp.raw_template[ad.opcode_offset] = ctrl_opcode;
      grp.sub1_offset = ad.sub1_offset;
      grp.sub2_offset = ad.sub2_offset;
      grp.ctl_sub1_override = entry.sub1;
    });
  }

  matchAndInject(ad); // 제조사 명세 기반 슬롯 주입
  saveToNvs();
}

bool ControlTemplateRegistry::buildControlPacket(uint8_t dev_id, uint8_t sub1,
                                                 uint8_t sub2,
                                                 ControlActionType action,
                                                 int value,
                                                 StaticPacket &out) const {
  GroupControlTemplate grp{};
  if (!findGroup(dev_id, grp) || grp.frame_len < 5)
    return false;
  auto &parser = Universal_GetEngine();

  const size_t act_idx = std::to_underlying(action);
  if (act_idx >= std::size(kActionBuilders))
    return false;

  out.channel_id = 1;
  out.length = grp.frame_len;
  out.data.fill(0);
  std::copy(grp.raw_template, grp.raw_template + grp.frame_len,
            out.data.begin());

  // 단일 유닛 기기는 학습된 sub1 을 사용
  const size_t units =
      s_device_unit_count_fn ? s_device_unit_count_fn(dev_id) : 1;
  const uint8_t actual_sub1 = (units <= 1 && grp.ctl_sub1_override != 0xFF)
                                  ? grp.ctl_sub1_override
                                  : sub1;
  if (grp.sub1_offset < grp.frame_len)
    out.data[grp.sub1_offset] = actual_sub1;
  if (grp.sub2_offset < grp.frame_len)
    out.data[grp.sub2_offset] = sub2;

  if (!kActionBuilders[act_idx](grp, value, out))
    return false;

  if (out.length >= 3) {
    out.data[out.length - 2] =
        parser.calculateChecksum(out.data.data(), out.length);
    out.data[out.length - 1] = parser.getEtx();
  }
  return true;
}

bool ControlTemplateRegistry::saveToNvs() {
  return saveToNvsForProfile(getCurrentProfileIndex());
}
void ControlTemplateRegistry::loadFromNvs() {
  loadFromNvsForProfile(getCurrentProfileIndex());
}

bool ControlTemplateRegistry::saveToNvsForProfile(uint8_t prof_idx) {
  uint8_t save_count = 0;
  bool ram_locked = false;
  bool nvs_opened = false;

  {
    MutexLocker nvs_lock(_nvs_mutex, kManageLockTimeout);
    if (!nvs_lock.isLocked()) {
      ESP_LOGW("CTRL_REG", "[WARN] _nvs_mutex timeout saving profile %u", prof_idx);
      return false;
    }

    {
      MutexLocker ram_lock(_mutex, kManageLockTimeout);
      if (ram_lock.isLocked()) {
        ram_locked = true;
        for (size_t i = 0; i < _group_count; ++i)
          if (_groups[i].dev_id != 0)
            s_nvs_transfer_buf[save_count++] = _groups[i];
      }
    }

    if (!ram_locked) {
      ESP_LOGW("CTRL_REG", "[WARN] _mutex timeout copying RAM snapshot for profile %u", prof_idx);
      return false;
    }

    char ns[16];
    getControlNamespace(ns, sizeof(ns), prof_idx);
    Preferences prefs;
    if (prefs.begin(ns, false)) {
      nvs_opened = true;
      prefs.putUChar("cnt", save_count);
      for (size_t i = 0; i < save_count; ++i) {
        char key[16];
        snprintf(key, sizeof(key), "grp_%u", static_cast<unsigned>(i));
        nvsPutEnv(prefs, key, s_nvs_transfer_buf[i]);
      }
      prefs.end();
    } else {
      ESP_LOGW("CTRL_REG", "[WARN] Failed to open NVS namespace %s", ns);
    }
  }

  return nvs_opened;
}

void ControlTemplateRegistry::loadFromNvsForProfile(uint8_t prof_idx) {
  MutexLocker nvs_lock(_nvs_mutex, kManageLockTimeout);
  if (!nvs_lock.isLocked())
    return;

  char ns[16];
  getControlNamespace(ns, sizeof(ns), prof_idx);

  size_t valid = 0;
  {
    Preferences prefs;
    const bool opened = prefs.begin(ns, true);
    uint8_t cnt = opened ? prefs.getUChar("cnt", 0) : 0;
    if (cnt > MAX_GROUPS)
      cnt = MAX_GROUPS;

    for (size_t i = 0; i < cnt; ++i) {
      char key[16];
      snprintf(key, sizeof(key), "grp_%u", static_cast<unsigned>(i));
      GroupControlTemplate temp{};
      bool loaded = nvsGetEnv(prefs, key, temp);
      if (!loaded &&
          prefs.getBytesLength(key) ==
              sizeof(GroupControlTemplate)) { // 구버전(봉투 없음) 호환
        loaded = prefs.getBytes(key, &temp, sizeof(temp)) == sizeof(temp);
      }
      if (!loaded || temp.dev_id == 0)
        continue;

      if (temp.sub1_offset == 2 && temp.sub2_offset > 2)
        temp.sub1_offset = temp.sub2_offset;
      if (temp.coverage.dev_class == DeviceClass::THERMOSTAT &&
          temp.temp_slot.min_val == 7)
        temp.temp_slot.min_val = 0;
      s_nvs_transfer_buf[valid++] = temp;
    }
    if (opened)
      prefs.end();
  }

  // RAM 에 원자적으로 반영 (저장된 항목이 없으면 비움)
  MutexLocker ram_lock(_mutex, kManageLockTimeout);
  if (!ram_lock.isLocked())
    return;
  std::fill(std::begin(_groups), std::end(_groups), GroupControlTemplate{});
  _group_count = 0;
  for (size_t i = 0; i < valid; ++i)
    insertSorted(&_groups[0], _group_count, MAX_GROUPS, s_nvs_transfer_buf[i]);
  rebuildNormSub1LutLocked();
}

void ControlTemplateRegistry::onProfileChanged(uint8_t old_prof_idx,
                                               uint8_t new_prof_idx) {
  if (old_prof_idx == new_prof_idx)
    return;
  saveToNvsForProfile(old_prof_idx);
  loadFromNvsForProfile(new_prof_idx);
  if (getGroupCount() == 0)
    synthesizeFromConvergedCache();
}

// ============================================================================
// GroupControlTemplate Optimized Methods
// ============================================================================

uint8_t GroupControlTemplate::getPowerOffset(uint8_t pkt_len) const noexcept {
  if (pkt_len > 0) {
    if (frame_len > 0 && pkt_len == frame_len && ack_slots.discovered &&
        ack_slots.power_offset != 0xFF) {
      return ack_slots.power_offset;
    }
    if (query_slots.discovered && query_slots.power_offset != 0xFF) {
      return query_slots.power_offset;
    }
  }
  if (ack_slots.discovered && ack_slots.power_offset != 0xFF)
    return ack_slots.power_offset;
  if (query_slots.discovered && query_slots.power_offset != 0xFF)
    return query_slots.power_offset;
  if (power_slot.discovered && power_slot.ack_state_offset != 0xFF)
    return power_slot.ack_state_offset;
  return 0xFF;
}

uint8_t
GroupControlTemplate::getTargetTempOffset(uint8_t pkt_len) const noexcept {
  if (pkt_len > 0) {
    if (frame_len > 0 && pkt_len == frame_len && ack_slots.discovered &&
        ack_slots.target_temp_offset != 0xFF) {
      return ack_slots.target_temp_offset;
    }
    if (query_slots.discovered && query_slots.target_temp_offset != 0xFF) {
      return query_slots.target_temp_offset;
    }
  }
  if (query_slots.discovered && query_slots.target_temp_offset != 0xFF)
    return query_slots.target_temp_offset;
  if (ack_slots.discovered && ack_slots.target_temp_offset != 0xFF)
    return ack_slots.target_temp_offset;
  return 0xFF;
}

uint8_t
GroupControlTemplate::getCurrentTempOffset(uint8_t pkt_len) const noexcept {
  if (pkt_len > 0) {
    if (frame_len > 0 && pkt_len == frame_len && ack_slots.discovered &&
        ack_slots.current_temp_offset != 0xFF) {
      return ack_slots.current_temp_offset;
    }
    if (query_slots.discovered && query_slots.current_temp_offset != 0xFF) {
      return query_slots.current_temp_offset;
    }
  }
  if (query_slots.discovered && query_slots.current_temp_offset != 0xFF)
    return query_slots.current_temp_offset;
  if (ack_slots.discovered && ack_slots.current_temp_offset != 0xFF)
    return ack_slots.current_temp_offset;
  return 0xFF;
}

uint8_t
GroupControlTemplate::getFanSpeedOffset(uint8_t pkt_len) const noexcept {
  if (pkt_len > 0) {
    if (frame_len > 0 && pkt_len == frame_len && ack_slots.discovered &&
        ack_slots.fan_speed_offset != 0xFF) {
      return ack_slots.fan_speed_offset;
    }
    if (query_slots.discovered && query_slots.fan_speed_offset != 0xFF) {
      return query_slots.fan_speed_offset;
    }
  }
  if (query_slots.discovered && query_slots.fan_speed_offset != 0xFF)
    return query_slots.fan_speed_offset;
  if (ack_slots.discovered && ack_slots.fan_speed_offset != 0xFF)
    return ack_slots.fan_speed_offset;
  return 0xFF;
}

uint8_t GroupControlTemplate::decodeFanSpeed(uint8_t raw_token) const noexcept {
  if (speed_slot.level_count > 0) {
    for (uint8_t i = 0; i < speed_slot.level_count; ++i) {
      if (speed_slot.level_tokens[i] == raw_token) {
        return static_cast<uint8_t>(i + 1);
      }
    }
  }
  // Lookup Table-driven Token Resolution (Anti-Pattern Elimination)
  switch (raw_token) {
  case 0x11:
  case 0x01:
  case 0x10:
    return 1;
  case 0x13:
  case 0x03:
  case 0x02:
    return 2;
  case 0x17:
  case 0x07:
    return 3;
  default:
    if (raw_token >= 1 && raw_token <= 3)
      return raw_token;
    return 1;
  }
}

uint8_t GroupControlTemplate::decodeVentMode(uint8_t raw_byte) const noexcept {
  // Byte #8 운전 모드 토큰 (1:일반, 2:바이패스, 3:자동, 4:공기청정,
  // 0x81:Reject)
  if (raw_byte >= 1 && raw_byte <= 4)
    return raw_byte;
  const uint8_t nibble = (raw_byte >> 4) & 0x0F;
  if (nibble >= 1 && nibble <= 4)
    return nibble;
  return 1; // 기본 일반 환기 (0x01)
}

uint8_t
GroupControlTemplate::getValveStateOffset(uint8_t pkt_len) const noexcept {
  if (pkt_len > 0) {
    if (frame_len > 0 && pkt_len == frame_len && ack_slots.discovered &&
        ack_slots.valve_state_offset != 0xFF) {
      return ack_slots.valve_state_offset;
    }
    if (query_slots.discovered && query_slots.valve_state_offset != 0xFF) {
      return query_slots.valve_state_offset;
    }
  }
  if (query_slots.discovered && query_slots.valve_state_offset != 0xFF)
    return query_slots.valve_state_offset;
  if (ack_slots.discovered && ack_slots.valve_state_offset != 0xFF)
    return ack_slots.valve_state_offset;
  return 0xFF;
}

uint8_t GroupControlTemplate::getWattageOffset(uint8_t pkt_len) const noexcept {
  if (pkt_len > 0 && pkt_len < 16)
    return 0xFF;
  if (query_slots.discovered && query_slots.power_w_offset != 0xFF)
    return query_slots.power_w_offset;
  return 0xFF;
}

bool GroupControlTemplate::isUnidirectional() const noexcept {
  return dev_id == 0x34; // 단방향 버스트 전송 (엘리베이터 등)
}

namespace {

static void decodeOutlet(const GroupControlTemplate &grp,
                         const StaticPacket &ack, const DeviceStateEntry *,
                         DecodedDeviceState &out) {
  out.power_w = 0.0f;
  const uint8_t w_off = grp.getWattageOffset(ack.length);
  const size_t valid_len = std::min<size_t>(ack.length, ack.data.size());
  if (auto raw_w = Endian::loadBe16At(std::span(ack.data.data(), valid_len), w_off)) {
    out.power_w = (*raw_w < 50000) ? static_cast<float>(*raw_w) : 0.0f;
  }
}

static void decodeSwitch(const GroupControlTemplate &grp,
                         const StaticPacket &ack, const DeviceStateEntry *dev,
                         DecodedDeviceState &out) {
  if (grp.frame_len >= 17) {
    out.dev_class = DeviceClass::OUTLET;
    decodeOutlet(grp, ack, dev, out);
  }
}

static void decodeGas(const GroupControlTemplate &grp, const StaticPacket &ack,
                         const DeviceStateEntry *, DecodedDeviceState &out) {
  uint8_t v_off = grp.getValveStateOffset(ack.length);
  bool is_closed =
      (v_off >= ack.length || ack.data[v_off] == grp.close_slot.off_val);
  snprintf(out.valve_state, sizeof(out.valve_state), "%s",
           is_closed ? "closed" : "open");
}

static void decodeMomentary(const GroupControlTemplate &grp,
                            const StaticPacket &ack,
                            const DeviceStateEntry *dev,
                            DecodedDeviceState &out) {
  out.floor = 15;
  out.direction = 0;
  out.ho = 0;
  if (grp.dev_id == 0x34) {
    out.power =
        (ack.length == 11 && ack.data[4] == 0x04)
            ? ((ack.data[8] == 0x06) ? 1 : 0)
            : ((dev && dev->last_ack_len > 0) ? (dev->last_ack_data[0] & 0x01)
                                              : 0);
  } else if (ack.length >= 6) {
    out.floor = constrain(static_cast<int>(ack.data[5]), 1, 60);
    out.direction = (ack.length >= 7) ? ack.data[6] : 0;
  }
}

static void decodeThermostat(const GroupControlTemplate &grp,
                             const StaticPacket &ack,
                             const DeviceStateEntry *dev,
                             DecodedDeviceState &out) {
  out.target_temp = dev ? dev->last_target_temp : 0;
  out.current_temp = (dev && dev->last_current_temp > 0)
                         ? dev->last_current_temp
                         : out.target_temp;

  uint8_t p_off = grp.getPowerOffset(ack.length);
  if (p_off < ack.length && grp.away_mode_token != 0 &&
      ack.data[p_off] == grp.away_mode_token)
    out.power = 2;

  uint8_t t_off = grp.getTargetTempOffset(ack.length);
  if (t_off < ack.length && ack.data[t_off] >= 5 && ack.data[t_off] <= 35) {
    out.target_temp = ack.data[t_off];
    if (dev)
      const_cast<DeviceStateEntry *>(dev)->last_target_temp = ack.data[t_off];
  }

  uint8_t c_off = grp.getCurrentTempOffset(ack.length);
  if (c_off < ack.length && ack.data[c_off] >= 5 && ack.data[c_off] <= 50) {
    out.current_temp = ack.data[c_off];
    if (dev)
      const_cast<DeviceStateEntry *>(dev)->last_current_temp = ack.data[c_off];
  }
}

static void decodeVent(const GroupControlTemplate &grp, const StaticPacket &ack,
                       const DeviceStateEntry *, DecodedDeviceState &out) {
  uint8_t p_off = grp.getPowerOffset(ack.length);
  out.power = (p_off < ack.length && ack.data[p_off] == 0x01) ? 1 : 0;
  out.fan_speed = 1;
  out.vent_mode = 1;

  uint8_t spd_off = grp.getFanSpeedOffset(ack.length);
  if (spd_off < ack.length)
    out.fan_speed = grp.decodeFanSpeed(ack.data[spd_off]);

  if (out.power == 1 && (ack.length >= 6 && ack.data[5] == 0x43) &&
      p_off < ack.length) {
    uint8_t m = ack.data[p_off];
    if (m >= 1 && m <= 4)
      out.vent_mode = m;
  }
}

static void decodeAircon(const GroupControlTemplate &, const StaticPacket &ack,
                         const DeviceStateEntry *dev, DecodedDeviceState &out) {
  size_t base = (ack.length == 14) ? 7 : 8;
  if (base + 4 >= ack.length)
    return;

  out.power = ((ack.data[base] & 0x7F) == 0x01) ? 1 : 0;
  out.vent_mode = constrain(static_cast<int>(ack.data[base + 1]), 1, 5);
  out.fan_speed = (ack.data[base + 2] >= 1 && ack.data[base + 2] <= 4)
                      ? ack.data[base + 2]
                      : 4;

  uint8_t amb = ack.data[base + 3];
  if (amb >= 5 && amb <= 50) {
    out.current_temp = amb;
    if (dev)
      const_cast<DeviceStateEntry *>(dev)->last_current_temp = amb;
  }
  uint8_t tgt = ack.data[base + 4] & 0x7F;
  if (tgt >= 5 && tgt <= 35) {
    out.target_temp = tgt;
    if (dev)
      const_cast<DeviceStateEntry *>(dev)->last_target_temp = tgt;
  }
}

static void decodeUnknown(const GroupControlTemplate &, const StaticPacket &,
                          const DeviceStateEntry *, DecodedDeviceState &) {}

using ClassDecoderFn = void (*)(const GroupControlTemplate &grp,
                                const StaticPacket &ack,
                                const DeviceStateEntry *dev,
                                DecodedDeviceState &out);

static constexpr ClassDecoderFn kClassDecoders[] = {
    decodeUnknown,    // UNKNOWN = 0
    decodeSwitch,     // SWITCH = 1
    decodeOutlet,     // OUTLET = 2
    decodeGas,        // GAS = 3
    decodeMomentary,  // MOMENTARY = 4
    decodeThermostat, // THERMOSTAT = 5
    decodeVent,       // VENT = 6
    decodeAircon      // AIRCON = 7
};
static_assert(std::size(kClassDecoders) == 8,
              "kClassDecoders size must match DeviceClass count");

} // namespace

void ControlTemplate_DecodeDeviceState(const GroupControlTemplate &grp,
                                       const StaticPacket &ack,
                                       const DeviceStateEntry *dev,
                                       DecodedDeviceState &out) noexcept {
  out.dev_class = grp.coverage.dev_class;
  out.should_broadcast = false;
  out.power = 0;
  out.target_temp = 0;
  out.current_temp = 0;
  out.fan_speed = 0;
  out.vent_mode = 1;
  out.power_w = 0.0f;
  out.floor = 1;
  out.direction = 0;
  out.ho = 0;
  snprintf(out.valve_state, sizeof(out.valve_state), "closed");

  // 1. 공통 기본 전원 슬롯 디코딩
  uint8_t p_off = grp.getPowerOffset(ack.length);
  if (p_off != 0xFF && p_off < ack.length) {
    out.power = (ack.data[p_off] == grp.power_slot.on_val) ? 1 : 0;
  }

  // 2. 클래스별 디스패치 (특수 전원 및 파라미터 개별 디코딩)
  const size_t idx = std::to_underlying(grp.coverage.dev_class);
  if (idx < std::size(kClassDecoders)) {
    kClassDecoders[idx](grp, ack, dev, out);
  } else {
    decodeUnknown(grp, ack, dev, out);
  }
}

bool ControlTemplate_DecodeByDevId(uint8_t dev_id,
                                   const StaticPacket &ack,
                                   const DeviceStateEntry *dev,
                                   DecodedDeviceState &out) noexcept {
  GroupControlTemplate grp{};
  bool has_grp = s_control_registry.findGroup(dev_id, grp);
  if (!has_grp && dev_id == 0x34) {
    grp.dev_id = 0x34;
    grp.coverage.dev_class = DeviceClass::MOMENTARY;
    has_grp = true;
  }
  if (!has_grp) {
    return false;
  }
  ControlTemplate_DecodeDeviceState(grp, ack, dev, out);
  return true;
}

uint8_t ControlTemplate_NormSub1(uint8_t dev_id, uint8_t sub1) noexcept {
  // Read-mostly derived cache (0-Lock, 0-Copy, O(1) integer comparison)
  // Max 8 elements (typically 1~2 active rules for HVAC). Rebuilt exclusively in cold path.
  const size_t count = s_norm_rule_count;
  for (size_t i = 0; i < count; ++i) {
    if (s_norm_rules[i].dev_id == dev_id) {
      if (sub1 == s_norm_rules[i].temp_sub1 || sub1 == s_norm_rules[i].speed_sub1) {
        return s_norm_rules[i].power_sub1;
      }
      return sub1;
    }
  }
  return sub1;
}


