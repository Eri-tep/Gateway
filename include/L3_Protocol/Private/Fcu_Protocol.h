#pragma once

// ============================================================================
// Fcu_Protocol: Level 3 Private Modbus-RTU FCU Protocol Engine & Codec
// 100% Encapsulated Private Core (AGENTS.md Rule 17)
// ============================================================================

#include "L0_Foundation/System_Buffer.h"
#include "L0_Foundation/System_Config.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace Fcu {

enum class Mode : uint16_t { Cool = 1, Heat = 2, FanOnly = 3 };

enum class FanSpeed : uint16_t {
  Off = 0, // 정지 (30~40초 지연 정지 트리거)
  Low = 1,
  Mid = 2,
  High = 3,
  Auto = 4
};

enum class Swing : uint16_t {
  Off = 0,
  On = 2 // 값 1은 예약/미사용, 스윙ON은 반드시 2
};

struct Snapshot {
  Mode mode{Mode::Cool};
  FanSpeed fan_speed{FanSpeed::Off};
  Swing swing{Swing::Off};
  uint8_t error_code{0};
  uint8_t target_temp{24}; // 희망 설정 온도 (℃)
  uint8_t room_temp{0};    // 실내 측정 온도 (℃)
  bool power{false};       // fan_speed != FanSpeed::Off
};

struct SlotRuntime {
  Snapshot snap{};
  uint32_t last_poll_ms{0};
  uint32_t query_sent_ms{0};
  uint8_t timeout_count{0};
  bool waiting_response{false};
  bool is_online{false};
  Mode last_active_mode{Mode::Cool};       // 기록 없을 시 안전 기본 냉방
  FanSpeed last_active_fan{FanSpeed::Low}; // 기록 없을 시 기본 약풍
  Swing last_active_swing{Swing::Off};     // 기록 없을 시 기본 고정
  bool has_active_record{false};           // 냉방/난방 운전 이력 여부
  uint32_t next_tx_ms{0};                  // 120ms 논블로킹 가드타임 만료 시각
  uint8_t pending_temp{0};                 // 120ms 후 전송할 대기 목표온도
  bool has_pending_temp{false};            // 온도 패킷 전송 대기 여부
  uint8_t pending_cmd_buf[16]{};           // RS-485 Stop-and-Wait 대기 명령 버퍼
  uint8_t pending_cmd_len{0};              // 대기 중인 명령 패킷 길이
  uint8_t pending_restore_swing{0};        // 전원 켜기 복원 시 스윙값
};

} // namespace Fcu

namespace ModbusRtu {

enum class ModbusParseError : uint8_t {
  InvalidLength,
  HeaderMismatch,
  CrcMismatch,
};

constexpr uint16_t kModbusCrcInit = 0xFFFF;
constexpr uint16_t kModbusPolynomial = 0xA001;

// Modbus-RTU CRC16 (Polynomial: 0xA001, Init: 0xFFFF, Zero-Heap)
[[nodiscard]] uint16_t calcCrc16(std::span<const uint8_t> data) noexcept;
inline uint16_t calcCrc16(const uint8_t *buf, size_t len) noexcept {
  return buf ? calcCrc16(std::span<const uint8_t>(buf, len)) : 0;
}

// §4.1 상태 조회 쿼리 (8B 고정 프레임, Read Holding Registers 0x0000..0x0006)
constexpr std::array<uint8_t, 8> kQueryPkt = {0x01, 0x03, 0x00, 0x00,
                                              0x00, 0x07, 0x04, 0x08};

// §4.2 전원 OFF (8B 고정 프레임, Write Single Register 0x0002 = 0)
constexpr std::array<uint8_t, 8> kPowerOffPkt = {0x01, 0x06, 0x00, 0x02,
                                                 0x00, 0x00, 0x28, 0x0A};

// §4.2 전원 ON: Reg 0x0001(모드), 0x0002(풍량), 0x0003(스윙) 일괄 (15B FC 0x10, Reg 0 절대 보존)
std::array<uint8_t, 15> buildWriteMultiplePowerOn(uint16_t mode, uint16_t fan,
                                                 uint16_t swing) noexcept;

// §4.3~§4.6 단일 레지스터 쓰기 (8B FC 0x06 표준 프레임)
std::array<uint8_t, 8> buildWriteSingle(uint16_t reg, uint16_t val) noexcept;

// 19B 상태 쿼리 응답 파싱 및 CRC-16 Little-Endian 검증 (std::expected)
[[nodiscard]] std::expected<Fcu::Snapshot, ModbusParseError>
parseStatusResponse(std::span<const uint8_t> data) noexcept;

bool parseStatusResponse(const uint8_t *data, size_t len,
                         Fcu::Snapshot &out) noexcept;

} // namespace ModbusRtu

// ── L3 Protocol Core FCU Engine APIs ──────────────────────────────────────────
void Fcu_Init() noexcept;
void Fcu_HandleRx(uint8_t slot_idx, const uint8_t *data, size_t len) noexcept;
void Fcu_PollTick(uint32_t now_ms) noexcept;

bool Fcu_SetPower(uint8_t slot_idx, bool on) noexcept;
bool Fcu_RestorePower(uint8_t slot_idx, uint16_t mode, uint16_t fan, uint16_t swing,
                      uint8_t temp) noexcept;
bool Fcu_SetMode(uint8_t slot_idx, Fcu::Mode m) noexcept;
bool Fcu_SetFanSpeed(uint8_t slot_idx, Fcu::FanSpeed f) noexcept;
bool Fcu_SetSwing(uint8_t slot_idx, Fcu::Swing s) noexcept;
bool Fcu_SetTargetTemp(uint8_t slot_idx, uint8_t temp_c) noexcept;
bool Fcu_GetSlotRuntime(uint8_t slot_idx, Fcu::SlotRuntime &out_rt) noexcept;
