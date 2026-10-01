#pragma once

// ============================================================================
// NetworkRouter: Level 2 IP Routing, Subnet Whitelist & Port Mapping
// ============================================================================

#include "Base/SystemPlatform.h"
#include "System/LockUtils.h"
#include <IPAddress.h>
#include <algorithm>
#include <cstdint>
#include <cstring>

// ── IP Subnet Whitelist Filters ──
[[nodiscard]] bool Tcp_IsAllowedIP(IPAddress ip);
[[nodiscard]] bool Telnet_IsAllowedIP(IPAddress ip);

// ── Physical UART Dynamic Configuration ──
bool System_ApplyUartConfig(uint8_t ch, uint32_t baud, const char *format);

// ── Routing Table & Multi-Channel Dispatch Registry ──
struct RouteEndpoint {
  uint8_t channel_id{1}; // 기본 채널: CH1 (메인 물리 RS-485)
  int8_t slot_idx{-1};   // CH5인 경우 슬롯 인덱스 (0~4), 그 외 -1
  uint32_t last_seen_ms{0};
};

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
  [[nodiscard]] bool lookupRoute(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                                 RouteEndpoint &out_ep) const;
  [[nodiscard]] size_t getRoutes(DeviceRouteEntry *out_buf,
                                 size_t max_count) const;
  void clear();
};

extern DeviceRouteRegistry g_route_registry;
