#pragma once

// ============================================================================
// Wifi_Driver.h — Level 1 Physical HAL Wi-Fi Driver
// Encapsulated ESP32 RF/PHY Hardware Driver & Lifecycle Manager
// ============================================================================

#include "L0_Foundation/System_Platform.h"
#include <WiFi.h>

void Wifi_Driver_Init();
bool Wifi_Driver_IsConnected() noexcept;
IPAddress Wifi_Driver_GetIp() noexcept;
int8_t Wifi_Driver_GetRssi() noexcept;
void Wifi_Driver_Reconnect() noexcept;
