#pragma once

// ============================================================================
// AutoProbingEngine: Level 3 Universal Auto-Probing Protocol Engine
// ============================================================================

#include "L0_Foundation/System_Config.h"
#include "L3_Protocol/Public/Device_Registry.h"
#include <cstddef>
#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <span>

// ============================================================================
// WALLPAD CHECKSUM ALGORITHM ENUM
// ============================================================================

enum class ChecksumAlgo : uint8_t {
  UNKNOWN = 0,
  XOR_ALL = 1,         // XOR from 0 to N-3 (Hyundai HT, EzVille, etc.)
  XOR_NO_STX = 2,      // XOR from 1 to N-3 (Kocom)
  SUM_ALL = 3,         // Sum 0 to N-3 modulo 256 (Commax Legacy)
  SUM_NO_STX = 4,      // Sum 1 to N-3 modulo 256 (Commax Modern)
  TWOS_COMPLEMENT = 5, // (0x100 - Sum[1..N-3]) % 256 (Samsung SDS / EZON)
  ONES_COMPLEMENT = 6, // (~Sum[0..N-3]) % 256
  CRC8_MAXIM = 7,      // CRC-8 (poly 0x31, init 0x00)
  NONE = 8 // Pure framing without checksum byte (Doorphone 0x02..0x03)
};

// ============================================================================
// AUTO-PROBE DESCRIPTOR
// ============================================================================

struct AutoProbeDescriptor {
  uint8_t stx{0xF7};
  uint8_t etx{0xEE};
  uint8_t min_len{3};
  uint8_t max_len{64};
  ChecksumAlgo checksum_algo{ChecksumAlgo::XOR_ALL};
  uint8_t opcode_offset{4};
  uint8_t query_opcode{0x01};
  uint8_t control_opcode{0x00};
  uint8_t ack_opcode{0x04};
  bool opcodes_locked{false};
  bool control_seen{false};
  uint8_t dev_id_offset{
      3}; // DevType 위치 (QUERY 기준 / swap 없으면 ACK도 동일)
  uint8_t sub1_offset{5};
  uint8_t sub2_offset{6};
  uint8_t payload_offset{8};
  bool is_swapped_addr{false};
  bool offsets_locked{false};
  // ★ swap 구조 보완 필드 (DA/SA 교차 프로토콜 지원)
  uint8_t gw_addr_offset{
      2};                // GW 주소 위치 (QUERY 기준) = ACK 기준 DevType 위치
  uint8_t gw_addr{0x01}; // 버스에서 관측된 GW 자신의 RS-485 주소값 (기본: 0x01)
  // ★ 학습된 쿼리 패킷 길이 (버스 관측 기반, buildQueryPacket 동적 길이 사용)
  uint8_t learned_query_len{11};   // 관측된 쿼리 패킷 최빈 길이 (기본: 11)
  uint8_t len_offset{0xFF};        // 패킷 내 길이 필드 위치 (0xFF: 고정 프레임)
  bool has_len_field{false};       // 패킷 내 명시적 길이 필드 유무
  uint8_t seq_offset{0xFF};        // 시퀀스 카운터 위치 (0xFF: 없음)
  bool has_seq_counter{false};     // 시퀀스 카운터 유무
  uint8_t ack_flag_offset{0xFF};   // ACK/Status 플래그 위치 (0xFF: 없음)
  uint8_t learned_ctrl_lens[4]{0}; // 관측된 제어(CMD/CTL) 패킷 가변 길이 목록
  uint8_t ctrl_len_cnt{0};         // 관측된 제어 패킷 길이 가짓수
  uint32_t matched_packets{0};
  uint32_t tested_packets{0};
  bool is_locked{false};
  char description[64]{"Probing bus traffic..."};
};

class AutoProbingEngine {
private:
  AutoProbeDescriptor _desc;
  uint16_t _stx_counts[256]{};
  uint16_t _etx_counts[256]{};
  uint16_t _algo_matches[9]{};
  uint16_t _consecutive_matches{0};
  uint16_t _consecutive_mismatches{0};
  ChecksumAlgo _candidate_algo{ChecksumAlgo::UNKNOWN};
  uint16_t _diff_idx_counts[16]{};
  uint8_t _control_matches{0};
  uint8_t _candidate_ctrl_op{0};
  mutable portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;

public:
  AutoProbingEngine();
  void initFromNvs();
  void feedFrame(span<const uint8_t> raw_frame);
  void feedOpcodePair(span<const uint8_t> req, span<const uint8_t> ack);
  void feedControlFrame(span<const uint8_t> ctrl_frame);
  bool isLocked() const;
  bool isOffsetsLocked() const;
  AutoProbeDescriptor getDescriptor() const;
  void reset();
  void injectControlSpec(uint8_t ctrl_op, uint8_t ctrl_len);
  bool analyzeCacheMatrix();
  uint8_t calculateChecksum(ChecksumAlgo algo,
                            span<const uint8_t> data) const noexcept;
  uint8_t calculateChecksum(ChecksumAlgo algo, const uint8_t *data,
                            size_t len) const;
  static const char *getAlgoName(ChecksumAlgo algo);

  // L2.3 DeviceRepository decoupled hooks
  using OnlineCountFn = size_t (*)();
  using DeviceAckLookupFn = bool (*)(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                                     const uint8_t **out_ack, size_t *out_len);
  using UpdateFromBusFn = void (*)(StaticPacket &ack);
  static void setDeviceHooks(OnlineCountFn count_fn, DeviceAckLookupFn lookup_fn,
                             UpdateFromBusFn update_fn);
};

extern AutoProbingEngine g_auto_probing_engine;
