#pragma once

// ============================================================================
// Wallpad_Parser: Level 3 Vendor Profile Repository & Frame Parser Interface
// ============================================================================

#include "L0_Foundation/System_Config.h"
#include "L3_Protocol/Public/Device_Registry.h"
#include "L3_Protocol/Private/Auto_Probing.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

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
// PROFILE REPOSITORY (NVS-BACKED DATA PROFILES)
// ============================================================================

class ProfileRepository {
public:
  static constexpr size_t MAX_PROFILES =
      4; // Slot 0: Auto, Slot 1..3: User Saved Profiles

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
// UNIVERSAL PROTOCOL ENGINE
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
