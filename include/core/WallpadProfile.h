#pragma once

#include <cstdint>
#include <cstddef>
#include "Common.h"
#include "ControlTemplate.h"
#include "WallpadParser.h"

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
