#include "L3_Routing/Modbus_Protocol.h"

namespace ModbusRtu {

uint16_t calcCrc16(std::span<const uint8_t> data) noexcept {
  uint16_t crc = kModbusCrcInit;
  for (uint8_t byte : data) {
    crc ^= static_cast<uint16_t>(byte);
    for (int i = 8; i != 0; i--) {
      if ((crc & 0x0001) != 0) {
        crc >>= 1;
        crc ^= kModbusPolynomial;
      } else {
        crc >>= 1;
      }
    }
  }
  return crc;
}

std::array<uint8_t, 15>
buildWriteMultiplePowerOn(uint16_t mode, uint16_t fan, uint16_t swing) noexcept {
  std::array<uint8_t, 15> frame = {
      0x01,
      0x10,
      0x00,
      0x01, // 시작 번지 0x0001 (Reg 0 절대 보존)
      0x00,
      0x03, // 레지스터 개수 3개
      0x06, // 데이터 바이트 수 6바이트
      static_cast<uint8_t>((mode >> 8) & 0xFF),
      static_cast<uint8_t>(mode & 0xFF),
      static_cast<uint8_t>((fan >> 8) & 0xFF),
      static_cast<uint8_t>(fan & 0xFF),
      static_cast<uint8_t>((swing >> 8) & 0xFF),
      static_cast<uint8_t>(swing & 0xFF),
      0x00,
      0x00 // CRC 필드
  };
  uint16_t crc = calcCrc16(std::span<const uint8_t>(frame.data(), 13));
  frame[13] = static_cast<uint8_t>(crc & 0xFF);
  frame[14] = static_cast<uint8_t>((crc >> 8) & 0xFF);
  return frame;
}

std::array<uint8_t, 8> buildWriteSingle(uint16_t reg, uint16_t val) noexcept {
  std::array<uint8_t, 8> frame = {0x01,
                                  0x06,
                                  static_cast<uint8_t>((reg >> 8) & 0xFF),
                                  static_cast<uint8_t>(reg & 0xFF),
                                  static_cast<uint8_t>((val >> 8) & 0xFF),
                                  static_cast<uint8_t>(val & 0xFF),
                                  0x00,
                                  0x00};
  uint16_t crc = calcCrc16(std::span<const uint8_t>(frame.data(), 6));
  frame[6] = static_cast<uint8_t>(crc & 0xFF);
  frame[7] = static_cast<uint8_t>((crc >> 8) & 0xFF);
  return frame;
}

std::expected<Fcu::Snapshot, ModbusParseError>
parseStatusResponse(std::span<const uint8_t> data) noexcept {
  if (data.size() < 19) {
    return std::unexpected(ModbusParseError::InvalidLength);
  }
  if (data[0] != 0x01 || data[1] != 0x03 || data[2] != 0x0E) {
    return std::unexpected(ModbusParseError::HeaderMismatch);
  }

  uint16_t calc_crc = calcCrc16(data.subspan(0, 17));
  uint16_t pkt_crc =
      static_cast<uint16_t>(data[17]) | (static_cast<uint16_t>(data[18]) << 8);
  if (calc_crc != pkt_crc) {
    return std::unexpected(ModbusParseError::CrcMismatch);
  }

  auto unpackBe16 = [](const uint8_t *p) -> uint16_t {
    return (static_cast<uint16_t>(p[0]) << 8) | p[1];
  };

  uint16_t reg1 = unpackBe16(&data[5]); // 운전 모드 (1: 냉방, 2: 난방, 3: 송풍)
  uint16_t reg2 = unpackBe16(
      &data[7]); // 풍량 / 전원 (0: 정지, 1: 미풍, 2: 약풍, 3: 강풍, 4: 자동)
  uint16_t reg3 = unpackBe16(&data[9]);  // 스윙 (0: 고정, 2: 회전)
  uint16_t reg4 = unpackBe16(&data[11]); // 에러 코드
  uint16_t reg5 = unpackBe16(&data[13]); // 설정 희망 온도
  uint16_t reg6 = unpackBe16(&data[15]); // 실내 측정 온도

  Fcu::Snapshot out;
  out.mode =
      (reg1 >= 1 && reg1 <= 3) ? static_cast<Fcu::Mode>(reg1) : Fcu::Mode::Cool;
  out.fan_speed =
      (reg2 <= 4) ? static_cast<Fcu::FanSpeed>(reg2) : Fcu::FanSpeed::Off;
  out.swing = (reg3 == 2) ? Fcu::Swing::On : Fcu::Swing::Off;
  out.error_code = static_cast<uint8_t>(reg4 & 0xFF);
  out.target_temp = static_cast<uint8_t>(reg5 & 0xFF);
  out.room_temp = static_cast<uint8_t>(reg6 & 0xFF);
  out.power = (out.fan_speed != Fcu::FanSpeed::Off);
  return out;
}

bool parseStatusResponse(const uint8_t *data, size_t len,
                         Fcu::Snapshot &out) noexcept {
  if (!data)
    return false;
  auto res = parseStatusResponse(std::span<const uint8_t>(data, len));
  if (!res)
    return false;
  out = *res;
  return true;
}

} // namespace ModbusRtu
