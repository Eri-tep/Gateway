#pragma once

#include "Core.h"


// ============================================================================
// From include/ControlTemplate.h
// ============================================================================

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstdint>
#include <cstddef>
#include <cstring>

struct AutoProbeDescriptor;

// ============================================================================
// CONTROL ACTION TYPES & SLOTS
// ============================================================================

enum class ControlActionType : uint8_t {
  POWER = 0,    // 전원 ON / OFF
  SET_TEMP,     // 설정 온도 변경 (난방/에어컨)
  FAN_SPEED,    // 풍량 변경 (환기/에어컨)
  VALVE_CLOSE,  // 밸브 닫기 (가스)
  MOMENTARY_TRIGGER, // 순간 호출 (엘리베이터 등)
  VENT_MODE,    // 운전 모드 변경 (환기: 일반 0x01, 바이패스 0x02, 자동 0x03)
  UNKNOWN = 0xFF
};

struct ActionSlot {
  bool discovered{false};
  uint8_t category_offset{0xFF}; // 카테고리/서브1 위치 (현대 0x45, 0x46 등 [CTX])
  uint8_t category_val{0x00};    // 해당 액션의 카테고리 바이트 값
  uint8_t action_offset{0xFF};   // 제어 파라미터가 위치하는 바이트 오프셋 ([VAL])
  uint8_t telemetry_offset{0xFF};// 실시간 환경 센서 텔레메트리 바이트 오프셋 ([ENV] 현재온도 등)
  uint8_t on_val{0x01};          // ON / Active 토큰
  uint8_t off_val{0x02};         // OFF / Inactive 토큰
  uint8_t min_val{0};            // 최소값 (온도 15℃, 풍량 1 등)
  uint8_t max_val{0};            // 최대값 (온도 30℃, 풍량 3 등)
  uint8_t level_tokens[4]{0};    // 이산 단계별 토큰 (예: 풍량 L1/L2/L3 등)
  uint8_t level_count{0};        // 등록된 이산 단계 토큰 개수
  uint8_t ack_state_offset{0xFF};     // 이 컨텍스트 채널 ACK 내 운전/가동 상태 오프셋
  uint8_t ack_target_offset{0xFF};    // 이 컨텍스트 채널 ACK 내 설정/제어값 오프셋
  uint8_t ack_telemetry_offset{0xFF}; // 이 컨텍스트 채널 ACK 내 환경 센서(현재온도 등) 오프셋
  uint16_t sample_count{0};
};

// ============================================================================
// DEVICE CAPABILITY CLASSIFICATION & SLOT COVERAGE
// ============================================================================

enum class DeviceClass : uint8_t {
  UNKNOWN     = 0,
  SWITCH      = 1, // 지속 릴레이 (ON/OFF) - 조명, 일괄소등
  OUTLET      = 2, // 스마트 콘센트 (대기전력/소비전력 모니터링 포함)
  GAS         = 3, // 차단 밸브 (단방향 닫기 / 차단) - 가스 밸브
  MOMENTARY   = 4, // 단방향 순간 펄스 트리거 (호출) - 엘리베이터 호출, 현관문 열림
  THERMOSTAT  = 5, // 연속 희망온도 파라미터 - 난방
  VENT        = 6, // 이산 다단계 풍량 파라미터 - 환기
  AIRCON      = 7  // 온도 + 풍량 복합 파라미터 - 에어컨
};

struct SlotCoverage {
  DeviceClass dev_class{DeviceClass::UNKNOWN};
};

struct DecodedDeviceState {
  int power{0};
  int target_temp{0};
  int current_temp{0};
  int fan_speed{0};
  int vent_mode{1};
  float power_w{0.0f};
  int floor{1};
  int direction{0};
  int ho{0};
  char valve_state[8]{"closed"}; // 고정 8바이트 버퍼로 수명 안전 보장
  DeviceClass dev_class{DeviceClass::UNKNOWN};
  bool should_broadcast{false};
};

inline const char *DeviceClassToName(DeviceClass cls) {
  static constexpr const char *kNames[] = {
    "Unknown", "Light", "Outlet", "Gas", "Elevator", "Thermo", "Vent", "Aircon"
  };
  const size_t idx = static_cast<size_t>(cls);
  return (idx < sizeof(kNames) / sizeof(kNames[0])) ? kNames[idx] : "Unknown";
}

inline const char *DeviceClassToCliString(DeviceClass cls) {
  static constexpr const char *kCliNames[] = {
    "UNKNOWN", "SWITCH", "OUTLET", "GAS", "MOMENT", "THERMO", "VENT", "AIRCON"
  };
  const size_t idx = static_cast<size_t>(cls);
  return (idx < sizeof(kCliNames) / sizeof(kCliNames[0])) ? kCliNames[idx] : "UNKNOWN";
}

inline const char *DeviceClassToTelemetryString(DeviceClass cls) {
  static constexpr const char *kTeleNames[] = {
    "unknown", "switch", "outlet", "gas", "momentary", "thermostat", "vent", "aircon"
  };
  const size_t idx = static_cast<size_t>(cls);
  return (idx < sizeof(kTeleNames) / sizeof(kTeleNames[0])) ? kTeleNames[idx] : "unknown";
}

// ============================================================================
// GROUP CONTROL TEMPLATE (CONTROL BLUEPRINT)
// ============================================================================

struct PacketSnapshot {
  uint8_t len{0};
  uint8_t raw[32]{0};
};

struct AckStateSlots {
  bool discovered{false};
  uint8_t power_offset{0xFF};        // ACK 내 전원/가동 상태 위치 [AS]
  uint8_t target_temp_offset{0xFF};  // ACK 내 설정 희망온도 위치 [TT]
  uint8_t current_temp_offset{0xFF}; // ACK 내 현재 환경온도 위치 [AT]
  uint8_t fan_speed_offset{0xFF};    // ACK 내 풍량 상태 위치 [FS]
  uint8_t valve_state_offset{0xFF};  // ACK 내 밸브 차단 상태 위치 [VS]
  uint8_t sample_count{0};
  uint32_t power_changed_mask{0};
};

struct QueryStateSlots {
  bool discovered{false};
  uint8_t expected_len{0};          // 쿼리 응답 패킷 길이 (예: 콘센트 18B, 난방 18B 등)
  uint8_t power_offset{0xFF};       // 평상시 전원/가동 상태 바이트 오프셋 [AS]
  uint8_t target_temp_offset{0xFF}; // 희망 설정온도 오프셋 [TT]
  uint8_t current_temp_offset{0xFF};// 현재 환경온도 오프셋 [AT]
  uint8_t fan_speed_offset{0xFF};   // 환기 풍량 오프셋 [FS]
  uint8_t power_w_offset{0xFF};     // 콘센트 실시간 소비전력(W) 오프셋
  uint8_t valve_state_offset{0xFF}; // 가스 차단 상태 오프셋 [VS]
  bool is_bitmap_power{false};      // 다채널 비트맵 전원 여부
};

struct GroupControlTemplate {
  uint8_t dev_id{0x00};          // 기기 그룹 코드 (예: 0x19 조명, 0x18 난방 등)
  char group_name[16]{"Unknown"};// 그룹 명칭 ("Light", "Thermo", "Vent" 등)
  uint8_t frame_len{0};          // 제어 패킷 프레임 길이 (11, 13 등)
  uint8_t raw_template[32]{0};   // 기본 제어 프레임 골격
  
  // 주소 마스킹 오프셋
  uint8_t sub1_offset{0xFF};     // 방 번호(Sub1) 주입 오프셋
  uint8_t sub2_offset{0xFF};     // 기기 번호(Sub2) 주입 오프셋
  uint8_t ctl_sub1_override{0xFF};// 전열교환기 등 특수 sub1 고정값 (0x40 등)
  
  // 기능별 액션 슬롯
  ActionSlot power_slot;         // 전원 제어 슬롯
  ActionSlot temp_slot;          // 온도 제어 슬롯 (난방)
  ActionSlot speed_slot;         // 풍량 제어 슬롯 (환기)
  ActionSlot mode_slot;          // 운전 모드 슬롯
  ActionSlot close_slot;         // 닫기 제어 슬롯 (가스)
  
  SlotCoverage coverage;         // 슬롯 매핑 정보
  uint8_t away_mode_token{0xFF}; // 외출 시 전원/모드 바이트 코드 (현대: 0x07)

  // ACK 상태 슬롯 (제어 트랜잭션 응답)
  AckStateSlots ack_slots;

  // 쿼리 상태 슬롯 (수기/명세 주입)
  QueryStateSlots query_slots;

  // ── 통합 슬롯 접근자 ──
  inline uint8_t getPowerOffset(uint8_t pkt_len = 0) const {
    if (pkt_len > 0) {
      if (frame_len > 0 && pkt_len == frame_len && ack_slots.discovered && ack_slots.power_offset != 0xFF) {
        return ack_slots.power_offset;
      }
      if (query_slots.discovered && query_slots.power_offset != 0xFF) {
        return query_slots.power_offset;
      }
    }
    if (ack_slots.discovered && ack_slots.power_offset != 0xFF) return ack_slots.power_offset;
    if (query_slots.discovered && query_slots.power_offset != 0xFF) return query_slots.power_offset;
    if (power_slot.discovered && power_slot.ack_state_offset != 0xFF) return power_slot.ack_state_offset;
    return 0xFF;
  }

  inline uint8_t getTargetTempOffset(uint8_t pkt_len = 0) const {
    if (pkt_len > 0) {
      if (frame_len > 0 && pkt_len == frame_len && ack_slots.discovered && ack_slots.target_temp_offset != 0xFF) {
        return ack_slots.target_temp_offset;
      }
      if (query_slots.discovered && query_slots.target_temp_offset != 0xFF) {
        return query_slots.target_temp_offset;
      }
    }
    if (query_slots.discovered && query_slots.target_temp_offset != 0xFF) return query_slots.target_temp_offset;
    if (ack_slots.discovered && ack_slots.target_temp_offset != 0xFF) return ack_slots.target_temp_offset;
    return 0xFF;
  }

  inline uint8_t getCurrentTempOffset(uint8_t pkt_len = 0) const {
    if (pkt_len > 0) {
      if (frame_len > 0 && pkt_len == frame_len && ack_slots.discovered && ack_slots.current_temp_offset != 0xFF) {
        return ack_slots.current_temp_offset;
      }
      if (query_slots.discovered && query_slots.current_temp_offset != 0xFF) {
        return query_slots.current_temp_offset;
      }
    }
    if (query_slots.discovered && query_slots.current_temp_offset != 0xFF) return query_slots.current_temp_offset;
    if (ack_slots.discovered && ack_slots.current_temp_offset != 0xFF) return ack_slots.current_temp_offset;
    return 0xFF;
  }

  inline uint8_t getFanSpeedOffset(uint8_t pkt_len = 0) const {
    if (pkt_len > 0) {
      if (frame_len > 0 && pkt_len == frame_len && ack_slots.discovered && ack_slots.fan_speed_offset != 0xFF) {
        return ack_slots.fan_speed_offset;
      }
      if (query_slots.discovered && query_slots.fan_speed_offset != 0xFF) {
        return query_slots.fan_speed_offset;
      }
    }
    if (query_slots.discovered && query_slots.fan_speed_offset != 0xFF) return query_slots.fan_speed_offset;
    if (ack_slots.discovered && ack_slots.fan_speed_offset != 0xFF) return ack_slots.fan_speed_offset;
    return 0xFF;
  }

  inline uint8_t decodeFanSpeed(uint8_t raw_token) const {
    if (speed_slot.level_count > 0) {
      for (uint8_t i = 0; i < speed_slot.level_count; ++i) {
        if (speed_slot.level_tokens[i] == raw_token) {
          return static_cast<uint8_t>(i + 1);
        }
      }
    }
    // 레벨 토큰 매핑 실패 또는 미설정 시 기본 1~3단 및 레거시/실측 토큰 호환
    // 실측 토큰: 0x11 (1단), 0x13 (2단), 0x17 (3단), 0x10 (자동/가변)
    if (raw_token == 0x11 || raw_token == 0x01) return 1;
    if (raw_token == 0x13 || raw_token == 0x03 || raw_token == 2) return 2;
    if (raw_token == 0x17 || raw_token == 0x07 || raw_token == 3) return 3;
    if (raw_token == 0x10) return 1; // 자동 가변 풍량 시 기본 1단 매핑
    if (raw_token >= 1 && raw_token <= 3) return raw_token;
    return 1;
  }

  inline uint8_t decodeVentMode(uint8_t raw_byte) const {
    // Byte #8 운전 모드 토큰 (1:일반, 2:바이패스, 3:자동, 4:공기청정, 0x81:Reject)
    if (raw_byte >= 1 && raw_byte <= 4) return raw_byte;
    uint8_t nibble_mode = (raw_byte >> 4) & 0x0F;
    if (nibble_mode >= 1 && nibble_mode <= 4) return nibble_mode;
    return 1; // 기본 일반 환기 (0x01)
  }

  inline uint8_t getValveStateOffset(uint8_t pkt_len = 0) const {
    if (pkt_len > 0) {
      if (frame_len > 0 && pkt_len == frame_len && ack_slots.discovered && ack_slots.valve_state_offset != 0xFF) {
        return ack_slots.valve_state_offset;
      }
      if (query_slots.discovered && query_slots.valve_state_offset != 0xFF) {
        return query_slots.valve_state_offset;
      }
    }
    if (query_slots.discovered && query_slots.valve_state_offset != 0xFF) return query_slots.valve_state_offset;
    if (ack_slots.discovered && ack_slots.valve_state_offset != 0xFF) return ack_slots.valve_state_offset;
    return 0xFF;
  }

  inline uint8_t getWattageOffset(uint8_t pkt_len = 0) const {
    if (pkt_len > 0 && pkt_len < 16) return 0xFF;
    if (query_slots.discovered && query_slots.power_w_offset != 0xFF) return query_slots.power_w_offset;
    return 0xFF;
  }

  inline bool isUnidirectional() const {
    return dev_id == 0x34; // 기존 코드의 dev_id == 0x34 단방향 버스트 전송 의미와 100% 일치
  }
};

static_assert(
    sizeof(GroupControlTemplate) == 180,
    "NVS ABI break: GroupControlTemplate size changed"
);

// ============================================================================
// CONTROL TEMPLATE REGISTRY
// ============================================================================

class ControlTemplateRegistry {
public:
  static constexpr size_t MAX_GROUPS = 8;
  static constexpr TickType_t kQueryLockTimeout = pdMS_TO_TICKS(5);
  static constexpr TickType_t kManageLockTimeout = pdMS_TO_TICKS(50);

  ControlTemplateRegistry();

  void init();
  void clear();

  // 수렴 완료 시점 자동 골격 합성 (ProfileMatcher 연계)
  void synthesizeFromConvergedCache();

  // 그룹 등록 및 조회
  bool findGroup(uint8_t dev_id, GroupControlTemplate &out, TickType_t timeout = kQueryLockTimeout) const;
  size_t getGroupsSnapshot(GroupControlTemplate *out_buf, size_t max_count, TickType_t timeout = kQueryLockTimeout) const;
  size_t getGroupCount() const;
  bool getGroupByIndex(size_t index, GroupControlTemplate &out) const;
  bool resetGroup(uint8_t dev_id, bool full_reset = false);
  bool setGroupName(uint8_t dev_id, const char *name);
  bool setGroupClass(uint8_t dev_id, DeviceClass cls, const char *name = nullptr);

  template <typename Func>
  bool modifyOrCreateGroup(uint8_t dev_id, Func&& mutator, const char *initial_name = nullptr, TickType_t timeout = kManageLockTimeout) {
    if (dev_id == 0) return false;
    MutexLocker lock(_mutex, timeout);
    if (!lock.isLocked()) return false;
    GroupControlTemplate *grp = registerOrTouchUnlocked(dev_id, initial_name);
    if (!grp) return false;
    mutator(*grp);
    return true;
  }

  // 제어 패킷 조립 (스마트싱스 및 외부 연동 공용)
  bool buildControlPacket(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                          ControlActionType action, int value,
                          StaticPacket &out) const;

  // NVS 저장 / 복원 (프로파일 격리 지원)
  void saveToNvs();
  void loadFromNvs();
  void saveToNvsForProfile(uint8_t prof_idx);
  void loadFromNvsForProfile(uint8_t prof_idx);
  void onProfileChanged(uint8_t old_prof_idx, uint8_t new_prof_idx);

private:
  GroupControlTemplate _groups[MAX_GROUPS];
  size_t _group_count{0};
  mutable StaticSemaphore_t _mutex_storage{};
  mutable SemaphoreHandle_t _mutex{nullptr};
  mutable StaticSemaphore_t _nvs_mutex_storage{};
  mutable SemaphoreHandle_t _nvs_mutex{nullptr};

  void autoAssignGroupName(GroupControlTemplate &group);
  GroupControlTemplate *registerOrTouchUnlocked(uint8_t dev_id, const char *name = nullptr);
};

extern ControlTemplateRegistry g_control_registry;

namespace ControlTemplateUtils {
inline void getControlNamespace(char *out_ns, size_t max_len, uint8_t prof_idx) {
  snprintf(out_ns, max_len, "ctl_p%u", prof_idx);
}

inline uint8_t getCurrentProfileIndex() {
  CriticalSectionLocker lock(&g_config_mux);
  return g_config.wallpad_profile;
}
} // namespace ControlTemplateUtils

// ============================================================================
// From include/core/WallpadProfile.h
// ============================================================================

#include <cstdint>
#include <cstddef>

// ============================================================================
// DEVICE SPECIFICATION (HARDCODED PER VENDOR)
// ============================================================================

struct DeviceSpec {
  uint8_t dev_id;
  DeviceClass dev_class;
  char name[16];

  // 제어 패킷 템플릿 (CTL)
  uint8_t ctl_len;
  uint8_t ctl_payload_offset;   // 제어 파라미터(Cmd/온도 등) 바이트 오프셋 (현대: Byte #7)
  uint8_t pwr_on_val;           // 전원 ON 토큰 (0x01)
  uint8_t pwr_off_val;          // 전원 OFF 토큰 (0x02, 0x04 등)
  uint8_t pwr_away_val;         // 외출 모드 토큰 (0x07, 미사용 시 0xFF)

  // 쿼리 응답 상태 슬롯 (QRY ACK)
  uint8_t qry_ack_len;          // 쿼리 응답 패킷 전체 길이
  uint8_t qry_power_offset;     // 운전/전원 상태 슬롯 (현대: Byte #8)
  uint8_t qry_settemp_offset;   // 설정 희망온도 슬롯 (난방: Byte #10, 미사용 시 0xFF)
  uint8_t qry_ambtemp_offset;   // 현재 환경온도 슬롯 (난방: Byte #9, 미사용 시 0xFF)
  uint8_t qry_fanspeed_offset;  // 풍량 상태 슬롯 (환기: Byte #9, 미사용 시 0xFF)
  bool    qry_fanspeed_nibble;  // 풍량 하위 4비트(buf & 0x0F) 마스킹 필요 여부
  uint8_t qry_valve_offset;     // 밸브 차단 상태 슬롯 (가스: Byte #8, 미사용 시 0xFF)
  uint8_t qry_watt_h_offset;    // 소비전력(W) 상위 바이트 (콘센트: Byte #9, 미사용 시 0xFF)
  uint8_t qry_watt_l_offset;    // 소비전력(W) 하위 바이트 (콘센트: Byte #10, 미사용 시 0xFF)

  // 제어 응답 상태 슬롯 (CTL ACK)
  uint8_t ctl_ack_len;          // 제어 응답 패킷 전체 길이
  uint8_t ctl_ack_echo_offset;  // 제어 명령 에코 바이트 오프셋 (현대: Byte #7)
  uint8_t ctl_ack_state_offset; // 제어 후 확정 상태 슬롯 (현대: Byte #8)
  uint8_t ctl_ack_ambtemp_offset; // 제어 응답 내 현재온도 슬롯 (난방: Byte #9, 미사용 시 0xFF)
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
  NONE = 8             // Pure framing without checksum byte (Doorphone 0x02..0x03)
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
// HYUNDAI WALLPAD PROFILE (현대통신 실측 데이터 기반 정규화)
// ============================================================================

constexpr DeviceSpec kHyundaiDevices[] = {
  // 0x19 일반 조명 (Switch)
  {
    0x19, DeviceClass::SWITCH, "Light",
    11, 7, 0x01, 0x02, 0xFF,
    11, 8, 0xFF, 0xFF, 0xFF, false, 0xFF, 0xFF, 0xFF,
    11, 7, 8, 0xFF
  },
  // 0x18 난방 / 보일러 (Thermostat)
  {
    0x18, DeviceClass::THERMOSTAT, "Thermo",
    11, 7, 0x01, 0x04, 0x07,
    18, 8, 10, 9, 0xFF, false, 0xFF, 0xFF, 0xFF,
    13, 7, 8, 9
  },
  // 0x1F 콘센트 (Outlet)
  {
    0x1F, DeviceClass::OUTLET, "Outlet",
    11, 7, 0x01, 0x02, 0xFF,
    18, 8, 0xFF, 0xFF, 0xFF, false, 0xFF, 9, 10,
    11, 7, 8, 0xFF
  },
  // 0x2B 환기 / 전열교환기 (Vent)
  {
    0x2B, DeviceClass::VENT, "Vent",
    11, 7, 0x01, 0x02, 0xFF,
    13, 8, 0xFF, 0xFF, 9, true, 0xFF, 0xFF, 0xFF,
    13, 7, 8, 0xFF
  },
  // 0x1B 가스 차단기 (Gas)
  {
    0x1B, DeviceClass::GAS, "Gas",
    11, 7, 0x00, 0x02, 0xFF,
    13, 0xFF, 0xFF, 0xFF, 0xFF, false, 8, 0xFF, 0xFF,
    13, 7, 8, 0xFF
  },
  // 0x34 엘리베이터 (Momentary)
  {
    0x34, DeviceClass::MOMENTARY, "Elevator",
    11, 7, 0x06, 0x00, 0xFF,
    13, 8, 0xFF, 0xFF, 0xFF, false, 0xFF, 0xFF, 0xFF,
    11, 7, 8, 0xFF
  },
  // 0x1C 시스템 에어컨 / FCU (Aircon)
  {
    0x1C, DeviceClass::AIRCON, "Aircon",
    11, 7, 0x01, 0x02, 0xFF,
    15, 8, 12, 11, 10, true, 9, 0xFF, 0xFF,
    11, 7, 8, 0xFF
  }
};

constexpr WallpadProfile kHyundaiProfile = {
  WallpadVendorId::HYUNDAI,
  "Hyundai HT",
  0xF7,
  0xEE,
  ChecksumAlgo::XOR_NO_STX,
  4, // opcode_offset
  2, // dev_id_offset
  6, // sub1_offset
  6, // sub2_offset
  kHyundaiDevices,
  sizeof(kHyundaiDevices) / sizeof(kHyundaiDevices[0]),
  {
    3860, 0x7F, 0xEE, 5, "Hyundai HT Standard",
    0xB5, 0x5A, 0xB9, 0x5F, 0xB4, 0x61, 0xB8, 0x60
  }
};

constexpr const WallpadProfile *kWallpadProfiles[] = {
  &kHyundaiProfile
};

constexpr size_t kWallpadProfileCount = sizeof(kWallpadProfiles) / sizeof(kWallpadProfiles[0]);

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
// 1ST TIER CACHE: POLLING TARGET REGISTRY
// ============================================================================

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
  bool is_verified{true}; // False if restored from warm cache until ACK/request seen
  std::array<uint8_t, 64> raw_query_data{};
  std::array<uint8_t, 64> raw_ack_data{};
};

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
  bool getQueryData(uint8_t entry_idx, const uint8_t *&out_data, uint8_t &out_len) const;
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
  uint8_t dev_id_offset{3};   // DevType 위치 (QUERY 기준 / swap 없으면 ACK도 동일)
  uint8_t sub1_offset{5};
  uint8_t sub2_offset{6};
  uint8_t payload_offset{8};
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
  void feedControlFrame(span<const uint8_t> ctrl_frame);
  bool isLocked() const;
  bool isOffsetsLocked() const;
  AutoProbeDescriptor getDescriptor() const;
  void reset();
  void injectControlSpec(uint8_t ctrl_op, uint8_t ctrl_len);
  bool analyzeCacheMatrix();
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

  bool validatePacket(span<const uint8_t> frame) const;
  bool isQueryPacket(span<const uint8_t> frame) const;
  bool isControlPacket(span<const uint8_t> frame) const;
  bool isAckPacket(span<const uint8_t> frame) const;

  bool extractDeviceKey(span<const uint8_t> frame, uint8_t &dev_id,
                        uint8_t &sub1, uint8_t &sub2) const;
  bool buildQueryPacket(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                        StaticPacket &out) const;

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


class ControlTemplateRegistry;

namespace ProfileMatcher {

// 현재 수렴/잠금된 AutoProbeDescriptor를 기반으로 일치하는 제조사 프로파일을 검색합니다.
const WallpadProfile *matchProfile(const AutoProbeDescriptor &ad);

// 현재 활성화/매칭된 프로파일 반환 (기본값: kHyundaiProfile)
const WallpadProfile *getActiveProfile();

// 도어폰 패킷 헤더 매칭
const DoorphoneSpec *matchDoorphone(uint8_t stx, uint8_t etx, uint8_t len);

// 특정 제조사 프로파일의 기기 명세(DeviceSpec)를 ControlTemplateRegistry에 주입합니다.
void injectProfile(const WallpadProfile *profile, ControlTemplateRegistry &registry);

// 캐시 수렴 시 호출되는 원스톱 엔트리포인트 (매칭 후 자동 슬롯 주입)
void matchAndInject(const AutoProbeDescriptor &ad, ControlTemplateRegistry &registry);

} // namespace ProfileMatcher
