// ============================================================================
// Packet_Router.cpp — L3 Routing / Protocol Layer
// Packet Routing Hub Bridge Implementation
// ============================================================================
//
// PHASED MIGRATION NOTE (Step 3 of 5):
//   This file implements the canonical Routing/Packet_Router.h API.
//   g_route_registry and its implementation remain in NetworkRouter.cpp
//   during migration. Step 5 merges routing impl here and removes
//   the DeviceRouteRegistry from Transport/NetworkRouter.
//
// INVARIANTS (AGENTS.md):
//   - No #include of L4 headers (Services).
//   - No dynamic allocation in any path (zero heap).
//   - g_route_registry is accessed only via Router_* API (Rule 17).
// ============================================================================

#include "L3_Routing/Packet_Router.h"
#include "L1_Drivers/RTOS_Driver.h"

#include <Arduino.h>
#include <algorithm>
#include <cstring>

namespace {

class DeviceRouteRegistry {
public:
  static constexpr size_t MAX_ROUTES = 64;

private:
  DeviceRouteEntry _entries[MAX_ROUTES]{};
  size_t _count{0};
  mutable portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;

public:
  void recordRoute(uint8_t channel_id, int8_t slot_idx, uint8_t dev_id,
                   uint8_t sub1, uint8_t sub2) noexcept {
    CriticalSectionLocker lock(&_mux);
    for (size_t i = 0; i < _count; ++i) {
      if (_entries[i].dev_id == dev_id && _entries[i].sub1 == sub1 &&
          _entries[i].sub2 == sub2) {
        _entries[i].endpoint.channel_id = channel_id;
        _entries[i].endpoint.slot_idx = slot_idx;
        _entries[i].endpoint.last_seen_ms = millis();
        return;
      }
    }
    if (_count < MAX_ROUTES) {
      size_t i = _count++;
      _entries[i].dev_id = dev_id;
      _entries[i].sub1 = sub1;
      _entries[i].sub2 = sub2;
      _entries[i].endpoint.channel_id = channel_id;
      _entries[i].endpoint.slot_idx = slot_idx;
      _entries[i].endpoint.last_seen_ms = millis();
    }
  }

  [[nodiscard]] bool lookupRoute(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                                 RouteEndpoint &out_ep) const noexcept {
    CriticalSectionLocker lock(&_mux);
    for (size_t i = 0; i < _count; ++i) {
      if (_entries[i].dev_id == dev_id && _entries[i].sub1 == sub1 &&
          _entries[i].sub2 == sub2) {
        out_ep = _entries[i].endpoint;
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] size_t getRoutes(DeviceRouteEntry *out_buf,
                                 size_t max_count) const noexcept {
    if (!out_buf || max_count == 0) return 0;
    CriticalSectionLocker lock(&_mux);
    const size_t n = std::min(_count, max_count);
    for (size_t i = 0; i < n; i++) {
      out_buf[i] = _entries[i];
    }
    return n;
  }

  void clear() noexcept {
    CriticalSectionLocker lock(&_mux);
    _count = 0;
    memset(_entries, 0, sizeof(_entries));
  }
};

static DeviceRouteRegistry s_route_registry;

} // anonymous namespace

// ── Router_RecordRoute ────────────────────────────────────────────────────────

void Router_RecordRoute(uint8_t channel_id, int8_t slot_idx,
                         uint8_t dev_id, uint8_t sub1, uint8_t sub2) noexcept {
  s_route_registry.recordRoute(channel_id, slot_idx, dev_id, sub1, sub2);
}

// ── Router_LookupRoute ────────────────────────────────────────────────────────

bool Router_LookupRoute(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                         RouteEndpoint &out_ep) noexcept {
  return s_route_registry.lookupRoute(dev_id, sub1, sub2, out_ep);
}

// ── Router_ClearRoutes ────────────────────────────────────────────────────────

void Router_ClearRoutes() noexcept {
  s_route_registry.clear();
}

// ── Router_GetRoutes ──────────────────────────────────────────────────────────

size_t Router_GetRoutes(DeviceRouteEntry *out_buf, size_t max_count) noexcept {
  if (!out_buf || max_count == 0) return 0;
  return s_route_registry.getRoutes(out_buf, max_count);
}

// ── Router_EnqueueDownlink ────────────────────────────────────────────────────
// Resolves channel_id to the canonical L2 TX enqueue API.
// channel_id:
//   1       = CH1 RS-485 main bus (normal priority)
//   2, 3    = CH2/CH3 sub-device RS-485 bus (enqueue via CH1 control path)
//   4       = CH4 Doorphone SW Serial passthrough
//   5       = CH5 EW11 virtual RS-485 TCP bridge (Bridge_ForwardPacket)
//   6       = CH1 VIP (high priority, bypasses normal queue head-of-line)

bool Router_EnqueueDownlink(uint8_t channel_id,
                              const StaticPacket &pkt) noexcept {
  switch (channel_id) {
  case 1:
    return RS485_EnqueueCh1Ctrl(pkt);
  case 6:
    return RS485_EnqueueCh1Vip(pkt);
  case 4:
    return RS485_EnqueueCh4Pass(pkt);
  // CH2, CH3: currently dispatched through CH1 control queue via ControlDispatcher.
  // Step 4 will add RS485_EnqueueCh2/Ch3 when RS485_CH.cpp owns the task loops.
  case 2:
  case 3:
    return RS485_EnqueueCh1Ctrl(pkt);
  default:
    return false;
  }
}

// ── Ch5 Forward Handler Implementation ──────────────────────────────────────

namespace {
Ch5ForwardHandler s_ch5_forwarder = nullptr;
} // namespace

void Router_RegisterCh5ForwardHandler(Ch5ForwardHandler handler) noexcept {
  s_ch5_forwarder = handler;
}

bool Router_ForwardToCh5(uint8_t slot_idx, const StaticPacket &pkt,
                         bool burst) noexcept {
  if (s_ch5_forwarder) {
    return s_ch5_forwarder(slot_idx, pkt, burst);
  }
  return false;
}
