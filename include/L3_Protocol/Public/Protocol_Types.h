#pragma once

// ============================================================================
// Protocol_Types: Level 3 Pure Shared Value Types & Enums (Leaf Header)
// ============================================================================
// Contains only trivially-copyable POD structs and enums shared between
// L3 Public interfaces (Protocol_Device, Protocol_Facade), L3 Private engines,
// and L4 Services, breaking bidirectional L3 Public <-> Private header cycles.
// Depends solely on L0 Foundation headers.
// ============================================================================

#include <cstdint>

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

enum class FramingStatus : uint8_t {
  WAITING = 0,
  LEARNING = 1,
  LOCKED = 2,
  NOISY = 3
};

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

struct ExtractedFrameResult {
  int length{-1};         // > 0: Valid frame length, 0: Partial/incomplete frame, -1: Invalid STX or bounds
  bool checksum_ok{false}; // True if STX/ETX/Length bounds AND Checksum were verified valid
};
