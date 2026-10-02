#pragma once

// ============================================================================
// WallpadProtocol: Level 3 Wallpad Profiles, Protocol Engine & Probing Cache
// ============================================================================

#include "Base/SystemConfig.h"
#include "Base/SystemPlatform.h"
#include "Protocol/ProtocolTypes.h"
#include "System/LockUtils.h"
#include "System/SystemStorage.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ============================================================================
// DEVICE SPECIFICATION (HARDCODED PER VENDOR)
// ============================================================================

struct DeviceSpec {
  uint8_t dev_id;
  DeviceClass dev_class;
  char name[16];

  // 제어 패킷 템플릿 (CTL)
  uint8_t ctl_len;
  uint8_t ctl_payload_offset; // 제어 파라미터(Cmd/온도 등) 바이트 오프셋 (현대:
                              // Byte #7)
  uint8_t pwr_on_val;         // 전원 ON 토큰 (0x01)
  uint8_t pwr_off_val;        // 전원 OFF 토큰 (0x02, 0x04 등)
  uint8_t pwr_away_val;       // 외출 모드 토큰 (0x07, 미사용 시 0xFF)

  // 쿼리 응답 상태 슬롯 (QRY ACK)
  uint8_t qry_ack_len;      // 쿼리 응답 패킷 전체 길이
  uint8_t qry_power_offset; // 운전/전원 상태 슬롯 (현대: Byte #8)
  uint8_t
      qry_settemp_offset; // 설정 희망온도 슬롯 (난방: Byte #10, 미사용 시 0xFF)
  uint8_t
      qry_ambtemp_offset; // 현재 환경온도 슬롯 (난방: Byte #9, 미사용 시 0xFF)
  uint8_t qry_fanspeed_offset; // 풍량 상태 슬롯 (환기: Byte #9, 미사용 시 0xFF)
  bool qry_fanspeed_nibble;    // 풍량 하위 4비트(buf & 0x0F) 마스킹 필요 여부
  uint8_t
      qry_valve_offset; // 밸브 차단 상태 슬롯 (가스: Byte #8, 미사용 시 0xFF)
  uint8_t qry_watt_h_offset; // 소비전력(W) 상위 바이트 (콘센트: Byte #9, 미사용
                             // 시 0xFF)
  uint8_t qry_watt_l_offset; // 소비전력(W) 하위 바이트 (콘센트: Byte #10,
                             // 미사용 시 0xFF)

  // 제어 응답 상태 슬롯 (CTL ACK)
  uint8_t ctl_ack_len;          // 제어 응답 패킷 전체 길이
  uint8_t ctl_ack_echo_offset;  // 제어 명령 에코 바이트 오프셋 (현대: Byte #7)
  uint8_t ctl_ack_state_offset; // 제어 후 확정 상태 슬롯 (현대: Byte #8)
  uint8_t ctl_ack_ambtemp_offset; // 제어 응답 내 현재온도 슬롯 (난방: Byte #9,
                                  // 미사용 시 0xFF)
};

// ============================================================================
// WALLPAD PROFILE DEFINITION
// ============================================================================

enum class WallpadVendorId : uint8_t {
  UNKNOWN = 0,
  HYUNDAI,
  KOCOM,
  BESTIN,
  COMMAX,
  EZVILLE,
  SAMSUNG
};

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

struct WallpadProfile {
  WallpadVendorId vendor_id;
  const char *vendor_name;
  uint8_t stx;
  uint8_t etx;
  ChecksumAlgo checksum_algo;
  uint8_t opcode_offset;
  uint8_t dev_id_offset;
  uint8_t sub1_offset;
  uint8_t sub2_offset;
  const DeviceSpec *devices;
  size_t device_count;
  DoorphoneSpec doorphone;
};

// ============================================================================
// WALLPAD PROFILES DECLARATIONS (Pure Declarations - Defined in Protocol.cpp)
// ============================================================================

extern const WallpadProfile kHyundaiProfile;
extern const WallpadProfile *const kWallpadProfiles[];
extern const size_t kWallpadProfileCount;

// ============================================================================
// From include/WallpadParser.h
// ============================================================================

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
  uint8_t is_swapped_addr{0}; // 1: DA/SA 교차 주소 모드, 0: 1:1 직접
  uint8_t gw_addr_offset{
      2};                // GW 주소 위치 (QUERY 기준) = ACK 기준 DevType 위치
  uint8_t gw_addr{0x01}; // GW 주소값
  uint8_t learned_query_len{11};   // 학습된 쿼리 길이
  uint8_t len_offset{0xFF};        // 패킷 내 길이 필드 위치 (0xFF: 고정 프레임)
  uint8_t has_len_field{0};        // 1: 길이 필드 보유, 0: 암묵적/고정 프레임
  uint8_t seq_offset{0xFF};        // 시퀀스 카운터 위치 (0xFF: 없음)
  uint8_t ack_flag_offset{0xFF};   // ACK 상태 플래그 위치 (0xFF: 없음)
  uint8_t learned_ctrl_lens[4]{0}; // 관측된 제어(CMD/CTL) 패킷 가변 길이 목록
  uint8_t ctrl_len_cnt{0};         // 관측된 제어 패킷 길이 가짓수
};

// ============================================================================
// 1ST TIER CACHE: POLLING TARGET REGISTRY
// ============================================================================

constexpr uint8_t kWallpadChMask = (1 << 2) | (1 << 3); // CH2, CH3 (월패드 유래)
constexpr uint32_t EXPIRED_TARGET_EVICTION_TIMEOUT_MS = 600000;

struct PollingTargetEntry {
  uint32_t last_requested_ms{0};
  uint32_t last_interval_ms{0};
  uint32_t restored_ms{0};
  uint16_t hit_count{0};
  uint8_t dev_id{0};
  uint8_t sub1{0};
  uint8_t sub2{0};
  uint8_t source_channels{0}; // Bitmask: bit 2=CH2, bit 3=CH3, bit 6=CH6
  uint8_t raw_query_len{0};
  uint8_t raw_ack_len{0};
  bool is_active{false};
  bool is_verified{
      true}; // False if restored from warm cache until ACK/request seen
  std::array<uint8_t, 64> raw_query_data{};
  std::array<uint8_t, 64> raw_ack_data{};
};

// ── 1st Tier Warm-Start Cache Data Structures (Level 3 Wallpad Domain) ──
struct RtcWarmCacheEntry {
  uint8_t dev_id;
  uint8_t sub1;
  uint8_t sub2;
  uint8_t source_channels;
  uint8_t raw_len;
  uint8_t raw_query[32];
};

struct RtcWarmCache {
  uint32_t magic; // 0x57415243 ('WARC')
  uint8_t count;
  uint8_t reserved[3];
  RtcWarmCacheEntry entries[48];
  uint32_t crc32;
};

constexpr uint32_t RTC_MAGIC_WARM_CACHE = 0x57415243; // 'WARC'

extern bool g_warm_cache_loaded;
extern uint8_t g_warm_cache_source; // 0: None/Cold, 1: RTC SRAM, 2: NVS Flash
extern uint8_t g_warm_cache_restored_count;
extern std::atomic<bool> g_warm_cache_dirty;
extern std::atomic<uint32_t> g_warm_cache_dirty_ms;

void WarmCache_SaveToRtc();
void WarmCache_SaveToNvs();
void WarmCache_RestoreOnBoot();
void WarmCache_CheckNvsDebounce();

class PollingTargetRegistry {
public:
  // 현대통신 세대망 환경(실제 28대 기기 수용 + 20대 여유 슬롯) 48대 설정
  static constexpr size_t MAX_TARGETS = 48;

  // 폴링 후보 선별을 위한 경량 메타데이터 구조체 (7 bytes)
  struct PollingCandidate {
    uint8_t dev_id{0};
    uint8_t sub1{0};
    uint8_t sub2{0};
    uint8_t source_channels{0};
    uint8_t raw_ack_len{0};
    uint8_t raw_query_len{0};
    uint8_t entry_idx{0};
  };

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
  size_t getActiveCandidates(PollingCandidate *out_cands, size_t max_count);
  bool getQueryData(uint8_t entry_idx, const uint8_t *&out_data,
                    uint8_t &out_len) const;
  size_t activeCount() const;
  size_t totalCount() const;
  size_t ackedCount() const;
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

// ============================================================================
// PROFILE REPOSITORY (NVS-BACKED DATA PROFILES)
// ============================================================================

class ProfileRepository {
public:
  static constexpr size_t MAX_PROFILES =
      4; // Slot 0: Auto, Slot 1..3: User Saved Profiles

  // Decoupled profile change listeners
  using ProfileChangeCallbackFn = void (*)(uint8_t old_idx, uint8_t new_idx);
  static void addProfileChangeListener(ProfileChangeCallbackFn cb);
  static void setProfileChangeListener(ProfileChangeCallbackFn cb);

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
  static void inferVendorDescription(const AutoProbeDescriptor &ad,
                                     char *out_desc, size_t max_len);
};

// ============================================================================
// WALLPAD PROTOCOL ENGINE
// ============================================================================

class UniversalProtocolEngine {
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
  static constexpr size_t kVendorNameMaxLen = 64;
  static constexpr size_t kProfileKeyMaxLen = 16;

  size_t getVendorName(char *out, size_t max_len) const;
  size_t getActiveProfileKey(char *out, size_t max_len) const;

  bool isLocked() const {
    VendorProfileDescriptor d = activeProfile();
    if (isAutoProfile(d)) {
      return g_auto_probing_engine.isLocked();
    }
    return true;
  }
  bool isAutoMode() const {
    VendorProfileDescriptor d = activeProfile();
    return isAutoProfile(d);
  }

  enum class FrameValidationError : uint8_t {
    InvalidLength,
    HeaderMismatch,
    ChecksumMismatch,
  };

  [[nodiscard]] std::expected<span<const uint8_t>, FrameValidationError>
  validateFrame(span<const uint8_t> frame) const noexcept;

  bool validatePacket(span<const uint8_t> frame) const;
  bool isQueryPacket(span<const uint8_t> frame) const;
  bool isControlPacket(span<const uint8_t> frame) const;
  bool isAckPacket(span<const uint8_t> frame) const;

  bool extractDeviceKey(span<const uint8_t> frame, uint8_t &dev_id,
                        uint8_t &sub1, uint8_t &sub2) const;
  bool buildQueryPacket(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                        StaticPacket &out) const;

  uint8_t calculateChecksum(span<const uint8_t> data) const noexcept;
  uint8_t calculateChecksum(const uint8_t *data, size_t len) const;
  uint8_t getStx() const;
  uint8_t getEtx() const;
  uint8_t getMinPacketLen() const;
  uint8_t getMaxPacketLen() const;
  int extractPacketLength(const uint8_t *stream, size_t stream_len,
                          size_t stx_idx) const;
};

// ============================================================================
// WALLPAD PARSER FACTORY (COMPATIBILITY FACADE)
// ============================================================================

class WallpadParserFactory {
public:
  static void init();
  static UniversalProtocolEngine *getActiveParser();
  static bool setProfile(uint8_t index);
};

// ============================================================================
// From include/ProfileMatcher.h
// ============================================================================

namespace ProfileMatcher {

// 현재 수렴/잠금된 AutoProbeDescriptor를 기반으로 일치하는 제조사 프로파일을
// 검색합니다.
const WallpadProfile *matchProfile(const AutoProbeDescriptor &ad);

// 현재 활성화/매칭된 프로파일 반환 (기본값: kHyundaiProfile)
const WallpadProfile *getActiveProfile();

// 도어폰 패킷 헤더 매칭
const DoorphoneSpec *matchDoorphone(uint8_t stx, uint8_t etx, uint8_t len);

} // namespace ProfileMatcher
