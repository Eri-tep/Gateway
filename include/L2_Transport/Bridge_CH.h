#pragma once

// ============================================================================
// Bridge_CH: Level 2 EW11 TCP Bridge Transport Channel
// Transport Independent Leaf (AGENTS.md Rule 17)
// ============================================================================

#include "L0_Foundation/System_Buffer.h"
#include "L0_Foundation/System_Config.h"
#include <cstddef>
#include <cstdint>
#include <span>
#include <sys/select.h>

// ── EW11 Slot Snapshot & Management API (0-extern 정보 은닉) ──
bool Bridge_GetSlotSnapshot(uint8_t slot_idx, HubClientSlotSnapshot &out);
bool Bridge_SetSlotEnabled(uint8_t slot_idx, bool enabled);
bool Bridge_SetFramingLock(uint8_t slot_idx, uint8_t stx, uint8_t etx, uint8_t len);
bool Bridge_ResetFramingTracker(uint8_t slot_idx);

bool Bridge_SetSlot(uint8_t slot_idx, bool enabled, const char *ip, uint16_t port,
                    const char *name = nullptr);
inline bool Hub_SetSlot(uint8_t slot_idx, bool enabled, const char *ip, uint16_t port,
                        const char *name = nullptr) {
  return Bridge_SetSlot(slot_idx, enabled, ip, port, name);
}

bool Bridge_SendRaw(uint8_t slot_idx, const uint8_t *data, size_t len) noexcept;
bool Bridge_SendRaw(uint8_t slot_idx, std::span<const uint8_t> data) noexcept;

void Bridge_Init();
void Bridge_StartServer() noexcept;
void Bridge_StopServer() noexcept;
void Bridge_ShutdownSockets() noexcept;

// ── Core 0 Network Reactor Interface ──
void Bridge_PopulateFds(fd_set &readfds, fd_set &errorfds, int &max_fd) noexcept;
void Bridge_ProcessEvents(fd_set &readfds, fd_set &errorfds, bool ota_now) noexcept;
void Bridge_Tick(bool ota_now, uint32_t now_ms) noexcept;

bool Bridge_ForwardPacket(uint8_t slot_idx, const StaticPacket &pkt,
                          bool burst) noexcept;

// ── L2 Bridge Packet Dispatcher SPI ──
struct Bridge_PacketDispatcher {
  uint8_t (*onGetStx)() noexcept = nullptr;
  int (*onExtractLength)(const uint8_t *data, size_t len, size_t offset) noexcept = nullptr;
  bool (*onValidatePacket)(const uint8_t *data, size_t len) noexcept = nullptr;
  void (*onPacketReceived)(uint8_t slot_idx, const StaticPacket &pkt) noexcept = nullptr;
};

void Bridge_RegisterDispatcher(const Bridge_PacketDispatcher &dispatcher) noexcept;

// ── Higher-Layer Inversion Hooks (Zero Upward Includes, Rule 17) ──
using BridgeRxCallback = void (*)(uint8_t slot_idx, const uint8_t *data, size_t len) noexcept;
using BridgeTickCallback = void (*)(uint32_t now_ms) noexcept;

void Bridge_RegisterSlotRxCallback(BridgeRxCallback cb) noexcept;
void Bridge_RegisterSlotTickCallback(BridgeTickCallback cb) noexcept;

inline void Bridge_RegisterFcuRxCallback(BridgeRxCallback cb) noexcept {
  Bridge_RegisterSlotRxCallback(cb);
}
inline void Bridge_RegisterFcuTickCallback(BridgeTickCallback cb) noexcept {
  Bridge_RegisterSlotTickCallback(cb);
}
