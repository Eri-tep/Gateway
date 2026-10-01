// ============================================================================
// RemoteService: Level 4 Network Remote Services Implementation
// ============================================================================

#include "Service/RemoteService.h"
#include "Service/ConsoleCli.h"


#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <esp_core_dump.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <fcntl.h>
#include <lwip/ip.h>
#include <lwip/sockets.h>
#include <lwip/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

// ── EW11 & Modbus FCU Engine (formerly Bridge.cpp) ──
static const char *TAG = "EW11";

// ============================================================================
// Domain 1: Modbus-RTU Protocol Codec (C++17 Type-Safe Frames, Zero-Heap)
// ============================================================================
namespace ModbusRtu {

constexpr uint16_t kModbusCrcInit = 0xFFFF;
constexpr uint16_t kModbusPolynomial = 0xA001;

// Modbus-RTU CRC16 (Polynomial: 0xA001, Init: 0xFFFF, Zero-Heap)
inline uint16_t calcCrc16(const uint8_t *buf, size_t len) {
  uint16_t crc = kModbusCrcInit;
  for (size_t pos = 0; pos < len; pos++) {
    crc ^= static_cast<uint16_t>(buf[pos]);
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

// §4.1 상태 조회 쿼리 (8B 고정 프레임, Read Holding Registers 0x0000..0x0006)
constexpr std::array<uint8_t, 8> kQueryPkt = {0x01, 0x03, 0x00, 0x00,
                                              0x00, 0x07, 0x04, 0x08};

// §4.2 전원 OFF (8B 고정 프레임, Write Single Register 0x0002 = 0)
constexpr std::array<uint8_t, 8> kPowerOffPkt = {0x01, 0x06, 0x00, 0x02,
                                                 0x00, 0x00, 0x28, 0x0A};

// §4.2 전원 ON: Reg 0x0001(모드), 0x0002(풍량), 0x0003(스윙) 일괄 (15B FC 0x10,
// Reg 0 절대 보존)
inline std::array<uint8_t, 15>
buildWriteMultiplePowerOn(uint16_t mode, uint16_t fan, uint16_t swing) {
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
  uint16_t crc = calcCrc16(frame.data(), 13);
  frame[13] = static_cast<uint8_t>(crc & 0xFF);
  frame[14] = static_cast<uint8_t>((crc >> 8) & 0xFF);
  return frame;
}

// §4.3~§4.6 단일 레지스터 쓰기 (8B FC 0x06 표준 프레임)
inline std::array<uint8_t, 8> buildWriteSingle(uint16_t reg, uint16_t val) {
  std::array<uint8_t, 8> frame = {0x01,
                                  0x06,
                                  static_cast<uint8_t>((reg >> 8) & 0xFF),
                                  static_cast<uint8_t>(reg & 0xFF),
                                  static_cast<uint8_t>((val >> 8) & 0xFF),
                                  static_cast<uint8_t>(val & 0xFF),
                                  0x00,
                                  0x00};
  uint16_t crc = calcCrc16(frame.data(), 6);
  frame[6] = static_cast<uint8_t>(crc & 0xFF);
  frame[7] = static_cast<uint8_t>((crc >> 8) & 0xFF);
  return frame;
}

// 19B 상태 쿼리 응답 파싱 및 CRC-16 Little-Endian 검증
inline bool parseStatusResponse(const uint8_t *data, size_t len,
                                Fcu::Snapshot &out) {
  if (len < 19 || data[0] != 0x01 || data[1] != 0x03 || data[2] != 0x0E) {
    return false;
  }

  // CRC-16 검증: data[0..16] (17바이트) -> CRC at data[17..18]
  uint16_t calc_crc = calcCrc16(data, 17);
  uint16_t pkt_crc =
      static_cast<uint16_t>(data[17]) | (static_cast<uint16_t>(data[18]) << 8);
  if (calc_crc != pkt_crc) {
    return false;
  }

  // Big-Endian 16비트 레지스터 언패킹 헬퍼
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

  out.mode =
      (reg1 >= 1 && reg1 <= 3) ? static_cast<Fcu::Mode>(reg1) : Fcu::Mode::Cool;
  out.fan_speed =
      (reg2 <= 4) ? static_cast<Fcu::FanSpeed>(reg2) : Fcu::FanSpeed::Off;
  out.swing = (reg3 == 2) ? Fcu::Swing::On : Fcu::Swing::Off;
  out.error_code = static_cast<uint8_t>(reg4 & 0xFF);
  out.target_temp = static_cast<uint8_t>(reg5 & 0xFF);
  out.room_temp = static_cast<uint8_t>(reg6 & 0xFF);
  out.power = (out.fan_speed != Fcu::FanSpeed::Off);
  return true;
}

} // namespace ModbusRtu

// ============================================================================
// Domain 2: Stream Framing & Buffer Sliding Engine (Zero-Allocation)
// ============================================================================
namespace {

inline void consumeRxBuffer(HubClientSlot *slot, size_t consumed) {
  if (!slot || consumed == 0)
    return;
  if (consumed >= slot->rx_len) {
    slot->rx_len = 0;
  } else {
    size_t remaining = slot->rx_len - consumed;
    memmove(slot->rx_buf, slot->rx_buf + consumed, remaining);
    slot->rx_len = remaining;
  }
}

void demuxElevatorStream(HubClientSlot *slot) {
  auto *parser = WallpadParserFactory::getActiveParser();
  uint8_t stx = parser ? parser->getStx() : PKT_STX;

  size_t p = 0;
  size_t loop_count = 0;
  while (p < slot->rx_len && ++loop_count < 256) {
    if (slot->rx_buf[p] != stx) {
      p++;
      continue;
    }

    int len_res =
        parser ? parser->extractPacketLength(slot->rx_buf, slot->rx_len, p)
               : -1;
    if (len_res == 0)
      break; // 불완전 패킷: 추가 수신 대기
    if (len_res < 0) {
      p++;
      continue;
    }

    uint8_t p_len = static_cast<uint8_t>(len_res);
    if (p_len == 0) {
      p++;
      continue;
    }

    span<const uint8_t> frame(&slot->rx_buf[p], p_len);
    if (!parser->validatePacket(frame)) {
      StaticPacket drp_pkt{5, p_len};
      std::copy(&slot->rx_buf[p], &slot->rx_buf[p + p_len],
                drp_pkt.data.begin());
      g_telnet_tracer.trace(5, false, TraceType::DRP, drp_pkt);
      g_pkt_stats.ch5.dropped_pkts.fetch_add(1, std::memory_order_relaxed);
      p += p_len;
      continue;
    }

    Hub_ProcessPacket(slot, &slot->rx_buf[p], p_len);
    p += p_len;
  }

  consumeRxBuffer(slot, p);
}

void demuxModbusStream(int slot_idx, HubClientSlot *slot) {
  size_t p = 0;
  while (p < slot->rx_len) {
    if (slot->rx_buf[p] != 0x01) {
      p++;
      continue;
    }

    size_t rem = slot->rx_len - p;
    // 19바이트 0x03 상태 응답
    if (rem >= 19 && slot->rx_buf[p + 1] == 0x03 &&
        slot->rx_buf[p + 2] == 0x0E) {
      slot->rx_pkts++;
      g_pkt_stats.ch5.rx_pkts.fetch_add(1, std::memory_order_relaxed);
      Fcu::handleSlotRx(static_cast<uint8_t>(slot_idx), &slot->rx_buf[p], 19);
      p += 19;
      continue;
    }

    // 8바이트 0x06 / 0x10 제어 ACK
    if (rem >= 8 &&
        (slot->rx_buf[p + 1] == 0x06 || slot->rx_buf[p + 1] == 0x10)) {
      slot->rx_pkts++;
      g_pkt_stats.ch5.rx_pkts.fetch_add(1, std::memory_order_relaxed);
      Fcu::handleSlotRx(static_cast<uint8_t>(slot_idx), &slot->rx_buf[p], 8);
      p += 8;
      continue;
    }

    // 아직 충분한 바이트가 도착하지 않은 경우 대기
    if (rem < 19) {
      break;
    }

    p++;
  }

  consumeRxBuffer(slot, p);
}

// ── 비동기 버스트 전송 FSM ──
struct BurstTxFsm {
  portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
  StaticPacket pkt{};
  uint8_t target_slot{0};
  uint8_t remaining_count{0};
  uint32_t silence_ms{20};
  uint32_t last_tx_ms{0};
  esp_timer_handle_t timer{nullptr};
};

static BurstTxFsm s_burst_fsm;

static void onBurstTimer(void *arg) {
  (void)arg;
  StaticPacket tx_pkt{};
  uint8_t slot = 0;
  uint32_t silence_req_ms = 20;

  portENTER_CRITICAL(&s_burst_fsm.mux);
  if (s_burst_fsm.remaining_count == 0) {
    portEXIT_CRITICAL(&s_burst_fsm.mux);
    return;
  }
  tx_pkt = s_burst_fsm.pkt;
  slot = s_burst_fsm.target_slot;
  silence_req_ms = s_burst_fsm.silence_ms;
  uint32_t last_tx = s_burst_fsm.last_tx_ms;
  portEXIT_CRITICAL(&s_burst_fsm.mux);

  // 1. 선로 상태 확인 (RX 및 이전 TX 기준 silence_req_ms 침묵 여부 점검)
  uint32_t last_rx = 0;
  {
    MutexLocker lock(g_ch5_mutex);
    const auto &slot_info = g_hub_slots[slot];
    if (!slot_info.enabled || slot_info.sock < 0 || !slot_info.is_connected) {
      portENTER_CRITICAL(&s_burst_fsm.mux);
      s_burst_fsm.remaining_count = 0;
      portEXIT_CRITICAL(&s_burst_fsm.mux);
      return;
    }
    last_rx = slot_info.last_rx_ms;
  }

  uint32_t now = millis();
  uint32_t rx_elapsed = (now >= last_rx) ? (now - last_rx) : 0;
  uint32_t tx_elapsed = (now >= last_tx) ? (now - last_tx) : 0;

  uint32_t rx_rem_ms =
      (rx_elapsed < silence_req_ms) ? (silence_req_ms - rx_elapsed) : 0;
  uint32_t tx_rem_ms = (last_tx > 0 && tx_elapsed < silence_req_ms)
                           ? (silence_req_ms - tx_elapsed)
                           : 0;
  uint32_t wait_ms = std::max(rx_rem_ms, tx_rem_ms);

  // 2. 선로에 다른 패킷이 유입되었거나 이전 전송 후 지연 시간이 지나지 않은
  // 경우 대기
  if (wait_ms > 0) {
    portENTER_CRITICAL(&s_burst_fsm.mux);
    if (s_burst_fsm.remaining_count > 0) {
      esp_timer_start_once(s_burst_fsm.timer, wait_ms * 1000);
    }
    portEXIT_CRITICAL(&s_burst_fsm.mux);
    return;
  }

  // 3. 선로 유휴 상태 확인 -> 패킷 전송
  bool sent = Hub_SendPacket(slot, tx_pkt);
  g_telnet_tracer.trace(5, true, sent ? TraceType::CTL : TraceType::DRP,
                        tx_pkt);

  portENTER_CRITICAL(&s_burst_fsm.mux);
  s_burst_fsm.last_tx_ms = millis();
  if (s_burst_fsm.remaining_count > 0) {
    s_burst_fsm.remaining_count--;
  }

  if (s_burst_fsm.remaining_count > 0) {
    esp_timer_start_once(s_burst_fsm.timer, silence_req_ms * 1000);
  }
  portEXIT_CRITICAL(&s_burst_fsm.mux);
}

} // anonymous namespace

namespace Ew11Manager {

void init() {
  if (!s_burst_fsm.timer) {
    esp_timer_create_args_t timer_args{};
    timer_args.callback = onBurstTimer;
    timer_args.arg = nullptr;
    timer_args.name = "ew11_burst_timer";
    esp_timer_create(&timer_args, &s_burst_fsm.timer);
  }
}

bool sendBurstPacket(uint8_t slot_idx, const StaticPacket &pkt, uint8_t count,
                     uint32_t silence_ms) {
  if (slot_idx >= Config::TCP::MAX_EW11_SLOTS || count == 0)
    return false;
  if (!s_burst_fsm.timer)
    init();

  esp_timer_stop(s_burst_fsm.timer);

  {
    MutexLocker lock(g_ch5_mutex);
    const auto &slot = g_hub_slots[slot_idx];
    if (!slot.enabled || slot.sock < 0 || !slot.is_connected) {
      return false;
    }
  }

  portENTER_CRITICAL(&s_burst_fsm.mux);
  s_burst_fsm.pkt = pkt;
  s_burst_fsm.target_slot = slot_idx;
  s_burst_fsm.remaining_count = count;
  s_burst_fsm.silence_ms = silence_ms;
  s_burst_fsm.last_tx_ms = 0;
  portEXIT_CRITICAL(&s_burst_fsm.mux);

  esp_timer_start_once(s_burst_fsm.timer, 1000); // 1ms 후 선로 유휴 검증 진입
  return true;
}

void processPacket(int slot_idx, const uint8_t *pkt_data, size_t pkt_len) {
  if (!pkt_data || pkt_len == 0)
    return;

  // Slot 0: 엘리베이터 (0x34) 전용 처리
  if (slot_idx == 0) {
    auto *parser = WallpadParserFactory::getActiveParser();
    if (!parser)
      return;

    span<const uint8_t> frame(pkt_data, pkt_len);
    uint8_t dev_id = 0, sub1 = 0, sub2 = 0;
    if (parser->extractDeviceKey(frame, dev_id, sub1, sub2) && dev_id == 0x34) {
      g_route_registry.recordRoute(5, 0, 0x34, sub1, sub2);

      // 1) 11-byte 상태 응답 (호출 ACK / 대기 복귀)
      // data[4] == 0x04: Byte #8 == 0x06 (호출 이동 중 / ON), 0x00 (대기 복귀 /
      // OFF)
      if (pkt_len == 11 && pkt_data[4] == 0x04) {
        static std::atomic<uint8_t> s_last_elev_pwr{0xFF};
        uint8_t new_pwr = (pkt_data[8] == 0x06) ? 1 : 0;
        uint8_t prev =
            s_last_elev_pwr.exchange(new_pwr, std::memory_order_acq_rel);
        if (prev != new_pwr) {
          ESP_LOGI(TAG, "[CH5] Elevator State Changed -> Power: %u", new_pwr);
          Mgmt_BroadcastDeviceState(0x34, sub1, sub2, DeviceClass::MOMENTARY,
                                    new_pwr, 0, 0, 0, nullptr, 0.0f, 15, 0, 0);
        }
      }
      // 2) 13-byte 도착 감지 브로드캐스트
      // data[4] == 0x01 && data[8] == 0x01: Byte #9 = 댁내 층수, Byte #10 =
      // 호기 번호
      else if (pkt_len == 13 && pkt_data[4] == 0x01 && pkt_data[8] == 0x01) {
        uint8_t floor = pkt_data[9];
        uint8_t ho = pkt_data[10];
        ESP_LOGI(TAG, "[CH5] Elevator Arrived -> Floor: %u, Car: %u", floor,
                 ho);
        Mgmt_BroadcastDeviceState(0x34, sub1, sub2, DeviceClass::MOMENTARY, 0,
                                  0, 0, 0, nullptr, 0.0f, floor, 0, ho);
      }
    }
    return;
  }

#if CONFIG_LOG_DEFAULT_LEVEL >= ESP_LOG_DEBUG
  ESP_LOGD(TAG, "[CH5] Slot %d RX len=%u", slot_idx, (unsigned)pkt_len);
#endif
}

void processStream(int slot_idx, HubClientSlot *slot) {
  if (!slot)
    return;
  if (slot_idx == 0) {
    demuxElevatorStream(slot);
  } else {
    demuxModbusStream(slot_idx, slot);
  }
}

} // namespace Ew11Manager

// ============================================================================
// Domain 3: FCU (Fan Coil Unit) Modbus-RTU Controller & FSM
// ============================================================================
namespace {

static Fcu::SlotRuntime s_fcu_slots[Config::TCP::MAX_EW11_SLOTS]{};

void syncDeviceRepository(uint8_t slot_idx, const Fcu::Snapshot &snap) {
  DeviceStateEntry *dev =
      g_device_repo.findMutable(Config::FCU::DEV_ID, slot_idx, 0, true);
  if (!dev)
    return;

  dev->last_ack_len = 19;
  dev->last_ack_data[6] = static_cast<uint8_t>(snap.mode);
  dev->last_ack_data[8] = static_cast<uint8_t>(snap.fan_speed);
  dev->last_ack_data[10] = static_cast<uint8_t>(snap.swing);
  dev->last_ack_data[14] = snap.target_temp;
  dev->last_target_temp = snap.target_temp;
  dev->last_updated_ms = millis();
  dev->is_online = true;
}

void applyOptimisticState(uint8_t slot_idx, uint16_t mode, uint16_t fan,
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

// 슬롯 소켓에 직접 전송하거나, 선로 점유 중(waiting_response)이면 대기 큐에
// 보관 (Stop-and-Wait 규약)
bool Fcu_SendRaw(uint8_t slot_idx, const uint8_t *pkt, size_t len) {
  if (slot_idx < 1 || slot_idx >= Config::TCP::MAX_EW11_SLOTS || !pkt ||
      len == 0 || len > 16)
    return false;
  auto &rt = s_fcu_slots[slot_idx];

  // 선로가 응답 대기 중이거나 인터패킷 갭 진행 중인 경우: 대기 큐에 보관
  // (Stop-and-Wait 규약)
  if (rt.waiting_response || rt.has_pending_temp ||
      (millis() < rt.next_tx_ms)) {
    memcpy(rt.pending_cmd_buf, pkt, len);
    rt.pending_cmd_len = static_cast<uint8_t>(len);
    return true;
  }

  MutexLocker lock(g_ch5_mutex);
  HubClientSlot &slot = g_hub_slots[slot_idx];
  if (!slot.enabled || slot.sock < 0 || !slot.is_connected)
    return false;

  bool ok =
      (send(slot.sock, pkt, len, MSG_DONTWAIT) == static_cast<ssize_t>(len));
  if (ok) {
    slot.tx_pkts++;
    g_pkt_stats.ch5.tx_pkts.fetch_add(1, std::memory_order_relaxed);
    rt.waiting_response = true;
    rt.query_sent_ms = millis();
    rt.next_tx_ms = millis() + Config::FCU::INTER_PACKET_DELAY_MS;
  }
  return ok;
}

// FCU 단일 레지스터 쓰기 공통 실행 파이프라인
template <typename MutateFn>
bool executeRegisterWrite(uint8_t slot_idx, uint16_t reg, uint16_t val,
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

} // anonymous namespace

namespace Fcu {

void handleSlotRx(uint8_t slot_idx, const uint8_t *data, size_t len) {
  if (slot_idx < 1 || slot_idx >= Config::TCP::MAX_EW11_SLOTS || !data)
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
    Fcu::Snapshot new_snap;
    if (!ModbusRtu::parseStatusResponse(data, len, new_snap))
      return; // CRC 불일치 시 드롭

    rt.waiting_response = false;
    rt.timeout_count = 0;
    rt.is_online = true;

    // 유효 운전 모드(냉방/난방) 기록
    if (new_snap.mode == Fcu::Mode::Cool || new_snap.mode == Fcu::Mode::Heat) {
      rt.last_active_mode = new_snap.mode;
      rt.has_active_record = true;
    }
    if (new_snap.fan_speed != Fcu::FanSpeed::Off) {
      rt.last_active_fan = new_snap.fan_speed;
    }
    rt.last_active_swing = new_snap.swing;

    // [펌웨어 레벨 스윙 자동 안착]
    // 복원 명령으로 회전(2)을 지시받았으나 모터 원점 복귀로 인해 swing != 2로
    // 보고된 경우 1회 자동 보정 ※ Fcu_SendRaw 직접 호출 시 g_ch5_mutex 재귀
    // 데드락이 발생하므로 대기 큐(pending_cmd_buf)에 적재
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

    // 스냅샷 갱신 및 레포지토리 동기화
    rt.snap = new_snap;
    syncDeviceRepository(slot_idx, rt.snap);
  }
}

void handleSlotLoop(uint8_t slot_idx, HubClientSlot *slot, uint32_t now) {
  if (slot_idx < 1 || slot_idx >= Config::TCP::MAX_EW11_SLOTS || !slot)
    return;
  if (!slot->enabled || slot->sock < 0 || !slot->is_connected)
    return;

  auto &rt = s_fcu_slots[slot_idx];

  // ── Step 1: Inter-Packet Delay Gate (선로 유휴 확보) ──
  if (now < rt.next_tx_ms) {
    return;
  }

  // ── Step 2: Two-Phase Sequencer (지연 온도 전송 파이프라인) ──
  if (rt.has_pending_temp) {
    if (!rt.waiting_response) {
      rt.has_pending_temp = false;
      auto temp_frame = ModbusRtu::buildWriteSingle(
          0x0005, static_cast<uint16_t>(rt.pending_temp));
      if (send(slot->sock, temp_frame.data(), temp_frame.size(),
               MSG_DONTWAIT) == static_cast<ssize_t>(temp_frame.size())) {
        slot->tx_pkts++;
        g_pkt_stats.ch5.tx_pkts.fetch_add(1, std::memory_order_relaxed);
        rt.waiting_response = true;
        rt.query_sent_ms = now;
        rt.next_tx_ms = now + Config::FCU::INTER_PACKET_DELAY_MS;
      }
    }
    return; // 인터패킷 갭 중에는 주기적 폴링 억제
  }

  // ── Step 3: Stop-and-Wait Queue (대기 중인 제어 명령 방출) ──
  if (rt.pending_cmd_len > 0) {
    if (!rt.waiting_response) {
      uint8_t len = rt.pending_cmd_len;
      rt.pending_cmd_len = 0;
      if (send(slot->sock, rt.pending_cmd_buf, len, MSG_DONTWAIT) ==
          static_cast<ssize_t>(len)) {
        slot->tx_pkts++;
        g_pkt_stats.ch5.tx_pkts.fetch_add(1, std::memory_order_relaxed);
        rt.waiting_response = true;
        rt.query_sent_ms = now;
        rt.next_tx_ms = now + Config::FCU::INTER_PACKET_DELAY_MS;
      }
    }
    return;
  }

  // ── Step 4: RX Timeout Monitor ──
  if (rt.waiting_response) {
    if (now - rt.query_sent_ms >= Config::FCU::RX_TIMEOUT_MS) {
      rt.waiting_response = false;
      rt.timeout_count++;
      if (rt.timeout_count >= Config::FCU::MAX_TIMEOUT_COUNT) {
        rt.is_online = false;
      }
    }
    return;
  }

  // ── Step 5: Periodic Polling Scheduler (슬롯별 독립 주기) ──
  if (rt.last_poll_ms == 0 ||
      (now - rt.last_poll_ms >= Config::FCU::POLL_INTERVAL_MS)) {
    rt.last_poll_ms = now;
    rt.query_sent_ms = now;
    rt.waiting_response = true;

    // 1st-Tier Polling Target 등록 (스핀락 경합 방지)
    g_polling_targets.registerOrTouch(5, Config::FCU::DEV_ID, slot_idx, 0,
                                      ModbusRtu::kQueryPkt.data(),
                                      ModbusRtu::kQueryPkt.size());

    if (send(slot->sock, ModbusRtu::kQueryPkt.data(),
             ModbusRtu::kQueryPkt.size(), MSG_DONTWAIT) ==
        static_cast<ssize_t>(ModbusRtu::kQueryPkt.size())) {
      slot->tx_pkts++;
      g_pkt_stats.ch5.tx_pkts.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

bool RestorePower(uint8_t slot_idx, uint16_t mode, uint16_t fan, uint16_t swing,
                  uint8_t temp) {
  if (slot_idx < 1 || slot_idx >= Config::TCP::MAX_EW11_SLOTS)
    return false;
  auto &rt = s_fcu_slots[slot_idx];

  if (mode != 1 && mode != 2 && mode != 3)
    mode = 1; // 기본 냉방
  if (fan == 0 || fan > 4)
    fan = 4; // 기본 자동
  if (swing != 0 && swing != 2)
    swing = 0; // 기본 고정
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

  // 모터 캘리브레이션으로 인한 스윙 풀림 대비: 회전(2) 요구 시
  // pending_restore_swing 등록
  rt.pending_restore_swing = (swing == 2) ? 2 : 0;
  return true;
}

bool SetPower(uint8_t slot_idx, bool on) {
  if (slot_idx < 1 || slot_idx >= Config::TCP::MAX_EW11_SLOTS)
    return false;
  auto &rt = s_fcu_slots[slot_idx];

  if (on) {
    Mode target_mode = rt.has_active_record ? rt.last_active_mode : Mode::Cool;
    FanSpeed target_fan =
        rt.has_active_record ? rt.last_active_fan : FanSpeed::Low;
    Swing target_swing =
        rt.has_active_record ? rt.last_active_swing : Swing::Off;

    if (target_fan == FanSpeed::Off)
      target_fan = FanSpeed::Low;
    rt.pending_restore_swing = (target_swing == Swing::On) ? 2 : 0;

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

bool SetMode(uint8_t slot_idx, Mode m) {
  return executeRegisterWrite(slot_idx, 0x0001, static_cast<uint16_t>(m),
                              [m](SlotRuntime &rt) {
                                rt.snap.mode = m;
                                if (m == Mode::Cool || m == Mode::Heat) {
                                  rt.last_active_mode = m;
                                  rt.has_active_record = true;
                                }
                              });
}

bool SetFanSpeed(uint8_t slot_idx, FanSpeed f) {
  return executeRegisterWrite(slot_idx, 0x0002, static_cast<uint16_t>(f),
                              [f](SlotRuntime &rt) {
                                rt.snap.fan_speed = f;
                                rt.snap.power = (f != FanSpeed::Off);
                                if (f != FanSpeed::Off) {
                                  rt.last_active_fan = f;
                                }
                              });
}

bool SetSwing(uint8_t slot_idx, Swing s) {
  return executeRegisterWrite(slot_idx, 0x0003, static_cast<uint16_t>(s),
                              [s](SlotRuntime &rt) {
                                rt.snap.swing = s;
                                rt.last_active_swing = s;
                              });
}

bool SetTargetTemp(uint8_t slot_idx, uint8_t temp_c) {
  if (temp_c < Config::FCU::TEMP_MIN)
    temp_c = Config::FCU::TEMP_MIN;
  if (temp_c > Config::FCU::TEMP_MAX)
    temp_c = Config::FCU::TEMP_MAX;

  return executeRegisterWrite(
      slot_idx, 0x0005, static_cast<uint16_t>(temp_c),
      [temp_c](SlotRuntime &rt) { rt.snap.target_temp = temp_c; });
}

bool GetSlotRuntime(uint8_t slot_idx, SlotRuntime &out_rt) {
  if (slot_idx < 1 || slot_idx >= Config::TCP::MAX_EW11_SLOTS)
    return false;
  out_rt = s_fcu_slots[slot_idx];
  return true;
}

} // namespace Fcu

// ============================================================================
// Domain 4: L4 TCP Hub Transport Layer (BSD Sockets & Lifecycle)
// ============================================================================
void Tcp_EnableKeepalive(int sock, int idle, int intvl, int cnt) {
  if (sock < 0)
    return;
  int keepalive = 1;
  setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
  setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
  setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
  setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
}

static void configureClientSocket(int sock) {
  int flags = fcntl(sock, F_GETFL, 0);
  fcntl(sock, F_SETFL, flags | O_NONBLOCK);
  int nodelay = 1;
  setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
  int sockbuf = Config::TCP::SOCKET_BUFFER_SIZE;
  setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &sockbuf, sizeof(sockbuf));
  setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sockbuf, sizeof(sockbuf));
  Tcp_EnableKeepalive(sock, 30, Config::TCP::DEFAULT_KEEPALIVE_INTVL_SEC,
                      Config::TCP::DEFAULT_KEEPALIVE_CNT);
}

int Hub_AcceptClient(int slot_idx, int server_fd) {
  if (slot_idx < 0 || slot_idx >= Config::TCP::MAX_EW11_SLOTS || server_fd < 0)
    return -1;

  struct sockaddr_in caddr;
  socklen_t clen = sizeof(caddr);
  int new_sock =
      accept(server_fd, reinterpret_cast<struct sockaddr *>(&caddr), &clen);
  if (new_sock < 0)
    return -1;

  const uint8_t *b = reinterpret_cast<const uint8_t *>(&caddr.sin_addr.s_addr);
  IPAddress remote_ip(b[0], b[1], b[2], b[3]);
  if (!Tcp_IsAllowedIP(remote_ip)) {
    close(new_sock);
    return -1;
  }

  char client_ip_str[16];
  snprintf(client_ip_str, sizeof(client_ip_str), "%u.%u.%u.%u", b[0], b[1],
           b[2], b[3]);

  MutexLocker lock(g_ch5_mutex);
  auto &slot = g_hub_slots[slot_idx];

  if (slot.target_ip[0] != '\0' && strcmp(slot.target_ip, client_ip_str) != 0) {
    ESP_LOGW(TAG,
             "[CH5] Client IP mismatch for Slot %d (%s): got %s, expected %s",
             slot_idx, slot.name, client_ip_str, slot.target_ip);
    close(new_sock);
    return -1;
  }

  configureClientSocket(new_sock);

  if (slot.sock >= 0) {
    close(slot.sock);
  }
  slot.sock = new_sock;
  slot.is_connected = true;
  slot.rx_len = 0;
  slot.last_rx_ms = millis();
  if (slot.target_ip[0] == '\0') {
    strncpy(slot.target_ip, client_ip_str, sizeof(slot.target_ip) - 1);
    slot.target_ip[sizeof(slot.target_ip) - 1] = '\0';
  }

  g_pkt_stats.ch5.is_connected.store(true, std::memory_order_relaxed);
  g_pkt_stats.ch5.connection_count.fetch_add(1, std::memory_order_relaxed);
  ESP_LOGI(TAG, "[CH5] Accepted EW11 client %s on port %u -> Slot %d (%s)",
           client_ip_str, slot.target_port, slot_idx, slot.name);
  return new_sock;
}

void Hub_ProcessPacket(HubClientSlot *slot, const uint8_t *pkt_data,
                       size_t pkt_len) {
  if (!slot || !pkt_data || pkt_len == 0)
    return;

  StaticPacket pkt{5, static_cast<uint8_t>(pkt_len)};
  std::copy(pkt_data, pkt_data + pkt_len, pkt.data.begin());

  slot->rx_pkts++;
  g_pkt_stats.ch5.rx_pkts.fetch_add(1, std::memory_order_relaxed);
  g_telnet_tracer.trace(5, false, TraceType::RMT, pkt);

  auto *parser = WallpadParserFactory::getActiveParser();
  if (parser) {
    span<const uint8_t> frame(pkt_data, pkt_len);
    uint8_t dev_id = 0, sub1 = 0, sub2 = 0;
    if (parser->extractDeviceKey(frame, dev_id, sub1, sub2) && dev_id != 0 &&
        dev_id != parser->getStx() && dev_id != parser->getEtx() &&
        dev_id != 0xFF) {
      int8_t s_idx = static_cast<int8_t>(slot - g_hub_slots);
      g_route_registry.recordRoute(5, s_idx, dev_id, sub1, sub2);

      bool is_query = parser->isQueryPacket(frame);
      bool is_ack = (pkt_len >= 5 && frame[4] == 0x04); // 표준 ACK Opcode(0x04)

      // 0x2A (신발장 서브 패널 / 원격검침)는 폴링 대상 및 단말 제어 기기가
      // 아니므로 캐시에서 완전 제외
      if (dev_id != 0x2A) {
        if (is_query) {
          g_polling_targets.registerOrTouch(5, dev_id, sub1, sub2, pkt_data,
                                            pkt_len);
        }
        if (is_ack) {
          g_device_repo.updateFromBus(pkt);
        }
      }
    }
  }

  int slot_idx = static_cast<int>(slot - g_hub_slots);
  Ew11Manager::processPacket(slot_idx, pkt_data, pkt_len);
}

void Hub_Data(HubClientSlot *slot, const uint8_t *data, size_t len) {
  if (!slot || slot->sock < 0 || !data || len == 0)
    return;

  slot->last_rx_ms = millis();

  // 오버플로우 방어: 수신 버퍼 여유가 부족할 경우 미완성 패킷 시작 바이트
  // 앞으로 슬라이딩
  if (slot->rx_len + len > sizeof(slot->rx_buf)) {
    uint8_t stx = slot->tracker.candidate_stx.load(std::memory_order_relaxed);
    if (stx == 0)
      stx = PKT_STX;
    size_t stx_pos = 0;
    while (stx_pos < slot->rx_len && slot->rx_buf[stx_pos] != stx) {
      stx_pos++;
    }
    consumeRxBuffer(slot, stx_pos);
  }

  size_t copy_len = std::min(len, sizeof(slot->rx_buf) - slot->rx_len);
  std::copy(data, data + copy_len, slot->rx_buf + slot->rx_len);
  slot->rx_len += copy_len;

  int slot_idx = static_cast<int>(slot - g_hub_slots);
  Ew11Manager::processStream(slot_idx, slot);
}

void Hub_LoadConfig() {
  Preferences p;
  p.begin("ew11-config", true);
  MutexLocker lock(g_ch5_mutex);

  // Slot 0 (엘리베이터)
  g_hub_slots[0].enabled = p.getBool("e0_en", true);
  p.getString("e0_name", "Elevator")
      .toCharArray(g_hub_slots[0].name, sizeof(g_hub_slots[0].name));
  p.getString("e0_ip", "172.30.1.245")
      .toCharArray(g_hub_slots[0].target_ip, sizeof(g_hub_slots[0].target_ip));
  uint16_t p0 = p.getUShort("e0_port", 8898);
  if (p0 == 0 || p0 == 8899)
    p0 = 8898;
  g_hub_slots[0].target_port = p0;
  g_hub_slots[0].dev_type = HubDeviceType::WALLPAD_COMPATIBLE;
  g_hub_slots[0].sock = -1;
  g_hub_slots[0].is_connected = false;
  g_hub_slots[0].rx_len = 0;

  // Slot 1~4 (FCU 에어컨)
  for (int i = 1; i < Config::TCP::MAX_EW11_SLOTS; i++) {
    char k_en[8], k_nm[8], k_ip[8], k_pt[8], def_nm[16];
    snprintf(k_en, sizeof(k_en), "e%d_en", i);
    snprintf(k_nm, sizeof(k_nm), "e%d_name", i);
    snprintf(k_ip, sizeof(k_ip), "e%d_ip", i);
    snprintf(k_pt, sizeof(k_pt), "e%d_port", i);
    snprintf(def_nm, sizeof(def_nm), "AC_%d", i);

    g_hub_slots[i].enabled = p.getBool(k_en, false);
    p.getString(k_nm, def_nm)
        .toCharArray(g_hub_slots[i].name, sizeof(g_hub_slots[i].name));
    p.getString(k_ip, "").toCharArray(g_hub_slots[i].target_ip,
                                      sizeof(g_hub_slots[i].target_ip));
    uint16_t def_slot_port =
        Config::TCP::EW11_SLOT_PORTS[i]; // 8891, 8892, 8893, 8894
    uint16_t pi = p.getUShort(k_pt, def_slot_port);
    if (pi == 0 || pi == 8899)
      pi = def_slot_port;
    g_hub_slots[i].target_port = pi;
    g_hub_slots[i].dev_type = HubDeviceType::AIR_CONDITIONER;
    g_hub_slots[i].sock = -1;
    g_hub_slots[i].is_connected = false;
    g_hub_slots[i].rx_len = 0;
  }

  for (int s = 0; s < Config::TCP::MAX_EW11_SLOTS; s++) {
    char ns[16], tag[16];
    snprintf(ns, sizeof(ns), "e%d_frame", s);
    snprintf(tag, sizeof(tag), "EW11_#%d", s);
    g_hub_slots[s].tracker.restoreFromNvs(ns, tag);
  }
  p.end();
}

void Hub_SaveConfig() {
  Preferences p;
  p.begin("ew11-config", false);
  MutexLocker lock(g_ch5_mutex);

  for (int i = 0; i < Config::TCP::MAX_EW11_SLOTS; i++) {
    char k_en[8], k_nm[8], k_ip[8], k_pt[8];
    snprintf(k_en, sizeof(k_en), "e%d_en", i);
    snprintf(k_nm, sizeof(k_nm), "e%d_name", i);
    snprintf(k_ip, sizeof(k_ip), "e%d_ip", i);
    snprintf(k_pt, sizeof(k_pt), "e%d_port", i);

    p.putBool(k_en, g_hub_slots[i].enabled);
    p.putString(k_nm, g_hub_slots[i].name);
    p.putString(k_ip, g_hub_slots[i].target_ip);
    p.putUShort(k_pt, g_hub_slots[i].target_port);
  }
  p.end();
}

bool Hub_SetSlot(uint8_t slot_idx, bool enabled, const char *ip, uint16_t port,
                 const char *name) {
  if (slot_idx >= Config::TCP::MAX_EW11_SLOTS)
    return false;

  MutexLocker lock(g_ch5_mutex);
  auto &slot = g_hub_slots[slot_idx];

  bool changed = (slot.enabled != enabled) ||
                 (strcmp(slot.target_ip, ip ? ip : "") != 0) ||
                 (port > 0 && slot.target_port != port) ||
                 (name && strlen(name) > 0 && strcmp(slot.name, name) != 0);

  if (!changed)
    return true;

  bool reconnect_needed = (slot.enabled != enabled) ||
                          (strcmp(slot.target_ip, ip ? ip : "") != 0) ||
                          (port > 0 && slot.target_port != port);

  slot.enabled = enabled;
  if (ip) {
    strncpy(slot.target_ip, ip, sizeof(slot.target_ip) - 1);
    slot.target_ip[sizeof(slot.target_ip) - 1] = '\0';
  } else {
    slot.target_ip[0] = '\0';
  }
  if (port > 0)
    slot.target_port = port;
  if (name && strlen(name) > 0) {
    strncpy(slot.name, name, sizeof(slot.name) - 1);
    slot.name[sizeof(slot.name) - 1] = '\0';
  }

  if (reconnect_needed && slot.sock >= 0) {
    close(slot.sock);
    slot.sock = -1;
    slot.is_connected = false;
    slot.rx_len = 0;
    slot.last_reconnect_ms = 0;
  }

  Hub_SaveConfig();
  return true;
}

bool Hub_SendPacket(uint8_t slot_idx, const StaticPacket &pkt) {
  if (slot_idx >= Config::TCP::MAX_EW11_SLOTS)
    return false;
  MutexLocker lock(g_ch5_mutex);
  auto &slot = g_hub_slots[slot_idx];
  if (!slot.enabled || slot.sock < 0 || !slot.is_connected)
    return false;

  int s = send(slot.sock, pkt.data.data(), pkt.length, MSG_DONTWAIT);
  if (s == static_cast<int>(pkt.length)) {
    slot.tx_pkts++;
    g_pkt_stats.ch5.tx_pkts.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  return false;
}


// ── JSON-RPC & TCP Management Server (formerly Service.cpp) ──
// ============================================================================
// From src/Management/Management.cpp
// ============================================================================

// ============================================================================
// From src/Management/HttpOta.cpp
// ============================================================================

HttpOtaState g_http_ota_state{};
static char s_ota_target_url[256] = {0};

extern void System_Restart(const char *reason);
extern EventGroupHandle_t g_system_event_group;

static constexpr char OTA_REPO_PREFIX[] = "/Eri-tep/Gateway/";
static constexpr size_t MAX_REDIRECT_LOCATION_LEN = 1024;

enum class OtaUrlContext { Initial, Redirect };

static void configure_public_tls(WiFiClientSecure &client) {
  // 이전 홉의 setInsecure() 잔여 상태(_use_insecure = true)를 확실히 해제
  client.setCACert(nullptr);

#if defined(ESP_ARDUINO_VERSION) &&                                            \
    (ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 3, 12))
  client.useBuiltinCACertBundle();
#else
  extern const uint8_t x509_crt_bundle_start[] asm(
      "_binary_x509_crt_bundle_start");
  client.setCACertBundle(x509_crt_bundle_start);
#endif
  client.setTimeout(5);
  client.setHandshakeTimeout(8); // CA 체인 검증을 감안하여 8초 확보
}

static bool is_private_host(const char *host) {
  if (!host)
    return false;
  if (strcmp(host, "localhost") == 0 || strcmp(host, "127.0.0.1") == 0) {
    return true;
  }
  unsigned a = 0, b = 0, c = 0, d = 0;
  char tail = '\0';
  if (sscanf(host, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) == 4 && a < 256 &&
      b < 256 && c < 256 && d < 256) {
    return a == 10 || (a == 192 && b == 168) ||
           (a == 172 && b >= 16 && b <= 31);
  }
  size_t n = strlen(host);
  return (n > 6 && strcasecmp(host + n - 6, ".local") == 0);
}

static bool has_unsafe_path_segments(const char *path) {
  if (!path)
    return true;
  return (strstr(path, "/./") != nullptr || strstr(path, "/../") != nullptr ||
          strstr(path, "/..") != nullptr || strstr(path, "/%2e") != nullptr ||
          strstr(path, "/%2E") != nullptr || strstr(path, "\\") != nullptr);
}

static bool extract_url_components(const char *url, char *out_host,
                                   size_t max_host_len, int &out_port,
                                   char *out_path, size_t max_path_len,
                                   bool &out_is_https) {
  if (!url)
    return false;
  out_is_https = (strncmp(url, "https://", 8) == 0);
  bool is_http = (strncmp(url, "http://", 7) == 0);
  if (!out_is_https && !is_http)
    return false;

  out_port = out_is_https ? 443 : 80;
  const char *host_start = out_is_https ? (url + 8) : (url + 7);

  // Authority 끝 지점 파싱: RFC 3986에 따라 '/', '?', '#' 중 가장 빠른 지점
  // 탐색
  const char *p = host_start;
  while (*p && *p != '/' && *p != '?' && *p != '#') {
    p++;
  }
  const char *auth_end = p;

  // 1. Userinfo ('@') 우회 공격 차단
  if (memchr(host_start, '@', static_cast<size_t>(auth_end - host_start)) !=
      nullptr) {
    return false;
  }

  // 2. 포트 번호 분리 및 엄격 검증
  const char *colon = static_cast<const char *>(
      memchr(host_start, ':', static_cast<size_t>(auth_end - host_start)));
  const char *host_end = colon ? colon : auth_end;
  size_t host_len = static_cast<size_t>(host_end - host_start);

  if (host_len == 0 || host_len >= max_host_len) {
    return false;
  }
  memcpy(out_host, host_start, host_len);
  out_host[host_len] = '\0';

  if (colon) {
    int port = 0;
    bool has_port_digits = false;
    for (const char *cp = colon + 1; cp < auth_end; ++cp) {
      if (*cp < '0' || *cp > '9')
        return false;
      has_port_digits = true;
      port = port * 10 + (*cp - '0');
      if (port > 65535)
        return false;
    }
    if (!has_port_digits || port == 0)
      return false;
    out_port = port;
  }

  // 3. Path 추출
  if (*auth_end == '/') {
    const char *path_end = auth_end;
    while (*path_end && *path_end != '?' && *path_end != '#') {
      path_end++;
    }
    size_t path_len = static_cast<size_t>(path_end - auth_end);
    if (path_len >= max_path_len)
      return false;
    memcpy(out_path, auth_end, path_len);
    out_path[path_len] = '\0';
  } else {
    if (max_path_len < 2)
      return false;
    out_path[0] = '/';
    out_path[1] = '\0';
  }

  return true;
}

static bool is_trusted_ota_url(const char *url, OtaUrlContext context) {
  char host[128] = {0};
  char path[256] = {0};
  int port = 0;
  bool is_https = false;
  if (!extract_url_components(url, host, sizeof(host), port, path, sizeof(path),
                              is_https)) {
    return false;
  }

  // 1. 로컬/사설망 IP 및 로컬 호스트 허용 (임의 포트, 임의 경로 허용)
  if (is_private_host(host)) {
    return true;
  }

  // 2. 외부 인터넷 도메인은 반드시 HTTPS + 443 포트여야 함
  if (!is_https || port != 443) {
    return false;
  }

  // 3. 공식 GitHub 저장소 호스트: 경로 검증 및 상위 디렉터리 순회(/../) 차단
  if (strcasecmp(host, "raw.githubusercontent.com") == 0 ||
      strcasecmp(host, "github.com") == 0) {
    if (has_unsafe_path_segments(path)) {
      return false;
    }
    return (strncmp(path, OTA_REPO_PREFIX, sizeof(OTA_REPO_PREFIX) - 1) == 0);
  }

  // 4. 공식 GitHub CDN 도메인: Redirect 컨텍스트에서만 허용 (최초 URL 직접 지정
  // 금지)
  if (strcasecmp(host, "objects.githubusercontent.com") == 0 ||
      strcasecmp(host, "release-assets.githubusercontent.com") == 0 ||
      strcasecmp(host, "github-releases.githubusercontent.com") == 0) {
    return (context == OtaUrlContext::Redirect);
  }

  return false;
}

class OtaInProgressGuard {
public:
  OtaInProgressGuard() {
    g_ota_in_progress.store(true, std::memory_order_release);
    if (g_system_event_group) {
      xEventGroupClearBits(g_system_event_group, SYS_EVT_OTA_IDLE);
    }
  }
  ~OtaInProgressGuard() {
    if (!_dismissed) {
      g_ota_in_progress.store(false, std::memory_order_release);
      if (g_system_event_group) {
        xEventGroupSetBits(g_system_event_group, SYS_EVT_OTA_IDLE);
      }
    }
  }
  void dismiss() noexcept { _dismissed = true; }
  OtaInProgressGuard(const OtaInProgressGuard &) = delete;
  OtaInProgressGuard &operator=(const OtaInProgressGuard &) = delete;

private:
  bool _dismissed{false};
};

static void ota_fail(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vsnprintf(g_http_ota_state.last_error, sizeof(g_http_ota_state.last_error),
            fmt, args);
  va_end(args);
  snprintf(g_http_ota_state.status, sizeof(g_http_ota_state.status), "Failed");
  ::Serial.printf("[OTA] Error: %s\r\n", g_http_ota_state.last_error);
}

static bool Ota_ResolveDownloadUrl(const char *initial_url,
                                   String &out_final_url,
                                   WiFiClientSecure &secure_client,
                                   WiFiClient &plain_client, HTTPClient &http,
                                   int &out_content_length) {
  char initial_host[128] = {0};
  char initial_path[256] = {0};
  int initial_port = 0;
  bool is_https_initial = false;
  extract_url_components(initial_url, initial_host, sizeof(initial_host),
                         initial_port, initial_path, sizeof(initial_path),
                         is_https_initial);

  if (!is_private_host(initial_host) && time(nullptr) < 1700000000) {
    ota_fail("System time not synced (NTP required for TLS)");
    return false;
  }

  ::Serial.printf("[OTA] Starting Stream OTA to target host: %s (port %d)\r\n",
                  initial_host, initial_port);
  snprintf(g_http_ota_state.status, sizeof(g_http_ota_state.status),
           "Connecting...");
  g_http_ota_state.progress_pct = 0;
  g_http_ota_state.last_error[0] = '\0';

  // EW11 소켓 일시 해제 (lwIP pcb + 소켓 수신 버퍼 힙 확보)
  {
    MutexLocker lock(g_ch5_mutex, pdMS_TO_TICKS(2000));
    if (!lock.isLocked()) {
      ota_fail("CH5 lock timeout (2s)");
      return false;
    }
    for (int s = 1; s < Config::TCP::MAX_EW11_SLOTS; ++s) {
      auto &slot = g_hub_slots[s];
      if (slot.sock >= 0) {
        close(slot.sock);
        slot.sock = -1;
        slot.is_connected = false;
        slot.rx_len = 0;
      }
    }
  }

  esp_task_wdt_reset();

  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  http.setConnectTimeout(5000);
  http.setTimeout(5000);
  const char *header_keys[] = {"Location"};
  http.collectHeaders(header_keys, 1);

  String current_url = initial_url;
  int redirect_count = 0;
  int httpCode = 0;

  ::Serial.printf("[OTA] Phase 1: Sending GET request...\r\n");

  while (redirect_count <= 2) {
    esp_task_wdt_reset();

    char host[128] = {0};
    char path[256] = {0};
    int port = 0;
    bool is_https = false;
    extract_url_components(current_url.c_str(), host, sizeof(host), port, path,
                           sizeof(path), is_https);

    OtaUrlContext ctx = (redirect_count == 0) ? OtaUrlContext::Initial
                                              : OtaUrlContext::Redirect;
    if (!is_trusted_ota_url(current_url.c_str(), ctx)) {
      ota_fail("Untrusted URL in chain (host: %s, port: %d)",
               host[0] ? host : "invalid", port);
      http.end();
      secure_client.stop();
      plain_client.stop();
      return false;
    }

    WiFiClient *transport = nullptr;
    if (is_https) {
      if (is_private_host(host)) {
        secure_client.setInsecure();
      } else {
        configure_public_tls(secure_client);
      }
      transport = &secure_client;
    } else {
      plain_client.setTimeout(5);
      transport = &plain_client;
    }

    if (!http.begin(*transport, current_url)) {
      ota_fail("HTTP begin failed");
      http.end();
      secure_client.stop();
      plain_client.stop();
      return false;
    }

    httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      break;
    } else if (httpCode == HTTP_CODE_MOVED_PERMANENTLY ||
               httpCode == HTTP_CODE_FOUND || httpCode == HTTP_CODE_SEE_OTHER ||
               httpCode == HTTP_CODE_TEMPORARY_REDIRECT || httpCode == 308) {
      redirect_count++;
      if (redirect_count > 2) {
        ota_fail("Too many redirects (>2)");
        http.end();
        secure_client.stop();
        plain_client.stop();
        return false;
      }

      String location = http.header("Location");
      http.end();
      secure_client.stop();
      plain_client.stop();

      if (location.isEmpty() || location.length() > MAX_REDIRECT_LOCATION_LEN) {
        ota_fail("Invalid redirect Location length: %u",
                 (unsigned)location.length());
        return false;
      }

      if (!location.startsWith("http://") && !location.startsWith("https://")) {
        ota_fail("Relative redirect rejected");
        return false;
      }

      char next_host[128] = {0};
      char next_path[256] = {0};
      int next_port = 0;
      bool next_is_https = false;
      if (!extract_url_components(location.c_str(), next_host,
                                  sizeof(next_host), next_port, next_path,
                                  sizeof(next_path), next_is_https)) {
        ota_fail("Failed to parse redirect URL");
        return false;
      }

      if (is_private_host(next_host) != is_private_host(initial_host)) {
        ota_fail("Redirect crosses trust boundary");
        return false;
      }

      current_url = location;
      ::Serial.printf("[OTA] Redirect #%d (%d) -> to host: %s (port %d)\r\n",
                      redirect_count, httpCode, next_host, next_port);
    } else {
      ota_fail("HTTP GET failed, code: %d", httpCode);
      http.end();
      secure_client.stop();
      plain_client.stop();
      return false;
    }
  }

  if (httpCode != HTTP_CODE_OK) {
    ota_fail("Failed to reach OK status (final code: %d)", httpCode);
    http.end();
    secure_client.stop();
    plain_client.stop();
    return false;
  }

  out_content_length = http.getSize();
  ::Serial.printf("[OTA] Phase 2: HTTP 200 OK, image size: %d bytes\r\n",
                  out_content_length);
  if (out_content_length <= 0) {
    ota_fail("Invalid Content-Length: %d", out_content_length);
    http.end();
    secure_client.stop();
    plain_client.stop();
    return false;
  }

  out_final_url = current_url;
  return true;
}

static bool Ota_StreamAndWritePartition(HTTPClient &http,
                                        WiFiClientSecure &secure_client,
                                        WiFiClient &plain_client,
                                        int contentLength) {
  esp_task_wdt_reset();

  if (!Update.begin(contentLength, U_FLASH)) {
    ota_fail("Update.begin failed (code %u)", (unsigned)Update.getError());
    http.end();
    secure_client.stop();
    plain_client.stop();
    return false;
  }

  snprintf(g_http_ota_state.status, sizeof(g_http_ota_state.status),
           "Downloading...");
  ::Serial.printf(
      "[OTA] Phase 3: Update.begin OK. Starting stream write...\r\n");

  WiFiClient *stream = http.getStreamPtr();
  if (!stream) {
    ota_fail("Failed to get stream pointer");
    http.end();
    secure_client.stop();
    plain_client.stop();
    return false;
  }

  static uint8_t s_ota_buff[4096];
  size_t written = 0;
  uint32_t last_progress_time = millis();
  uint32_t download_start_ms = millis();
  uint32_t last_diag_ms = millis();
  int last_pct = -1;

  while (written < static_cast<size_t>(contentLength)) {
    esp_task_wdt_reset();

    if (millis() - download_start_ms > Config::OTA::DOWNLOAD_DEADLINE_MS) {
      ota_fail("Total download deadline exceeded");
      http.end();
      secure_client.stop();
      plain_client.stop();
      return false;
    }

    if (millis() - last_progress_time > Config::OTA::STALL_TIMEOUT_MS) {
      ota_fail("Stream read stall timeout (25s)");
      http.end();
      secure_client.stop();
      plain_client.stop();
      return false;
    }

    int avail = stream->available();
    if (!stream->connected() && avail == 0) {
      ota_fail("Connection lost prematurely at %u/%d bytes", (unsigned)written,
               contentLength);
      http.end();
      secure_client.stop();
      plain_client.stop();
      return false;
    }

    if (avail <= 0) {
      vTaskDelay(pdMS_TO_TICKS(Config::OTA::IDLE_DELAY_MS));
      continue;
    }

    size_t to_read = (avail < static_cast<int>(sizeof(s_ota_buff)))
                         ? static_cast<size_t>(avail)
                         : sizeof(s_ota_buff);

    if (written + to_read > static_cast<size_t>(contentLength)) {
      to_read = static_cast<size_t>(contentLength) - written;
    }

    size_t read_bytes = stream->readBytes(s_ota_buff, to_read);
    if (read_bytes == 0) {
      vTaskDelay(pdMS_TO_TICKS(Config::OTA::IDLE_DELAY_MS));
      continue;
    }

    size_t written_bytes = Update.write(s_ota_buff, read_bytes);
    if (written_bytes != read_bytes) {
      ota_fail("Update.write mismatch (exp %u, got %u)", (unsigned)read_bytes,
               (unsigned)written_bytes);
      http.end();
      secure_client.stop();
      plain_client.stop();
      return false;
    }

    written += read_bytes;
    last_progress_time = millis();

    int pct = (written * 100) / contentLength;
    g_http_ota_state.progress_pct = pct;
    if (pct != last_pct && (pct % 10 == 0 || pct == 100)) {
      last_pct = pct;
      ::Serial.printf("[OTA] Progress: %d%% (%u / %d bytes)\r\n", pct,
                      (unsigned)written, contentLength);
    }

    if (TimeUtils::isElapsed(last_diag_ms, 5000)) {
      last_diag_ms = millis();
      uint32_t cur_free =
          heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      uint32_t cur_largest = heap_caps_get_largest_free_block(
          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      uint32_t cur_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL |
                                                         MALLOC_CAP_8BIT);
      ::Serial.printf(
          "[OTA] %u/%d bytes (%d%%), free=%u, largest=%u, min=%u\r\n",
          (unsigned)written, contentLength, pct, (unsigned)cur_free,
          (unsigned)cur_largest, (unsigned)cur_min);
    }

    vTaskDelay(pdMS_TO_TICKS(5));
  }

  ::Serial.printf(
      "[OTA] Phase 4: Download complete (%u bytes). Finalizing update...\r\n",
      (unsigned)written);
  esp_task_wdt_reset();

  if (!Update.end()) {
    ota_fail("Update.end failed (code %u)", (unsigned)Update.getError());
    http.end();
    secure_client.stop();
    plain_client.stop();
    return false;
  }

  if (!Update.isFinished()) {
    ota_fail("Update not finished");
    http.end();
    secure_client.stop();
    plain_client.stop();
    return false;
  }

  http.end();
  secure_client.stop();
  plain_client.stop();
  return true;
}

static bool do_ota(const char *initial_url) {
  OtaInProgressGuard ota_guard;

  WiFiClientSecure secure_client;
  WiFiClient plain_client;
  HTTPClient http;
  String final_url;
  int content_length = 0;

  if (!Ota_ResolveDownloadUrl(initial_url, final_url, secure_client,
                              plain_client, http, content_length)) {
    return false;
  }

  if (!Ota_StreamAndWritePartition(http, secure_client, plain_client,
                                   content_length)) {
    return false;
  }

  snprintf(g_http_ota_state.status, sizeof(g_http_ota_state.status), "Success");
  g_http_ota_state.progress_pct = 100;
  ::Serial.printf(
      "[OTA] Firmware update SUCCESS! Rebooting in 1 second...\r\n");
  ota_guard.dismiss();
  return true;
}

static void Task_HttpOta(void *pvParameters) {
  // 진입 즉시 FreeRTOS WDT 감시 등록 (접속 단계 행 방지)
  bool wdt_registered = (esp_task_wdt_add(nullptr) == ESP_OK);

  const char *url = static_cast<const char *>(pvParameters);
  bool success = false;
  if (url && strlen(url) > 0) {
    success = do_ota(url);
  }

  if (!success) {
    if (wdt_registered) {
      esp_task_wdt_delete(nullptr);
    }
    Update.abort(); // 실패 시 OTA 파티션 언락
    // 모든 실패 경로는 여기서 단 한 번 안전하게 원복됨
    if (g_system_event_group) {
      xEventGroupSetBits(g_system_event_group, SYS_EVT_OTA_IDLE);
    }
    g_ota_in_progress.store(false, std::memory_order_release);
    g_http_ota_state.in_progress.store(false, std::memory_order_release);
    ::Serial.printf("[OTA] Task aborted. Stack high water mark: %u bytes\r\n",
                    (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    vTaskDelete(nullptr);
    return;
  }

  // 성공 경로: WDT 감시를 유지한 채 esp_task_wdt_reset() 호출 후 System_Restart
  // 실행 (System_Restart 내부의 NVS 쓰기나 CH5 락 블로킹 시 30초 WDT 패닉
  // 리부트로 자가 복구)
  ::Serial.printf("[OTA] Task complete. Stack high water mark: %u bytes\r\n",
                  (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  esp_task_wdt_reset();
  vTaskDelay(pdMS_TO_TICKS(1000));
  System_Restart("OTA Complete");

  // System_Restart가 반환되는 예외 상황에 대비한 복구 (정상 시 여기까지
  // 도달하지 않음)
  if (wdt_registered) {
    esp_task_wdt_delete(nullptr);
  }
  if (g_system_event_group) {
    xEventGroupSetBits(g_system_event_group, SYS_EVT_OTA_IDLE);
  }
  g_ota_in_progress.store(false, std::memory_order_release);
  g_http_ota_state.in_progress.store(false, std::memory_order_release);
  vTaskDelete(nullptr);
}

class OtaAdmissionGuard {
public:
  OtaAdmissionGuard() {
    g_ota_in_progress.store(true, std::memory_order_release);
    if (g_system_event_group) {
      xEventGroupClearBits(g_system_event_group, SYS_EVT_OTA_IDLE);
    }
  }
  ~OtaAdmissionGuard() {
    if (!_dismissed) {
      g_ota_in_progress.store(false, std::memory_order_release);
      g_http_ota_state.in_progress.store(false, std::memory_order_release);
      if (g_system_event_group) {
        xEventGroupSetBits(g_system_event_group, SYS_EVT_OTA_IDLE);
      }
    }
  }
  void dismiss() noexcept { _dismissed = true; }
  OtaAdmissionGuard(const OtaAdmissionGuard &) = delete;
  OtaAdmissionGuard &operator=(const OtaAdmissionGuard &) = delete;

private:
  bool _dismissed{false};
};

void Mgmt_StartHttpOta(const char *url) {
  // 1. 이미 진행 중인지 원자적으로 확인 (CAS)
  bool expected = false;
  if (!g_http_ota_state.in_progress.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel)) {
    ::Serial.println("[OTA] Start requested but already in progress");
    return;
  }

  // 2. Admission Guard 생성 (모든 조기 리턴 및 실패 경로에서 자동 롤백 보장)
  OtaAdmissionGuard admission_guard;

  const char *target = url;
  if (!target || strlen(target) == 0) {
    target = DEFAULT_CLOUD_OTA_URL;
  }

  constexpr size_t MAX_INITIAL_URL_LEN = sizeof(s_ota_target_url) - 1;
  if (strlen(target) > MAX_INITIAL_URL_LEN) {
    ::Serial.printf("[OTA] Initial URL too long (%u bytes, max %u)\r\n",
                    (unsigned)strlen(target), (unsigned)MAX_INITIAL_URL_LEN);
    snprintf(g_http_ota_state.status, sizeof(g_http_ota_state.status),
             "Failed");
    snprintf(g_http_ota_state.last_error, sizeof(g_http_ota_state.last_error),
             "Initial OTA URL too long");
    return;
  }

  char target_host[128] = {0};
  char target_path[256] = {0};
  int target_port = 0;
  bool target_is_https = false;
  extract_url_components(target, target_host, sizeof(target_host), target_port,
                         target_path, sizeof(target_path), target_is_https);

  // 3. 외부 HTTPS인 경우 유효한 시스템 시간 Sanity check (NTP 미동기화 시
  // 인증서 검증 실패 방지)
  if (!is_private_host(target_host) && time(nullptr) < 1700000000) {
    ::Serial.println("[OTA] System time not synced (NTP required for TLS)");
    snprintf(g_http_ota_state.status, sizeof(g_http_ota_state.status),
             "Failed");
    snprintf(g_http_ota_state.last_error, sizeof(g_http_ota_state.last_error),
             "System time not synced (NTP required)");
    return;
  }

  if (!is_trusted_ota_url(target, OtaUrlContext::Initial)) {
    ::Serial.printf("[OTA] Untrusted OTA URL: host=%s, port=%d, path=%s\r\n",
                    target_host[0] ? target_host : "invalid", target_port,
                    target_path);
    snprintf(g_http_ota_state.status, sizeof(g_http_ota_state.status),
             "Failed");
    snprintf(g_http_ota_state.last_error, sizeof(g_http_ota_state.last_error),
             "Untrusted OTA URL domain, port, or path");
    return;
  }

  // 4. 힙 메모리 여유 사전 검사 (TLS 핸드셰이크 최소 내부 SRAM 60KB & 연속 30KB
  // 확보)
  uint32_t free_heap =
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  uint32_t largest_block =
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  ::Serial.printf(
      "[OTA] Internal SRAM check: free=%u bytes, largest_block=%u bytes\r\n",
      (unsigned)free_heap, (unsigned)largest_block);

  if (free_heap < 60000 || largest_block < 30000) {
    ::Serial.printf("[OTA] Heap too low/fragmented: %u bytes (largest %u, need "
                    ">= 60000 / 30000)\r\n",
                    (unsigned)free_heap, (unsigned)largest_block);
    snprintf(g_http_ota_state.status, sizeof(g_http_ota_state.status),
             "Failed");
    snprintf(g_http_ota_state.last_error, sizeof(g_http_ota_state.last_error),
             "Heap too low (%u bytes, largest %u, need >= 60000)",
             (unsigned)free_heap, (unsigned)largest_block);
    return;
  }

  strncpy(s_ota_target_url, target, sizeof(s_ota_target_url) - 1);
  s_ota_target_url[sizeof(s_ota_target_url) - 1] = '\0';

  snprintf(g_http_ota_state.status, sizeof(g_http_ota_state.status),
           "Starting...");
  g_http_ota_state.progress_pct = 0;
  g_http_ota_state.last_error[0] = '\0';

  // 5. OTA 전용 백그라운드 태스크 생성 (Core 1, 우선순위 10, 스택 12KB)
  BaseType_t res = xTaskCreatePinnedToCore(Task_HttpOta, "HttpOtaTask", 12288,
                                           s_ota_target_url, 10, nullptr, 1);

  if (res != pdPASS) {
    ::Serial.printf("[OTA] Failed to create HttpOtaTask: %d\r\n", res);
    snprintf(g_http_ota_state.status, sizeof(g_http_ota_state.status),
             "Failed");
    snprintf(g_http_ota_state.last_error, sizeof(g_http_ota_state.last_error),
             "Failed to spawn OTA task");
    return;
  }

  // 6. 태스크 생성 성공: Task_HttpOta로 생명주기 인계
  admission_guard.dismiss();
}

// ============================================================================
// From src/Management/Telemetry.cpp
// ============================================================================

static void serializeSysMetrics(AppendBuf &out, uint8_t c0, uint8_t c1,
                                int8_t temp_c, int8_t rssi, uint32_t uptime_s,
                                uint32_t free_heap_kb,
                                uint32_t min_free_heap_kb, bool ntp_synced) {
  out.appendFormat(
      "\"system\":{\"temp_c\":%d,\"wifi_rssi\":%d,\"uptime_s\":%u,"
      "\"cpu0_load\":%u,\"cpu1_load\":%u,\"free_heap_kb\":%u,\"min_free_heap_"
      "kb\":%u,"
      "\"ntp_synced\":%s,\"firmware\":\"%s\",\"latest_firmware\":\"Ready\"},",
      static_cast<int>(temp_c), static_cast<int>(rssi), uptime_s,
      static_cast<unsigned>(c0), static_cast<unsigned>(c1), free_heap_kb,
      min_free_heap_kb, ntp_synced ? "true" : "false",
      Config::FIRMWARE_VERSION);

  wifi_mode_t cur_wmode = WIFI_MODE_NULL;
  esp_wifi_get_mode(&cur_wmode);
  const char *mode_str =
      (cur_wmode == WIFI_MODE_AP)
          ? "AP"
          : (cur_wmode == WIFI_MODE_APSTA ? "AP_STA" : "STA");

  out.appendFormat(
      "\"wifi\":{\"ssid\":\"%s\",\"rssi\":%d,\"ip\":\"%s\",\"mode\":\"%s\"},",
      WiFi.status() == WL_CONNECTED ? WiFi.SSID().c_str() : "Disconnected",
      WiFi.status() == WL_CONNECTED ? static_cast<int>(WiFi.RSSI()) : -100,
      WiFi.localIP().toString().c_str(), mode_str);

  out.appendFormat("\"ota\":{\"in_progress\":%s,\"status\":\"%s\",\"progress_"
                   "pct\":%u,\"last_error\":\"%s\"},",
                   g_http_ota_state.in_progress.load() ? "true" : "false",
                   g_http_ota_state.status, g_http_ota_state.progress_pct,
                   g_http_ota_state.last_error);
}

static void serializeProfileAndTiming(
    AppendBuf &out, const VendorProfileDescriptor &active_prof,
    const AutoProbeDescriptor &auto_desc, const char *wc_src, size_t total_devs,
    size_t online_devs, size_t stale_devs, uint32_t ch2_rx,
    uint32_t ch2_uncached) {
  char cat_match_buf[64] = "None";
  const auto *matched_p = ProfileMatcher::getActiveProfile();
  if (matched_p) {
    snprintf(cat_match_buf, sizeof(cat_match_buf), "%s",
             matched_p->vendor_name);
  }

  char bp_buf[64];
  snprintf(bp_buf, sizeof(bp_buf), "%u Groups",
           static_cast<unsigned>(g_control_registry.getGroupCount()));

  bool fully_locked = (g_config.wallpad_profile != 0) ||
                      (auto_desc.is_locked && auto_desc.opcodes_locked &&
                       auto_desc.offsets_locked);

  out.appendFormat(
      "\"profile\":{\"active_slot\":%u,\"active_key\":\"%s\",\"active_name\":"
      "\"%s\","
      "\"is_locked\":%s,\"fully_locked\":%s,\"catalog_match\":\"%s\","
      "\"blueprints\":\"%s\","
      "\"stx\":\"0x%02X\",\"etx\":\"0x%02X\","
      "\"cs_algo\":\"%s\",\"opcodes\":{\"query\":\"0x%02X\",\"control\":\"0x%"
      "02X\",\"ack\":\"0x%02X\"},"
      "\"match_count\":%u},",
      static_cast<unsigned>(g_config.wallpad_profile), active_prof.key,
      active_prof.name, auto_desc.is_locked ? "true" : "false",
      fully_locked ? "true" : "false", cat_match_buf, bp_buf, auto_desc.stx,
      auto_desc.etx, AutoProbingEngine::getAlgoName(auto_desc.checksum_algo),
      auto_desc.query_opcode, auto_desc.control_opcode, auto_desc.ack_opcode,
      auto_desc.matched_packets);

  out.appendFormat("\"timing\":{\"ch1_poll_interval_ms\":%u,\"ch2_ack_delay_"
                   "ms\":%u,\"ch3_ack_delay_ms\":%u,"
                   "\"vip_preemptions\":%u,\"last_cmd_latency_ms\":%u},",
                   static_cast<unsigned>(g_timing_config.ch1_poll_interval_ms),
                   static_cast<unsigned>(g_timing_config.ch2_cache_delay_ms),
                   static_cast<unsigned>(g_timing_config.ch3_cache_delay_ms),
                   g_ch1_state_metrics.vip_cnt.load(std::memory_order_relaxed),
                   22);

  out.appendFormat(
      "\"cache\":{\"source\":\"%s\",\"total_devices\":%u,\"online_devices\":%u,"
      "\"stale_devices\":%u,\"cache_hit_rate\":%.1f},",
      wc_src, static_cast<unsigned>(total_devs),
      static_cast<unsigned>(online_devs), static_cast<unsigned>(stale_devs),
      (ch2_rx > 0
           ? (100.0f - (static_cast<float>(ch2_uncached) * 100.0f / ch2_rx))
           : 100.0f));

  const char *f1 = formatFramingStr(
      g_config.uart_data_bits, g_config.uart_parity, g_config.uart_stop_bits);
  const char *f2 = formatFramingStr(g_config.ch2_data_bits, g_config.ch2_parity,
                                    g_config.ch2_stop_bits);
  const char *f3 = formatFramingStr(g_config.ch3_data_bits, g_config.ch3_parity,
                                    g_config.ch3_stop_bits);
  const char *f4 =
      formatFramingStr(g_config.doorphone_data_bits, g_config.doorphone_parity,
                       g_config.doorphone_stop_bits);

  out.appendFormat("\"uart\":{"
                   "\"ch1\":{\"baud\":%u,\"format\":\"%s\"},"
                   "\"ch2\":{\"baud\":%u,\"format\":\"%s\"},"
                   "\"ch3\":{\"baud\":%u,\"format\":\"%s\"},"
                   "\"ch4\":{\"baud\":%u,\"format\":\"%s\"}},",
                   static_cast<unsigned>(g_config.uart_baud_rate), f1,
                   static_cast<unsigned>(g_config.ch2_baud_rate), f2,
                   static_cast<unsigned>(g_config.ch3_baud_rate), f3,
                   static_cast<unsigned>(g_doorphone_serial.baudRate() > 0
                                             ? g_doorphone_serial.baudRate()
                                             : g_config.doorphone_baud_rate),
                   f4);
}

static void serializeDiagnostics(AppendBuf &out, const char *rst_reason) {
  out.append("\"diagnostics\":{");
  out.appendFormat("\"last_reboot_reason\":\"%s\",\"rollback_detected\":%s,"
                   "\"rescue_mode\":%s,",
                   rst_reason, g_rollback_detected ? "true" : "false",
                   g_rescue_mode.load(std::memory_order_relaxed) ? "true"
                                                                 : "false");

  out.append("\"coredump\":{");
  if (g_coredump_info.valid) {
    out.appendFormat("\"valid\":true,\"task\":\"%s\",\"pc\":\"0x%08X\","
                     "\"cause\":%u,\"bt_depth\":%u,"
                     "\"summary\":\"⚠️ Crash in %s at 0x%08X (Cause %u)\"},",
                     g_coredump_info.task_name, g_coredump_info.exc_pc,
                     g_coredump_info.exc_cause, g_coredump_info.bt_depth,
                     g_coredump_info.task_name, g_coredump_info.exc_pc,
                     g_coredump_info.exc_cause);
  } else {
    out.append("\"valid\":false,\"task\":\"\",\"pc\":\"0x00000000\",\"cause\":"
               "0,\"bt_depth\":0,"
               "\"summary\":\"No Crash Dump (Flash Clean)\"},");
  }

  out.append("\"reboot_logs\":[");
  size_t log_cnt = LogManager::getLogCount();
  size_t max_logs_to_emit = (log_cnt > 5) ? 5 : log_cnt;
  for (size_t i = 0; i < max_logs_to_emit; i++) {
    LogEntry e{};
    if (LogManager::getLogEntry(i, e)) {
      char time_buf[32] = "N/A";
      if (e.timestamp > 0) {
        struct tm timeinfo;
        time_t sec = static_cast<time_t>(e.timestamp);
        localtime_r(&sec, &timeinfo);
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
      }
      uint32_t up_s = e.stats_snapshot.uptime_ms / 1000;
      char up_str[24];
      snprintf(up_str, sizeof(up_str), "%02uh %02um", up_s / 3600,
               (up_s % 3600) / 60);

      if (i > 0)
        out.append(",");
      out.appendFormat(
          "{\"id\":%u,\"time\":\"%s\",\"reason\":\"%s\",\"uptime\":\"%s\"}",
          static_cast<unsigned>(i + 1), time_buf, e.reason, up_str);
    }
  }
  out.append("],");

  bool f_bell = g_doorphone_state.front_bell.load(std::memory_order_relaxed);
  bool l_bell = g_doorphone_state.lobby_bell.load(std::memory_order_relaxed);
  uint32_t b_ms =
      g_doorphone_state.last_bell_ms.load(std::memory_order_relaxed);
  out.appendFormat(
      "\"doorphone\":{\"front_bell\":%s,\"lobby_bell\":%s,\"last_bell_ms\":%u}",
      f_bell ? "true" : "false", l_bell ? "true" : "false",
      static_cast<unsigned>(b_ms));

  out.append("}}");
}

void Mgmt_SerializeTelemetry(AppendBuf &out, long req_id) {
  uint8_t c0 = 0, c1 = 0;
  System_ReadCpuPct(c0, c1);
  int8_t temp_c = System_ReadTempC();
  int8_t rssi = WiFi.isConnected() ? WiFi.RSSI() : 0;
  uint32_t uptime_s = millis() / 1000;
  uint32_t free_heap_kb = heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024;
  uint32_t min_free_heap_kb =
      heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT) / 1024;
  bool ntp_synced = (time(nullptr) > 1672531200);

  VendorProfileDescriptor active_prof{};
  ProfileRepository::getActiveProfile(active_prof);
  auto auto_desc = g_auto_probing_engine.getDescriptor();

  const char *wc_src =
      (g_warm_cache_source == 1)
          ? "RTC_SRAM"
          : (g_warm_cache_source == 2 ? "NVS_FLASH" : "COLD_BOOT");
  size_t total_devs = g_device_repo.count();
  size_t online_devs = g_device_repo.getOnlineCount();
  size_t stale_devs =
      (total_devs >= online_devs) ? (total_devs - online_devs) : 0;

  uint32_t ch1_rx = g_pkt_stats.ch1.rx_pkts.load(std::memory_order_relaxed);
  uint32_t ch1_tx = g_pkt_stats.ch1.tx_pkts.load(std::memory_order_relaxed);
  uint32_t ch1_crc = g_pkt_stats.ch1.crc_errors.load(std::memory_order_relaxed);
  uint32_t ch1_tout = g_pkt_stats.ch1.timeouts.load(std::memory_order_relaxed);

  uint32_t ch2_rx = g_pkt_stats.ch2.rx_pkts.load(std::memory_order_relaxed);
  uint32_t ch2_tx = g_pkt_stats.ch2.tx_pkts.load(std::memory_order_relaxed);
  uint32_t ch2_uncached =
      g_pkt_stats.ch2.uncached_pkts.load(std::memory_order_relaxed);

  uint32_t ch3_rx = g_pkt_stats.ch3.rx_pkts.load(std::memory_order_relaxed);
  uint32_t ch3_tx = g_pkt_stats.ch3.tx_pkts.load(std::memory_order_relaxed);
  uint32_t ch3_uncached =
      g_pkt_stats.ch3.uncached_pkts.load(std::memory_order_relaxed);

  uint32_t ch4_rx = g_pkt_stats.ch4.rx_pkts.load(std::memory_order_relaxed);
  uint32_t ch4_tx = g_pkt_stats.ch4.tx_pkts.load(std::memory_order_relaxed);
  uint32_t ch4_inv =
      g_pkt_stats.ch4.invalid_frames.load(std::memory_order_relaxed);

  uint32_t ch5_rx = g_pkt_stats.ch5.rx_pkts.load(std::memory_order_relaxed);
  uint32_t ch5_tx = g_pkt_stats.ch5.tx_pkts.load(std::memory_order_relaxed);
  uint32_t ch5_drp =
      g_pkt_stats.ch5.dropped_pkts.load(std::memory_order_relaxed);

  uint32_t ch6_rx = g_pkt_stats.ch6.rx_pkts.load(std::memory_order_relaxed);
  uint32_t ch6_tx = g_pkt_stats.ch6.tx_pkts.load(std::memory_order_relaxed);

  float crc_rate =
      (ch1_rx > 0)
          ? (static_cast<float>(ch1_crc) * 100.0f / static_cast<float>(ch1_rx))
          : 0.0f;

  const char *rst_reason = "Normal Boot";
  esp_reset_reason_t rr = esp_reset_reason();
  switch (rr) {
  case ESP_RST_POWERON:
    rst_reason = "Power-On Reset";
    break;
  case ESP_RST_EXT:
    rst_reason = "Hardware Reset Pin (EXT)";
    break;
  case ESP_RST_PANIC:
    rst_reason = "CPU Panic / Crash Exception";
    break;
  case ESP_RST_TASK_WDT:
    rst_reason = "Task Watchdog Reset";
    break;
  case ESP_RST_BROWNOUT:
    rst_reason = "HW: Brownout (Low Voltage)";
    break;
  case ESP_RST_SW:
    rst_reason = "Software Restart";
    break;
  default:
    rst_reason = "Other Reset";
    break;
  }

  if (req_id != -1) {
    out.appendFormat("{\"id\":%ld,\"res\":\"ok\",", req_id);
  } else {
    out.append("{\"res\":\"ok\",");
  }

  serializeSysMetrics(out, c0, c1, temp_c, rssi, uptime_s, free_heap_kb,
                      min_free_heap_kb, ntp_synced);
  serializeProfileAndTiming(out, active_prof, auto_desc, wc_src, total_devs,
                            online_devs, stale_devs, ch2_rx, ch2_uncached);

  out.appendFormat("\"channels\":{\"ch1\":{\"rx\":%u,\"tx\":%u,\"crc_err\":%u,"
                   "\"timeout\":%u,\"crc_rate\":%.2f},"
                   "\"ch2\":{\"rx\":%u,\"tx\":%u,\"uncached\":%u},"
                   "\"ch3\":{\"rx\":%u,\"tx\":%u,\"uncached\":%u},"
                   "\"ch4\":{\"rx\":%u,\"tx\":%u,\"inv\":%u},"
                   "\"ch5\":{\"rx\":%u,\"tx\":%u,\"dropped\":%u},"
                   "\"ch6\":{\"rx\":%u,\"tx\":%u}},",
                   ch1_rx, ch1_tx, ch1_crc, ch1_tout, crc_rate, ch2_rx, ch2_tx,
                   ch2_uncached, ch3_rx, ch3_tx, ch3_uncached, ch4_rx, ch4_tx,
                   ch4_inv, ch5_rx, ch5_tx, ch5_drp, ch6_rx, ch6_tx);

  serializeDiagnostics(out, rst_reason);
}

void Mgmt_SerializeDevices(AppendBuf &out, long req_id) {
  if (req_id != -1) {
    out.appendFormat("{\"id\":%ld,\"res\":\"ok\",\"devices\":[", req_id);
  } else {
    out.append("{\"res\":\"ok\",\"devices\":[");
  }
  size_t count = g_device_repo.count();
  size_t locked_count = 0;

  for (size_t i = 0; i < count; ++i) {
    DeviceStateEntry snap{};
    if (!g_device_repo.getSnapshot(i, snap) || snap.dev_id == 0)
      continue;

    GroupControlTemplate grp{};
    if (!g_control_registry.findGroup(snap.dev_id, grp))
      continue;

    StaticPacket ack{};
    ack.length = snap.last_ack_len;
    memcpy(ack.data.data(), snap.last_ack_data.data(),
           std::min<size_t>(snap.last_ack_len, 32));

    DecodedDeviceState st{};
    DeviceRepository::decodeDeviceState(grp, ack, &snap, st);

    DeviceClass dc = st.dev_class;
    const char *cls_str = DeviceClassToTelemetryString(dc);
    const char *grp_name = grp.group_name;
    bool is_outlet = (dc == DeviceClass::OUTLET);

    char name_buf[32];
    if (dc == DeviceClass::GAS || dc == DeviceClass::VENT ||
        dc == DeviceClass::MOMENTARY) {
      snprintf(name_buf, sizeof(name_buf), "%s", grp_name);
    } else {
      snprintf(name_buf, sizeof(name_buf), "%s %u-%u", grp_name, snap.sub1,
               snap.sub2);
    }

    RouteEndpoint ep{1, -1, 0};
    uint8_t ch = 1;
    if (g_route_registry.lookupRoute(snap.dev_id, snap.sub1, snap.sub2, ep)) {
      ch = ep.channel_id;
    }

    if (locked_count > 0)
      out.append(",");
    out.appendFormat("{\"dev_id\":%u,\"sub1\":%u,\"sub2\":%u,\"class\":\"%s\","
                     "\"name\":\"%s\",\"channel\":%u,\"power\":%d",
                     snap.dev_id, snap.sub1, snap.sub2, cls_str, name_buf, ch,
                     st.power);

    if (dc == DeviceClass::THERMOSTAT) {
      out.appendFormat(",\"target_temp\":%d,\"current_temp\":%d",
                       st.target_temp, st.current_temp);
    } else if (dc == DeviceClass::VENT) {
      out.appendFormat(",\"fan_speed\":%d,\"vent_mode\":%d", st.fan_speed,
                       st.vent_mode);
    } else if (dc == DeviceClass::GAS) {
      out.appendFormat(",\"valve\":\"%s\"", st.valve_state);
    } else if (is_outlet) {
      out.appendFormat(",\"power_w\":%.1f", st.power_w);
    } else if (dc == DeviceClass::MOMENTARY) {
      out.appendFormat(",\"floor\":%d,\"direction\":%d", st.floor,
                       st.direction);
    }
    out.append("}");
    locked_count++;
  }

  // ── CH5 FCU 슬롯(1~4) 활성 기기 직렬화 (SmartThings get_devices 자식 기기
  // 목록 추가) ──
  {
    MutexLocker lock(g_ch5_mutex);
    for (uint8_t s = 1; s < Config::TCP::MAX_EW11_SLOTS; ++s) {
      const auto &slot = g_hub_slots[s];
      const DeviceStateEntry *fcu_dev =
          g_device_repo.find(Config::FCU::DEV_ID, s, 0);

      // 소켓 설정이 활성화되어 있거나 수신 이력이 있는 경우 노출
      if (slot.enabled || (fcu_dev && fcu_dev->last_ack_len > 0)) {
        if (locked_count > 0)
          out.append(",");
        char name_buf[32];
        snprintf(name_buf, sizeof(name_buf), "%s",
                 slot.name[0] ? slot.name : "Air Conditioner");

        int pwr = (fcu_dev && fcu_dev->last_ack_len >= 9 &&
                   fcu_dev->last_ack_data[8] != 0)
                      ? 1
                      : 0;
        int mode = (fcu_dev && fcu_dev->last_ack_len >= 7)
                       ? fcu_dev->last_ack_data[6]
                       : 1;
        int fan = (fcu_dev && fcu_dev->last_ack_len >= 9)
                      ? fcu_dev->last_ack_data[8]
                      : 4;
        int swg = (fcu_dev && fcu_dev->last_ack_len >= 11)
                      ? fcu_dev->last_ack_data[10]
                      : 0;
        int tgt = (fcu_dev && fcu_dev->last_target_temp > 0)
                      ? fcu_dev->last_target_temp
                      : 24;
        int cur = (fcu_dev && fcu_dev->last_current_temp > 0)
                      ? fcu_dev->last_current_temp
                      : tgt;

        out.appendFormat(
            "{\"dev_id\":%u,\"sub1\":%u,\"sub2\":0,\"class\":\"fcu\",\"name\":"
            "\"%s\",\"channel\":5,\"power\":%d,\"mode\":%d,\"fan_speed\":%d,"
            "\"swing\":%d,\"target_temp\":%d,\"current_temp\":%d}",
            Config::FCU::DEV_ID, s, name_buf, pwr, mode, fan, swg, tgt, cur);
        locked_count++;
      }
    }
  }

  out.appendFormat("],\"count\":%u}\n", static_cast<unsigned>(locked_count));
}
void Mgmt_BroadcastDoorphoneEvent(bool front_bell, bool lobby_bell) {
  char buf[128];
  int len = snprintf(
      buf, sizeof(buf),
      "{\"event\":\"doorphone\",\"front_bell\":%s,\"lobby_bell\":%s}\n",
      front_bell ? "true" : "false", lobby_bell ? "true" : "false");
  if (len <= 0 || !g_mgmt_mutex)
    return;

  MutexLocker lock(g_mgmt_mutex);
  for (int i = 0; i < Config::TCP::MAX_MGMT_CLIENTS; i++) {
    if (g_mgmt_sessions[i].sock >= 0) {
      send(g_mgmt_sessions[i].sock, buf, len, MSG_DONTWAIT);
      g_pkt_stats.ch6.tx_pkts.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

void Mgmt_BroadcastDeviceState(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                               DeviceClass dev_class, int power,
                               int target_temp, int current_temp, int speed,
                               const char *valve_state, float power_w,
                               int floor, int direction, int ho,
                               int vent_mode) {
  char buf[256];
  int len = 0;
  switch (dev_class) {
  case DeviceClass::THERMOSTAT:
    len = snprintf(buf, sizeof(buf),
                   "{\"event\":\"device_state\",\"dev_id\":%u,\"sub1\":%u,"
                   "\"sub2\":%u,\"class\":\"thermostat\",\"power\":%d,\"target_"
                   "temp\":%d,\"current_temp\":%d}\n",
                   dev_id, sub1, sub2, power, target_temp, current_temp);
    break;
  case DeviceClass::VENT:
    len = snprintf(
        buf, sizeof(buf),
        "{\"event\":\"device_state\",\"dev_id\":%u,\"sub1\":%u,\"sub2\":%u,"
        "\"class\":\"vent\",\"power\":%d,\"fan_speed\":%d,\"vent_mode\":%d}\n",
        dev_id, sub1, sub2, power, speed, vent_mode);
    break;
  case DeviceClass::GAS:
    len = snprintf(buf, sizeof(buf),
                   "{\"event\":\"device_state\",\"dev_id\":%u,\"sub1\":%u,"
                   "\"sub2\":%u,\"class\":\"gas\",\"valve\":\"%s\"}\n",
                   dev_id, sub1, sub2, valve_state ? valve_state : "closed");
    break;
  case DeviceClass::OUTLET:
    len = snprintf(
        buf, sizeof(buf),
        "{\"event\":\"device_state\",\"dev_id\":%u,\"sub1\":%u,\"sub2\":%u,"
        "\"class\":\"outlet\",\"power\":%d,\"power_w\":%.1f}\n",
        dev_id, sub1, sub2, power, power_w);
    break;
  case DeviceClass::MOMENTARY:
    len = snprintf(buf, sizeof(buf),
                   "{\"event\":\"device_state\",\"dev_id\":%u,\"sub1\":%u,"
                   "\"sub2\":%u,\"class\":\"momentary\",\"power\":%d,\"floor\":"
                   "%d,\"direction\":%d,\"ho\":%d}\n",
                   dev_id, sub1, sub2, power, floor, direction, ho);
    break;
  default:
    len = snprintf(buf, sizeof(buf),
                   "{\"event\":\"device_state\",\"dev_id\":%u,\"sub1\":%u,"
                   "\"sub2\":%u,\"class\":\"switch\",\"power\":%d}\n",
                   dev_id, sub1, sub2, power);
    break;
  }

  if (len <= 0 || !g_mgmt_mutex)
    return;

  MutexLocker lock(g_mgmt_mutex);
  for (int i = 0; i < Config::TCP::MAX_MGMT_CLIENTS; i++) {
    if (g_mgmt_sessions[i].sock >= 0) {
      send(g_mgmt_sessions[i].sock, buf, len, MSG_DONTWAIT);
      g_pkt_stats.ch6.tx_pkts.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

void Mgmt_BroadcastDevicesUpdated() {
  const char *msg = "{\"event\":\"devices_updated\"}\n";
  size_t len = strlen(msg);
  if (!g_mgmt_mutex)
    return;

  MutexLocker lock(g_mgmt_mutex);
  for (int i = 0; i < Config::TCP::MAX_MGMT_CLIENTS; i++) {
    if (g_mgmt_sessions[i].sock >= 0) {
      send(g_mgmt_sessions[i].sock, msg, len, MSG_DONTWAIT);
      g_pkt_stats.ch6.tx_pkts.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

void Mgmt_BroadcastRawJson(const char *json_payload) {
  if (!json_payload || !g_mgmt_mutex)
    return;

  char buf[256];
  int len = snprintf(buf, sizeof(buf), "%s\n", json_payload);
  if (len <= 0)
    return;

  MutexLocker lock(g_mgmt_mutex);
  for (int i = 0; i < Config::TCP::MAX_MGMT_CLIENTS; i++) {
    if (g_mgmt_sessions[i].sock >= 0) {
      send(g_mgmt_sessions[i].sock, buf, len, MSG_DONTWAIT);
      g_pkt_stats.ch6.tx_pkts.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

// ============================================================================
// From src/Management/MgmtRpc.cpp
// ============================================================================

static IPAddress s_trusted_hub_ip(0, 0, 0, 0);

static IPAddress get_client_ip(int sock) {
  struct sockaddr_in peer;
  socklen_t len = sizeof(peer);
  if (getpeername(sock, reinterpret_cast<struct sockaddr *>(&peer), &len) ==
      0) {
    const uint8_t *b = reinterpret_cast<const uint8_t *>(&peer.sin_addr.s_addr);
    return IPAddress(b[0], b[1], b[2], b[3]);
  }
  return IPAddress(0, 0, 0, 0);
}

RuntimeTimingConfig g_timing_config{};
MgmtSession g_mgmt_sessions[Config::TCP::MAX_MGMT_CLIENTS];
SemaphoreHandle_t g_mgmt_mutex = nullptr;

void TimingConfig_Load() {
  Preferences p;
  if (p.begin("timing_cfg", true)) {
    g_timing_config.ch1_poll_interval_ms = p.getUShort("ch1_poll", 1000);
    g_timing_config.ch2_cache_delay_ms = p.getUShort("ch2_del", 30);
    g_timing_config.ch3_cache_delay_ms = p.getUShort("ch3_del", 240);
    p.end();
  } else {
    g_timing_config.ch1_poll_interval_ms = 1000;
    g_timing_config.ch2_cache_delay_ms = 30;
    g_timing_config.ch3_cache_delay_ms = 240;
  }

  if (g_timing_config.ch1_poll_interval_ms < 200 ||
      g_timing_config.ch1_poll_interval_ms > 5000)
    g_timing_config.ch1_poll_interval_ms = 1000;
  if (g_timing_config.ch2_cache_delay_ms < 5 ||
      g_timing_config.ch2_cache_delay_ms > 300)
    g_timing_config.ch2_cache_delay_ms = 30;
  if (g_timing_config.ch3_cache_delay_ms < 20 ||
      g_timing_config.ch3_cache_delay_ms > 1000)
    g_timing_config.ch3_cache_delay_ms = 240;

  ::Serial.printf(
      "[TIMING] Loaded: CH1 Poll %u ms, CH2 Delay %u ms, CH3 Delay %u ms\r\n",
      g_timing_config.ch1_poll_interval_ms, g_timing_config.ch2_cache_delay_ms,
      g_timing_config.ch3_cache_delay_ms);
}

void TimingConfig_Save() {
  Preferences p;
  if (p.begin("timing_cfg", false)) {
    p.putUShort("ch1_poll", g_timing_config.ch1_poll_interval_ms);
    p.putUShort("ch2_del", g_timing_config.ch2_cache_delay_ms);
    p.putUShort("ch3_del", g_timing_config.ch3_cache_delay_ms);
    p.end();
    ::Serial.printf("[TIMING] Saved to NVS: CH1 Poll %u ms, CH2 Delay %u ms, "
                    "CH3 Delay %u ms\r\n",
                    g_timing_config.ch1_poll_interval_ms,
                    g_timing_config.ch2_cache_delay_ms,
                    g_timing_config.ch3_cache_delay_ms);
  }
}

namespace {

struct DoorphoneFsm {
  enum class Step : uint8_t { IDLE = 0, CALL_SENT, OPEN_SENT };
  std::atomic<Step> step{Step::IDLE};
  std::atomic<uint8_t> c_stx{0x7F};
  std::atomic<uint8_t> c_etx{0xEE};
  std::atomic<uint8_t> op_open{0};
  std::atomic<uint8_t> op_end{0};
  esp_timer_handle_t timer{nullptr};
};

static DoorphoneFsm s_dp_fsm;

static void sendDpPacket(uint8_t stx, uint8_t op, uint8_t etx) {
  StaticPacket pkt{4, 5};
  pkt.data[0] = stx;
  pkt.data[1] = op;
  pkt.data[2] = 0x00;
  pkt.data[3] = 0x00;
  pkt.data[4] = etx;
  if (g_ch4_passthrough_queue) {
    xQueueSend(g_ch4_passthrough_queue, &pkt, 0);
  }
}

static void onDoorphoneTimer(void *arg) {
  (void)arg;
  DoorphoneFsm::Step cur = s_dp_fsm.step.load(std::memory_order_acquire);
  if (cur == DoorphoneFsm::Step::CALL_SENT) {
    sendDpPacket(s_dp_fsm.c_stx.load(std::memory_order_acquire),
                 s_dp_fsm.op_open.load(std::memory_order_acquire),
                 s_dp_fsm.c_etx.load(std::memory_order_acquire));
    s_dp_fsm.step.store(DoorphoneFsm::Step::OPEN_SENT,
                        std::memory_order_release);
    esp_timer_start_once(s_dp_fsm.timer, 750000); // 750ms 후 종료 패킷 전송
  } else if (cur == DoorphoneFsm::Step::OPEN_SENT) {
    sendDpPacket(s_dp_fsm.c_stx.load(std::memory_order_acquire),
                 s_dp_fsm.op_end.load(std::memory_order_acquire),
                 s_dp_fsm.c_etx.load(std::memory_order_acquire));
    g_doorphone_state.front_bell.store(false, std::memory_order_release);
    g_doorphone_state.lobby_bell.store(false, std::memory_order_release);
    s_dp_fsm.step.store(DoorphoneFsm::Step::IDLE, std::memory_order_release);
  }
}

} // anonymous namespace

void Mgmt_Init() {
  if (!g_mgmt_mutex) {
    g_mgmt_mutex = xSemaphoreCreateMutex();
  }
  for (size_t i = 0; i < Config::TCP::MAX_MGMT_CLIENTS; i++) {
    g_mgmt_sessions[i].sock = -1;
    g_mgmt_sessions[i].len = 0;
    g_mgmt_sessions[i].connected_at_ms = 0;
  }
  if (!s_dp_fsm.timer) {
    esp_timer_create_args_t timer_args{};
    timer_args.callback = onDoorphoneTimer;
    timer_args.name = "dp_fsm_timer";
    esp_timer_create(&timer_args, &s_dp_fsm.timer);
  }
  TimingConfig_Load();
}

static inline const char *findJsonStringValue(const char *json, const char *key,
                                              char *out_val, size_t max_len) {
  if (!json || !key || !out_val || max_len == 0)
    return nullptr;
  char pattern[64];
  snprintf(pattern, sizeof(pattern), "\"%s\"", key);
  const char *p = strstr(json, pattern);
  if (!p)
    return nullptr;
  p += strlen(pattern);
  while (*p == ' ' || *p == ':' || *p == '\t')
    p++;
  if (*p != '\"')
    return nullptr; // 문자열 시작 따옴표 필수 확인
  p++;
  size_t idx = 0;
  while (*p && *p != '\"' && *p != '\r' && *p != '\n' && idx + 1 < max_len) {
    out_val[idx++] = *p++;
  }
  out_val[idx] = '\0';
  return out_val;
}

static inline long findJsonIntValue(const char *json, const char *key,
                                    long default_val = -1) {
  if (!json || !key)
    return default_val;
  char pattern[64];
  snprintf(pattern, sizeof(pattern), "\"%s\"", key);
  const char *p = strstr(json, pattern);
  if (!p)
    return default_val;
  p += strlen(pattern);
  while (*p == ' ' || *p == ':' || *p == '\t')
    p++;
  char *endp = nullptr;
  long val = strtol(p, &endp, 10);
  if (endp == p)
    return default_val;
  return val;
}

static void sendRpcResponse(int sock, long req_id, const char *res,
                            const char *msg = nullptr) {
  if (sock < 0)
    return;
  char buf[384];
  int len = 0;
  if (req_id >= 0) {
    if (msg) {
      len = snprintf(buf, sizeof(buf),
                     "{\"res\":\"%s\",\"msg\":\"%s\",\"id\":%ld}\n", res, msg,
                     req_id);
    } else {
      len = snprintf(buf, sizeof(buf), "{\"res\":\"%s\",\"id\":%ld}\n", res,
                     req_id);
    }
  } else {
    if (msg) {
      len = snprintf(buf, sizeof(buf), "{\"res\":\"%s\",\"msg\":\"%s\"}\n", res,
                     msg);
    } else {
      len = snprintf(buf, sizeof(buf), "{\"res\":\"%s\"}\n", res);
    }
  }
  if (len > 0 && static_cast<size_t>(len) < sizeof(buf)) {
    send(sock, buf, len, MSG_DONTWAIT);
  }
}

// Task_Network 단일 태스크 동기 전송 환경: 직렬 응답 버퍼 단일화 (-3,072B)
static char s_mgmt_resp_buf[4096];

// ── Individual RPC Command Handlers ──

static void HandleRpc_GetTelemetry(int sock, long req_id,
                                   const char * /*json_str*/,
                                   const IPAddress & /*client_ip*/) {
  s_mgmt_resp_buf[0] = '\0';
  AppendBuf ab{s_mgmt_resp_buf, sizeof(s_mgmt_resp_buf)};
  Mgmt_SerializeTelemetry(ab, req_id);
  ab.append("\n");
  send(sock, ab.buf, ab.offset, MSG_DONTWAIT);
  g_pkt_stats.ch6.tx_pkts.fetch_add(1, std::memory_order_relaxed);
}

static void HandleRpc_SetProfile(int sock, long req_id, const char *json_str,
                                 const IPAddress & /*client_ip*/) {
  long slot = findJsonIntValue(json_str, "slot", -1);
  if (slot >= 0 && slot < static_cast<long>(ProfileRepository::MAX_PROFILES)) {
    ProfileRepository::setActiveProfileIndex(static_cast<size_t>(slot));
    {
      std::unique_lock lock(g_config_rw);
      g_config.wallpad_profile = static_cast<uint8_t>(slot);
    }
    Config_Save();
    sendRpcResponse(sock, req_id, "ok", "Profile updated");
  } else {
    sendRpcResponse(sock, req_id, "error", "Invalid profile slot (0~3)");
  }
}

static void HandleRpc_SaveAutoToSlot(int sock, long req_id,
                                     const char *json_str,
                                     const IPAddress & /*client_ip*/) {
  char name_buf[36] = {0};
  findJsonStringValue(json_str, "name", name_buf, sizeof(name_buf));
  if (strlen(name_buf) == 0)
    strncpy(name_buf, "Saved Custom", sizeof(name_buf) - 1);

  size_t saved_slot = 1;
  if (ProfileRepository::saveCurrentAutoAs(name_buf, saved_slot)) {
    char resp[96];
    if (req_id >= 0) {
      snprintf(resp, sizeof(resp),
               "{\"res\":\"ok\",\"saved_slot\":%u,\"msg\":\"Auto profile "
               "saved\",\"id\":%ld}\n",
               static_cast<unsigned>(saved_slot), req_id);
    } else {
      snprintf(
          resp, sizeof(resp),
          "{\"res\":\"ok\",\"saved_slot\":%u,\"msg\":\"Auto profile saved\"}\n",
          static_cast<unsigned>(saved_slot));
    }
    send(sock, resp, strlen(resp), MSG_DONTWAIT);
  } else {
    sendRpcResponse(
        sock, req_id, "error",
        "Failed to save auto profile (Auto not locked or slots full)");
  }
}

static void HandleRpc_SetTiming(int sock, long req_id, const char *json_str,
                                const IPAddress & /*client_ip*/) {
  long ch1_poll = findJsonIntValue(json_str, "ch1_poll_intvl", -1);
  long ch2_del = findJsonIntValue(json_str, "ch2_delay", -1);
  long ch3_del = findJsonIntValue(json_str, "ch3_delay", -1);

  bool updated = false;
  if (ch1_poll >= 50 && ch1_poll <= 5000) {
    g_timing_config.ch1_poll_interval_ms = static_cast<uint16_t>(ch1_poll);
    updated = true;
  }
  if (ch2_del >= 0 && ch2_del <= 500) {
    g_timing_config.ch2_cache_delay_ms = static_cast<uint16_t>(ch2_del);
    updated = true;
  }
  if (ch3_del >= 0 && ch3_del <= 1000) {
    g_timing_config.ch3_cache_delay_ms = static_cast<uint16_t>(ch3_del);
    updated = true;
  }

  if (updated) {
    TimingConfig_Save();
    sendRpcResponse(sock, req_id, "ok", "Timing config updated & saved to NVS");
  } else {
    sendRpcResponse(sock, req_id, "error",
                    "No valid timing parameters provided");
  }
}

static void HandleRpc_CacheSync(int sock, long req_id,
                                const char * /*json_str*/,
                                const IPAddress & /*client_ip*/) {
  Cache_SaveToNvs();
  sendRpcResponse(sock, req_id, "ok", "Warm cache synced to NVS");
}

static void HandleRpc_Ping(int sock, long req_id, const char * /*json_str*/,
                           const IPAddress & /*client_ip*/) {
  char pong_msg[64];
  if (req_id >= 0) {
    snprintf(pong_msg, sizeof(pong_msg),
             "{\"res\":\"ok\",\"pong\":true,\"id\":%ld}\n", req_id);
  } else {
    snprintf(pong_msg, sizeof(pong_msg), "{\"res\":\"ok\",\"pong\":true}\n");
  }
  send(sock, pong_msg, strlen(pong_msg), MSG_DONTWAIT);
}

static void HandleRpc_CachePurgeRescan(int sock, long req_id,
                                       const char * /*json_str*/,
                                       const IPAddress & /*client_ip*/) {
  char dp_ns[16];
  Config::Doorphone::FramingTracker::getNvsNamespace(g_config.wallpad_profile,
                                                     dp_ns, sizeof(dp_ns));
  g_auto_probing_engine.reset();
  g_doorphone_tracker.clearNvs(dp_ns);
  g_polling_targets.clear();
  g_device_repo.clear();
  g_probe_convergence_reset.store(true, std::memory_order_release);
  sendRpcResponse(sock, req_id, "ok",
                  "Auto-probing reset and cache purged, bus rescan triggered");
}

static void HandleRpc_WallpadReset(int sock, long req_id,
                                   const char * /*json_str*/,
                                   const IPAddress & /*client_ip*/) {
  char dp_ns[16];
  Config::Doorphone::FramingTracker::getNvsNamespace(g_config.wallpad_profile,
                                                     dp_ns, sizeof(dp_ns));
  g_auto_probing_engine.reset();
  g_doorphone_tracker.clearNvs(dp_ns);
  g_polling_targets.clear();
  g_device_repo.clear();
  g_probe_convergence_reset.store(true, std::memory_order_release);
  sendRpcResponse(sock, req_id, "ok",
                  "Wallpad auto-probing and framing reset completed");
}

static void HandleRpc_ClearCoredump(int sock, long req_id,
                                    const char * /*json_str*/,
                                    const IPAddress & /*client_ip*/) {
  esp_core_dump_image_erase();
  g_coredump_info.valid = false;
  memset(&g_coredump_info, 0, sizeof(g_coredump_info));
  sendRpcResponse(sock, req_id, "ok", "Flash core dump erased");
}

static void HandleRpc_ClearRebootLogs(int sock, long req_id,
                                      const char * /*json_str*/,
                                      const IPAddress & /*client_ip*/) {
  LogManager::clearRebootLog();
  sendRpcResponse(sock, req_id, "ok", "Reboot logs cleared from NVS");
}

static void HandleRpc_WifiScan(int sock, long req_id, const char * /*json_str*/,
                               const IPAddress & /*client_ip*/) {
  int n = WiFi.scanNetworks(false, true);
  if (n <= 0) {
    WiFi.scanDelete();
    char no_ap_msg[128];
    if (req_id >= 0) {
      snprintf(no_ap_msg, sizeof(no_ap_msg),
               "{\"id\":%ld,\"res\":\"ok\",\"count\":0,\"ap_count\":0,\"aps\":["
               "],\"msg\":\"No Networks Found\"}\n",
               req_id);
    } else {
      snprintf(no_ap_msg, sizeof(no_ap_msg),
               "{\"res\":\"ok\",\"count\":0,\"ap_count\":0,\"aps\":[],\"msg\":"
               "\"No Networks Found\"}\n");
    }
    send(sock, no_ap_msg, strlen(no_ap_msg), MSG_DONTWAIT);
    return;
  }

  std::vector<int> indices(n);
  for (int i = 0; i < n; ++i)
    indices[i] = i;
  std::sort(indices.begin(), indices.end(),
            [](int a, int b) { return WiFi.RSSI(a) > WiFi.RSSI(b); });

  struct ApInfo {
    String ssid;
    int pct;
  };
  std::vector<ApInfo> top_aps;
  top_aps.reserve(4);

  for (int idx : indices) {
    String s = WiFi.SSID(idx);
    s.trim();
    if (s.length() == 0)
      continue;

    if (std::any_of(top_aps.begin(), top_aps.end(),
                    [&s](const auto &item) { return item.ssid == s; })) {
      continue;
    }

    int rssi = WiFi.RSSI(idx);
    int pct = std::min(100, std::max(0, 2 * (rssi + 100)));
    s.replace("\"", "\\\""); // JSON escape
    top_aps.push_back({s, pct});
    if (top_aps.size() >= 4)
      break;
  }
  WiFi.scanDelete();

  char resp[512];
  int offset = 0;
  if (req_id >= 0) {
    offset = snprintf(
        resp, sizeof(resp),
        "{\"id\":%ld,\"res\":\"ok\",\"count\":%d,\"ap_count\":%u,\"aps\":[",
        req_id, n, static_cast<unsigned>(top_aps.size()));
  } else {
    offset = snprintf(resp, sizeof(resp),
                      "{\"res\":\"ok\",\"count\":%d,\"ap_count\":%u,\"aps\":[",
                      n, static_cast<unsigned>(top_aps.size()));
  }
  for (size_t i = 0; i < top_aps.size(); ++i) {
    offset += snprintf(resp + offset, sizeof(resp) - offset,
                       "%s{\"ssid\":\"%s\",\"pct\":%d}", (i > 0 ? "," : ""),
                       top_aps[i].ssid.c_str(), top_aps[i].pct);
    if (offset >= (int)sizeof(resp) - 8)
      break;
  }
  snprintf(resp + offset, sizeof(resp) - offset, "]}\n");
  send(sock, resp, strlen(resp), MSG_DONTWAIT);
}

static void HandleRpc_StartOta(int sock, long req_id, const char *json_str,
                               const IPAddress & /*client_ip*/) {
  char url[256] = {0};
  findJsonStringValue(json_str, "url", url, sizeof(url));
  const char *target_url = url[0] ? url : DEFAULT_CLOUD_OTA_URL;
  if (strncmp(target_url, "https://", 8) != 0 || strlen(target_url) < 10) {
    sendRpcResponse(sock, req_id, "error",
                    "Invalid OTA URL: Only HTTPS allowed");
    return;
  }
  Mgmt_StartHttpOta(target_url);
  sendRpcResponse(sock, req_id, "ok", "Cloud HTTPS OTA started in background");
}

static void HandleRpc_SystemReboot(int sock, long req_id, const char *json_str,
                                   const IPAddress & /*client_ip*/) {
  char reason_buf[64] = {0};
  findJsonStringValue(json_str, "reason", reason_buf, sizeof(reason_buf));
  sendRpcResponse(sock, req_id, "ok");
  vTaskDelay(pdMS_TO_TICKS(100));
  System_Restart(reason_buf[0] ? reason_buf : "RPC Requested Reboot");
}

static void HandleRpc_SetWifiMode(int sock, long req_id, const char *json_str,
                                  const IPAddress & /*client_ip*/) {
  char mode_buf[16] = {0};
  if (findJsonStringValue(json_str, "mode", mode_buf, sizeof(mode_buf))) {
    if (strcasecmp(mode_buf, "AP") == 0) {
      WiFi.mode(WIFI_AP);
    } else if (strcasecmp(mode_buf, "AP_STA") == 0 ||
               strcasecmp(mode_buf, "AP+STA") == 0) {
      WiFi.mode(WIFI_AP_STA);
    } else {
      WiFi.mode(WIFI_STA);
    }
    sendRpcResponse(sock, req_id, "ok");
  } else {
    sendRpcResponse(sock, req_id, "error", "Missing mode parameter");
  }
}

static void HandleRpc_SetWifi(int sock, long req_id, const char *json_str,
                              const IPAddress & /*client_ip*/) {
  char new_ssid[64] = {0};
  char new_pass[64] = {0};
  bool has_ssid =
      findJsonStringValue(json_str, "ssid", new_ssid, sizeof(new_ssid));
  bool has_pass =
      findJsonStringValue(json_str, "password", new_pass, sizeof(new_pass));

  if (!has_ssid || strlen(new_ssid) == 0 || strlen(new_ssid) > 32) {
    sendRpcResponse(sock, req_id, "error", "Invalid SSID length (1-32 chars)");
    return;
  }

  auto has_bad_chars = [](const char *str) {
    for (size_t i = 0; str[i] != '\0'; i++) {
      unsigned char c = static_cast<unsigned char>(str[i]);
      if (c < 32 || c == 127 || c == '\r' || c == '\n')
        return true;
    }
    return false;
  };
  if (has_bad_chars(new_ssid) || (has_pass && has_bad_chars(new_pass))) {
    sendRpcResponse(sock, req_id, "error",
                    "SSID or password contains invalid control characters");
    return;
  }

  if (has_pass && strlen(new_pass) > 0 &&
      (strlen(new_pass) < 8 || strlen(new_pass) > 63)) {
    sendRpcResponse(sock, req_id, "error",
                    "Wi-Fi password must be between 8 and 63 characters (or "
                    "empty for open network)");
    return;
  }

  strncpy(g_wifi_guard.prev_ssid, g_config.wifi_ssid,
          sizeof(g_wifi_guard.prev_ssid) - 1);
  strncpy(g_wifi_guard.prev_pass, g_config.wifi_password,
          sizeof(g_wifi_guard.prev_pass) - 1);
  g_wifi_guard.start_ms = millis();
  g_wifi_guard.testing.store(true, std::memory_order_release);

  strncpy(g_config.wifi_ssid, new_ssid, sizeof(g_config.wifi_ssid) - 1);
  strncpy(g_config.wifi_password, new_pass, sizeof(g_config.wifi_password) - 1);

  sendRpcResponse(sock, req_id, "ok");
  vTaskDelay(pdMS_TO_TICKS(50));

  WiFi.disconnect(false);
  vTaskDelay(pdMS_TO_TICKS(50));
  WiFi.begin(g_config.wifi_ssid, g_config.wifi_password);
}

static void HandleRpc_SetUart(int sock, long req_id, const char *json_str,
                              const IPAddress & /*client_ip*/) {
  long ch = findJsonIntValue(json_str, "ch", 0);
  long baud = findJsonIntValue(json_str, "baud", 0);
  char format[16] = {0};
  findJsonStringValue(json_str, "format", format, sizeof(format));

  if (ch >= 1 && ch <= 4 && baud >= 1200 && baud <= 921600 && format[0]) {
    if (System_ApplyUartConfig(static_cast<uint8_t>(ch),
                               static_cast<uint32_t>(baud), format)) {
      sendRpcResponse(sock, req_id, "ok");
      return;
    }
  }
  sendRpcResponse(
      sock, req_id, "error",
      "Invalid ch (1-4), baud (1200-921600), or format (8N1/8E1/8O1/8N2)");
}

static void HandleRpc_DoorphoneAction(int sock, long req_id,
                                      const char *json_str,
                                      const IPAddress & /*client_ip*/) {
  char action_buf[32] = {0};
  findJsonStringValue(json_str, "action", action_buf, sizeof(action_buf));

  bool is_open_front = (strcasecmp(action_buf, "open_front") == 0 ||
                        strcasecmp(action_buf, "open") == 0);
  bool is_open_lobby = (strcasecmp(action_buf, "open_lobby") == 0);

  if (is_open_front || is_open_lobby) {
    uint8_t dp_stx =
        g_doorphone_tracker.candidate_stx.load(std::memory_order_relaxed);
    uint8_t dp_etx =
        g_doorphone_tracker.candidate_etx.load(std::memory_order_relaxed);
    uint8_t dp_len =
        g_doorphone_tracker.candidate_len.load(std::memory_order_relaxed);

    if (dp_stx == 0)
      dp_stx = 0x7F;
    if (dp_etx == 0)
      dp_etx = 0xEE;

    const DoorphoneSpec *dp_prof =
        ProfileMatcher::matchDoorphone(dp_stx, dp_etx, dp_len);

    uint8_t op_call = is_open_front ? (dp_prof ? dp_prof->call_front : 0xB9)
                                    : (dp_prof ? dp_prof->call_lobby : 0x5F);
    uint8_t op_open = is_open_front ? (dp_prof ? dp_prof->open_front : 0xB4)
                                    : (dp_prof ? dp_prof->open_lobby : 0x61);
    uint8_t op_end = is_open_front ? (dp_prof ? dp_prof->end_front : 0xB8)
                                   : (dp_prof ? dp_prof->end_lobby : 0x60);

    DoorphoneFsm::Step expected = DoorphoneFsm::Step::IDLE;
    if (!s_dp_fsm.step.compare_exchange_strong(expected,
                                               DoorphoneFsm::Step::CALL_SENT)) {
      sendRpcResponse(sock, req_id, "busy",
                      "Doorphone sequence already in progress");
      return;
    }

    s_dp_fsm.c_stx.store(dp_stx, std::memory_order_release);
    s_dp_fsm.c_etx.store(dp_etx, std::memory_order_release);
    s_dp_fsm.op_open.store(op_open, std::memory_order_release);
    s_dp_fsm.op_end.store(op_end, std::memory_order_release);

    // 50ms Pre-Guard Time: 벨 수신 직후 3840 bps 반이중 버스 충돌 방지용 Line
    // Silent 대기
    uint32_t last_bell =
        g_doorphone_state.last_bell_ms.load(std::memory_order_acquire);
    if (last_bell > 0) {
      uint32_t now_ms = millis();
      constexpr uint32_t kDpPreGuardMs = 50;
      if (now_ms - last_bell < kDpPreGuardMs) {
        uint32_t rem_ms = kDpPreGuardMs - (now_ms - last_bell);
        if (rem_ms > 0) {
          vTaskDelay(pdMS_TO_TICKS(rem_ms) > 0 ? pdMS_TO_TICKS(rem_ms) : 1);
        }
      }
    }

    sendDpPacket(dp_stx, op_call, dp_etx);
    esp_timer_start_once(s_dp_fsm.timer, 350000); // 350ms 후 문열림 패킷 전송

    sendRpcResponse(sock, req_id, "ok");
    return;
  }

  sendRpcResponse(sock, req_id, "error",
                  "Unknown doorphone action (Use open_front or open_lobby)");
}

static void HandleRpc_SetEw11(int sock, long req_id, const char *json_str,
                              const IPAddress & /*client_ip*/) {
  long slot = findJsonIntValue(json_str, "slot", -1);
  long port = findJsonIntValue(json_str, "port", -1);
  char ip[32] = {0};
  char name[16] = {0};
  findJsonStringValue(json_str, "ip", ip, sizeof(ip));
  findJsonStringValue(json_str, "name", name, sizeof(name));

  int en_val = findJsonIntValue(json_str, "enabled", -1);
  bool enabled = (en_val == 1) || (en_val == -1 && strlen(ip) > 0);

  if (slot >= 0 && slot < Config::TCP::MAX_EW11_SLOTS) {
    uint16_t def_slot_port = Config::TCP::EW11_SLOT_PORTS[slot];
    uint16_t target_port =
        (port > 0 && port <= 65535)
            ? static_cast<uint16_t>(port)
            : (g_hub_slots[slot].target_port > 0 ? g_hub_slots[slot].target_port
                                                 : def_slot_port);
    if (target_port == 8899)
      target_port = def_slot_port; // 구버전 8899 기본값 보정
    if (Hub_SetSlot(static_cast<uint8_t>(slot), enabled, ip[0] ? ip : nullptr,
                    target_port, name[0] ? name : nullptr)) {
      const char *ok_msg = "{\"res\":\"ok\"}\n";
      send(sock, ok_msg, strlen(ok_msg), MSG_DONTWAIT);
      return;
    }
  }
  const char *err_msg =
      "{\"res\":\"error\",\"msg\":\"Invalid EW11 slot (0-4) or parameters\"}\n";
  send(sock, err_msg, strlen(err_msg), MSG_DONTWAIT);
}

static void HandleRpc_GetDevices(int sock, long req_id,
                                 const char * /*json_str*/,
                                 const IPAddress & /*client_ip*/) {
  s_mgmt_resp_buf[0] = '\0';
  AppendBuf ab{s_mgmt_resp_buf, sizeof(s_mgmt_resp_buf)};
  Mgmt_SerializeDevices(ab, req_id);
  send(sock, ab.buf, ab.offset, MSG_DONTWAIT);
  g_pkt_stats.ch6.tx_pkts.fetch_add(1, std::memory_order_relaxed);
}

static void HandleRpc_DeviceControl(int sock, long req_id, const char *json_str,
                                    const IPAddress & /*client_ip*/) {
  long dev_id = findJsonIntValue(json_str, "d", -1);
  if (dev_id == -1)
    dev_id = findJsonIntValue(json_str, "dev_id", 0);

  long sub1 = findJsonIntValue(json_str, "s1", -1);
  if (sub1 == -1)
    sub1 = findJsonIntValue(json_str, "sub1", 0);

  long sub2 = findJsonIntValue(json_str, "s2", -1);
  if (sub2 == -1)
    sub2 = findJsonIntValue(json_str, "sub2", 0);

  char act_str[32] = {0};
  if (!findJsonStringValue(json_str, "a", act_str, sizeof(act_str))) {
    findJsonStringValue(json_str, "action", act_str, sizeof(act_str));
  }

  long val = findJsonIntValue(json_str, "v", -9999);
  if (val == -9999)
    val = findJsonIntValue(json_str, "value", 0);

  if (dev_id <= 0 || dev_id > 255 || sub1 < 0 || sub1 > 255 || sub2 < 0 ||
      sub2 > 255) {
    const char *err_msg = "{\"res\":\"error\",\"msg\":\"Invalid or "
                          "out-of-range device parameters\"}\n";
    send(sock, err_msg, strlen(err_msg), MSG_DONTWAIT);
    return;
  }

  // ── FCU (0x2C) 전용 Modbus 제어 디스패치 (AGENTS.md & MODERN_CPP_GUIDELINES
  // §1) ──
  if (dev_id == Config::FCU::DEV_ID) {
    uint8_t slot_idx = static_cast<uint8_t>(sub1);
    if (slot_idx < 1 || slot_idx >= Config::TCP::MAX_EW11_SLOTS) {
      sendRpcResponse(sock, req_id, "error", "Invalid FCU slot (1-4)");
      return;
    }

    std::string_view sv{act_str};
    if (sv == "power_restore") {
      long m = findJsonIntValue(json_str, "mode", 1);
      long f = findJsonIntValue(json_str, "fan", 4);
      long s = findJsonIntValue(json_str, "swing", 0);
      long t = findJsonIntValue(json_str, "temp", 24);
      if (Fcu::RestorePower(slot_idx, static_cast<uint16_t>(m),
                            static_cast<uint16_t>(f), static_cast<uint16_t>(s),
                            static_cast<uint8_t>(t))) {
        sendRpcResponse(sock, req_id, "ok");
      } else {
        sendRpcResponse(sock, req_id, "error",
                        "Failed to send FCU restore packet");
      }
      return;
    }

    using CmdFn = bool (*)(uint8_t, int);
    struct CmdEntry {
      std::string_view key;
      CmdFn fn;
    };
    static constexpr CmdEntry kFcuCmds[] = {
        {"power", [](uint8_t s, int v) { return Fcu::SetPower(s, v == 1); }},
        {"mode",
         [](uint8_t s, int v) {
           return Fcu::SetMode(s, static_cast<Fcu::Mode>(v));
         }},
        {"fan_speed",
         [](uint8_t s, int v) {
           return Fcu::SetFanSpeed(s, static_cast<Fcu::FanSpeed>(v));
         }},
        {"swing",
         [](uint8_t s, int v) {
           return Fcu::SetSwing(s, static_cast<Fcu::Swing>(v));
         }},
        {"set_temp",
         [](uint8_t s, int v) {
           return Fcu::SetTargetTemp(s, static_cast<uint8_t>(v));
         }},
    };

    for (const auto &e : kFcuCmds) {
      if (e.key == sv) {
        if (e.fn(slot_idx, val)) {
          sendRpcResponse(sock, req_id, "ok");
        } else {
          sendRpcResponse(sock, req_id, "error",
                          "Failed to send FCU Modbus packet to socket");
        }
        return;
      }
    }
    sendRpcResponse(sock, req_id, "error",
                    "Unknown FCU action "
                    "(power_restore/power/mode/fan_speed/swing/set_temp)");
    return;
  }

  struct ActionEntry {
    const char *name;
    ControlActionType type;
  };
  static constexpr ActionEntry kActionTable[] = {
      {"power", ControlActionType::POWER},
      {"pwr", ControlActionType::POWER},
      {"set_temp", ControlActionType::SET_TEMP},
      {"temp", ControlActionType::SET_TEMP},
      {"fan_speed", ControlActionType::FAN_SPEED},
      {"spd", ControlActionType::FAN_SPEED},
      {"valve_close", ControlActionType::VALVE_CLOSE},
      {"cls", ControlActionType::VALVE_CLOSE},
      {"momentary", ControlActionType::MOMENTARY_TRIGGER},
      {"mom", ControlActionType::MOMENTARY_TRIGGER},
      {"vent_mode", ControlActionType::VENT_MODE},
      {"vnt", ControlActionType::VENT_MODE},
      {"mode", ControlActionType::VENT_MODE},
      {"ac_mode", ControlActionType::VENT_MODE},
  };

  ControlActionType act = ControlActionType::UNKNOWN;
  if (act_str[0] != '\0') {
    for (const auto &entry : kActionTable) {
      if (strcasecmp(act_str, entry.name) == 0) {
        act = entry.type;
        break;
      }
    }
  }

  if (act == ControlActionType::UNKNOWN) {
    const char *err_msg =
        "{\"res\":\"error\",\"msg\":\"Invalid action "
        "(power/set_temp/fan_speed/valve_close/momentary/vent_mode/mode)\"}\n";
    send(sock, err_msg, strlen(err_msg), MSG_DONTWAIT);
    return;
  }

  GroupControlTemplate grp{};
  if (!g_control_registry.findGroup(static_cast<uint8_t>(dev_id), grp)) {
    const char *err_msg = "{\"res\":\"error\",\"msg\":\"Device is not "
                          "registered in ctl_spec registry\"}\n";
    send(sock, err_msg, strlen(err_msg), MSG_DONTWAIT);
    return;
  }

  StaticPacket req{};
  if (!g_control_registry.buildControlPacket(
          static_cast<uint8_t>(dev_id), static_cast<uint8_t>(sub1),
          static_cast<uint8_t>(sub2), act, static_cast<int>(val), req)) {
    const char *err_msg =
        "{\"res\":\"error\",\"msg\":\"Failed to build control packet (ctl_spec "
        "missing or forbidden action)\"}\n";
    send(sock, err_msg, strlen(err_msg), MSG_DONTWAIT);
    return;
  }

  req.channel_id = 6;
  StaticPacket dummy{};
  g_control_dispatcher.dispatch(req, dummy);

  if (act == ControlActionType::SET_TEMP) {
    g_device_repo.setTargetTemp(
        static_cast<uint8_t>(dev_id), static_cast<uint8_t>(sub1),
        static_cast<uint8_t>(sub2), static_cast<uint8_t>(val));
  }

  // 현대통신 환기 장치(0x2B) 전원 ON 시 게이트웨이가 자체적으로 0x43 운전 모드
  // 조회 패킷을 연계 주입
  if (dev_id == 0x2B && act == ControlActionType::POWER && val == 1) {
    const auto *const parser = WallpadParserFactory::getActiveParser();
    StaticPacket qry_req{};
    qry_req.channel_id = 6;
    qry_req.length = 11;
    qry_req.data[0] = 0xF7;
    qry_req.data[1] = 0x0B;
    qry_req.data[2] = 0x01;
    qry_req.data[3] = 0x2B;
    qry_req.data[4] = 0x01; // QRY
    qry_req.data[5] = 0x43; // Category: 운전 모드
    qry_req.data[6] = 0x11;
    qry_req.data[7] = 0x00;
    qry_req.data[8] = 0x00;
    qry_req.data[9] =
        parser ? parser->calculateChecksum(qry_req.data.data(), 11) : 0x84;
    qry_req.data[10] = 0xEE;
    g_control_dispatcher.dispatch(qry_req, dummy);
  }

  sendRpcResponse(sock, req_id, "ok");
}

// ── Table-Driven Dispatcher ──

using RpcHandlerFunc = void (*)(int sock, long req_id, const char *json_str,
                                const IPAddress &client_ip);

struct RpcEntry {
  const char *cmd;
  RpcHandlerFunc handler;
  bool is_dangerous;
};

static const RpcEntry kRpcDispatchTable[] = {
    {"get_telemetry", HandleRpc_GetTelemetry, false},
    {"set_profile", HandleRpc_SetProfile, true},
    {"save_auto_to_slot", HandleRpc_SaveAutoToSlot, true},
    {"set_timing", HandleRpc_SetTiming, true},
    {"cache_sync", HandleRpc_CacheSync, true},
    {"ping", HandleRpc_Ping, false},
    {"cache_purge_rescan", HandleRpc_CachePurgeRescan, true},
    {"wallpad_reset", HandleRpc_WallpadReset, true},
    {"clear_coredump", HandleRpc_ClearCoredump, true},
    {"clear_reboot_logs", HandleRpc_ClearRebootLogs, true},
    {"wifi_scan", HandleRpc_WifiScan, false},
    {"start_ota", HandleRpc_StartOta, true},
    {"system_reboot", HandleRpc_SystemReboot, true},
    {"set_wifi_mode", HandleRpc_SetWifiMode, false},
    {"set_wifi", HandleRpc_SetWifi, true},
    {"set_uart", HandleRpc_SetUart, true},
    {"doorphone_action", HandleRpc_DoorphoneAction, true},
    {"set_ew11", HandleRpc_SetEw11, false},
    {"get_devices", HandleRpc_GetDevices, false},
    {"gd", HandleRpc_GetDevices, false},
    {"get_locked_devices", HandleRpc_GetDevices, false},
    {"gld", HandleRpc_GetDevices, false},
    {"device_control", HandleRpc_DeviceControl, true},
    {"ctl", HandleRpc_DeviceControl, true},
    {"control", HandleRpc_DeviceControl, true},
};

void Mgmt_DispatchJsonRpc(int sock, const char *json_str) {
  if (sock < 0 || !json_str)
    return;

  char cmd[64] = {0};
  if (!findJsonStringValue(json_str, "c", cmd, sizeof(cmd))) {
    findJsonStringValue(json_str, "cmd", cmd, sizeof(cmd));
  }
  if (cmd[0] == '\0') {
    const char *err_msg =
        "{\"res\":\"error\",\"msg\":\"Missing 'cmd' or 'c' field\"}\n";
    send(sock, err_msg, strlen(err_msg), MSG_DONTWAIT);
    return;
  }

  long req_id = findJsonIntValue(json_str, "id", -1);
  IPAddress client_ip = get_client_ip(sock);

  // SmartThings Edge Driver가 주기적으로 telemetry 요청 시 허브 IP 자동 학습 및
  // 갱신
  if (strcasecmp(cmd, "get_telemetry") == 0 &&
      client_ip != IPAddress(0, 0, 0, 0)) {
    s_trusted_hub_ip = client_ip;
  }

  for (const auto &entry : kRpcDispatchTable) {
    if (strcasecmp(cmd, entry.cmd) == 0) {
      if (entry.is_dangerous) {
        if (s_trusted_hub_ip != IPAddress(0, 0, 0, 0) &&
            client_ip != s_trusted_hub_ip) {
          sendRpcResponse(sock, req_id, "error",
                          "403 Access Denied: Unauthorized client IP");
          return;
        }
      }
      entry.handler(sock, req_id, json_str, client_ip);
      return;
    }
  }

  sendRpcResponse(sock, req_id, "error", "Unknown command");
}

void Mgmt_Data(MgmtSession *s, const uint8_t *data, size_t len) {
  if (!s || s->sock < 0 || !data || len == 0)
    return;

  g_pkt_stats.ch6.rx_pkts.fetch_add(1, std::memory_order_relaxed);

  if (len > sizeof(s->buffer)) {
    s->len = 0; // 단일 패킷 크기가 전체 수신 버퍼 초과 시 드롭
    return;
  }

  if (s->len + len > sizeof(s->buffer)) {
    s->len = 0; // 누적 버퍼 오버플로우 방어: 기존 미완성 데이터 플러시
  }

  std::copy(data, data + len, s->buffer + s->len);
  s->len += len;

  size_t p = 0;
  while (p < s->len) {
    if (s->buffer[p] == '\n' || s->buffer[p] == '\r') {
      s->buffer[p] = '\0';
      if (p > 0) {
        Mgmt_DispatchJsonRpc(s->sock,
                             reinterpret_cast<const char *>(s->buffer));
      }
      size_t rem = s->len - (p + 1);
      if (rem > 0) {
        memmove(s->buffer, s->buffer + p + 1, rem);
      }
      s->len = rem;
      p = 0;
      continue;
    }
    p++;
  }
}

// ============================================================================
// From src/Network/Network.cpp
// ============================================================================

extern const char *s_pending_reboot_reason;
extern EventGroupHandle_t g_wifi_event_group;
static constexpr EventBits_t WIFI_BIT_CONNECTED = BIT0;
static constexpr EventBits_t WIFI_BIT_DISCONNECTED = BIT1;
static constexpr EventBits_t WIFI_BIT_GOT_IP = BIT2;

template <typename SessionType, size_t N>
void Tcp_CloseAllSessions(SessionType (&sessions)[N],
                          SemaphoreHandle_t mux) noexcept {
  MutexLocker lock(mux);
  for (size_t i = 0; i < N; i++) {
    if (sessions[i].sock >= 0) {
      close(sessions[i].sock);
      sessions[i].sock = -1;
      sessions[i].len = 0;
    }
  }
}

template <typename SessionType, size_t N>
[[nodiscard]] bool Tcp_HasActiveSession(SessionType (&sessions)[N],
                                        SemaphoreHandle_t mux) noexcept {
  MutexLocker lock(mux);
  for (size_t i = 0; i < N; i++) {
    if (sessions[i].sock >= 0)
      return true;
  }
  return false;
}

template <typename SessionType, size_t N, typename DataHandler>
void Tcp_PollAndReceive(SessionType (&sessions)[N], SemaphoreHandle_t mux,
                        fd_set &readfds, fd_set &errorfds,
                        DataHandler handler) {
  MutexLocker lock(mux);
  for (size_t i = 0; i < N; i++) {
    int s = sessions[i].sock;
    if (s < 0)
      continue;

    if (FD_ISSET(s, &errorfds)) {
      close(s);
      sessions[i].sock = -1;
      sessions[i].len = 0;
      continue;
    }

    if (FD_ISSET(s, &readfds)) {
      uint8_t rx_buf[Config::TCP::POLL_RX_CHUNK_SIZE];
      int r = recv(s, rx_buf, sizeof(rx_buf), 0);
      if (r > 0) {
        handler(&sessions[i], rx_buf, r);
      } else if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        close(s);
        sessions[i].sock = -1;
        sessions[i].len = 0;
      }
    }
  }
}

void Tcp_EnableKeepalive(int sock, int idle, int intvl, int cnt);

template <typename SessionType, size_t N>
int Tcp_AcceptAndAssignSlot(int server_fd, SessionType (&sessions)[N],
                            SemaphoreHandle_t mux, int keepalive_idle,
                            int keepalive_intvl, int keepalive_cnt,
                            TcpSocketStats &stat) {
  struct sockaddr_in caddr;
  socklen_t clen = sizeof(caddr);
  int new_sock =
      accept(server_fd, reinterpret_cast<struct sockaddr *>(&caddr), &clen);
  if (new_sock < 0)
    return -1;

  const uint8_t *b = reinterpret_cast<const uint8_t *>(&caddr.sin_addr.s_addr);
  IPAddress remote_ip(b[0], b[1], b[2], b[3]);
  if (!Tcp_IsAllowedIP(remote_ip)) {
    close(new_sock);
    return -1;
  }

  int flags = fcntl(new_sock, F_GETFL, 0);
  fcntl(new_sock, F_SETFL, flags | O_NONBLOCK);
  int nodelay = 1;
  setsockopt(new_sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
  int sockbuf = Config::TCP::SOCKET_BUFFER_SIZE;
  setsockopt(new_sock, SOL_SOCKET, SO_RCVBUF, &sockbuf, sizeof(sockbuf));
  setsockopt(new_sock, SOL_SOCKET, SO_SNDBUF, &sockbuf, sizeof(sockbuf));
  Tcp_EnableKeepalive(new_sock, keepalive_idle, keepalive_intvl, keepalive_cnt);

  MutexLocker lock(mux);
  int slot = -1;
  for (size_t i = 0; i < N; i++) {
    if (sessions[i].sock < 0) {
      slot = static_cast<int>(i);
      break;
    }
  }
  if (slot == -1) {
    uint32_t oldest_time = 0xFFFFFFFF;
    int oldest_idx = 0;
    for (size_t i = 0; i < N; i++) {
      if (sessions[i].connected_at_ms < oldest_time) {
        oldest_time = sessions[i].connected_at_ms;
        oldest_idx = static_cast<int>(i);
      }
    }
    close(sessions[oldest_idx].sock);
    sessions[oldest_idx].sock = -1;
    sessions[oldest_idx].len = 0;
    slot = oldest_idx;
  }
  sessions[slot].sock = new_sock;
  sessions[slot].len = 0;
  sessions[slot].connected_at_ms = millis();
  stat.is_connected.store(true, std::memory_order_relaxed);
  stat.connection_count.fetch_add(1, std::memory_order_relaxed);
  return new_sock;
}

int Hub_AcceptClient(int slot_idx, int server_fd);

// From src/Network/NetworkManager.cpp
// ============================================================================

namespace {
constexpr uint32_t POST_BOOT_LOG_DELAY_MS = 5000;
} // namespace

EventGroupHandle_t g_wifi_event_group = nullptr;
static uint32_t s_wifi_disconnect_count = 0;
static uint32_t s_last_sta_retry_ms = 0;
static uint32_t s_sta_retry_interval_ms =
    Config::Timing::WIFI_BACKGROUND_RETRY_INTERVAL_MS;
constexpr uint32_t kMaxStaRetryIntervalMs = 60000;

void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
  case ARDUINO_EVENT_WIFI_STA_START:
    Serial.println(F("[WIFI EVENT] STA Started"));
    break;
  case ARDUINO_EVENT_WIFI_STA_CONNECTED:
    Serial.println(F("[WIFI EVENT] STA Connected to AP"));
    if (g_wifi_event_group) {
      xEventGroupSetBits(g_wifi_event_group, WIFI_BIT_CONNECTED);
      xEventGroupClearBits(g_wifi_event_group, WIFI_BIT_DISCONNECTED);
    }
    break;
  case ARDUINO_EVENT_WIFI_STA_GOT_IP:
    s_wifi_disconnect_count = 0;
    Serial.printf("[WIFI EVENT] STA Got IP: %s\r\n",
                  IPAddress(info.got_ip.ip_info.ip.addr).toString().c_str());
    if (g_wifi_event_group) {
      xEventGroupSetBits(g_wifi_event_group, WIFI_BIT_GOT_IP);
    }
    break;
  case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
    s_wifi_disconnect_count++;
    Serial.printf("[WIFI EVENT] STA Disconnected (Reason: %d, Count: %u)\r\n",
                  info.wifi_sta_disconnected.reason, s_wifi_disconnect_count);
    if (g_wifi_event_group) {
      xEventGroupSetBits(g_wifi_event_group, WIFI_BIT_DISCONNECTED);
      xEventGroupClearBits(g_wifi_event_group, WIFI_BIT_CONNECTED);
    }
    break;
  case ARDUINO_EVENT_WIFI_AP_START:
    Serial.println(F("[WIFI EVENT] SoftAP Started"));
    break;
  case ARDUINO_EVENT_WIFI_AP_STOP:
    Serial.println(F("[WIFI EVENT] SoftAP Stopped"));
    break;
  case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
    Serial.printf(
        "[WIFI EVENT] AP Station Connected! MAC: "
        "%02X:%02X:%02X:%02X:%02X:%02X, AID: %d\r\n",
        info.wifi_ap_staconnected.mac[0], info.wifi_ap_staconnected.mac[1],
        info.wifi_ap_staconnected.mac[2], info.wifi_ap_staconnected.mac[3],
        info.wifi_ap_staconnected.mac[4], info.wifi_ap_staconnected.mac[5],
        info.wifi_ap_staconnected.aid);
    break;
  case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
    Serial.printf("[WIFI EVENT] AP Station Disconnected! MAC: "
                  "%02X:%02X:%02X:%02X:%02X:%02X, AID: %d\r\n",
                  info.wifi_ap_stadisconnected.mac[0],
                  info.wifi_ap_stadisconnected.mac[1],
                  info.wifi_ap_stadisconnected.mac[2],
                  info.wifi_ap_stadisconnected.mac[3],
                  info.wifi_ap_stadisconnected.mac[4],
                  info.wifi_ap_stadisconnected.mac[5],
                  info.wifi_ap_stadisconnected.aid);
    break;
  default:
    break;
  }
}

static void
Network_ProcessSockets(int mgmt_server_fd,
                       const int ew11_server_fds[Config::TCP::MAX_EW11_SLOTS],
                       fd_set &readfds, fd_set &errorfds, bool ota_now) {
  if (mgmt_server_fd >= 0 && FD_ISSET(mgmt_server_fd, &readfds)) {
    Tcp_AcceptAndAssignSlot(mgmt_server_fd, g_mgmt_sessions, g_mgmt_mutex,
                            Config::TCP::DEFAULT_KEEPALIVE_IDLE_SEC,
                            Config::TCP::DEFAULT_KEEPALIVE_INTVL_SEC,
                            Config::TCP::DEFAULT_KEEPALIVE_CNT,
                            g_pkt_stats.ch6);
  }

  for (int s = 0; s < Config::TCP::MAX_EW11_SLOTS; s++) {
    if (!ota_now && ew11_server_fds[s] >= 0 &&
        FD_ISSET(ew11_server_fds[s], &readfds)) {
      Hub_AcceptClient(s, ew11_server_fds[s]);
    }
  }

  Tcp_PollAndReceive(g_mgmt_sessions, g_mgmt_mutex, readfds, errorfds,
                     [](MgmtSession *sess, const uint8_t *data, size_t len) {
                       Mgmt_Data(sess, data, len);
                     });

  if (!ota_now) {
    MutexLocker lock(g_ch5_mutex);
    for (int s = 0; s < Config::TCP::MAX_EW11_SLOTS; s++) {
      auto &slot = g_hub_slots[s];
      if (slot.sock < 0)
        continue;

      if (FD_ISSET(slot.sock, &errorfds)) {
        close(slot.sock);
        slot.sock = -1;
        slot.is_connected = false;
        slot.rx_len = 0;
        ESP_LOGW("EW11", "[CH5] Slot %d (%s) socket error detected. Closed.", s,
                 slot.name);
        continue;
      }

      if (FD_ISSET(slot.sock, &readfds)) {
        uint8_t temp_buf[Config::TCP::POLL_RX_CHUNK_SIZE];
        int r = recv(slot.sock, temp_buf, sizeof(temp_buf), 0);
        if (r > 0) {
          Hub_Data(&slot, temp_buf, r);
        } else if (r == 0 ||
                   (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
          close(slot.sock);
          slot.sock = -1;
          slot.is_connected = false;
          slot.rx_len = 0;
        }
      }
    }
  }
}

static void Network_HandleFcuLoop(bool ota_now, uint32_t now_ms) {
  if (!ota_now) {
    MutexLocker lock(g_ch5_mutex);
    for (int s = 1; s < Config::TCP::MAX_EW11_SLOTS; s++) {
      auto &slot = g_hub_slots[s];
      if (slot.sock >= 0 && slot.is_connected) {
        Fcu::handleSlotLoop(static_cast<uint8_t>(s), &slot, now_ms);
      }
    }
  }
}

static void Network_HandleMaintenance(uint32_t &t_chk, uint32_t &t_met,
                                      uint32_t &t_tcp, uint32_t now) {
  if (TimeUtils::isElapsed(t_chk, Config::Timing::SYSTEM_MONITOR_INTERVAL_MS)) {
    t_chk = now;
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    if (!g_http_ota_state.in_progress.load(std::memory_order_relaxed)) {
      if (free_heap < Config::Memory::MIN_HEAP_THRESHOLD_KB * 1024) {
        System_Restart("Low Heap Memory");
      }
    } else {
      if (free_heap < 8 * 1024) {
        System_Restart("Low Heap Memory (OTA)");
      }
    }
  }

  if (TimeUtils::isElapsed(t_met, Config::Metrics::SAMPLE_INTERVAL_MS)) {
    t_met = now;
    uint16_t used_ram = (heap_caps_get_total_size(MALLOC_CAP_8BIT) -
                         heap_caps_get_free_size(MALLOC_CAP_8BIT)) /
                        1024;
    uint8_t c0 = 0, c1 = 0;
    System_ReadCpuPct(c0, c1);
    g_metrics.addSample(c0, c1, used_ram, System_ReadTempC());
  }

  if (TimeUtils::isElapsed(t_tcp, Config::TCP::CLEANUP_INTERVAL_MS)) {
    t_tcp = now;
    bool any_ew11_conn = false;
    {
      MutexLocker lock(g_ch5_mutex);
      for (int s = 0; s < Config::TCP::MAX_EW11_SLOTS; s++) {
        if (g_hub_slots[s].is_connected) {
          any_ew11_conn = true;
          break;
        }
      }
    }
    g_pkt_stats.ch5.is_connected.store(any_ew11_conn,
                                       std::memory_order_relaxed);
    g_pkt_stats.ch6.is_connected.store(
        Tcp_HasActiveSession(g_mgmt_sessions, g_mgmt_mutex),
        std::memory_order_relaxed);
  }
}

void Task_Network(void *pvParameters) {
  esp_task_wdt_add(nullptr);
  uint32_t t_chk = millis(), t_met = millis(), t_tcp = millis();

  int mgmt_server_fd = -1;
  int ew11_server_fds[Config::TCP::MAX_EW11_SLOTS];
  for (int s = 0; s < Config::TCP::MAX_EW11_SLOTS; s++) {
    ew11_server_fds[s] = -1;
  }

  if (!g_rescue_mode.load(std::memory_order_relaxed)) {
    mgmt_server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (mgmt_server_fd >= 0) {
      int opt = 1;
      setsockopt(mgmt_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
      int flags = fcntl(mgmt_server_fd, F_GETFL, 0);
      fcntl(mgmt_server_fd, F_SETFL, flags | O_NONBLOCK);

      struct sockaddr_in saddr;
      memset(&saddr, 0, sizeof(saddr));
      saddr.sin_family = AF_INET;
      saddr.sin_addr.s_addr = htonl(INADDR_ANY);
      saddr.sin_port = htons(Config::TCP::MGMT_PORT);
      if (bind(mgmt_server_fd, reinterpret_cast<struct sockaddr *>(&saddr),
               sizeof(saddr)) < 0 ||
          listen(mgmt_server_fd, Config::TCP::MAX_MGMT_CLIENTS) < 0) {
        ESP_LOGE("NET", "Failed to bind/listen mgmt server (8900): errno %d",
                 errno);
        close(mgmt_server_fd);
        mgmt_server_fd = -1;
      }
    }

    Hub_LoadConfig();

    for (int s = 0; s < Config::TCP::MAX_EW11_SLOTS; s++) {
      uint16_t listen_port = g_hub_slots[s].target_port;
      if (listen_port == 0) {
        listen_port = Config::TCP::EW11_SLOT_PORTS[s];
        g_hub_slots[s].target_port = listen_port;
      }

      int sfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
      if (sfd >= 0) {
        int opt = 1;
        setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        int flags = fcntl(sfd, F_GETFL, 0);
        fcntl(sfd, F_SETFL, flags | O_NONBLOCK);

        struct sockaddr_in saddr;
        memset(&saddr, 0, sizeof(saddr));
        saddr.sin_family = AF_INET;
        saddr.sin_addr.s_addr = htonl(INADDR_ANY);
        saddr.sin_port = htons(listen_port);
        if (bind(sfd, reinterpret_cast<struct sockaddr *>(&saddr),
                 sizeof(saddr)) < 0 ||
            listen(sfd, 1) < 0) {
          ESP_LOGE("EW11",
                   "Failed to bind/listen EW11 slot %d on port %u: errno %d", s,
                   listen_port, errno);
          close(sfd);
          sfd = -1;
        } else {
          ESP_LOGI("EW11", "[CH5] Listening for EW11 slot %d (%s) on port %u",
                   s, g_hub_slots[s].name, listen_port);
        }
      }
      ew11_server_fds[s] = sfd;
    }
  } else {
    Serial.println(F("[RESCUE] CH6 TCP server port disabled in Rescue "
                     "Mode. Dedicated to OTA & Telnet."));
  }

  for (;;) {
    esp_task_wdt_reset();
    g_wdt_monitor.feed(4);
    ArduinoOTA.handle();

    const bool ota_now = g_ota_in_progress.load(std::memory_order_relaxed);

    System_CheckOtaHealth();
    Cache_CheckNvsDebounce();

    if (!g_rescue_mode.load(std::memory_order_relaxed) && g_wifi_event_group) {
      EventBits_t bits = xEventGroupGetBits(g_wifi_event_group);

      if (bits & WIFI_BIT_GOT_IP) {
        xEventGroupClearBits(g_wifi_event_group, WIFI_BIT_GOT_IP);
        if (g_wifi_guard.testing.load(std::memory_order_acquire)) {
          g_wifi_guard.testing.store(false, std::memory_order_release);
          Config_Save();
          Serial.printf("[WIFI] ★ New Wi-Fi '%s' connected successfully! Saved "
                        "to NVS.\r\n",
                        g_config.wifi_ssid);
        }
      }

      if (g_wifi_guard.testing.load(std::memory_order_acquire)) {
        if (TimeUtils::isElapsed(g_wifi_guard.start_ms, 15000)) {
          g_wifi_guard.testing.store(false, std::memory_order_release);
          Serial.printf("[WIFI] ⚠️ New Wi-Fi '%s' failed to connect within 15s! "
                        "Reverting to '%s'...\r\n",
                        g_config.wifi_ssid, g_wifi_guard.prev_ssid);
          {
            std::unique_lock lock(g_config_rw);
            strncpy(g_config.wifi_ssid, g_wifi_guard.prev_ssid,
                    sizeof(g_config.wifi_ssid) - 1);
            strncpy(g_config.wifi_password, g_wifi_guard.prev_pass,
                    sizeof(g_config.wifi_password) - 1);
          }
          WiFi.disconnect(false);
          vTaskDelay(pdMS_TO_TICKS(100));
          WiFi.begin(g_config.wifi_ssid, g_config.wifi_password);
        }
      }

      if (bits & WIFI_BIT_CONNECTED) {
        s_sta_retry_interval_ms =
            Config::Timing::WIFI_BACKGROUND_RETRY_INTERVAL_MS;
      }

      if (bits & WIFI_BIT_DISCONNECTED) {
        if (TimeUtils::isElapsed(s_last_sta_retry_ms,
                                 s_sta_retry_interval_ms)) {
          s_last_sta_retry_ms = millis();
          Serial.printf("[WIFI] Event: DISCONNECTED. Background STA "
                        "reconnection attempt (interval: %u ms)...\r\n",
                        static_cast<unsigned>(s_sta_retry_interval_ms));
          esp_wifi_connect();
          s_sta_retry_interval_ms =
              std::min(s_sta_retry_interval_ms * 2, kMaxStaRetryIntervalMs);
        }
      }
    }

    if (s_pending_reboot_reason && millis() > POST_BOOT_LOG_DELAY_MS) {
      LogManager::writeRebootLog(s_pending_reboot_reason);
      s_pending_reboot_reason = nullptr;
    }

    fd_set readfds, writefds, errorfds;
    FD_ZERO(&readfds);
    FD_ZERO(&writefds);
    FD_ZERO(&errorfds);
    int max_fd = -1;

    auto add_read_fd = [&](int fd) {
      if (fd >= 0) {
        FD_SET(fd, &readfds);
        FD_SET(fd, &errorfds);
        if (fd > max_fd)
          max_fd = fd;
      }
    };

    add_read_fd(mgmt_server_fd);
    for (int s = 0; s < Config::TCP::MAX_EW11_SLOTS; s++) {
      if (ew11_server_fds[s] >= 0) {
        add_read_fd(ew11_server_fds[s]);
      }
    }

    {
      MutexLocker lock(g_mgmt_mutex);
      for (int m = 0; m < Config::TCP::MAX_MGMT_CLIENTS; m++) {
        if (g_mgmt_sessions[m].sock >= 0) {
          add_read_fd(g_mgmt_sessions[m].sock);
        }
      }
    }

    {
      MutexLocker lock(g_ch5_mutex);
      for (int s = 0; s < Config::TCP::MAX_EW11_SLOTS; s++) {
        if (g_hub_slots[s].sock >= 0) {
          add_read_fd(g_hub_slots[s].sock);
        }
      }
    }

    int act = 0;
    struct timeval tv = {0, 10000};
    if (max_fd >= 0) {
      act = select(max_fd + 1, &readfds, nullptr, &errorfds, &tv);
      if (ota_now) {
        vTaskDelay(pdMS_TO_TICKS(10));
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(10));
    }

    esp_task_wdt_reset();
    g_wdt_monitor.feed(4);

    if (act > 0) {
      Network_ProcessSockets(mgmt_server_fd, ew11_server_fds, readfds, errorfds,
                             ota_now);
    }

    // FCU 슬롯(1~4) 120ms 논블로킹 가드타임 및 20초 주기 폴링 처리
    Network_HandleFcuLoop(ota_now, millis());

    // 주기적 시스템 및 소켓 상태 유지보수
    Network_HandleMaintenance(t_chk, t_met, t_tcp, millis());
  }
}
