// ============================================================================
// Fcu_Protocol: Level 3 Private Modbus-RTU FCU Protocol Engine & Codec
// 100% Encapsulated Private Core (AGENTS.md Rule 17)
// ============================================================================

#include "L3_Protocol/Private/Fcu_Protocol.h"
#include "L0_Foundation/System_Platform.h"
#include "L2_Transport/Bridge_CH.h"
#include "L3_Protocol/Public/Device_Registry.h"
#include "L3_Protocol/Public/Packet_Router.h"
#include "L3_Protocol/Public/Protocol_Diagnostics.h"

#include <algorithm>
#include <cstring>
#include <esp_log.h>

static const char *TAG = "FCU_PROTO";

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

// ── FCU Runtime State & FSM Implementation ────────────────────────────────────
static Fcu::SlotRuntime s_fcu_slots[Config::TCP::MAX_EW11_SLOTS]{};

static void syncDeviceRepository(uint8_t slot_idx, const Fcu::Snapshot &snap,
                                const uint8_t *raw_pkt = nullptr, size_t raw_len = 0) {
  Device_SyncFcuState(slot_idx, snap.target_temp, snap.room_temp, true, raw_pkt, raw_len);
}

static void applyOptimisticState(uint8_t slot_idx, uint16_t mode, uint16_t fan,
                                 uint16_t swing, uint8_t temp) {
  if (slot_idx < 1 || slot_idx >= Config::TCP::MAX_EW11_SLOTS)
    return;
  auto &rt = s_fcu_slots[slot_idx];

  rt.snap.power = (fan != 0);
  if (mode >= 1 && mode <= 3)
    rt.snap.mode = static_cast<Fcu::Mode>(mode);
  if (fan <= 4)
    rt.snap.fan_speed = static_cast<Fcu::FanSpeed>(fan);
  if (swing == 0 || swing == 2)
    rt.snap.swing = static_cast<Fcu::Swing>(swing);
  if (temp >= Config::FCU::TEMP_MIN && temp <= Config::FCU::TEMP_MAX) {
    rt.snap.target_temp = temp;
  }
  if (mode == 1 || mode == 2) {
    rt.last_active_mode = rt.snap.mode;
    rt.has_active_record = true;
  }
  if (fan != 0) {
    rt.last_active_fan = rt.snap.fan_speed;
  }
  rt.last_active_swing = rt.snap.swing;

  syncDeviceRepository(slot_idx, rt.snap);
}

static bool Fcu_SendRaw(uint8_t slot_idx, const uint8_t *pkt, size_t len) {
  if (slot_idx < 1 || slot_idx >= Config::TCP::MAX_EW11_SLOTS || !pkt ||
      len == 0 || len > 16)
    return false;
  auto &rt = s_fcu_slots[slot_idx];

  if (rt.waiting_response) {
    memcpy(rt.pending_cmd_buf, pkt, len);
    rt.pending_cmd_len = static_cast<uint8_t>(len);
    return true;
  }

  bool ok = Bridge_SendRaw(slot_idx, pkt, len);
  if (ok) {
    System_RecordCh5Tx();
    StaticPacket trace_pkt{5, static_cast<uint8_t>(len)};
    std::copy(pkt, pkt + len, trace_pkt.data.begin());
    System_TracePacket(5, true, TraceType::CTL, trace_pkt);
    rt.waiting_response = true;
    rt.query_sent_ms = millis();
    rt.next_tx_ms = millis() + Config::FCU::INTER_PACKET_DELAY_MS;
  }
  return ok;
}

template <typename MutateFn>
static bool executeRegisterWrite(uint8_t slot_idx, uint16_t reg, uint16_t val,
                                 MutateFn &&mutate) {
  if (slot_idx < 1 || slot_idx >= Config::TCP::MAX_EW11_SLOTS)
    return false;

  auto frame = ModbusRtu::buildWriteSingle(reg, val);
  bool ok = Fcu_SendRaw(slot_idx, frame.data(), frame.size());
  if (ok) {
    auto &rt = s_fcu_slots[slot_idx];
    mutate(rt);
    syncDeviceRepository(slot_idx, rt.snap);
  }
  return ok;
}

void Fcu_Init() noexcept {
  for (size_t i = 0; i < Config::TCP::MAX_EW11_SLOTS; ++i) {
    s_fcu_slots[i] = Fcu::SlotRuntime{};
  }
}

void Fcu_HandleRx(uint8_t slot_idx, const uint8_t *data, size_t len) noexcept {
  if (slot_idx < 1 || slot_idx >= Config::TCP::MAX_EW11_SLOTS || !data || len == 0)
    return;
  auto &rt = s_fcu_slots[slot_idx];

  // 1) 제어 명령(0x06, 0x10) ACK 수신 확인 (8바이트 에코 응답)
  if (len >= 8 && data[0] == 0x01 && (data[1] == 0x06 || data[1] == 0x10)) {
    rt.waiting_response = false;
    rt.timeout_count = 0;
    rt.is_online = true;
    rt.next_tx_ms = millis() + Config::FCU::INTER_PACKET_DELAY_MS;
    return;
  }

  // 2) 19바이트 0x03 상태 쿼리 응답 처리
  if (len >= 19 && data[0] == 0x01 && data[1] == 0x03 && data[2] == 0x0E) {
    auto parse_res = ModbusRtu::parseStatusResponse(std::span<const uint8_t>(data, len));
    if (!parse_res)
      return; // 파싱/CRC 실패 시 드롭

    const auto &new_snap = *parse_res;
    rt.waiting_response = false;
    rt.timeout_count = 0;
    rt.is_online = true;

    if (new_snap.mode == Fcu::Mode::Cool || new_snap.mode == Fcu::Mode::Heat) {
      rt.last_active_mode = new_snap.mode;
      rt.has_active_record = true;
    }
    if (new_snap.fan_speed != Fcu::FanSpeed::Off) {
      rt.last_active_fan = new_snap.fan_speed;
    }
    rt.last_active_swing = new_snap.swing;

    if (new_snap.power && rt.pending_restore_swing == 2) {
      if (new_snap.swing != Fcu::Swing::On) {
        ESP_LOGI(TAG,
                 "[FCU#%d] Flap motor calibrated. Queuing swing restore (2)...",
                 slot_idx);
        auto sw_frame = ModbusRtu::buildWriteSingle(0x0003, 0x0002);
        memcpy(rt.pending_cmd_buf, sw_frame.data(), sw_frame.size());
        rt.pending_cmd_len = static_cast<uint8_t>(sw_frame.size());
      }
      rt.pending_restore_swing = 0;
    }

    rt.snap = new_snap;
    syncDeviceRepository(slot_idx, rt.snap, data, len);
  }
}

void Fcu_PollTick(uint32_t now_ms) noexcept {
  for (uint8_t slot_idx = 1; slot_idx < Config::TCP::MAX_EW11_SLOTS; ++slot_idx) {
    HubClientSlotSnapshot slot{};
    if (!Bridge_GetSlotSnapshot(slot_idx, slot))
      continue;
    if (!slot.enabled || !slot.is_connected)
      continue;

    auto &rt = s_fcu_slots[slot_idx];

    // Step 1: Inter-Packet Delay Gate
    if (now_ms < rt.next_tx_ms) {
      continue;
    }

    // Step 2: Two-Phase Sequencer (지연 온도 전송 파이프라인)
    if (rt.has_pending_temp) {
      if (!rt.waiting_response) {
        rt.has_pending_temp = false;
        auto temp_frame = ModbusRtu::buildWriteSingle(
            0x0005, static_cast<uint16_t>(rt.pending_temp));
        if (Bridge_SendRaw(slot_idx, temp_frame.data(), temp_frame.size())) {
          System_RecordCh5Tx();
          StaticPacket trace_pkt{5, static_cast<uint8_t>(temp_frame.size())};
          std::copy(temp_frame.begin(), temp_frame.end(), trace_pkt.data.begin());
          System_TracePacket(5, true, TraceType::CTL, trace_pkt);
          rt.waiting_response = true;
          rt.query_sent_ms = now_ms;
          rt.next_tx_ms = now_ms + Config::FCU::INTER_PACKET_DELAY_MS;
        }
      }
      continue;
    }

    // Step 3: Stop-and-Wait Queue (대기 중인 제어 명령 방출)
    if (rt.pending_cmd_len > 0) {
      if (!rt.waiting_response) {
        uint8_t len = rt.pending_cmd_len;
        rt.pending_cmd_len = 0;
        if (Bridge_SendRaw(slot_idx, rt.pending_cmd_buf, len)) {
          System_RecordCh5Tx();
          StaticPacket trace_pkt{5, len};
          std::copy(rt.pending_cmd_buf, rt.pending_cmd_buf + len, trace_pkt.data.begin());
          System_TracePacket(5, true, TraceType::CTL, trace_pkt);
          rt.waiting_response = true;
          rt.query_sent_ms = now_ms;
          rt.next_tx_ms = now_ms + Config::FCU::INTER_PACKET_DELAY_MS;
        }
      }
      continue;
    }

    // Step 4: RX Timeout Monitor
    if (rt.waiting_response) {
      if (now_ms - rt.query_sent_ms >= Config::FCU::RX_TIMEOUT_MS) {
        rt.waiting_response = false;
        rt.timeout_count++;
        if (rt.timeout_count >= Config::FCU::MAX_TIMEOUT_COUNT) {
          rt.is_online = false;
        }
      }
      continue;
    }

    // Step 5: Periodic Polling Scheduler
    if (rt.last_poll_ms == 0 ||
        (now_ms - rt.last_poll_ms >= Config::FCU::POLL_INTERVAL_MS)) {
      rt.last_poll_ms = now_ms;
      rt.query_sent_ms = now_ms;
      rt.waiting_response = true;

      ProtocolDiag_PollingRegisterOrTouch(5, Config::FCU::DEV_ID, slot_idx, 0,
                                          ModbusRtu::kQueryPkt.data(),
                                          ModbusRtu::kQueryPkt.size());
      Router_RecordRoute(5, slot_idx, Config::FCU::DEV_ID, slot_idx, 0);

      if (Bridge_SendRaw(slot_idx, ModbusRtu::kQueryPkt.data(),
                         ModbusRtu::kQueryPkt.size())) {
        System_RecordCh5Tx();
        StaticPacket trace_pkt{5, static_cast<uint8_t>(ModbusRtu::kQueryPkt.size())};
        std::copy(ModbusRtu::kQueryPkt.begin(), ModbusRtu::kQueryPkt.end(), trace_pkt.data.begin());
        System_TracePacket(5, true, TraceType::QRY, trace_pkt);
      }
    }
  }
}

bool Fcu_RestorePower(uint8_t slot_idx, uint16_t mode, uint16_t fan, uint16_t swing,
                      uint8_t temp) noexcept {
  if (slot_idx < 1 || slot_idx >= Config::TCP::MAX_EW11_SLOTS)
    return false;
  auto &rt = s_fcu_slots[slot_idx];

  if (mode != 1 && mode != 2 && mode != 3)
    mode = 1;
  if (fan == 0 || fan > 4)
    fan = 4;
  if (swing != 0 && swing != 2)
    swing = 0;
  if (temp < Config::FCU::TEMP_MIN || temp > Config::FCU::TEMP_MAX)
    temp = 24;

  auto frame = ModbusRtu::buildWriteMultiplePowerOn(mode, fan, swing);
  if (!Fcu_SendRaw(slot_idx, frame.data(), frame.size()))
    return false;

  uint8_t prev_temp = rt.snap.target_temp;
  applyOptimisticState(slot_idx, mode, fan, swing, temp);

  if (prev_temp != temp) {
    rt.has_pending_temp = true;
    rt.pending_temp = temp;
    rt.next_tx_ms = millis() + Config::FCU::INTER_PACKET_DELAY_MS;
  }

  rt.pending_restore_swing = (swing == 2) ? 2 : 0;
  return true;
}

bool Fcu_SetPower(uint8_t slot_idx, bool on) noexcept {
  if (slot_idx < 1 || slot_idx >= Config::TCP::MAX_EW11_SLOTS)
    return false;
  auto &rt = s_fcu_slots[slot_idx];

  if (on) {
    Fcu::Mode target_mode = rt.has_active_record ? rt.last_active_mode : Fcu::Mode::Cool;
    Fcu::FanSpeed target_fan =
        rt.has_active_record ? rt.last_active_fan : Fcu::FanSpeed::Low;
    Fcu::Swing target_swing =
        rt.has_active_record ? rt.last_active_swing : Fcu::Swing::Off;

    if (target_fan == Fcu::FanSpeed::Off)
      target_fan = Fcu::FanSpeed::Low;
    rt.pending_restore_swing = (target_swing == Fcu::Swing::On) ? 2 : 0;

    auto frame = ModbusRtu::buildWriteMultiplePowerOn(
        static_cast<uint16_t>(target_mode), static_cast<uint16_t>(target_fan),
        static_cast<uint16_t>(target_swing));
    bool ok = Fcu_SendRaw(slot_idx, frame.data(), frame.size());
    if (ok) {
      applyOptimisticState(slot_idx, static_cast<uint16_t>(target_mode),
                           static_cast<uint16_t>(target_fan),
                           static_cast<uint16_t>(target_swing),
                           rt.snap.target_temp);
    }
    return ok;
  } else {
    rt.pending_restore_swing = 0;
    bool ok = Fcu_SendRaw(slot_idx, ModbusRtu::kPowerOffPkt.data(),
                          ModbusRtu::kPowerOffPkt.size());
    if (ok) {
      applyOptimisticState(slot_idx, static_cast<uint16_t>(rt.snap.mode), 0,
                           static_cast<uint16_t>(rt.snap.swing),
                           rt.snap.target_temp);
    }
    return ok;
  }
}

bool Fcu_SetMode(uint8_t slot_idx, Fcu::Mode m) noexcept {
  return executeRegisterWrite(slot_idx, 0x0001, static_cast<uint16_t>(m),
                              [m](Fcu::SlotRuntime &rt) {
                                rt.snap.mode = m;
                                if (m == Fcu::Mode::Cool || m == Fcu::Mode::Heat) {
                                  rt.last_active_mode = m;
                                  rt.has_active_record = true;
                                }
                              });
}

bool Fcu_SetFanSpeed(uint8_t slot_idx, Fcu::FanSpeed f) noexcept {
  return executeRegisterWrite(slot_idx, 0x0002, static_cast<uint16_t>(f),
                              [f](Fcu::SlotRuntime &rt) {
                                rt.snap.fan_speed = f;
                                rt.snap.power = (f != Fcu::FanSpeed::Off);
                                if (f != Fcu::FanSpeed::Off) {
                                  rt.last_active_fan = f;
                                }
                              });
}

bool Fcu_SetSwing(uint8_t slot_idx, Fcu::Swing s) noexcept {
  return executeRegisterWrite(slot_idx, 0x0003, static_cast<uint16_t>(s),
                              [s](Fcu::SlotRuntime &rt) {
                                rt.snap.swing = s;
                                rt.last_active_swing = s;
                              });
}

bool Fcu_SetTargetTemp(uint8_t slot_idx, uint8_t temp_c) noexcept {
  if (temp_c < Config::FCU::TEMP_MIN)
    temp_c = Config::FCU::TEMP_MIN;
  if (temp_c > Config::FCU::TEMP_MAX)
    temp_c = Config::FCU::TEMP_MAX;

  return executeRegisterWrite(
      slot_idx, 0x0005, static_cast<uint16_t>(temp_c),
      [temp_c](Fcu::SlotRuntime &rt) { rt.snap.target_temp = temp_c; });
}

bool Fcu_GetSlotRuntime(uint8_t slot_idx, Fcu::SlotRuntime &out_rt) noexcept {
  if (slot_idx < 1 || slot_idx >= Config::TCP::MAX_EW11_SLOTS)
    return false;
  out_rt = s_fcu_slots[slot_idx];
  return true;
}
