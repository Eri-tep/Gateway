#pragma once

// ============================================================================
// RemoteService: Level 4 Network Remote Services (SmartThings JSON-RPC & EW11
// Hub)
// ============================================================================

#include "L0_Base/System_Buffer.h"
#include "L0_Base/System_Config.h"
#include "L3_Routing/ControlTemplate.h"
#include "L3_Routing/Device_Registry.h"
#include <lwip/sockets.h>
#include "L1_Drivers/SystemOta.h"

// ============================================================================
// 2. HTTP(S) Cloud OTA (Delegated to Level 1 SystemOta)
// ============================================================================
inline void Mgmt_StartHttpOta(const char *url) {
  System_StartHttpOta(url);
}

// ============================================================================
// 3. Port 8900 Management TCP Session Structure
// ============================================================================
struct MgmtSession {
  int sock{-1};
  uint8_t buffer[Config::TCP::MGMT_BUFFER_SIZE];
  size_t len{0};
  uint32_t connected_at_ms{0};
};

// ============================================================================
// 4. Management JSON-RPC Functions
// ============================================================================
void Mgmt_Init();
void Mgmt_Data(MgmtSession *s, std::span<const uint8_t> data);
inline void Mgmt_Data(MgmtSession *s, const uint8_t *data, size_t len) {
  if (data)
    Mgmt_Data(s, std::span<const uint8_t>(data, len));
}
void Mgmt_SerializeTelemetry(AppendBuf &out, long req_id = -1);
void Mgmt_SerializeDevices(AppendBuf &out, long req_id = -1);
void Mgmt_DispatchJsonRpc(int sock, const char *json_str);

void Mgmt_BroadcastDoorphoneEvent(bool front_bell, bool lobby_bell) noexcept;
void Mgmt_BroadcastDeviceState(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                               DeviceClass dev_class, int power,
                               int target_temp = 0, int current_temp = 0,
                               int speed = 0, const char *valve_state = nullptr,
                               float power_w = 0.0f, int floor = 0,
                               int direction = 0, int ho = 0,
                               int vent_mode = 1);
void Mgmt_BroadcastDeviceResult(const DeviceUpdateResult &res) noexcept;
void Mgmt_BroadcastElevatorEvent(uint8_t sub1, uint8_t sub2, uint8_t floor,
                                 uint8_t ho, uint8_t power,
                                 bool is_arrival) noexcept;
void Mgmt_BroadcastDevicesUpdated();
void Mgmt_BroadcastRawJson(const char *json_payload);

// ── Network Subsystem Entry Points ──
void Remote_Init();
void Remote_PopulateFds(fd_set &readfds, fd_set &errorfds, int &max_fd) noexcept;
void Remote_ProcessEvents(fd_set &readfds, fd_set &errorfds, bool ota_now) noexcept;
void Remote_Tick(bool ota_now, uint32_t now_ms) noexcept;

extern EventGroupHandle_t g_wifi_event_group;

// ── Remote Control Handler Registration ──
using DeviceControlHandler = bool (*)(StaticPacket &req,
                                      StaticPacket &out_ack) noexcept;
void Remote_RegisterControlHandler(DeviceControlHandler handler) noexcept;
