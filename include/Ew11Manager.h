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
