#pragma once

// ============================================================================
// Device_Registry.h — L3 Routing / Protocol Layer
// Canonical L3 Device State SSOT (Single Source of Truth)
// Canonical 4+1 Layer: L3 — depends only on L2↓, L1↓, L0↓. Zero L4 includes.
// ============================================================================
//
// This header is the canonical L3 entry point for device state management.
// During phased migration (Step 3), it re-exports the legacy DeviceRegistry
// and adds the new Domain_VerbNoun accessor APIs that replace raw extern access.
//
// SSOT Rules:
//   - All device state writes must go through Device_UpdateFromBus() or
//     Device_SetDesiredState() — never modify DeviceStateEntry fields directly.
//   - All reads must use Device_GetSnapshot() or Device_FindEntry() — never
//     store raw pointers across task boundaries.
//   - g_device_repo is static-sealed in DeviceRegistry.cpp (Rule 17).
//     External callers use the functional API below, NOT the extern object.
// ============================================================================

#include "L0_Base/System_Buffer.h"
#include "L0_Base/System_Config.h"
#include "L3_Routing/ProtocolTypes.h"
#include "L3_Routing/ControlTemplate.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

struct DeviceStateEntry {
  uint8_t dev_id;
  uint8_t sub1, sub2;
  std::array<uint8_t, 64> last_ack_data;
  uint8_t last_ack_len{0};
  uint8_t last_target_temp{0};
  uint8_t last_current_temp{0};
  uint32_t last_updated_ms{0};
  mutable uint32_t last_stale_poll_ms{0};
  uint8_t timeout_count{0};
  bool is_online{false};

  [[nodiscard]] bool isStale() const noexcept {
    return last_updated_ms > 0 &&
           TimeUtils::isElapsed(last_updated_ms,
                                Config::Timing::STALE_DEVICE_THRESHOLD_MS);
  }
};

struct DeviceUpdateResult {
  bool updated{false};
  bool should_broadcast{false};
  uint8_t dev_id{0};
  uint8_t sub1{0};
  uint8_t sub2{0};
  DecodedDeviceState state{};
  uint8_t extra_count{0};
  struct ExtraDevice {
    uint8_t sub1{0};
    DecodedDeviceState state{};
  } extra[7]{};
};

class DeviceRepository {
private:
  static constexpr size_t MAX_DEVICES = 48;
  DeviceStateEntry cache[MAX_DEVICES]{};
  int8_t dev_lookup_map[256]{};
  size_t device_count = 0;
  SemaphoreHandle_t _cache_mutex = nullptr;

  const DeviceStateEntry *findInternal(uint8_t dev_id, uint8_t sub1,
                                       uint8_t sub2) const noexcept;
  DeviceStateEntry *findInternal(uint8_t dev_id, uint8_t sub1,
                                 uint8_t sub2) noexcept;

public:
  [[nodiscard]] const DeviceStateEntry *
  findEntry(uint8_t dev_id, uint8_t sub1, uint8_t sub2) const noexcept {
    return findInternal(dev_id, sub1, sub2);
  }
  [[nodiscard]] DeviceStateEntry *
  findEntry(uint8_t dev_id, uint8_t sub1, uint8_t sub2) noexcept {
    return findInternal(dev_id, sub1, sub2);
  }

  DeviceStateEntry *findMutable(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                                bool auto_create = false) noexcept;
  void initDevices();
  void clear();
  [[nodiscard]] const DeviceStateEntry *find(uint8_t dev_id, uint8_t sub1,
                                             uint8_t sub2) const noexcept;
  [[nodiscard]] const DeviceStateEntry *getAt(size_t index) const noexcept;
  [[nodiscard]] DeviceStateEntry *getAt(size_t index) noexcept;
  [[nodiscard]] bool getSnapshot(size_t index,
                                 DeviceStateEntry &out_copy) noexcept;
  [[nodiscard]] size_t count() const noexcept;
  [[nodiscard]] size_t getOnlineCount() const noexcept;
  void setLastStalePollMs(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                          uint32_t ms) noexcept;
  void setLastStalePollMsByIndex(size_t index, uint32_t ms) noexcept;
  bool setTargetTemp(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                     uint8_t temp) noexcept;
  DeviceUpdateResult updateFromBus(StaticPacket &ack);
  static void decodeDeviceState(const GroupControlTemplate &grp,
                                const StaticPacket &ack,
                                const DeviceStateEntry *dev,
                                DecodedDeviceState &out);
  void handlePollingTimeout(const DeviceStateEntry *dev);
  void handlePollingTimeout(uint8_t dev_id, uint8_t sub1, uint8_t sub2);
  [[nodiscard]] bool copyVirtualAck(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                                    StaticPacket &out) noexcept;
};

// ── Device State Query & Management API (L3 SSOT Snapshot Interface) ─────────

/// Initialise device repository tables.
void Device_Init() noexcept;

/// Clear all registered devices from the cache.
void Device_Clear() noexcept;

/// Returns a read-only snapshot of a device entry by (dev_id, sub1, sub2).
/// Thread-safe copy. Returns false if device not found.
/// @param out_copy  Filled with cached state on success.
[[nodiscard]] bool Device_GetSnapshot(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                                       DeviceStateEntry &out_copy) noexcept;

/// Returns snapshot of device at given index (for iteration).
/// Returns false if index out of range.
[[nodiscard]] bool Device_GetSnapshot(size_t index,
                                       DeviceStateEntry &out_copy) noexcept;
[[nodiscard]] bool Device_GetSnapshotAt(size_t index,
                                         DeviceStateEntry &out_copy) noexcept;

/// Returns current online device count (all channels combined).
[[nodiscard]] size_t Device_GetOnlineCount() noexcept;

/// Returns total registered device count.
[[nodiscard]] size_t Device_GetCount() noexcept;

/// Find pointer to device entry (read-only inspect).
[[nodiscard]] const DeviceStateEntry *Device_Find(uint8_t dev_id, uint8_t sub1,
                                                  uint8_t sub2) noexcept;

/// Get device entry at index (read-only inspect).
[[nodiscard]] const DeviceStateEntry *Device_GetAt(size_t index) noexcept;

/// Register FCU sub-device slot into repository cache.
void Device_RegisterFcu(uint8_t slot_idx) noexcept;

/// Synchronize FCU runtime snapshot state into repository cache.
void Device_SyncFcuState(uint8_t slot_idx, uint8_t target_temp, uint8_t room_temp,
                         bool is_online, const uint8_t *raw_pkt, size_t raw_len) noexcept;

/// Record polling timeout for device.
void Device_HandlePollingTimeout(uint8_t dev_id, uint8_t sub1, uint8_t sub2) noexcept;

/// Update last stale poll timestamp for device.
void Device_SetLastStalePollMs(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                              uint32_t ms) noexcept;
void Device_SetLastStalePollMsByIndex(size_t index, uint32_t ms) noexcept;

// ── Device State Update API (called by L3 Packet_Router after bus decode) ─────

/// Update device cached state from a raw bus ACK packet.
/// Parses, decodes, and stores into SSOT. Returns update result with event.
/// Zero heap — all processing on stack-local temporaries.
DeviceUpdateResult Device_UpdateFromBus(StaticPacket &ack_pkt) noexcept;

/// Set desired target temperature for a thermostat device.
/// Used by L4 service (CTL_Service) when SmartThings sends a setpoint command.
bool Device_SetTargetTemp(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                           uint8_t temp) noexcept;

/// Copy the last known virtual ACK frame for a device into out_pkt.
/// Used by L3 Packet_Router for cache virtual response (Fast-Path).
/// Returns false if no cached ACK available.
[[nodiscard]] bool Device_CopyVirtualAck(uint8_t dev_id, uint8_t sub1,
                                          uint8_t sub2,
                                          StaticPacket &out_pkt) noexcept;

// ── L4 State Change Listener Subscription API ────────────────────────────────
using DeviceStateListener    = void (*)(const DeviceUpdateResult &res) noexcept;
using DoorphoneEventListener = void (*)(bool front_bell, bool lobby_bell) noexcept;

void Device_RegisterStateListener(DeviceStateListener listener) noexcept;
void Device_RegisterDoorphoneListener(DoorphoneEventListener listener) noexcept;

/// Process incoming bus ACK packet: updates SSOT cache and dispatches to registered listener.
void Device_ProcessBusPacket(StaticPacket &ack_pkt) noexcept;

/// Dispatch doorphone bell state change to registered listener.
void Device_NotifyDoorphoneEvent(bool front_bell, bool lobby_bell) noexcept;
