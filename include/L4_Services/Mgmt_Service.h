#pragma once

// ============================================================================
// RemoteService: Level 4 Network Remote Services (SmartThings JSON-RPC & EW11
// Hub)
// ============================================================================

#include "L0_Foundation/System_Buffer.h"
#include "L0_Foundation/System_Config.h"
#include "L3_Protocol/Public/Protocol_Device.h"
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <span>
#include <sys/select.h>

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

void Mgmt_BroadcastDeviceState(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                               DeviceClass dev_class, int power,
                               int target_temp = 0, int current_temp = 0,
                               int speed = 0, const char *valve_state = nullptr,
                               float power_w = 0.0f, int floor = 0,
                               int direction = 0, int ho = 0,
                               int vent_mode = 1);
void Mgmt_BroadcastDevicesUpdated();
void Mgmt_BroadcastRawJson(const char *json_payload);
void Mgmt_DrainTelemetryQueue() noexcept;

// ── Network Subsystem Entry Points & Lifecycle ──
void Remote_Init();
void Remote_StartServer() noexcept;
void Remote_StopServer() noexcept;
void Remote_PopulateFds(fd_set &readfds, fd_set &errorfds, int &max_fd) noexcept;
void Remote_ProcessEvents(fd_set &readfds, fd_set &errorfds, bool ota_now) noexcept;
void Remote_Tick(bool ota_now, uint32_t now_ms) noexcept;

// ── Management Remote Server Session & Fallback Contracts (Rule 17) ──
MgmtSession *Remote_GetSessions();
SemaphoreHandle_t Remote_GetSessionMutex();
IPAddress Remote_GetClientIp(int sock);
void Remote_SendRpcResponse(int sock, long req_id, const char *res,
                            const char *msg = nullptr);
void Remote_StartWifiFallbackTest(const char *prev_ssid, const char *prev_pass) noexcept;
void Remote_ResetTrustedHubIp() noexcept;
IPAddress Remote_GetTrustedHubIp() noexcept;
