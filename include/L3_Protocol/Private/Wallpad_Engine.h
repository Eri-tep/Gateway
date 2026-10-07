#pragma once

// ============================================================================
// WallpadProtocol: Level 3 Wallpad Profiles, Protocol Engine & Probing Cache
// ============================================================================
// Consolidates Vendor Profiles, Universal Protocol Engine, Wallpad Orchestration,
// and Doorphone FSM into a single, cohesive private domain for Level 3 Protocol.
// ============================================================================

#include "L0_Foundation/System_Config.h"
#include "L0_Foundation/System_Platform.h"
#include "L3_Protocol/Public/Protocol_Device.h"
#include "L3_Protocol/Private/Wallpad_Learning.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <freertos/FreeRTOS.h>
#include <span>

// ============================================================================
// 1. DATA-DRIVEN VENDOR PROFILE DESCRIPTOR (STORED IN NVS)
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
  uint8_t gw_addr_offset{2};  // GW 주소 위치 (QUERY 기준) = ACK 기준 DevType 위치
  uint8_t gw_addr{0x01};      // GW 주소값
  uint8_t learned_query_len{11};   // 학습된 쿼리 길이
  uint8_t len_offset{0xFF};        // 패킷 내 길이 필드 위치 (0xFF: 고정 프레임)
  uint8_t has_len_field{0};        // 1: 길이 필드 보유, 0: 암묵적/고정 프레임
  uint8_t seq_offset{0xFF};        // 시퀀스 카운터 위치 (0xFF: 없음)
  uint8_t ack_flag_offset{0xFF};   // ACK 상태 플래그 위치 (0xFF: 없음)
  uint8_t learned_ctrl_lens[4]{0}; // 관측된 제어(CMD/CTL) 패킷 가변 길이 목록
  uint8_t ctrl_len_cnt{0};         // 관측된 제어 패킷 길이 가짓수
};

// ============================================================================
// 2. PROFILE REPOSITORY (NVS-BACKED DATA PROFILES)
// ============================================================================

class ProfileRepository {
public:
  static constexpr size_t MAX_PROFILES = 4; // Slot 0: Auto, Slot 1..3: User Saved Profiles

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
  static bool saveCustomProfile(size_t index, const VendorProfileDescriptor &profile);
  static bool saveCurrentAutoAs(const char *name, size_t &saved_idx);
  static bool deleteProfile(size_t index);
  static void syncAutoProfileToNvs(const AutoProbeDescriptor &auto_desc);
  static bool commitAutoProfileNvsIfPending() noexcept;
  static void resetAllToDefaults();
  static void inferVendorDescription(const AutoProbeDescriptor &ad, char *out_desc, size_t max_len);
};

// ============================================================================
// 3. UNIVERSAL PROTOCOL ENGINE
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
      return AutoProbe_GetEngine().isLocked();
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

  [[nodiscard]] std::expected<std::span<const uint8_t>, FrameValidationError>
  validateFrame(std::span<const uint8_t> frame) const noexcept;

  bool validatePacket(std::span<const uint8_t> frame) const;
  bool isQueryPacket(std::span<const uint8_t> frame) const;
  bool isControlPacket(std::span<const uint8_t> frame) const;
  bool isAckPacket(std::span<const uint8_t> frame) const;

  bool extractDeviceKey(std::span<const uint8_t> frame, uint8_t &dev_id,
                        uint8_t &sub1, uint8_t &sub2) const;
  bool buildQueryPacket(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                        StaticPacket &out) const;

  uint8_t calculateChecksum(std::span<const uint8_t> data) const noexcept;
  uint8_t calculateChecksum(const uint8_t *data, size_t len) const;
  uint8_t getStx() const;
  uint8_t getEtx() const;
  uint8_t getMinPacketLen() const;
  uint8_t getMaxPacketLen() const;
  int extractPacketLength(const uint8_t *stream, size_t stream_len, size_t stx_idx) const;
};

// ============================================================================
// 4. WALLPAD PARSER FACTORY (COMPATIBILITY FACADE)
// ============================================================================

class WallpadParserFactory {
public:
  static void init();
  static UniversalProtocolEngine *getActiveParser();
  static bool setProfile(uint8_t index);
};

// ============================================================================
// 5. HARDCODED PROFILE & DEVICE SPECIFICATIONS
// ============================================================================

struct DeviceSpec {
  uint8_t dev_id;
  DeviceClass dev_class;
  char name[16];

  uint8_t ctl_len;
  uint8_t ctl_payload_offset;
  uint8_t pwr_on_val;
  uint8_t pwr_off_val;
  uint8_t pwr_away_val;

  uint8_t qry_ack_len;
  uint8_t qry_power_offset;
  uint8_t qry_settemp_offset;
  uint8_t qry_ambtemp_offset;
  uint8_t qry_fanspeed_offset;
  bool qry_fanspeed_nibble;
  uint8_t qry_valve_offset;
  uint8_t qry_watt_h_offset;
  uint8_t qry_watt_l_offset;

  uint8_t ctl_ack_len;
  uint8_t ctl_ack_echo_offset;
  uint8_t ctl_ack_state_offset;
  uint8_t ctl_ack_ambtemp_offset;
};

enum class WallpadVendorId : uint8_t {
  UNKNOWN = 0,
  HYUNDAI,
  KOCOM,
  BESTIN,
  COMMAX,
  EZVILLE,
  SAMSUNG
};

struct DoorphoneSpec {
  uint32_t baud_rate;
  uint8_t stx;
  uint8_t etx;
  uint8_t len;
  const char *desc;
  uint8_t bell_front;
  uint8_t bell_lobby;
  uint8_t call_front;
  uint8_t call_lobby;
  uint8_t open_front;
  uint8_t open_lobby;
  uint8_t end_front;
  uint8_t end_lobby;
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

extern const WallpadProfile kHyundaiProfile;
extern const WallpadProfile *const kWallpadProfiles[];
extern const size_t kWallpadProfileCount;

namespace ProfileMatcher {
const WallpadProfile *matchProfile(const AutoProbeDescriptor &ad);
const WallpadProfile *getActiveProfile();
const DoorphoneSpec *matchDoorphone(uint8_t stx, uint8_t etx, uint8_t len);
} // namespace ProfileMatcher

// ============================================================================
// 6. WALLPAD PROTOCOL ORCHESTRATION & FSM INTERFACE
// ============================================================================

enum class ControlAction : uint8_t {
  DROP = 0,
  TRANSMIT_LOCAL,
  FORWARD_CH5,
  VIRTUAL_ACK_IMMEDIATE
};

bool Wallpad_BuildNextPollPacket(StaticPacket &out_pkt, uint8_t &poll_dev_id,
                                 uint8_t &poll_sub1, uint8_t &poll_sub2) noexcept;
void Wallpad_HandleBusPacket(uint8_t channel_id, const StaticPacket &ack_pkt,
                             const StaticPacket *matching_query) noexcept;
void Wallpad_HandlePollTimeout(uint8_t poll_dev_id, uint8_t poll_sub1,
                               uint8_t poll_sub2) noexcept;

ControlAction Wallpad_EvaluateControl(StaticPacket &req, StaticPacket &virtual_ack_out,
                                      bool &out_unidir) noexcept;

uint32_t Wallpad_GetPollIntervalMs() noexcept;
bool Wallpad_CheckConvergence(bool reset) noexcept;
void Wallpad_RequestRelearn() noexcept;
bool Wallpad_TakeRelearnRequest() noexcept;
uint32_t Wallpad_GetStalePollCount() noexcept;

uint8_t Wallpad_GetStx() noexcept;
bool Wallpad_IsAutoUnlocked() noexcept;
void Wallpad_FeedAutoFrame(std::span<const uint8_t> frame) noexcept;
int Wallpad_ExtractLength(const uint8_t *stream, size_t stream_len, size_t stx_idx) noexcept;
bool Wallpad_ValidatePacket(std::span<const uint8_t> frame) noexcept;

bool Wallpad_HandleSubBusQuery(uint8_t channel_id, const StaticPacket &req,
                               StaticPacket &virtual_ack_out) noexcept;
void Wallpad_FeedControlFrame(std::span<const uint8_t> frame) noexcept;

// ── Doorphone (CH4) Handling & FSM ──────────────────────────────────────────
struct FramingTracker {
  std::atomic<FramingStatus> status{FramingStatus::WAITING};
  std::atomic<uint8_t> candidate_stx{0};
  std::atomic<uint8_t> candidate_etx{0};
  std::atomic<uint8_t> candidate_len{0};
  std::atomic<uint8_t> consecutive_matches{0};
  std::atomic<uint8_t> consecutive_mismatches{0};
  std::atomic<bool> is_custom_fixed{false};

  void setFixedLock(uint8_t stx, uint8_t etx, uint8_t len) noexcept;
  void reset() noexcept;
  void clearNvs(const char *nvs_ns, const char *tag = "FRAMING") noexcept;
  void processFrame(uint8_t stx, uint8_t etx, uint8_t len, const char *nvs_ns,
                    const char *tag = "FRAMING") noexcept;

  static void getNvsNamespace(uint8_t prof_idx, char *out_ns, size_t max_len) noexcept {
    snprintf(out_ns, max_len, "dp_frame_p%u", static_cast<unsigned int>(prof_idx & 0x03));
  }

  void restoreFromNvs(const char *nvs_ns = "dp_frame_p0", const char *tag = "FRAMING") noexcept;
  void saveToNvs(const char *nvs_ns = "dp_frame_p0", const char *tag = "FRAMING") noexcept;
  [[nodiscard]] bool isConsistent(uint8_t stx, uint8_t etx) const noexcept;
};

void Wallpad_DoorphoneInit() noexcept;
bool Wallpad_DoorphoneOpen(bool is_lobby = false) noexcept;
void Wallpad_InitDecoupledHooks() noexcept;
bool Wallpad_DoorphoneStartSequence(uint8_t stx, uint8_t etx, uint8_t op_call,
                                    uint8_t op_open, uint8_t op_end) noexcept;
void Wallpad_DoorphoneCancel() noexcept;
[[nodiscard]] bool Wallpad_DoorphoneIsBusy() noexcept;
void Wallpad_DoorphoneGetState(bool &out_front_bell, bool &out_lobby_bell,
                              uint32_t &out_last_bell_ms) noexcept;
void Wallpad_DoorphoneGetFraming(FramingStatus &out_status, uint8_t &out_stx,
                                uint8_t &out_etx, uint8_t &out_len) noexcept;
bool Wallpad_DoorphoneGetLockedFraming(uint8_t &stx, uint8_t &etx, uint8_t &len) noexcept;
void Wallpad_DoorphoneFrameDetected(uint8_t stx, uint8_t etx, uint8_t len) noexcept;
void Wallpad_DoorphoneCheckBellTimeout() noexcept;
void Wallpad_DoorphoneClearNvs(const char *nvs_ns) noexcept;
void Wallpad_DoorphoneRestoreNvs(const char *nvs_ns) noexcept;
void Wallpad_DoorphoneSaveNvs(const char *nvs_ns) noexcept;
void Wallpad_DoorphoneOnProfileChanged(uint8_t old_idx, uint8_t new_idx) noexcept;

using DoorphoneTxHandler = void (*)(const StaticPacket &pkt) noexcept;
void Wallpad_DoorphoneRegisterTxHandler(DoorphoneTxHandler handler) noexcept;

void Wallpad_HandleDoorphonePacket(const StaticPacket &packet) noexcept;
void Wallpad_ResetDoorphoneBellState() noexcept;
const DoorphoneSpec *Wallpad_MatchDoorphone(uint8_t stx, uint8_t etx, uint8_t len) noexcept;
bool Wallpad_MatchDoorphoneLock(uint8_t stx, uint8_t etx, uint8_t len, uint8_t &out_fixed_len) noexcept;
bool Wallpad_IsQueryPacket(std::span<const uint8_t> frame) noexcept;
