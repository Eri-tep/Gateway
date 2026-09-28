#pragma once

#include "Common.h"
#include <cstdint>
#include <cstddef>

/**
 * @brief EW11 TCP 브릿지 전용 격리 관리자
 * 
 * 월패드 RS-485 메인 프로토콜과 완전히 격리되어 동작하는 TCP 기반 외부 연동 모듈:
 * - Slot 0: 엘리베이터 전용 (수동 스니핑, 11B ACK 상태 전환, 13B 도착 알림 파싱, CH5 L2 라우팅 등록)
 * - Slot 1~4: 시스템 에어컨 전용 (독자 프레이밍 추적, 패킷 디코딩, 실시간 텔레메트리)
 */

namespace Fcu {

enum class Mode : uint16_t {
  Cool = 1,
  Heat = 2,
  FanOnly = 3
};

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
  // ※ 바이트 Diff 비교는 g_device_repo의 last_ack_data를 직접 사용 (기존 Ch1Engine 패턴과 동일)
  uint32_t last_poll_ms{0};
  uint32_t query_sent_ms{0};
  uint8_t timeout_count{0};
  bool waiting_response{false};
  bool is_online{false};
};

// ── 외부 공개 제어 API (MgmtRpc에서 호출, 스레드-세이프) ──
bool SetPower(uint8_t slot_idx, bool on);
bool SetMode(uint8_t slot_idx, Mode m);
bool SetFanSpeed(uint8_t slot_idx, FanSpeed f);
bool SetSwing(uint8_t slot_idx, Swing s);
bool SetTargetTemp(uint8_t slot_idx, uint8_t temp_c);

// ── 슬롯별 런타임 조회 (CLI 상태 출력용) ──
bool GetSlotRuntime(uint8_t slot_idx, SlotRuntime &out_rt);

// ── 슬롯별 20초 독립 폴링 및 응답 처리 (Ew11Manager 내부 호출) ──
void handleSlotLoop(uint8_t slot_idx, struct HubClientSlot *slot, uint32_t now);
void handleSlotRx(uint8_t slot_idx, const uint8_t *data, size_t len);

} // namespace Fcu

namespace Ew11Manager {

// EW11 매니저 초기화 (비동기 버스트 전송 esp_timer 등록)
void init();

// EW11 수신 패킷 파싱 및 처리 (HubManager::Hub_ProcessPacket에서 호출)
void processPacket(int slot_idx, const uint8_t *pkt_data, size_t pkt_len);

// EW11 TCP 스트림 프레이밍 및 패킷 추출 처리 (HubManager::Hub_Data에서 호출)
void processStream(int slot_idx, struct HubClientSlot *slot);

// EW11 선로 유휴 시간 감지 및 N회 비동기 버스트 전송 (esp_timer 기반 Non-blocking)
bool sendBurstPacket(uint8_t slot_idx, const StaticPacket &pkt, uint8_t count = 2, uint32_t silence_ms = 20);

} // namespace Ew11Manager
