#pragma once

// ============================================================================
// BridgeService: Level 4 EW11 TCP Bridge & Air Conditioner (FCU) Subsystem
// ============================================================================

#include "Base/BufferUtils.h"
#include "Base/SystemConfig.h"
#include "Protocol/DeviceRegistry.h"
#include "Protocol/ModbusProtocol.h"

namespace Fcu {

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
  uint8_t pending_cmd_buf[16]{};    // RS-485 Stop-and-Wait 대기 명령 버퍼
  uint8_t pending_cmd_len{0};       // 대기 중인 명령 패킷 길이
  uint8_t pending_restore_swing{0}; // 전원 켜기 복원 시 스윙값
};

// ── 외부 공개 제어 API ──
bool SetPower(uint8_t slot_idx, bool on);
bool RestorePower(uint8_t slot_idx, uint16_t mode, uint16_t fan, uint16_t swing,
                  uint8_t temp);
bool SetMode(uint8_t slot_idx, Mode m);
bool SetFanSpeed(uint8_t slot_idx, FanSpeed f);
bool SetSwing(uint8_t slot_idx, Swing s);
bool SetTargetTemp(uint8_t slot_idx, uint8_t temp_c);
bool GetSlotRuntime(uint8_t slot_idx, SlotRuntime &out_rt);
void handleSlotLoop(uint8_t slot_idx, HubClientSlot *slot, uint32_t now);
void handleSlotRx(uint8_t slot_idx, const uint8_t *data, size_t len);

} // namespace Fcu

namespace Ew11Manager {
void init();
void processPacket(int slot_idx, const uint8_t *pkt_data, size_t pkt_len);
void processStream(int slot_idx, HubClientSlot *slot);
bool sendBurstPacket(uint8_t slot_idx, const StaticPacket &pkt,
                     uint8_t count = 2, uint32_t silence_ms = 20);
} // namespace Ew11Manager

int Hub_AcceptClient(int slot_idx, int server_fd);
void Hub_ProcessPacket(HubClientSlot *slot, const uint8_t *pkt_data,
                       size_t pkt_len);
void Hub_Data(HubClientSlot *slot, const uint8_t *data, size_t len);
void Hub_LoadConfig();
void Hub_SaveConfig();
bool Hub_SetSlot(uint8_t slot_idx, bool enabled, const char *ip, uint16_t port,
                 const char *name = nullptr);
bool Hub_SendPacket(uint8_t slot_idx, const StaticPacket &pkt);
void Bridge_ShutdownSockets() noexcept;

// ── Bridge Event Listeners & Forwarding API ──
using BridgeDeviceStateListener = void (*)(const DeviceUpdateResult &res) noexcept;
using ElevatorStateListener = void (*)(uint8_t sub1, uint8_t sub2, uint8_t floor,
                                       uint8_t ho, uint8_t power,
                                       bool is_arrival) noexcept;

void Bridge_RegisterDeviceStateListener(BridgeDeviceStateListener listener) noexcept;
void Bridge_RegisterElevatorListener(ElevatorStateListener listener) noexcept;
bool Bridge_ForwardPacket(uint8_t slot_idx, const StaticPacket &pkt,
                          bool burst) noexcept;
