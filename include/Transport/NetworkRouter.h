#pragma once

// ============================================================================
// NetworkRouter: Level 2 IP Routing, Subnet Whitelist & Port Mapping
// ============================================================================

#include "Base/SystemPlatform.h"
#include "System/LockUtils.h"
#include "Transport/TransportTypes.h"
#include <IPAddress.h>
#include <SoftwareSerial.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

extern SoftwareSerial g_doorphone_serial;
SoftwareSerialConfig Door_SerialConfig(uint8_t data_bits, uint8_t parity,
                                       uint8_t stop_bits);

// ── IP Subnet Whitelist Filters ──
[[nodiscard]] bool Tcp_IsAllowedIP(IPAddress ip);
[[nodiscard]] bool Telnet_IsAllowedIP(IPAddress ip);

// ── Physical UART Dynamic Configuration ──
bool System_ApplyUartConfig(uint8_t ch, uint32_t baud, const char *format);

// ── Routing Table & Multi-Channel Dispatch Registry ──

struct DeviceRouteEntry {
  uint8_t dev_id{0};
  uint8_t sub1{0};
  uint8_t sub2{0};
  RouteEndpoint endpoint{};
};

class DeviceRouteRegistry {
public:
  static constexpr size_t MAX_ROUTES = 64;

private:
  DeviceRouteEntry _entries[MAX_ROUTES]{};
  size_t _count{0};
  mutable portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;

public:
  void recordRoute(uint8_t channel_id, int8_t slot_idx, uint8_t dev_id,
                   uint8_t sub1, uint8_t sub2);
  [[nodiscard]] std::optional<RouteEndpoint>
  lookupRoute(uint8_t dev_id, uint8_t sub1, uint8_t sub2) const noexcept;
  [[nodiscard]] bool lookupRoute(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                                 RouteEndpoint &out_ep) const;
  [[nodiscard]] size_t getRoutes(std::span<DeviceRouteEntry> out_buf) const noexcept;
  [[nodiscard]] size_t getRoutes(DeviceRouteEntry *out_buf,
                                 size_t max_count) const;
  void clear();
};

extern DeviceRouteRegistry g_route_registry;
