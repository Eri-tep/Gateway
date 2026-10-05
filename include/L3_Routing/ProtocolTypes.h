#pragma once

// ============================================================================
// ProtocolTypes: Level 2 Leaf Common Types, Enums & Contract Definitions
// ============================================================================

#include <cstddef>
#include <cstdint>

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
// DEVICE KEY IDENTIFICATION
// ============================================================================

struct DeviceKey {
  uint8_t dev_id{0};
  uint8_t sub1{0};
  uint8_t sub2{0};

  constexpr bool operator==(const DeviceKey &o) const noexcept {
    return dev_id == o.dev_id && sub1 == o.sub1 && sub2 == o.sub2;
  }
  constexpr bool operator!=(const DeviceKey &o) const noexcept {
    return !(*this == o);
  }
};
