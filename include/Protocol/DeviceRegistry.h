#pragma once

// ============================================================================
// DeviceRegistry: Level 3 Physical Device Repository & Cached State Model
// ============================================================================

#include "Base/BufferUtils.h"
#include "Base/SystemConfig.h"
#include "Protocol/ProtocolTypes.h"
#include "Protocol/ControlTemplate.h"
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
  // Unified const & non-const lookup without const_cast
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

extern DeviceRepository g_device_repo;
