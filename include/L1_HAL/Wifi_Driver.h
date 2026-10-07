#pragma once

// ============================================================================
// Wifi_Driver.h — Level 1 Physical HAL Wi-Fi Driver
// Encapsulated ESP32 RF/PHY Hardware Driver & Lifecycle Manager
// ============================================================================

#include "L0_Foundation/System_Platform.h"
#include <WiFi.h>

struct WifiHwConfig {
  const char *sta_ssid{nullptr};
  const char *sta_password{nullptr};
  uint16_t timeout_s{30};
  const char *ap_ssid{nullptr};
  const char *ap_password{nullptr};
};

void Wifi_Driver_Init(const WifiHwConfig &cfg);
[[nodiscard]] bool Wifi_Driver_IsConnected() noexcept;
[[nodiscard]] IPAddress Wifi_Driver_GetIp() noexcept;
[[nodiscard]] int8_t Wifi_Driver_GetRssi() noexcept;
void Wifi_Driver_Reconnect() noexcept;
[[nodiscard]] EventBits_t Wifi_Driver_GetEventBits() noexcept;
void Wifi_Driver_StartFallbackAp() noexcept;
[[nodiscard]] bool Wifi_Driver_IsApActive() noexcept;
