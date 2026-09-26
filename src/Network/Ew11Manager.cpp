#include "Ew11Manager.h"
#include "NetworkInternal.h"
#include "WallpadParser.h"
#include "MgmtRpc.h"
#include <esp_timer.h>
#include <esp_log.h>
#include <atomic>
#include <cstring>
#include <algorithm>

static const char *TAG = "Ew11Mgr";

namespace Ew11Manager {

namespace {

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

  // 1. 선로 상태 확인 (RX 및 이전 TX 기준 20ms 침묵 여부 점검)
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

  uint32_t rx_rem_ms = (rx_elapsed < silence_req_ms) ? (silence_req_ms - rx_elapsed) : 0;
  uint32_t tx_rem_ms = (last_tx > 0 && tx_elapsed < silence_req_ms) ? (silence_req_ms - tx_elapsed) : 0;
  uint32_t wait_ms = std::max(rx_rem_ms, tx_rem_ms);

  // 2. 선로에 다른 패킷이 유입되었거나 이전 전송 후 20ms가 지나지 않은 경우 동적 연장 대기 (충돌 100% 회피)
  if (wait_ms > 0) {
    portENTER_CRITICAL(&s_burst_fsm.mux);
    if (s_burst_fsm.remaining_count > 0) {
      esp_timer_start_once(s_burst_fsm.timer, wait_ms * 1000);
    }
    portEXIT_CRITICAL(&s_burst_fsm.mux);
    return;
  }

  // 3. 선로가 20ms 이상 완전히 유휴인 상태 확인 -> 패킷 전송
  bool sent = Hub_SendPacket(slot, tx_pkt);
  g_telnet_tracer.trace(5, true, sent ? TraceType::CTL : TraceType::DRP, tx_pkt);

  portENTER_CRITICAL(&s_burst_fsm.mux);
  s_burst_fsm.last_tx_ms = millis();
  if (s_burst_fsm.remaining_count > 0) {
    s_burst_fsm.remaining_count--;
  }

  // 다음 회차가 남아있다면 20ms 후 동적 유휴 확인 타이머 스케줄
  if (s_burst_fsm.remaining_count > 0) {
    esp_timer_start_once(s_burst_fsm.timer, silence_req_ms * 1000);
  }
  portEXIT_CRITICAL(&s_burst_fsm.mux);
}

} // anonymous namespace

void init() {
  if (!s_burst_fsm.timer) {
    esp_timer_create_args_t timer_args{};
    timer_args.callback = onBurstTimer;
    timer_args.arg = nullptr;
    timer_args.name = "ew11_burst_timer";
    esp_timer_create(&timer_args, &s_burst_fsm.timer);
  }
}

bool sendBurstPacket(uint8_t slot_idx, const StaticPacket &pkt, uint8_t count, uint32_t silence_ms) {
  if (slot_idx >= Config::TCP::MAX_EW11_SLOTS || count == 0) return false;
  if (!s_burst_fsm.timer) init();

  // 기존 진행 중인 타이머 취소
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

  // 즉시 onBurstTimer를 1ms 후 발화시켜 선로 20ms 유휴 검증 파이프라인으로 진입
  esp_timer_start_once(s_burst_fsm.timer, 1000); // 1ms (1000us)
  return true;
}

void processPacket(int slot_idx, const uint8_t *pkt_data, size_t pkt_len) {
  if (!pkt_data || pkt_len == 0) return;

  // --------------------------------------------------------------------------
  // Slot 0: 엘리베이터 (0x34) 전용 처리
  // --------------------------------------------------------------------------
  if (slot_idx == 0) {
    auto *parser = WallpadParserFactory::getActiveParser();
    if (!parser) return;

    span<const uint8_t> frame(pkt_data, pkt_len);
    uint8_t dev_id = 0, sub1 = 0, sub2 = 0;
    if (parser->extractDeviceKey(frame, dev_id, sub1, sub2) && dev_id == 0x34) {
      // EW11 Slot 0 수신 시 L2 라우터에 CH5 Slot 0 경로 자동 학습/등록
      g_route_registry.recordRoute(5, 0, 0x34, sub1, sub2);

      // 1) 11-byte 상태 응답 (호출 ACK / 대기 복귀)
      // data[4] == 0x04: Byte #8 == 0x06 (호출 이동 중 / ON), 0x00 (대기 복귀 / OFF)
      if (pkt_len == 11 && pkt_data[4] == 0x04) {
        static std::atomic<uint8_t> s_last_elev_pwr{0xFF};
        uint8_t new_pwr = (pkt_data[8] == 0x06) ? 1 : 0;
        uint8_t prev = s_last_elev_pwr.exchange(new_pwr, std::memory_order_acq_rel);
        if (prev != new_pwr) {
          ESP_LOGI(TAG, "Elevator State Changed -> Power: %u", new_pwr);
          Mgmt_BroadcastDeviceState(0x34, sub1, sub2, DeviceClass::MOMENTARY,
                                    new_pwr, 0, 0, 0, nullptr, 0.0f, 15, 0, 0);
        }
      }
      // 2) 13-byte 도착 감지 브로드캐스트
      // data[4] == 0x01 && data[8] == 0x01: Byte #9 = 댁내 층수, Byte #10 = 호기 번호
      else if (pkt_len == 13 && pkt_data[4] == 0x01 && pkt_data[8] == 0x01) {
        uint8_t floor = pkt_data[9];
        uint8_t ho = pkt_data[10];
        ESP_LOGI(TAG, "Elevator Arrived -> Floor: %u, Car: %u", floor, ho);
        Mgmt_BroadcastDeviceState(0x34, sub1, sub2, DeviceClass::MOMENTARY,
                                  0, 0, 0, 0, nullptr, 0.0f, floor, 0, ho);
      }
    }
    return;
  }

  // --------------------------------------------------------------------------
  // Slot 1~4: 시스템 에어컨 (1~4호) 독자 프로토콜 처리
  // --------------------------------------------------------------------------
  // 에어컨 패킷 파싱 및 스마트싱스 상태 텔레메트리 연동
  ESP_LOGD(TAG, "EW11 Slot %d RX len=%u", slot_idx, (unsigned)pkt_len);
}

void processStream(int slot_idx, HubClientSlot *slot) {
  if (!slot) return;

  if (slot_idx == 0) {
    // Slot 0 (엘리베이터): 현대통신 월패드 호환 프레이밍
    auto *parser = WallpadParserFactory::getActiveParser();
    uint8_t stx = parser ? parser->getStx() : PKT_STX;

    size_t p = 0;
    size_t loop_count = 0;
    while (p < slot->rx_len && ++loop_count < 256) {
      if (slot->rx_buf[p] != stx) {
        p++;
        continue;
      }

      int len_res = parser ? parser->extractPacketLength(slot->rx_buf, slot->rx_len, p) : -1;
      if (len_res == 0) break;
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
        std::copy(&slot->rx_buf[p], &slot->rx_buf[p + p_len], drp_pkt.data.begin());
        g_telnet_tracer.trace(5, false, TraceType::DRP, drp_pkt);
        g_pkt_stats.ch5.dropped_pkts.fetch_add(1, std::memory_order_relaxed);
        p += p_len;
        continue;
      }

      Hub_ProcessPacket(slot, &slot->rx_buf[p], p_len);
      p += p_len;
    }

    if (p > 0) {
      slot->rx_len -= p;
      if (slot->rx_len > 0) {
        memmove(slot->rx_buf, slot->rx_buf + p, slot->rx_len);
      }
    }
    return;
  }

  // Slot 1~4 (에어컨): EW11 스트림 프레이밍 트래커
  char ns[16], tag[16];
  snprintf(ns, sizeof(ns), "e%d_frame", slot_idx);
  snprintf(tag, sizeof(tag), "EW11_#%d", slot_idx);

  uint8_t c_stx = slot->tracker.candidate_stx.load(std::memory_order_relaxed);
  uint8_t c_etx = slot->tracker.candidate_etx.load(std::memory_order_relaxed);
  uint8_t c_len = slot->tracker.candidate_len.load(std::memory_order_relaxed);

  uint8_t target_stx = (c_stx != 0) ? c_stx : PKT_STX;
  uint8_t target_etx = (c_etx != 0) ? c_etx : PKT_ETX;

  size_t p = 0;
  while (p < slot->rx_len) {
    if (slot->rx_buf[p] != target_stx) {
      p++;
      continue;
    }

    if (c_len >= 3 && (p + c_len) <= slot->rx_len) {
      if (slot->rx_buf[p + c_len - 1] == target_etx) {
        slot->tracker.processFrame(target_stx, target_etx, c_len, ns, tag);
        Hub_ProcessPacket(slot, &slot->rx_buf[p], c_len);
        p += c_len;
        continue;
      }
    }

    size_t end_idx = 0;
    bool found_frame = false;
    for (size_t k = p + 2; k < slot->rx_len && (k - p) < 64; ++k) {
      if (slot->rx_buf[k] == target_etx) {
        end_idx = k;
        found_frame = true;
        break;
      }
    }

    if (found_frame) {
      uint8_t frame_len = static_cast<uint8_t>(end_idx - p + 1);
      slot->tracker.processFrame(target_stx, target_etx, frame_len, ns, tag);
      Hub_ProcessPacket(slot, &slot->rx_buf[p], frame_len);
      p += frame_len;
    } else {
      if (slot->rx_len - p < 64) {
        break;
      } else {
        p++;
      }
    }
  }

  if (p > 0) {
    slot->rx_len -= p;
    if (slot->rx_len > 0) {
      memmove(slot->rx_buf, slot->rx_buf + p, slot->rx_len);
    }
  }
}

} // namespace Ew11Manager
