#pragma once

// ============================================================================
// ControlTemplate: Level 3 Device Capability Blueprint & Group Control Engine
// ============================================================================

#include "Base/SystemPlatform.h"
#include "Base/SystemConfig.h"
#include "System/LockUtils.h"

#include <Arduino.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ============================================================================
// CONTROL ACTION TYPES & SLOTS
// ============================================================================

enum class ControlActionType : uint8_t {
  POWER = 0,         // 전원 ON / OFF
  SET_TEMP,          // 설정 온도 변경 (난방/에어컨)
  FAN_SPEED,         // 풍량 변경 (환기/에어컨)
  VALVE_CLOSE,       // 밸브 닫기 (가스)
  MOMENTARY_TRIGGER, // 순간 호출 (엘리베이터 등)
  VENT_MODE, // 운전 모드 변경 (환기: 일반 0x01, 바이패스 0x02, 자동 0x03)
  UNKNOWN = 0xFF
};

struct ActionSlot {
  bool discovered{false};
  uint8_t category_offset{
      0xFF};                   // 카테고리/서브1 위치 (현대 0x45, 0x46 등 [CTX])
  uint8_t category_val{0x00};  // 해당 액션의 카테고리 바이트 값
  uint8_t action_offset{0xFF}; // 제어 파라미터가 위치하는 바이트 오프셋 ([VAL])
  uint8_t telemetry_offset{
      0xFF}; // 실시간 환경 센서 텔레메트리 바이트 오프셋 ([ENV] 현재온도 등)
  uint8_t on_val{0x01};       // ON / Active 토큰
  uint8_t off_val{0x02};      // OFF / Inactive 토큰
  uint8_t min_val{0};         // 최소값 (온도 15℃, 풍량 1 등)
  uint8_t max_val{0};         // 최대값 (온도 30℃, 풍량 3 등)
  uint8_t level_tokens[4]{0}; // 이산 단계별 토큰 (예: 풍량 L1/L2/L3 등)
  uint8_t level_count{0};     // 등록된 이산 단계 토큰 개수
  uint8_t ack_state_offset{
      0xFF}; // 이 컨텍스트 채널 ACK 내 운전/가동 상태 오프셋
  uint8_t ack_target_offset{0xFF}; // 이 컨텍스트 채널 ACK 내 설정/제어값 오프셋
  uint8_t ack_telemetry_offset{
      0xFF}; // 이 컨텍스트 채널 ACK 내 환경 센서(현재온도 등) 오프셋
  uint16_t sample_count{0};
};

// ============================================================================
// DEVICE CAPABILITY CLASSIFICATION & SLOT COVERAGE
// ============================================================================

enum class DeviceClass : uint8_t {
  UNKNOWN = 0,
  SWITCH = 1, // 지속 릴레이 (ON/OFF) - 조명, 일괄소등
  OUTLET = 2, // 스마트 콘센트 (대기전력/소비전력 모니터링 포함)
  GAS = 3,    // 차단 밸브 (단방향 닫기 / 차단) - 가스 밸브
  MOMENTARY =
      4, // 단방향 순간 펄스 트리거 (호출) - 엘리베이터 호출, 현관문 열림
  THERMOSTAT = 5, // 연속 희망온도 파라미터 - 난방
  VENT = 6,       // 이산 다단계 풍량 파라미터 - 환기
  AIRCON = 7      // 온도 + 풍량 복합 파라미터 - 에어컨
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
  static constexpr const char *kNames[] = {"Unknown", "Light",    "Outlet",
                                           "Gas",     "Elevator", "Thermo",
                                           "Vent",    "Aircon"};
  const size_t idx = static_cast<size_t>(cls);
  return (idx < sizeof(kNames) / sizeof(kNames[0])) ? kNames[idx] : "Unknown";
}

inline const char *DeviceClassToCliString(DeviceClass cls) {
  static constexpr const char *kCliNames[] = {"UNKNOWN", "SWITCH", "OUTLET",
                                              "GAS",     "MOMENT", "THERMO",
                                              "VENT",    "AIRCON"};
  const size_t idx = static_cast<size_t>(cls);
  return (idx < sizeof(kCliNames) / sizeof(kCliNames[0])) ? kCliNames[idx]
                                                          : "UNKNOWN";
}

inline const char *DeviceClassToTelemetryString(DeviceClass cls) {
  static constexpr const char *kTeleNames[] = {
      "unknown",   "switch",     "outlet", "gas",
      "momentary", "thermostat", "vent",   "aircon"};
  const size_t idx = static_cast<size_t>(cls);
  return (idx < sizeof(kTeleNames) / sizeof(kTeleNames[0])) ? kTeleNames[idx]
                                                            : "unknown";
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
  uint8_t expected_len{0}; // 쿼리 응답 패킷 길이 (예: 콘센트 18B, 난방 18B 등)
  uint8_t power_offset{0xFF};        // 평상시 전원/가동 상태 바이트 오프셋 [AS]
  uint8_t target_temp_offset{0xFF};  // 희망 설정온도 오프셋 [TT]
  uint8_t current_temp_offset{0xFF}; // 현재 환경온도 오프셋 [AT]
  uint8_t fan_speed_offset{0xFF};    // 환기 풍량 오프셋 [FS]
  uint8_t power_w_offset{0xFF};      // 콘센트 실시간 소비전력(W) 오프셋
  uint8_t valve_state_offset{0xFF};  // 가스 차단 상태 오프셋 [VS]
  bool is_bitmap_power{false};       // 다채널 비트맵 전원 여부
};

struct GroupControlTemplate {
  uint8_t dev_id{0x00}; // 기기 그룹 코드 (예: 0x19 조명, 0x18 난방 등)
  char group_name[16]{"Unknown"}; // 그룹 명칭 ("Light", "Thermo", "Vent" 등)
  uint8_t frame_len{0};           // 제어 패킷 프레임 길이 (11, 13 등)
  uint8_t raw_template[32]{0};    // 기본 제어 프레임 골격

  // 주소 마스킹 오프셋
  uint8_t sub1_offset{0xFF};       // 방 번호(Sub1) 주입 오프셋
  uint8_t sub2_offset{0xFF};       // 기기 번호(Sub2) 주입 오프셋
  uint8_t ctl_sub1_override{0xFF}; // 전열교환기 등 특수 sub1 고정값 (0x40 등)

  // 기능별 액션 슬롯
  ActionSlot power_slot; // 전원 제어 슬롯
  ActionSlot temp_slot;  // 온도 제어 슬롯 (난방)
  ActionSlot speed_slot; // 풍량 제어 슬롯 (환기)
  ActionSlot mode_slot;  // 운전 모드 슬롯
  ActionSlot close_slot; // 닫기 제어 슬롯 (가스)

  SlotCoverage coverage;         // 슬롯 매핑 정보
  uint8_t away_mode_token{0xFF}; // 외출 시 전원/모드 바이트 코드 (현대: 0x07)

  // ACK 상태 슬롯 (제어 트랜잭션 응답)
  AckStateSlots ack_slots;

  // 쿼리 상태 슬롯 (수기/명세 주입)
  QueryStateSlots query_slots;

  // ── 통합 슬롯 접근자 (Pure Declarations) ──
  uint8_t getPowerOffset(uint8_t pkt_len = 0) const noexcept;
  uint8_t getTargetTempOffset(uint8_t pkt_len = 0) const noexcept;
  uint8_t getCurrentTempOffset(uint8_t pkt_len = 0) const noexcept;
  uint8_t getFanSpeedOffset(uint8_t pkt_len = 0) const noexcept;
  uint8_t decodeFanSpeed(uint8_t raw_token) const noexcept;
  uint8_t decodeVentMode(uint8_t raw_byte) const noexcept;
  uint8_t getValveStateOffset(uint8_t pkt_len = 0) const noexcept;
  uint8_t getWattageOffset(uint8_t pkt_len = 0) const noexcept;
  bool isUnidirectional() const noexcept;
};

static_assert(sizeof(GroupControlTemplate) == 180,
              "NVS ABI break: GroupControlTemplate size changed");

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
  bool findGroup(uint8_t dev_id, GroupControlTemplate &out,
                 TickType_t timeout = kQueryLockTimeout) const;
  size_t getGroupsSnapshot(GroupControlTemplate *out_buf, size_t max_count,
                           TickType_t timeout = kQueryLockTimeout) const;
  size_t getGroupCount() const;
  bool getGroupByIndex(size_t index, GroupControlTemplate &out) const;
  bool resetGroup(uint8_t dev_id, bool full_reset = false);
  bool setGroupName(uint8_t dev_id, const char *name);
  bool setGroupClass(uint8_t dev_id, DeviceClass cls,
                     const char *name = nullptr);

  template <typename Func>
  bool modifyOrCreateGroup(uint8_t dev_id, Func &&mutator,
                           const char *initial_name = nullptr,
                           TickType_t timeout = kManageLockTimeout) {
    if (dev_id == 0)
      return false;
    MutexLocker lock(_mutex, timeout);
    if (!lock.isLocked())
      return false;
    GroupControlTemplate *grp = registerOrTouchUnlocked(dev_id, initial_name);
    if (!grp)
      return false;
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
  GroupControlTemplate *registerOrTouchUnlocked(uint8_t dev_id,
                                                const char *name = nullptr);
};

extern ControlTemplateRegistry g_control_registry;

namespace ControlTemplateUtils {
inline void getControlNamespace(char *out_ns, size_t max_len,
                                uint8_t prof_idx) {
  snprintf(out_ns, max_len, "ctl_p%u", prof_idx);
}

inline uint8_t getCurrentProfileIndex() {
  CriticalSectionLocker lock(&g_config_mux);
  return g_config.wallpad_profile;
}
} // namespace ControlTemplateUtils
