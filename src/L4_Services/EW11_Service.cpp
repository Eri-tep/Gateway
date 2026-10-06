// ============================================================================
// BridgeService: Level 4 EW11 TCP Bridge & Air Conditioner Implementation
// ============================================================================

#include "L4_Services/EW11_Service.h"
#include "L3_Protocol/Public/Packet_Router.h"
#include "L3_Protocol/Public/Device_Registry.h"
#include "L3_Protocol/Public/Protocol_Diagnostics.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <esp_log.h>
#include <fcntl.h>
#include <lwip/sockets.h>
#include <unistd.h>

struct HubClientSlot {
  bool enabled{false};
  char name[16]{""};
  char target_ip[16]{""};
  uint16_t target_port{8898};
  HubDeviceType dev_type{HubDeviceType::WALLPAD_COMPATIBLE};
  uint8_t frame_stx{0};
  uint8_t frame_etx{0};
  uint8_t frame_len{0};
  bool frame_locked{false};
  int sock{-1};
  bool is_connected{false};
  uint32_t last_reconnect_ms{0};
  uint8_t rx_buf[Config::TCP::HUB_RX_BUFFER_SIZE];
  size_t rx_len{0};
  uint32_t last_rx_ms{0};
  uint32_t rx_pkts{0};
  uint32_t tx_pkts{0};
  uint32_t dropped_pkts{0};
  uint8_t last_query_data[64]{0};
  uint8_t last_query_len{0};
};

static HubClientSlot s_hub_slots[Config::TCP::MAX_EW11_SLOTS];
static SemaphoreHandle_t s_ch5_mutex = nullptr;

bool Bridge_GetSlotSnapshot(uint8_t slot_idx, HubClientSlotSnapshot &out) {
  if (slot_idx >= Config::TCP::MAX_EW11_SLOTS) {
    return false;
  }
  MutexLocker lock(s_ch5_mutex);
  const auto &s = s_hub_slots[slot_idx];
  out.enabled = s.enabled;
  out.is_connected = s.is_connected;
  strncpy(out.name, s.name, sizeof(out.name));
  out.name[sizeof(out.name) - 1] = '\0';
  strncpy(out.target_ip, s.target_ip, sizeof(out.target_ip));
  out.target_ip[sizeof(out.target_ip) - 1] = '\0';
  out.target_port = s.target_port;
  out.dev_type = s.dev_type;
  out.last_rx_ms = s.last_rx_ms;
  out.rx_pkts = s.rx_pkts;
  out.tx_pkts = s.tx_pkts;
  out.dropped_pkts = s.dropped_pkts;
  return true;
}

bool Bridge_SetSlotEnabled(uint8_t slot_idx, bool enabled) {
  if (slot_idx >= Config::TCP::MAX_EW11_SLOTS) {
    return false;
  }
  {
    MutexLocker lock(s_ch5_mutex);
    s_hub_slots[slot_idx].enabled = enabled;
    if (!enabled && s_hub_slots[slot_idx].sock >= 0) {
      close(s_hub_slots[slot_idx].sock);
      s_hub_slots[slot_idx].sock = -1;
      s_hub_slots[slot_idx].is_connected = false;
      s_hub_slots[slot_idx].rx_len = 0;
    }
  }
  Hub_SaveConfig();
  return true;
}

bool Bridge_SetFramingLock(uint8_t slot_idx, uint8_t stx, uint8_t etx, uint8_t len) {
  if (slot_idx >= Config::TCP::MAX_EW11_SLOTS) {
    return false;
  }
  char ns[16];
  snprintf(ns, sizeof(ns), "e%d_frame", slot_idx);
  MutexLocker lock(s_ch5_mutex);
  s_hub_slots[slot_idx].frame_stx = stx;
  s_hub_slots[slot_idx].frame_etx = etx;
  s_hub_slots[slot_idx].frame_len = len;
  s_hub_slots[slot_idx].frame_locked = true;

  Preferences p;
  p.begin(ns, false);
  p.putUChar("stx", stx);
  p.putUChar("etx", etx);
  p.putUChar("len", len);
  p.putBool("locked", true);
  p.end();
  return true;
}

bool Bridge_ResetFramingTracker(uint8_t slot_idx) {
  if (slot_idx >= Config::TCP::MAX_EW11_SLOTS) {
    return false;
  }
  char ns[16];
  snprintf(ns, sizeof(ns), "e%d_frame", slot_idx);
  MutexLocker lock(s_ch5_mutex);
  s_hub_slots[slot_idx].frame_stx = 0;
  s_hub_slots[slot_idx].frame_etx = 0;
  s_hub_slots[slot_idx].frame_len = 0;
  s_hub_slots[slot_idx].frame_locked = false;

  Preferences p;
  p.begin(ns, false);
  p.clear();
  p.end();
  return true;
}

namespace Ew11Manager {
bool sendBurstPacket(uint8_t slot_idx, const StaticPacket &pkt,
                     uint8_t count = 2, uint32_t silence_ms = 20);
} // namespace Ew11Manager

static void Hub_ProcessPacket(HubClientSlot *slot, const uint8_t *pkt_data,
                              size_t pkt_len);

static BridgeDeviceStateListener s_bridge_dev_listener = nullptr;
static ElevatorStateListener s_elevator_listener = nullptr;

void Bridge_RegisterDeviceStateListener(BridgeDeviceStateListener listener) noexcept {
  s_bridge_dev_listener = listener;
}

void Bridge_RegisterElevatorListener(ElevatorStateListener listener) noexcept {
  s_elevator_listener = listener;
}

bool Bridge_ForwardPacket(uint8_t slot_idx, const StaticPacket &pkt,
                          bool burst) noexcept {
  if (burst || slot_idx == 0) {
    return Ew11Manager::sendBurstPacket(slot_idx, pkt, 2, 20);
  } else {
    const bool sent = Hub_SendPacket(slot_idx, pkt);
    System_TracePacket(5, true, sent ? TraceType::CTL : TraceType::DRP, pkt);
    return sent;
  }
}


// ── EW11 & Modbus FCU Engine (formerly Bridge.cpp) ──
static const char *TAG = "EW11";

// ============================================================================
// Modbus-RTU Protocol Codec is canonically located in Protocol/ModbusProtocol.h

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
  uint8_t stx = ProtocolDiag_GetActiveStx();

  size_t p = 0;
  size_t loop_count = 0;
  while (p < slot->rx_len && ++loop_count < 256) {
    if (slot->rx_buf[p] != stx) {
      p++;
      continue;
    }

    int len_res = ProtocolDiag_ExtractPacketLength(slot->rx_buf, slot->rx_len, p);
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

    if (!ProtocolDiag_ValidatePacket(&slot->rx_buf[p], p_len)) {
      StaticPacket drp_pkt{5, p_len};
      std::copy(&slot->rx_buf[p], &slot->rx_buf[p + p_len],
                drp_pkt.data.begin());
      System_TracePacket(5, false, TraceType::DRP, drp_pkt);
      System_RecordCh5Dropped();
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
      System_RecordCh5Rx();
      StaticPacket trace_pkt{5, 19};
      std::copy(&slot->rx_buf[p], &slot->rx_buf[p + 19], trace_pkt.data.begin());
      System_TracePacket(5, false, TraceType::RMT, trace_pkt);
      Fcu::handleSlotRx(static_cast<uint8_t>(slot_idx), &slot->rx_buf[p], 19);
      p += 19;
      continue;
    }

    // 8바이트 0x06 / 0x10 제어 ACK
    if (rem >= 8 &&
        (slot->rx_buf[p + 1] == 0x06 || slot->rx_buf[p + 1] == 0x10)) {
      slot->rx_pkts++;
      System_RecordCh5Rx();
      StaticPacket trace_pkt{5, 8};
      std::copy(&slot->rx_buf[p], &slot->rx_buf[p + 8], trace_pkt.data.begin());
      System_TracePacket(5, false, TraceType::RMT, trace_pkt);
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
    MutexLocker lock(s_ch5_mutex);
    const auto &slot_info = s_hub_slots[slot];
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
  System_TracePacket(5, true, sent ? TraceType::CTL : TraceType::DRP,
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
    MutexLocker lock(s_ch5_mutex);
    const auto &slot = s_hub_slots[slot_idx];
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
    uint8_t dev_id = 0, sub1 = 0, sub2 = 0;
    if (ProtocolDiag_ExtractDeviceKey(pkt_data, pkt_len, dev_id, sub1, sub2) && dev_id == 0x34) {
      Router_RecordRoute(5, 0, 0x34, sub1, sub2);

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
          if (s_elevator_listener) {
            s_elevator_listener(sub1, sub2, 15, 0, new_pwr, false);
          }
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
        if (s_elevator_listener) {
          s_elevator_listener(sub1, sub2, floor, ho, 0, true);
        }
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

void syncDeviceRepository(uint8_t slot_idx, const Fcu::Snapshot &snap,
                          const uint8_t *raw_pkt = nullptr, size_t raw_len = 0) {
  Device_SyncFcuState(slot_idx, snap.target_temp, snap.room_temp, true, raw_pkt, raw_len);
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

  MutexLocker lock(s_ch5_mutex);
  HubClientSlot &slot = s_hub_slots[slot_idx];
  if (!slot.enabled || slot.sock < 0 || !slot.is_connected)
    return false;

  bool ok =
      (send(slot.sock, pkt, len, MSG_DONTWAIT) == static_cast<ssize_t>(len));
  if (ok) {
    slot.tx_pkts++;
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

void handleSlotRx(uint8_t slot_idx, std::span<const uint8_t> data) noexcept {
  if (slot_idx < 1 || slot_idx >= Config::TCP::MAX_EW11_SLOTS || data.empty())
    return;
  auto &rt = s_fcu_slots[slot_idx];

  // 1) 제어 명령(0x06, 0x10) ACK 수신 확인 (8바이트 에코 응답)
  if (data.size() >= 8 && data[0] == 0x01 && (data[1] == 0x06 || data[1] == 0x10)) {
    rt.waiting_response = false;
    rt.timeout_count = 0;
    rt.is_online = true;
    rt.next_tx_ms = millis() + Config::FCU::INTER_PACKET_DELAY_MS;
    return;
  }

  // 2) 19바이트 0x03 상태 쿼리 응답 처리
  if (data.size() >= 19 && data[0] == 0x01 && data[1] == 0x03 && data[2] == 0x0E) {
    auto parse_res = ModbusRtu::parseStatusResponse(data);
    if (!parse_res)
      return; // 파싱/CRC 실패 시 드롭

    const auto &new_snap = *parse_res;
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
    // 보고된 경우 1회 자동 보정 ※ Fcu_SendRaw 직접 호출 시 s_ch5_mutex 재귀
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
    syncDeviceRepository(slot_idx, rt.snap, data.data(), data.size());
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
        System_RecordCh5Tx();
        StaticPacket trace_pkt{5, static_cast<uint8_t>(temp_frame.size())};
        std::copy(temp_frame.begin(), temp_frame.end(), trace_pkt.data.begin());
        System_TracePacket(5, true, TraceType::CTL, trace_pkt);
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
        System_RecordCh5Tx();
        StaticPacket trace_pkt{5, len};
        std::copy(rt.pending_cmd_buf, rt.pending_cmd_buf + len, trace_pkt.data.begin());
        System_TracePacket(5, true, TraceType::CTL, trace_pkt);
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
    ProtocolDiag_PollingRegisterOrTouch(5, Config::FCU::DEV_ID, slot_idx, 0,
                                      ModbusRtu::kQueryPkt.data(),
                                      ModbusRtu::kQueryPkt.size());
    Router_RecordRoute(5, slot_idx, Config::FCU::DEV_ID, slot_idx, 0);

    if (send(slot->sock, ModbusRtu::kQueryPkt.data(),
             ModbusRtu::kQueryPkt.size(), MSG_DONTWAIT) ==
        static_cast<ssize_t>(ModbusRtu::kQueryPkt.size())) {
      slot->tx_pkts++;
      System_RecordCh5Tx();
      StaticPacket trace_pkt{5, static_cast<uint8_t>(ModbusRtu::kQueryPkt.size())};
      std::copy(ModbusRtu::kQueryPkt.begin(), ModbusRtu::kQueryPkt.end(), trace_pkt.data.begin());
      System_TracePacket(5, true, TraceType::QRY, trace_pkt);
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

  MutexLocker lock(s_ch5_mutex);
  auto &slot = s_hub_slots[slot_idx];

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

  System_RecordCh5Connection();
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
  System_RecordCh5Rx();
  System_TracePacket(5, false, TraceType::RMT, pkt);

  uint8_t dev_id = 0, sub1 = 0, sub2 = 0;
  uint8_t stx = ProtocolDiag_GetActiveStx();
  uint8_t etx = ProtocolDiag_GetActiveEtx();
  if (ProtocolDiag_ExtractDeviceKey(pkt_data, pkt_len, dev_id, sub1, sub2) && dev_id != 0 &&
      dev_id != stx && dev_id != etx &&
      dev_id != 0xFF) {
    int8_t s_idx = static_cast<int8_t>(slot - s_hub_slots);
    Router_RecordRoute(5, s_idx, dev_id, sub1, sub2);

    bool is_query = ProtocolDiag_IsQueryPacket(pkt_data, pkt_len);
    bool is_ack = (pkt_len >= 5 && pkt_data[4] == 0x04); // 표준 ACK Opcode(0x04)

    // 0x2A (신발장 서브 패널 / 원격검침)는 폴링 대상 및 단말 제어 기기가
    // 아니므로 캐시에서 완전 제외
    if (dev_id != 0x2A) {
      if (is_query) {
        ProtocolDiag_PollingRegisterOrTouch(5, dev_id, sub1, sub2, pkt_data,
                                            pkt_len);
      }
      if (is_ack) {
        if (s_bridge_dev_listener) {
          s_bridge_dev_listener(Device_UpdateFromBus(pkt));
        }
      }
    }
  }

  int slot_idx = static_cast<int>(slot - s_hub_slots);
  Ew11Manager::processPacket(slot_idx, pkt_data, pkt_len);
}

void Hub_Data(HubClientSlot *slot, const uint8_t *data, size_t len) {
  if (!slot || slot->sock < 0 || !data || len == 0)
    return;

  slot->last_rx_ms = millis();

  // 오버플로우 방어: 수신 버퍼 여유가 부족할 경우 미완성 패킷 시작 바이트
  // 앞으로 슬라이딩
  if (slot->rx_len + len > sizeof(slot->rx_buf)) {
    uint8_t stx = slot->frame_stx;
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

  int slot_idx = static_cast<int>(slot - s_hub_slots);
  Ew11Manager::processStream(slot_idx, slot);
}

void Hub_LoadConfig() {
  Preferences p;
  p.begin("ew11-config", true);
  MutexLocker lock(s_ch5_mutex);

  // Slot 0 (엘리베이터)
  s_hub_slots[0].enabled = p.getBool("e0_en", true);
  p.getString("e0_name", "Elevator")
      .toCharArray(s_hub_slots[0].name, sizeof(s_hub_slots[0].name));
  p.getString("e0_ip", "172.30.1.245")
      .toCharArray(s_hub_slots[0].target_ip, sizeof(s_hub_slots[0].target_ip));
  uint16_t p0 = p.getUShort("e0_port", 8898);
  if (p0 == 0 || p0 == 8899)
    p0 = 8898;
  s_hub_slots[0].target_port = p0;
  s_hub_slots[0].dev_type = HubDeviceType::WALLPAD_COMPATIBLE;
  s_hub_slots[0].sock = -1;
  s_hub_slots[0].is_connected = false;
  s_hub_slots[0].rx_len = 0;

  // Slot 1~4 (FCU 에어컨)
  for (int i = 1; i < Config::TCP::MAX_EW11_SLOTS; i++) {
    char k_en[8], k_nm[8], k_ip[8], k_pt[8], def_nm[16];
    snprintf(k_en, sizeof(k_en), "e%d_en", i);
    snprintf(k_nm, sizeof(k_nm), "e%d_name", i);
    snprintf(k_ip, sizeof(k_ip), "e%d_ip", i);
    snprintf(k_pt, sizeof(k_pt), "e%d_port", i);
    snprintf(def_nm, sizeof(def_nm), "AC_%d", i);

    s_hub_slots[i].enabled = p.getBool(k_en, false);
    p.getString(k_nm, def_nm)
        .toCharArray(s_hub_slots[i].name, sizeof(s_hub_slots[i].name));
    p.getString(k_ip, "").toCharArray(s_hub_slots[i].target_ip,
                                      sizeof(s_hub_slots[i].target_ip));
    uint16_t def_slot_port =
        Config::TCP::EW11_SLOT_PORTS[i]; // 8891, 8892, 8893, 8894
    uint16_t pi = p.getUShort(k_pt, def_slot_port);
    if (pi == 0 || pi == 8899)
      pi = def_slot_port;
    s_hub_slots[i].target_port = pi;
    s_hub_slots[i].dev_type = HubDeviceType::AIR_CONDITIONER;
    s_hub_slots[i].sock = -1;
    s_hub_slots[i].is_connected = false;
    s_hub_slots[i].rx_len = 0;
  }

  for (int s = 0; s < Config::TCP::MAX_EW11_SLOTS; s++) {
    char ns[16];
    snprintf(ns, sizeof(ns), "e%d_frame", s);
    Preferences fp;
    fp.begin(ns, true);
    if (fp.getBool("locked", false)) {
      s_hub_slots[s].frame_stx = fp.getUChar("stx", 0);
      s_hub_slots[s].frame_etx = fp.getUChar("etx", 0);
      s_hub_slots[s].frame_len = fp.getUChar("len", 0);
      s_hub_slots[s].frame_locked = true;
    }
    fp.end();
  }
  p.end();
}

void Hub_SaveConfig() {
  Preferences p;
  p.begin("ew11-config", false);
  MutexLocker lock(s_ch5_mutex);

  for (int i = 0; i < Config::TCP::MAX_EW11_SLOTS; i++) {
    char k_en[8], k_nm[8], k_ip[8], k_pt[8];
    snprintf(k_en, sizeof(k_en), "e%d_en", i);
    snprintf(k_nm, sizeof(k_nm), "e%d_name", i);
    snprintf(k_ip, sizeof(k_ip), "e%d_ip", i);
    snprintf(k_pt, sizeof(k_pt), "e%d_port", i);

    p.putBool(k_en, s_hub_slots[i].enabled);
    p.putString(k_nm, s_hub_slots[i].name);
    p.putString(k_ip, s_hub_slots[i].target_ip);
    p.putUShort(k_pt, s_hub_slots[i].target_port);
  }
  p.end();
}

bool Hub_SetSlot(uint8_t slot_idx, bool enabled, const char *ip, uint16_t port,
                 const char *name) {
  if (slot_idx >= Config::TCP::MAX_EW11_SLOTS)
    return false;

  MutexLocker lock(s_ch5_mutex);
  auto &slot = s_hub_slots[slot_idx];

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
  MutexLocker lock(s_ch5_mutex);
  auto &slot = s_hub_slots[slot_idx];
  if (!slot.enabled || slot.sock < 0 || !slot.is_connected)
    return false;

  int s = send(slot.sock, pkt.data.data(), pkt.length, MSG_DONTWAIT);
  if (s == static_cast<int>(pkt.length)) {
    slot.tx_pkts++;
    System_RecordCh5Tx();
    return true;
  }
  return false;
}

static int s_ew11_server_fds[Config::TCP::MAX_EW11_SLOTS] = {-1, -1, -1, -1, -1};

void Bridge_PopulateFds(fd_set &readfds, fd_set &errorfds, int &max_fd) noexcept {
  auto add_fd = [&](int fd) {
    if (fd >= 0) {
      FD_SET(fd, &readfds);
      FD_SET(fd, &errorfds);
      if (fd > max_fd)
        max_fd = fd;
    }
  };

  for (int s = 0; s < Config::TCP::MAX_EW11_SLOTS; s++) {
    add_fd(s_ew11_server_fds[s]);
  }

  MutexLocker lock(s_ch5_mutex);
  for (int s = 0; s < Config::TCP::MAX_EW11_SLOTS; s++) {
    add_fd(s_hub_slots[s].sock);
  }
}

void Bridge_ProcessEvents(fd_set &readfds, fd_set &errorfds,
                          bool ota_now) noexcept {
  for (int s = 0; s < Config::TCP::MAX_EW11_SLOTS; s++) {
    if (!ota_now && s_ew11_server_fds[s] >= 0 &&
        FD_ISSET(s_ew11_server_fds[s], &readfds)) {
      Hub_AcceptClient(s, s_ew11_server_fds[s]);
    }
  }

  if (!ota_now) {
    MutexLocker lock(s_ch5_mutex);
    for (int s = 0; s < Config::TCP::MAX_EW11_SLOTS; s++) {
      auto &slot = s_hub_slots[s];
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

void Bridge_Tick(bool ota_now, uint32_t now_ms) noexcept {
  if (!ota_now) {
    MutexLocker lock(s_ch5_mutex);
    for (int s = 1; s < Config::TCP::MAX_EW11_SLOTS; s++) {
      auto &slot = s_hub_slots[s];
      if (slot.sock >= 0 && slot.is_connected) {
        Fcu::handleSlotLoop(static_cast<uint8_t>(s), &slot, now_ms);
      }
    }
  }
}

void Bridge_Init() {
  if (!s_ch5_mutex) {
    s_ch5_mutex = xSemaphoreCreateMutex();
  }
  Ew11Manager::init();
  Hub_LoadConfig();

  if (!g_rescue_mode.load(std::memory_order_relaxed)) {
    for (int s = 0; s < Config::TCP::MAX_EW11_SLOTS; s++) {
      uint16_t listen_port = s_hub_slots[s].target_port;
      if (listen_port == 0) {
        listen_port = Config::TCP::EW11_SLOT_PORTS[s];
        s_hub_slots[s].target_port = listen_port;
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
                   s, s_hub_slots[s].name, listen_port);
        }
      }
      s_ew11_server_fds[s] = sfd;
    }
  }

  ProtocolTcpParticipant p;
  p.name = "BridgeService";
  p.populateFds = Bridge_PopulateFds;
  p.processEvents = Bridge_ProcessEvents;
  p.tick = Bridge_Tick;
  ProtocolDiag_RegisterTcpParticipant(p);
}

void Bridge_ShutdownSockets() noexcept {
  MutexLocker lock(s_ch5_mutex);
  for (int s = 0; s < Config::TCP::MAX_EW11_SLOTS; s++) {
    if (s_hub_slots[s].sock >= 0) {
      close(s_hub_slots[s].sock);
      s_hub_slots[s].sock = -1;
      s_hub_slots[s].is_connected = false;
      s_hub_slots[s].rx_len = 0;
    }
    if (s_ew11_server_fds[s] >= 0) {
      close(s_ew11_server_fds[s]);
      s_ew11_server_fds[s] = -1;
    }
  }
}
