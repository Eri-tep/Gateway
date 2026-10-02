#pragma once

// ============================================================================
// ModbusProtocol: Level 3 Modbus-RTU Codec, Framing & CRC Engine
// ============================================================================

#include "Base/BufferUtils.h"
#include <array>
#include <cstddef>
#include <cstdint>

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

} // namespace Fcu

namespace ModbusRtu {

constexpr uint16_t kModbusCrcInit = 0xFFFF;
constexpr uint16_t kModbusPolynomial = 0xA001;

// Modbus-RTU CRC16 (Polynomial: 0xA001, Init: 0xFFFF, Zero-Heap)
uint16_t calcCrc16(const uint8_t *buf, size_t len) noexcept;

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

// 19B 상태 쿼리 응답 파싱 및 CRC-16 Little-Endian 검증
bool parseStatusResponse(const uint8_t *data, size_t len,
                         Fcu::Snapshot &out) noexcept;

} // namespace ModbusRtu
