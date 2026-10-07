// ============================================================================
// Routing_Engine.cpp — L3 Routing / Protocol Layer (Private Core Engine)
// Packet Routing Hub Implementation
// ============================================================================
//
// INVARIANTS (AGENTS.md):
//   - No #include of L4 headers (Services).
//   - No dynamic allocation in any path (zero heap).
//   - g_route_registry is accessed only via Router_* API (Rule 17).
// ============================================================================

#include "L3_Protocol/Private/Routing_Engine.h"
#include "L3_Protocol/Private/Wallpad_Engine.h"
#include "L2_Transport/RS485_CH.h"
#include "L0_Foundation/System_Config.h"

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

  [[nodiscard]] size_t getRoutes(std::span<DeviceRouteEntry> out_span) const noexcept {
    if (out_span.empty()) return 0;
    CriticalSectionLocker lock(&_mux);
    const size_t n = std::min(_count, out_span.size());
    for (size_t i = 0; i < n; i++) {
      out_span[i] = _entries[i];
    }
    return n;
  }

  [[nodiscard]] size_t getRoutes(DeviceRouteEntry *out_buf,
                                 size_t max_count) const noexcept {
    if (!out_buf || max_count == 0) return 0;
    return getRoutes(std::span<DeviceRouteEntry>(out_buf, max_count));
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

size_t Router_GetRoutes(std::span<DeviceRouteEntry> out_buf) noexcept {
  return s_route_registry.getRoutes(out_buf);
}

size_t Router_GetRoutes(DeviceRouteEntry *out_buf, size_t max_count) noexcept {
  if (!out_buf || max_count == 0) return 0;
  return s_route_registry.getRoutes(std::span<DeviceRouteEntry>(out_buf, max_count));
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

// ── Canonical L3 Central Ingress & Orchestration Implementations ────────────

bool Router_DispatchControl(StaticPacket &req,
                            StaticPacket &virtual_ack_out) noexcept {
  if (UNLIKELY(req.length < 5))
    return false;

  bool unidir = false;
  ControlAction act = Wallpad_EvaluateControl(req, virtual_ack_out, unidir);

  switch (act) {
  case ControlAction::VIRTUAL_ACK_IMMEDIATE:
    return true;

  case ControlAction::TRANSMIT_LOCAL: {
    // Check if device is routed via EW11 (CH5)
    uint8_t dev_id = 0, sub1 = 0, sub2 = 0;
    auto *parser = WallpadParserFactory::getActiveParser();
    if (parser && parser->extractDeviceKey(
                      std::span<const uint8_t>(req.data.data(), req.length),
                      dev_id, sub1, sub2)) {
      RouteEndpoint ep{1, -1, 0};
      if (Router_LookupRoute(dev_id, sub1, sub2, ep) &&
          ep.channel_id == 5 && ep.slot_idx >= 0 &&
          ep.slot_idx < Config::TCP::MAX_EW11_SLOTS) {
        return Router_ForwardToCh5(static_cast<uint8_t>(ep.slot_idx), req,
                                   unidir);
      }
    }

    bool is_vip = (req.channel_id == 6);
    return RS485_EnqueueControl(req, is_vip);
  }

  case ControlAction::FORWARD_CH5: {
    uint8_t dev_id = 0, sub1 = 0, sub2 = 0;
    auto *parser = WallpadParserFactory::getActiveParser();
    if (parser && parser->extractDeviceKey(
                      std::span<const uint8_t>(req.data.data(), req.length),
                      dev_id, sub1, sub2)) {
      RouteEndpoint ep{5, -1, 0};
      if (Router_LookupRoute(dev_id, sub1, sub2, ep) &&
          ep.slot_idx >= 0 && ep.slot_idx < Config::TCP::MAX_EW11_SLOTS) {
        return Router_ForwardToCh5(static_cast<uint8_t>(ep.slot_idx), req,
                                   unidir);
      }
    }
    return false;
  }

  case ControlAction::DROP:
  default:
    return false;
  }
}
