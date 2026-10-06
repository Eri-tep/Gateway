#pragma once

// ============================================================================
// Packet_Router.h — L3 Routing / Protocol Layer
// Central L3 Packet Routing Hub: U-Turn Bypass, Cache Virtual Response,
// Route Registry, and Channel Dispatch
// Canonical 4+1 Layer: L3 — depends only on L2↓ (RS485_CH/TCP_CH), L1↓, L0↓.
//                           Zero L4 includes. Zero horizontal L3 coupling.
// ============================================================================
//
// Architecture invariants:
//   1. Packet_Router is the ONLY L3 Hub. All other L3 modules (codecs,
//      Device_Registry) are pure Leaf modules called only by this Router.
//   2. U-Turn bypass (CH1 ↔ CH2/3) occurs entirely within this module —
//      L4 is never involved in channel-to-channel routing.
//   3. Cache virtual response: CH2/3 polling is answered directly from
//      Device_Registry cache and returned to CH2/3 — CH1 traffic = 0%.
//   4. g_route_registry is static-sealed in NetworkRouter.cpp (Rule 17).
//      External callers use Router_RecordRoute / Router_LookupRoute API.
// ============================================================================

#include "L0_Foundation/System_Buffer.h"
#include <cstddef>
#include <cstdint>
// ── Multi-Channel Route Endpoint & Entry PODs ──────────────────────────────
struct RouteEndpoint {
  uint8_t channel_id{1}; // Default channel: CH1 (Main Physical RS-485)
  int8_t slot_idx{-1};   // If CH5, EW11 slot index (0~4), otherwise -1
  uint32_t last_seen_ms{0};
};

struct DeviceRouteEntry {
  uint8_t dev_id{0};
  uint8_t sub1{0};
  uint8_t sub2{0};
  RouteEndpoint endpoint{};
};

// ── Route Registry API (replaces direct g_route_registry extern access) ───────

/// Record a channel route for a device (dev_id, sub1, sub2) → (channel, slot).
/// Called during device discovery / polling registration.
void Router_RecordRoute(uint8_t channel_id, int8_t slot_idx,
                         uint8_t dev_id, uint8_t sub1, uint8_t sub2) noexcept;

/// Lookup the route endpoint for a device.
/// Returns false if device has no registered route (defaults to CH1).
[[nodiscard]] bool Router_LookupRoute(uint8_t dev_id, uint8_t sub1,
                                       uint8_t sub2,
                                       RouteEndpoint &out_ep) noexcept;

/// Clear all registered routes (called on profile change / factory reset).
void Router_ClearRoutes() noexcept;

/// Copy all route entries into caller-supplied buffer for inspection/export.
/// Returns number of entries copied.
[[nodiscard]] size_t Router_GetRoutes(DeviceRouteEntry *out_buf,
                                       size_t max_count) noexcept;

// ── Downlink Enqueue API (L4 → L3 → L2 dispatch) ────────────────────────────

/// Enqueue a packet for downlink transmission on the specified channel.
/// Resolves channel_id to the correct L2 TX queue (RS485 or TCP).
/// Normal priority. Non-blocking — drops head if queue full.
/// @param channel_id  1=CH1, 2=CH2, 3=CH3, 4=CH4(doorphone), 5=EW11(TCP), 6=CH1_VIP
[[nodiscard]] bool Router_EnqueueDownlink(uint8_t channel_id,
                                           const StaticPacket &pkt) noexcept;

// ── CH5 Forward Handler Subscription API ────────────────────────────────────
using Ch5ForwardHandler = bool (*)(uint8_t slot_idx, const StaticPacket &pkt,
                                    bool burst) noexcept;

void Router_RegisterCh5ForwardHandler(Ch5ForwardHandler handler) noexcept;
bool Router_ForwardToCh5(uint8_t slot_idx, const StaticPacket &pkt,
                         bool burst) noexcept;


/// Dispatches an RX packet received from physical bus (CH1~CH4) into L3 domain decoders.
void Router_HandleBusPacket(uint8_t channel_id, const StaticPacket &ack_pkt,
                            const StaticPacket *matching_query) noexcept;

/// Assembles the next device polling packet for CH1 transmission.
bool Router_BuildNextPoll(StaticPacket &out_pkt, uint8_t &poll_dev_id,
                          uint8_t &poll_sub1, uint8_t &poll_sub2) noexcept;

/// Notifies L3 subsystem of polling timeout on a target device.
void Router_HandlePollTimeout(uint8_t poll_dev_id, uint8_t poll_sub1,
                              uint8_t poll_sub2) noexcept;

/// Handles sub-bus (CH2/CH3) query packets with cache virtual response.
bool Router_HandleSubBusQuery(uint8_t channel_id, const StaticPacket &req,
                              StaticPacket &virtual_ack_out) noexcept;

/// Dispatches a control or query request: handles virtual ACK, routing to CH5, or enqueuing to local bus.
[[nodiscard]] bool Router_DispatchControl(StaticPacket &req, StaticPacket &virtual_ack_out) noexcept;

// ── Bridge Transport Slot Control API (L4 → L3 Gateway) ──────────────────────
bool Router_SetBridgeSlotEnabled(uint8_t slot_idx, bool enabled) noexcept;
bool Router_SetBridgeSlotConfig(uint8_t slot_idx, bool enabled, const char *ip,
                                uint16_t port, const char *name) noexcept;
bool Router_SetBridgeFramingLock(uint8_t slot_idx, uint8_t stx, uint8_t etx, uint8_t len) noexcept;
bool Router_ResetBridgeFraming(uint8_t slot_idx) noexcept;
[[nodiscard]] bool Router_IsBridgeSlotOnline(uint8_t slot_idx) noexcept;
bool Router_SendBridgeRaw(uint8_t slot_idx, const uint8_t *data, size_t len) noexcept;
void Router_RecordBridgeSlotRx(uint8_t slot_idx) noexcept;



