#pragma once

#include "L4_Services/Mgmt_Service.h"
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <atomic>

// WiFi event bits are canonically defined in L0 System_Platform.h

// 15-second Fallback Guard for remote Wi-Fi setting
struct WifiFallbackGuard {
  std::atomic<bool> testing{false};
  uint32_t start_ms{0};
  char prev_ssid[64]{0};
  char prev_pass[64]{0};
};

extern WifiFallbackGuard g_wifi_guard;

// Session & Mutex access for Port 8900
MgmtSession *Remote_GetSessions();
SemaphoreHandle_t Remote_GetSessionMutex();
IPAddress Remote_GetClientIp(int sock);
DeviceControlHandler Remote_GetControlHandler();

// Internal entry points
void Remote_SendRpcResponse(int sock, long req_id, const char *res,
                            const char *msg = nullptr);
