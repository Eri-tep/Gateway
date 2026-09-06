#pragma once

#include "Common.h"

// ============================================================================
// CHECKSUM ALGORITHMS
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
// DATA-DRIVEN VENDOR PROFILE DESCRIPTOR (STORED IN NVS)
// ============================================================================

struct VendorProfileDescriptor {
  char key[12];          // "auto", "hyundai", "commax", "samsung", "kocom" 등
  char name[36];         // "Hyundai HT (F7..EE, 11B, XOR)", etc.
  uint8_t stx;           // 시작 바이트 (예: 0xF7, 0x02, 0xAA)
  uint8_t etx;           // 종료 바이트 (예: 0xEE, 0x03, 0x0D)
  uint8_t min_len;       // 최소 길이 (예: 3)
  uint8_t max_len;       // 최대 길이 (예: 32)
  ChecksumAlgo cs_algo;  // 체크섬 공식 (XOR_ALL, SUM_ALL 등)
  uint8_t opcode_offset; // 커맨드 위치 (예: 4)
  uint8_t query_op;      // 조회 코드 (예: 0x01, 0x41)
  uint8_t ctrl_op;       // 제어 코드 (예: 0x02, 0x42)
  uint8_t ack_op;        // 응답 코드 (예: 0x04, 0xC1)
  uint8_t dev_id_offset; // 장치 ID 위치 (예: 3)
  uint8_t sub1_offset;   // 서브 ID #1 위치 (예: 5)
  uint8_t sub2_offset;   // 서브 ID #2 위치 (예: 6)
  uint8_t door_stx;      // 도어폰 STX (예: 0x7F, 0x02 등, 0이면 비활성)
  uint8_t door_etx;      // 도어폰 ETX (예: 0xEE, 0x03 등)
  uint8_t door_len;      // 도어폰 패킷 길이 (예: 9)
  uint8_t is_swapped_addr{0};     // 1: DA/SA 교차 주소 모드, 0: 1:1 직접
  uint8_t gw_addr_offset{2};      // GW 주소 위치 (QUERY 기준) = ACK 기준 DevType 위치
  uint8_t gw_addr{0x01};          // GW 주소값
  uint8_t learned_query_len{11};  // 학습된 쿼리 길이
  uint8_t len_offset{0xFF};       // 패킷 내 길이 필드 위치 (0xFF: 고정 프레임)
  uint8_t has_len_field{0};       // 1: 길이 필드 보유, 0: 암묵적/고정 프레임
  uint8_t seq_offset{0xFF};       // 시퀀스 카운터 위치 (0xFF: 없음)
  uint8_t ack_flag_offset{0xFF};  // ACK 상태 플래그 위치 (0xFF: 없음)
  uint8_t learned_ctrl_lens[4]{0}; // 관측된 제어(CMD/CTL) 패킷 가변 길이 목록
  uint8_t ctrl_len_cnt{0};        // 관측된 제어 패킷 길이 가짓수
};

// ============================================================================
// PROFILE INDEX ENUM (4 SLOTS: AUTO + CUSTOM1~3)
// ============================================================================

enum class WallpadProfileIndex : uint8_t {
  ADAPTIVE = 0,
  CUSTOM1 = 1,
  CUSTOM2 = 2,
  CUSTOM3 = 3,
  COUNT = 4
};

// ============================================================================
// 1ST TIER CACHE: POLLING TARGET REGISTRY
// ============================================================================

struct PollingTargetEntry {
  uint8_t dev_id{0};
  uint8_t sub1{0};
  uint8_t sub2{0};
  uint32_t last_requested_ms{0};
  uint32_t last_interval_ms{0};
  uint8_t source_channels{0}; // Bitmask: bit 2=CH2, bit 3=CH3, bit 6=CH6
  uint16_t hit_count{0};
  bool is_active{false};
  bool is_verified{
      true}; // False if restored from warm cache until ACK/request seen
  uint32_t restored_ms{0};
  uint8_t raw_query_len{0};
  std::array<uint8_t, 64> raw_query_data{};
  uint8_t raw_ack_len{0};
  std::array<uint8_t, 64> raw_ack_data{};
};

class PollingTargetRegistry {
public:
  static constexpr size_t MAX_TARGETS = 64;

private:
  PollingTargetEntry _entries[MAX_TARGETS]{};
  size_t _count{0};
  mutable portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;

public:
  void registerOrTouch(uint8_t ch, uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                       const uint8_t *raw_pkt = nullptr, size_t raw_len = 0);
  void updateResponse(const uint8_t *query_pkt, size_t query_len,
                      const uint8_t *ack_pkt, size_t ack_len);
  void reindexWithOffsets(uint8_t dev_id_offset, uint8_t sub1_offset,
                          uint8_t sub2_offset);
  void sweepExpired(uint32_t ttl_ms = 30000);
  size_t getActiveTargets(PollingTargetEntry *out_buf, size_t max_count);
  size_t activeCount() const;
  size_t totalCount() const;
  bool getEntry(size_t index, PollingTargetEntry &out) const;
  void resetHits();
  void clear();

  // Warm-Start Cache Interface
  void loadFromWarmCache(const RtcWarmCacheEntry *entries, size_t count,
                         uint32_t now_ms);
  size_t getWarmCacheEntries(RtcWarmCacheEntry *out_entries,
                             size_t max_count) const;
  void markVerified(uint8_t dev_id, uint8_t sub1, uint8_t sub2);
  size_t verifiedCount() const;
};

extern PollingTargetRegistry g_polling_targets;

// ============================================================================
// UNIVERSAL AUTO-PROBING PROTOCOL ENGINE
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
  uint8_t dev_id_offset{3};   // DevType 위치 (QUERY 기준 / swap 없으면 ACK도 동일)
  uint8_t sub1_offset{5};
  uint8_t sub2_offset{6};
  uint8_t payload_offset{7};
  bool is_swapped_addr{false};
  bool offsets_locked{false};
  // ★ swap 구조 보완 필드 (DA/SA 교차 프로토콜 지원)
  uint8_t gw_addr_offset{2};  // GW 주소 위치 (QUERY 기준) = ACK 기준 DevType 위치
  uint8_t gw_addr{0x01};      // 버스에서 관측된 GW 자신의 RS-485 주소값 (기본: 0x01)
  // ★ 학습된 쿼리 패킷 길이 (버스 관측 기반, buildQueryPacket 동적 길이 사용)
  uint8_t learned_query_len{11};  // 관측된 쿼리 패킷 최빈 길이 (기본: 11)
  uint8_t len_offset{0xFF};       // 패킷 내 길이 필드 위치 (0xFF: 고정 프레임)
  bool has_len_field{false};      // 패킷 내 명시적 길이 필드 유무
  uint8_t seq_offset{0xFF};       // 시퀀스 카운터 위치 (0xFF: 없음)
  bool has_seq_counter{false};    // 시퀀스 카운터 유무
  uint8_t ack_flag_offset{0xFF};  // ACK/Status 플래그 위치 (0xFF: 없음)
  uint8_t learned_ctrl_lens[4]{0}; // 관측된 제어(CMD/CTL) 패킷 가변 길이 목록
  uint8_t ctrl_len_cnt{0};        // 관측된 제어 패킷 길이 가짓수
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
  void feedControlPair(span<const uint8_t> ctrl_req,
                       span<const uint8_t> ack_res);
  bool isLocked() const;
  bool isOffsetsLocked() const;
  bool analyzeCacheMatrix();
  AutoProbeDescriptor getDescriptor() const;
  void reset();
  uint8_t calculateChecksum(ChecksumAlgo algo, const uint8_t *data,
                            size_t len) const;
  static const char *getAlgoName(ChecksumAlgo algo);
};

extern AutoProbingEngine g_auto_probing_engine;

// ============================================================================
// PROFILE REPOSITORY (NVS-BACKED DATA PROFILES)
// ============================================================================

class ProfileRepository {
public:
  static constexpr size_t MAX_PROFILES =
      4; // Slot 0: Auto, Slot 1..3: User Saved Profiles

  static void init();
  static size_t getProfileCount();
  static bool getProfile(size_t index, VendorProfileDescriptor &out);
  static bool getProfileByKey(const char *key, VendorProfileDescriptor &out);
  static bool getActiveProfile(VendorProfileDescriptor &out);
  static bool setActiveProfileIndex(size_t index);
  static bool setActiveProfileByKey(const char *key);
  static bool saveCustomProfile(size_t index,
                                const VendorProfileDescriptor &profile);
  static bool saveCurrentAutoAs(const char *name, size_t &saved_idx);
  static bool deleteProfile(size_t index);
  static void syncAutoProfileToNvs(const AutoProbeDescriptor &auto_desc);
  static void resetAllToDefaults();
  static void inferVendorDescription(const AutoProbeDescriptor &ad, char *out_desc, size_t max_len);
};

// ============================================================================
// MODULAR WALLPAD PARSER INTERFACE & DATA-DRIVEN UNIVERSAL ENGINE
// ============================================================================

class IWallpadParser {
public:
  virtual ~IWallpadParser() = default;

  virtual const char *getVendorName() const = 0;
  virtual const char *getProfileKey() const = 0;
  virtual uint8_t getVendorId() const = 0;

  // Frame validation (STX, ETX, length, checksum)
  virtual bool validatePacket(span<const uint8_t> frame) const = 0;

  // Packet classification
  virtual bool isQueryPacket(span<const uint8_t> frame) const = 0;
  virtual bool isControlPacket(span<const uint8_t> frame) const = 0;
  virtual bool isAckPacket(span<const uint8_t> frame) const = 0;

  // Device key extraction
  virtual bool extractDeviceKey(span<const uint8_t> frame, uint8_t &dev_id,
                                uint8_t &sub1, uint8_t &sub2) const = 0;

  // Polling query frame builder
  virtual bool buildQueryPacket(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                                StaticPacket &out) const = 0;

  // Checksum calculation
  virtual uint8_t calculateChecksum(const uint8_t *data, size_t len) const = 0;

  // Framing characteristics
  virtual uint8_t getStx() const = 0;
  virtual uint8_t getEtx() const = 0;
  virtual uint8_t getMinPacketLen() const = 0;
  virtual uint8_t getMaxPacketLen() const = 0;
  virtual bool isLocked() const = 0;
  virtual bool isAutoMode() const = 0;

  // Stream packet length extraction:
  // > 0 : Full packet length extracted
  //   0 : Incomplete packet in stream (need more bytes)
  //  -1 : Invalid framing at stx_idx (skip STX)
  virtual int extractPacketLength(const uint8_t *stream, size_t stream_len,
                                  size_t stx_idx) const = 0;
};

class UniversalProtocolEngine : public IWallpadParser {
private:
  inline VendorProfileDescriptor activeProfile() const {
    VendorProfileDescriptor d;
    ProfileRepository::getActiveProfile(d);
    return d;
  }
  inline bool isAutoProfile(const VendorProfileDescriptor &desc) const {
    return strcasecmp(desc.key, "auto") == 0;
  }

public:
  const char *getVendorName() const override;
  const char *getProfileKey() const override;
  uint8_t getVendorId() const override;

  bool isLocked() const override {
    VendorProfileDescriptor d = activeProfile();
    if (isAutoProfile(d)) {
      return g_auto_probing_engine.isLocked();
    }
    return true;
  }
  bool isAutoMode() const override {
    VendorProfileDescriptor d = activeProfile();
    return isAutoProfile(d);
  }

  bool validatePacket(span<const uint8_t> frame) const override;
  bool isQueryPacket(span<const uint8_t> frame) const override;
  bool isControlPacket(span<const uint8_t> frame) const override;
  bool isAckPacket(span<const uint8_t> frame) const override;

  bool extractDeviceKey(span<const uint8_t> frame, uint8_t &dev_id,
                        uint8_t &sub1, uint8_t &sub2) const override;
  bool buildQueryPacket(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                        StaticPacket &out) const override;

  uint8_t calculateChecksum(const uint8_t *data, size_t len) const override;
  uint8_t getStx() const override;
  uint8_t getEtx() const override;
  uint8_t getMinPacketLen() const override;
  uint8_t getMaxPacketLen() const override;
  int extractPacketLength(const uint8_t *stream, size_t stream_len,
                          size_t stx_idx) const override;
};

// ============================================================================
// WALLPAD PARSER FACTORY (COMPATIBILITY FACADE)
// ============================================================================

class WallpadParserFactory {
public:
  static void init();
  static IWallpadParser *getActiveParser();
  static bool setProfile(uint8_t index);
  static bool setProfileByKey(const char *key);
  static size_t getParserCount();
};
