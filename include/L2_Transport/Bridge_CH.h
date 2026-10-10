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

enum class HubDeviceType : uint8_t {
  WALLPAD_COMPATIBLE = 0,
  AIR_CONDITIONER = 1
};

struct HubClientSlotSnapshot {
  bool enabled{false};
  bool is_connected{false};
  char name[16]{""};
  char target_ip[16]{""};
  uint16_t target_port{0};
  HubDeviceType dev_type{HubDeviceType::WALLPAD_COMPATIBLE};
  uint32_t last_rx_ms{0};
  uint32_t rx_pkts{0};
  uint32_t tx_pkts{0};
  uint32_t crc_errors{0};
  uint32_t invalid_frames{0};
  uint32_t timeouts{0};
  uint32_t uncached_pkts{0};
  uint32_t dropped_pkts{0};
};

// ── EW11 Slot Snapshot & Management API (0-extern 정보 은닉) ──
bool Bridge_GetSlotSnapshot(uint8_t slot_idx, HubClientSlotSnapshot &out);
bool Bridge_IsSlotOnline(uint8_t slot_idx) noexcept;
bool Bridge_SetSlotEnabled(uint8_t slot_idx, bool enabled);
bool Bridge_SetFramingLock(uint8_t slot_idx, uint8_t stx, uint8_t etx, uint8_t len);
bool Bridge_ResetFramingTracker(uint8_t slot_idx);

bool Bridge_SetSlot(uint8_t slot_idx, bool enabled, const char *ip, uint16_t port,
                    const char *name = nullptr);

bool Bridge_SendRaw(uint8_t slot_idx, const uint8_t *data, size_t len) noexcept;
bool Bridge_SendRaw(uint8_t slot_idx, std::span<const uint8_t> data) noexcept;
void Bridge_RecordSlotRx(uint8_t slot_idx) noexcept;
void Bridge_RecordSlotCrcError(uint8_t slot_idx) noexcept;
void Bridge_RecordSlotInvalidFrame(uint8_t slot_idx) noexcept;
void Bridge_RecordSlotTimeout(uint8_t slot_idx) noexcept;
void Bridge_RecordSlotUncached(uint8_t slot_idx) noexcept;
void Bridge_ResetStats() noexcept;

void Bridge_Init();
void Bridge_StartServer() noexcept;
void Bridge_StopServer() noexcept;
void Bridge_ShutdownSockets() noexcept;

// ── Core 0 Network Reactor Interface ──
void Bridge_PopulateFds(fd_set &readfds, fd_set &errorfds, int &max_fd) noexcept;
void Bridge_ProcessEvents(fd_set &readfds, fd_set &errorfds, bool ota_now) noexcept;
void Bridge_Tick(bool ota_now, uint32_t now_ms) noexcept;
bool Bridge_HasActiveClients() noexcept;

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
using BridgeRxCallback = size_t (*)(uint8_t slot_idx, std::span<const uint8_t> stream) noexcept;
using BridgeTickCallback = void (*)(uint32_t now_ms) noexcept;

struct BridgeSlotDriver {
  BridgeRxCallback onRxStream{nullptr};
  BridgeTickCallback onTick{nullptr};
};

void Bridge_RegisterSlotDriver(uint8_t slot_idx, const BridgeSlotDriver &driver) noexcept;
void Bridge_RegisterSlotRxCallback(BridgeRxCallback cb) noexcept;
void Bridge_RegisterSlotTickCallback(BridgeTickCallback cb) noexcept;
