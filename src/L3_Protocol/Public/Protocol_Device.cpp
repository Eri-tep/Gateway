// ============================================================================
// DeviceRegistry: Level 3 Physical Device Repository & Cached State Model
// Implementation
// ============================================================================

#include "L3_Protocol/Public/Protocol_Device.h"
#include "L3_Protocol/Private/Fcu_Engine.h"
#include "L3_Protocol/Private/Wallpad_Engine.h"
#include "L3_Protocol/Private/Wallpad_Learning.h"
#include "L3_Protocol/Private/Routing_Engine.h"
#include "L2_Transport/Bridge_CH.h"
#include "L0_Foundation/Lockless_RingBuffer.h"
#include "L0_Foundation/System_Platform.h"
#include "L0_Foundation/Seqlock.h"

#include <Arduino.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {

std::atomic<bool> s_dp_front_bell{false};
std::atomic<bool> s_dp_lobby_bell{false};
std::atomic<uint32_t> s_dp_last_bell_ms{0};
static std::atomic<uint32_t> s_cache_lock_timeouts{0};

static inline uint8_t Device_NormSub1(uint8_t dev_id, uint8_t sub1) noexcept {
  return ControlTemplate_NormSub1(dev_id, sub1);
}

static inline uint8_t Device_Hash(uint8_t dev_id, uint8_t sub1,
                                  uint8_t sub2) noexcept {
  return Hash::deviceKey8(dev_id, sub1, sub2);
}

static inline void copyEntryBounded(DeviceStateEntry &dst,
                                    const DeviceStateEntry &src) noexcept {
  dst.dev_id = src.dev_id;
  dst.sub1 = src.sub1;
  dst.sub2 = src.sub2;
  const uint8_t ack_len =
      std::min<uint8_t>(src.last_ack_len, static_cast<uint8_t>(dst.last_ack_data.size()));
  dst.last_ack_len = ack_len;
  if (ack_len > 0) {
    memcpy(dst.last_ack_data.data(), src.last_ack_data.data(), ack_len);
  }
  dst.last_target_temp = src.last_target_temp;
  dst.last_current_temp = src.last_current_temp;
  dst.last_updated_ms = src.last_updated_ms;
  dst.last_stale_poll_ms = src.last_stale_poll_ms;
  dst.timeout_count = src.timeout_count;
  dst.is_online = src.is_online;
}

class DeviceRepository {
private:
  static constexpr size_t MAX_DEVICES = 48;
  DeviceStateEntry cache[MAX_DEVICES]{};
  int8_t dev_lookup_map[256]{};
  std::atomic<size_t> _device_count{0};
  std::atomic<size_t> _online_count{0};
  mutable portMUX_TYPE _cache_mux = portMUX_INITIALIZER_UNLOCKED;
  mutable Gateway::Foundation::SequenceLock _seqlock;

  const DeviceStateEntry *findInternalFast(uint8_t dev_id, uint8_t norm_sub1,
                                           uint8_t sub2, uint8_t h) const noexcept;
  DeviceStateEntry *findInternalFast(uint8_t dev_id, uint8_t norm_sub1,
                                     uint8_t sub2, uint8_t h) noexcept;

  const DeviceStateEntry *findInternal(uint8_t dev_id, uint8_t sub1,
                                       uint8_t sub2) const noexcept;
  DeviceStateEntry *findInternal(uint8_t dev_id, uint8_t sub1,
                                 uint8_t sub2) noexcept;

public:
  DeviceStateEntry *findMutableFast(uint8_t dev_id, uint8_t norm_sub1,
                                    uint8_t sub2, uint8_t h,
                                    bool auto_create = false) noexcept;
  DeviceStateEntry *findMutable(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                                bool auto_create = false) noexcept;
  void initDevices();
  void clear();
  [[nodiscard]] bool findCopy(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                              DeviceStateEntry &out_copy) const noexcept;
  [[nodiscard]] bool exists(uint8_t dev_id, uint8_t sub1,
                            uint8_t sub2) const noexcept;
  [[nodiscard]] bool getPackedState(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                                    uint64_t &out_packed) const noexcept;
  [[nodiscard]] bool getMetadata(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                                 DeviceMetadata &out_meta) const noexcept;
  [[nodiscard]] bool getSnapshot(size_t index,
                                 DeviceStateEntry &out_copy) const noexcept;
  [[nodiscard]] bool getAtMetadata(size_t index,
                                   DeviceMetadata &out_meta) const noexcept;
  [[nodiscard]] size_t getSnapshotChunk(size_t start_idx,
                                        DeviceStateEntry *out_buf,
                                        size_t max_count) const noexcept;
  [[nodiscard]] size_t count() const noexcept;
  [[nodiscard]] size_t getOnlineCount() const noexcept;
  void setLastStalePollMs(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                          uint32_t ms) noexcept;
  void setLastStalePollMsByIndex(size_t index, uint32_t ms) noexcept;
  bool setTargetTemp(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                     uint8_t temp) noexcept;
  DeviceUpdateResult updateFromBus(StaticPacket &ack);
  void handlePollingTimeout(uint8_t dev_id, uint8_t sub1, uint8_t sub2);
  [[nodiscard]] bool copyVirtualAck(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                                    StaticPacket &out) noexcept;
  void syncFcuState(uint8_t slot_idx, uint8_t target_temp, uint8_t room_temp,
                    bool is_online, const uint8_t *raw_pkt, size_t raw_len) noexcept;
#if defined(BENCHMARK_BUILD)
  [[nodiscard]] portMUX_TYPE *getMux() const noexcept { return &_cache_mux; }
  [[nodiscard]] Gateway::Foundation::SequenceLock &getSeqlock() const noexcept { return _seqlock; }
#endif
};

// ── Static Sealed Repository Instance (Rule 17) ──
static DeviceRepository s_device_repo;

} // namespace

// ============================================================================
// Device State Repository & CH1 Control Pipeline
// ============================================================================

DeviceStateEntry *
DeviceRepository::findMutableFast(uint8_t dev_id, uint8_t norm_sub1,
                                  uint8_t sub2, uint8_t h,
                                  bool auto_create) noexcept {
  size_t attempts = 0;
  uint8_t cur_h = h;
  const size_t cnt = _device_count.load(std::memory_order_relaxed);

  while (attempts < MAX_DEVICES) {
    int8_t idx = dev_lookup_map[cur_h];
    if (idx == -1)
      break;
    if (idx >= 0 && static_cast<size_t>(idx) < cnt &&
        cache[idx].dev_id == dev_id && cache[idx].sub1 == norm_sub1 &&
        cache[idx].sub2 == sub2) {
      return &cache[idx];
    }
    cur_h = (cur_h + 1) & 0xFF;
    attempts++;
  }

  if (auto_create && cnt < MAX_DEVICES) {
    size_t idx = cnt;
    _device_count.store(cnt + 1, std::memory_order_relaxed);
    auto &e = cache[idx];
    e.dev_id = dev_id;
    e.sub1 = norm_sub1;
    e.sub2 = sub2;
    e.last_target_temp = 0;
    e.last_ack_len = 0;
    e.last_updated_ms = 0;
    e.last_stale_poll_ms = 0;
    e.timeout_count = 0;
    e.is_online = false;
    memset(e.last_ack_data.data(), 0, sizeof(e.last_ack_data));

    uint8_t map_h = h;
    size_t map_attempts = 0;
    while (dev_lookup_map[map_h] != -1 && map_attempts < 256) {
      map_h = (map_h + 1) & 0xFF;
      map_attempts++;
    }
    if (map_attempts < 256) {
      dev_lookup_map[map_h] = static_cast<int8_t>(idx);
    }
    return &cache[idx];
  }

  return nullptr;
}

DeviceStateEntry *DeviceRepository::findMutable(uint8_t dev_id, uint8_t sub1,
                                                uint8_t sub2,
                                                bool auto_create) noexcept {
  sub1 = Device_NormSub1(dev_id, sub1);
  const uint8_t h = Device_Hash(dev_id, sub1, sub2);
  return findMutableFast(dev_id, sub1, sub2, h, auto_create);
}

const DeviceStateEntry *
DeviceRepository::findInternalFast(uint8_t dev_id, uint8_t norm_sub1,
                                   uint8_t sub2, uint8_t h) const noexcept {
  const size_t cnt = _device_count.load(std::memory_order_relaxed);

  // Fast-Path: Direct attempt 0 hit (O(1))
  int8_t direct_idx = dev_lookup_map[h];
  if (direct_idx >= 0 && static_cast<size_t>(direct_idx) < cnt &&
      cache[direct_idx].dev_id == dev_id && cache[direct_idx].sub1 == norm_sub1 &&
      cache[direct_idx].sub2 == sub2) [[likely]] {
    return &cache[direct_idx];
  }
  if (direct_idx == -1) {
    return nullptr;
  }

  // Fallback: Collision open addressing loop
  size_t attempts = 1;
  h = (h + 1) & 0xFF;
  while (attempts < MAX_DEVICES) {
    int8_t idx = dev_lookup_map[h];
    if (idx == -1)
      break;
    if (idx >= 0 && static_cast<size_t>(idx) < cnt &&
        cache[idx].dev_id == dev_id && cache[idx].sub1 == norm_sub1 &&
        cache[idx].sub2 == sub2) {
      return &cache[idx];
    }
    h = (h + 1) & 0xFF;
    attempts++;
  }
  return nullptr;
}

DeviceStateEntry *
DeviceRepository::findInternalFast(uint8_t dev_id, uint8_t norm_sub1,
                                   uint8_t sub2, uint8_t h) noexcept {
  return findMutableFast(dev_id, norm_sub1, sub2, h, false);
}

const DeviceStateEntry *
DeviceRepository::findInternal(uint8_t dev_id, uint8_t sub1,
                               uint8_t sub2) const noexcept {
  sub1 = Device_NormSub1(dev_id, sub1);
  const uint8_t h = Device_Hash(dev_id, sub1, sub2);
  return findInternalFast(dev_id, sub1, sub2, h);
}

DeviceStateEntry *DeviceRepository::findInternal(uint8_t dev_id, uint8_t sub1,
                                                 uint8_t sub2) noexcept {
  return findMutable(dev_id, sub1, sub2, false);
}

bool DeviceRepository::findCopy(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                                DeviceStateEntry &out_copy) const noexcept {
  const uint8_t norm_sub1 = Device_NormSub1(dev_id, sub1);
  const uint8_t h = Device_Hash(dev_id, norm_sub1, sub2);

  constexpr size_t MAX_RETRIES = 8;
  for (size_t retry = 0; retry < MAX_RETRIES; ++retry) {
    const uint32_t seq = _seqlock.read_begin();
    const DeviceStateEntry *e = findInternalFast(dev_id, norm_sub1, sub2, h);
    if (!e) [[unlikely]] {
      if (_seqlock.read_retry(seq))
        continue;
      return false;
    }
    copyEntryBounded(out_copy, *e);
    if (!_seqlock.read_retry(seq)) [[likely]] {
      return true;
    }
  }

  CriticalSectionLocker lock(&_cache_mux);
  const DeviceStateEntry *e = findInternalFast(dev_id, norm_sub1, sub2, h);
  if (!e) [[unlikely]]
    return false;
  copyEntryBounded(out_copy, *e);
  return true;
}

bool DeviceRepository::exists(uint8_t dev_id, uint8_t sub1,
                              uint8_t sub2) const noexcept {
  const uint8_t norm_sub1 = Device_NormSub1(dev_id, sub1);
  const uint8_t h = Device_Hash(dev_id, norm_sub1, sub2);

  constexpr size_t MAX_RETRIES = 8;
  for (size_t retry = 0; retry < MAX_RETRIES; ++retry) {
    const uint32_t seq = _seqlock.read_begin();
    const DeviceStateEntry *e = findInternalFast(dev_id, norm_sub1, sub2, h);
    if (!e) [[unlikely]] {
      if (_seqlock.read_retry(seq))
        continue;
      return false;
    }
    if (!_seqlock.read_retry(seq)) [[likely]] {
      return true;
    }
  }

  CriticalSectionLocker lock(&_cache_mux);
  return findInternalFast(dev_id, norm_sub1, sub2, h) != nullptr;
}

bool DeviceRepository::getPackedState(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                                      uint64_t &out_packed) const noexcept {
  const uint8_t norm_sub1 = Device_NormSub1(dev_id, sub1);
  const uint8_t h = Device_Hash(dev_id, norm_sub1, sub2);

  constexpr size_t MAX_RETRIES = 8;
  for (size_t retry = 0; retry < MAX_RETRIES; ++retry) {
    const uint32_t seq = _seqlock.read_begin();
    const DeviceStateEntry *e = findInternalFast(dev_id, norm_sub1, sub2, h);
    if (!e) [[unlikely]] {
      if (_seqlock.read_retry(seq))
        continue;
      return false;
    }
    const uint64_t packed = e->shadow_packed_state;
    if (!_seqlock.read_retry(seq)) [[likely]] {
      out_packed = packed;
      return true;
    }
  }

  CriticalSectionLocker lock(&_cache_mux);
  const DeviceStateEntry *e = findInternalFast(dev_id, norm_sub1, sub2, h);
  if (!e) [[unlikely]]
    return false;
  out_packed = e->shadow_packed_state;
  return true;
}

bool DeviceRepository::getMetadata(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                                   DeviceMetadata &out_meta) const noexcept {
  const uint8_t norm_sub1 = Device_NormSub1(dev_id, sub1);
  const uint8_t h = Device_Hash(dev_id, norm_sub1, sub2);

  constexpr size_t MAX_RETRIES = 8;
  for (size_t retry = 0; retry < MAX_RETRIES; ++retry) {
    const uint32_t seq = _seqlock.read_begin();
    const DeviceStateEntry *e = findInternalFast(dev_id, norm_sub1, sub2, h);
    if (!e) [[unlikely]] {
      if (_seqlock.read_retry(seq))
        continue;
      return false;
    }
    const DeviceMetadata meta = *static_cast<const DeviceMetadata*>(e);
    if (!_seqlock.read_retry(seq)) [[likely]] {
      out_meta = meta;
      return true;
    }
  }

  CriticalSectionLocker lock(&_cache_mux);
  const DeviceStateEntry *e = findInternalFast(dev_id, norm_sub1, sub2, h);
  if (!e) [[unlikely]]
    return false;
  out_meta = *static_cast<const DeviceMetadata*>(e);
  return true;
}

bool DeviceRepository::getSnapshot(size_t index,
                                   DeviceStateEntry &out_copy) const noexcept {
  constexpr size_t MAX_RETRIES = 8;
  for (size_t retry = 0; retry < MAX_RETRIES; ++retry) {
    const uint32_t seq = _seqlock.read_begin();
    if (index >= _device_count.load(std::memory_order_relaxed)) {
      if (_seqlock.read_retry(seq))
        continue;
      return false;
    }
    copyEntryBounded(out_copy, cache[index]);
    if (!_seqlock.read_retry(seq)) [[likely]] {
      return true;
    }
  }

  CriticalSectionLocker lock(&_cache_mux);
  if (index >= _device_count.load(std::memory_order_relaxed))
    return false;
  copyEntryBounded(out_copy, cache[index]);
  return true;
}

bool DeviceRepository::getAtMetadata(size_t index,
                                     DeviceMetadata &out_meta) const noexcept {
  constexpr size_t MAX_RETRIES = 8;
  for (size_t retry = 0; retry < MAX_RETRIES; ++retry) {
    const uint32_t seq = _seqlock.read_begin();
    if (index >= _device_count.load(std::memory_order_relaxed)) {
      if (_seqlock.read_retry(seq))
        continue;
      return false;
    }
    const DeviceMetadata meta = static_cast<const DeviceMetadata&>(cache[index]);
    if (!_seqlock.read_retry(seq)) [[likely]] {
      out_meta = meta;
      return true;
    }
  }

  CriticalSectionLocker lock(&_cache_mux);
  if (index >= _device_count.load(std::memory_order_relaxed))
    return false;
  out_meta = static_cast<const DeviceMetadata&>(cache[index]);
  return true;
}

size_t DeviceRepository::getSnapshotChunk(size_t start_idx,
                                          DeviceStateEntry *out_buf,
                                          size_t max_count) const noexcept {
  if (!out_buf || max_count == 0)
    return 0;
  CriticalSectionLocker lock(&_cache_mux);
  const size_t cnt = _device_count.load(std::memory_order_relaxed);
  if (start_idx >= cnt)
    return 0;
  size_t to_copy = std::min(max_count, cnt - start_idx);
  for (size_t i = 0; i < to_copy; ++i) {
    copyEntryBounded(out_buf[i], cache[start_idx + i]);
  }
  return to_copy;
}

bool DeviceRepository::setTargetTemp(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                                     uint8_t temp) noexcept {
  const uint8_t norm_sub1 = Device_NormSub1(dev_id, sub1);
  const uint8_t h = Device_Hash(dev_id, norm_sub1, sub2);

  Gateway::Foundation::CriticalSeqWriterGuard lock(&_cache_mux, _seqlock);
  auto *dev = findMutableFast(dev_id, norm_sub1, sub2, h, false);
  if (dev) {
    dev->last_target_temp = temp;
    return true;
  }
  return false;
}

bool DeviceRepository::copyVirtualAck(uint8_t dev_id, uint8_t sub1,
                                      uint8_t sub2,
                                      StaticPacket &out) noexcept {
  const uint8_t norm_sub1 = Device_NormSub1(dev_id, sub1);
  const uint8_t h = Device_Hash(dev_id, norm_sub1, sub2);

  constexpr size_t MAX_RETRIES = 8;
  for (size_t retry = 0; retry < MAX_RETRIES; ++retry) {
    const uint32_t seq = _seqlock.read_begin();
    const auto *dev = findInternalFast(dev_id, norm_sub1, sub2, h);
    if (!dev || dev->last_ack_len == 0) {
      if (_seqlock.read_retry(seq))
        continue;
      return false;
    }
    const size_t copy_len = std::min({static_cast<size_t>(dev->last_ack_len),
                                      dev->last_ack_data.size(),
                                      out.data.size()});
    std::array<uint8_t, 64> temp_data;
    memcpy(temp_data.data(), dev->last_ack_data.data(), copy_len);
    if (!_seqlock.read_retry(seq)) [[likely]] {
      out.length = static_cast<uint8_t>(copy_len);
      memcpy(out.data.data(), temp_data.data(), copy_len);
      return true;
    }
  }

  CriticalSectionLocker lock(&_cache_mux);
  const auto *dev = findMutableFast(dev_id, norm_sub1, sub2, h, false);
  if (dev && dev->last_ack_len > 0) {
    const size_t copy_len = std::min({static_cast<size_t>(dev->last_ack_len),
                                      dev->last_ack_data.size(),
                                      out.data.size()});
    out.length = static_cast<uint8_t>(copy_len);
    memcpy(out.data.data(), dev->last_ack_data.data(), copy_len);
    return true;
  }
  return false;
}

void DeviceRepository::syncFcuState(uint8_t slot_idx, uint8_t target_temp, uint8_t room_temp,
                                    bool is_online, const uint8_t *raw_pkt, size_t raw_len) noexcept {
  const uint8_t norm_sub1 = Device_NormSub1(Config::FCU::DEV_ID, slot_idx);
  const uint8_t h = Device_Hash(Config::FCU::DEV_ID, norm_sub1, 0);
  const uint32_t now = millis();

  Gateway::Foundation::CriticalSeqWriterGuard lock(&_cache_mux, _seqlock);
  DeviceStateEntry *dev = findMutableFast(Config::FCU::DEV_ID, norm_sub1, 0, h, true);
  if (!dev) return;

  dev->last_ack_len = static_cast<uint8_t>(std::min(raw_len, sizeof(dev->last_ack_data)));
  if (raw_pkt && dev->last_ack_len > 0) {
    memcpy(dev->last_ack_data.data(), raw_pkt, dev->last_ack_len);
  }
  dev->last_target_temp = target_temp;
  dev->last_current_temp = room_temp;
  dev->last_updated_ms = now;

  if (is_online && !dev->is_online) {
    _online_count.fetch_add(1, std::memory_order_relaxed);
  } else if (!is_online && dev->is_online && _online_count.load(std::memory_order_relaxed) > 0) {
    _online_count.fetch_sub(1, std::memory_order_relaxed);
  }
  dev->is_online = is_online;
}

void DeviceRepository::setLastStalePollMs(uint8_t dev_id, uint8_t sub1,
                                          uint8_t sub2, uint32_t ms) noexcept {
  const uint8_t norm_sub1 = Device_NormSub1(dev_id, sub1);
  const uint8_t h = Device_Hash(dev_id, norm_sub1, sub2);

  Gateway::Foundation::CriticalSeqWriterGuard lock(&_cache_mux, _seqlock);
  auto *dev = findMutableFast(dev_id, norm_sub1, sub2, h, false);
  if (dev) {
    dev->last_stale_poll_ms = ms;
  }
}

void DeviceRepository::setLastStalePollMsByIndex(size_t index,
                                                 uint32_t ms) noexcept {
  if (index < _device_count.load(std::memory_order_relaxed)) {
    Gateway::Foundation::CriticalSeqWriterGuard lock(&_cache_mux, _seqlock);
    if (index < _device_count.load(std::memory_order_relaxed)) {
      cache[index].last_stale_poll_ms = ms;
    }
  }
}

void DeviceRepository::initDevices() {
  Gateway::Foundation::CriticalSeqWriterGuard lock(&_cache_mux, _seqlock);
  memset(dev_lookup_map, -1, sizeof(dev_lookup_map));
  _device_count.store(0, std::memory_order_relaxed);
  _online_count.store(0, std::memory_order_relaxed);
}

void DeviceRepository::clear() {
  Gateway::Foundation::CriticalSeqWriterGuard lock(&_cache_mux, _seqlock);
  memset(dev_lookup_map, -1, sizeof(dev_lookup_map));
  _device_count.store(0, std::memory_order_relaxed);
  _online_count.store(0, std::memory_order_relaxed);
}

namespace {
static bool handleLegacyThermostatBroadcast(DeviceRepository &repo,
                                            const StaticPacket &ack,
                                            portMUX_TYPE &mux,
                                            Gateway::Foundation::SequenceLock &seq,
                                            DeviceUpdateResult &out_res) {
  if (ack.length != 34 || ack.data[3] != 0x18 || ack.data[4] != 0x04)
    return false;

  out_res.updated = true;
  out_res.should_broadcast = true;
  out_res.dev_id = 0x18;
  out_res.extra_count = 0;
  const uint32_t now = millis();

  for (uint8_t r = 1; r <= 8; ++r) {
    size_t base = 8 + (r - 1) * 3;
    uint8_t r_state = ack.data[base];
    uint8_t r_amb = ack.data[base + 1];
    uint8_t r_tgt = ack.data[base + 2];
    if (r_state == 0x00)
      continue;

    uint8_t r_sub1 = 0x10 + r;
    int r_pwr = (r_state == 0x01) ? 1 : ((r_state == 0x07) ? 2 : 0);
    const uint8_t r_norm_sub1 = Device_NormSub1(0x18, r_sub1);
    const uint8_t r_h = Device_Hash(0x18, r_norm_sub1, 0);
    {
      Gateway::Foundation::CriticalSeqWriterGuard lock(mux, seq);
      DeviceStateEntry *r_dev = repo.findMutableFast(0x18, r_norm_sub1, 0, r_h, true);
      if (r_dev) {
        r_dev->last_updated_ms = now;
        r_dev->timeout_count = 0;
        r_dev->is_online = true;
        r_dev->last_current_temp = r_amb;
        r_dev->last_target_temp = r_tgt;
      }
    }
    DecodedDeviceState legacy_st{};
    legacy_st.dev_class = DeviceClass::THERMOSTAT;
    legacy_st.power = r_pwr;
    legacy_st.target_temp = r_tgt;
    legacy_st.current_temp = r_amb;
    legacy_st.fan_speed = 0;
    snprintf(legacy_st.valve_state, sizeof(legacy_st.valve_state), "closed");
    legacy_st.power_w = 0.0f;
    legacy_st.floor = 1;
    legacy_st.direction = 0;
    legacy_st.ho = 0;
    legacy_st.vent_mode = 1;
    legacy_st.should_broadcast = true;

    if (out_res.sub1 == 0) {
      out_res.sub1 = r_sub1;
      out_res.sub2 = 0;
      out_res.state = legacy_st;
    } else if (out_res.extra_count < 7) {
      out_res.extra[out_res.extra_count].sub1 = r_sub1;
      out_res.extra[out_res.extra_count].state = legacy_st;
      out_res.extra_count++;
    }
  }
  return true;
}

static inline void handleElevatorSpecialState(DeviceStateEntry *dev,
                                              DecodedDeviceState &st,
                                              bool prev_pwr, uint8_t prev_dir,
                                              uint8_t prev_ho) {
  bool ev_state_changed = (st.power != prev_pwr) ||
                          (st.direction != prev_dir) || (st.ho != prev_ho);
  if (ev_state_changed) {
    st.should_broadcast = true;
    dev->last_current_temp = static_cast<uint8_t>(st.floor);
    dev->last_target_temp = static_cast<uint8_t>(st.direction);
    dev->last_ack_data[0] = static_cast<uint8_t>(st.power);
    dev->last_ack_data[1] = static_cast<uint8_t>(st.ho);
  }
}
} // namespace

DeviceUpdateResult DeviceRepository::updateFromBus(StaticPacket &ack) {
  DeviceUpdateResult res{};
  // 2차 캐시는 CH#1 (물리 서브기기 응답) 및 CH#5 (EW11 스니핑 응답)만 등록 허용
  // (CH2, CH3, CH4, CH6 금지)
  if (ack.channel_id != 1 && ack.channel_id != 5) {
    return res;
  }

  if (UNLIKELY(ack.length < 5))
    return res;

  // 구형(Legacy) 34B 난방 브로드캐스트 처리 (Packet[1]==0x22 && Dev==0x18 &&
  // Opcode==0x04)
  if (handleLegacyThermostatBroadcast(*this, ack, _cache_mux, _seqlock, res)) {
    return res;
  }

  if (!Universal_GetEngine().isAckPacket(
          std::span<const uint8_t>(ack.data.data(), ack.length)))
    return res;

  uint8_t dev_id = 0, sub1 = 0, sub2 = 0;
  if (!Universal_GetEngine().extractDeviceKey(
          std::span<const uint8_t>(ack.data.data(), ack.length), dev_id, sub1,
          sub2)) {
    return res;
  }

  // 0x2A (신발장 서브 패널 / 원격검침)는 제어 단말기가 아니므로 상태 캐시에서
  // 완전 제외
  if (dev_id == 0x2A) {
    return res;
  }

  // 0x81 / NAK / 에러 패킷 필터링 (장비의 명령 거부 응답이 정상 캐시를
  // 오염시키거나 UI를 끄지 않도록 방어)
  if (ack.length >= 7 && (ack.data[5] == 0x81 || ack.data[6] == 0x81)) {
    return res;
  }

  DecodedDeviceState st{};
  DeviceStateEntry dev_snap{};
  bool ack_changed = false;
  bool prev_pwr = false;
  uint8_t prev_dir = 0;
  uint8_t prev_ho = 0;

  // Precompute outside lock:
  const uint8_t norm_sub1 = Device_NormSub1(dev_id, sub1);
  const uint8_t h = Device_Hash(dev_id, norm_sub1, sub2);
  const uint32_t now = millis();
  const bool is_vent_mode_ack =
      (dev_id == 0x2B && ack.length >= 6 && ack.data[5] == 0x43);
  const size_t ack_len =
      std::min(static_cast<size_t>(ack.length), sizeof(dev_snap.last_ack_data));

  {
    Gateway::Foundation::CriticalSeqWriterGuard lock(&_cache_mux, _seqlock);
    DeviceStateEntry *dev = findMutableFast(dev_id, norm_sub1, sub2, h, true);
    if (UNLIKELY(!dev)) {
      return res;
    }

    // 엘리베이터(0x34) 이전 상태 백업: memcpy로 dev->last_ack_data를 덮어쓰기
    // 전에 반드시 수행
    if (dev_id == 0x34) {
      prev_pwr =
          (dev->last_ack_len > 0) ? (dev->last_ack_data[0] & 0x01) : false;
      prev_dir = dev->last_target_temp;
      prev_ho = (dev->last_ack_len > 1) ? dev->last_ack_data[1] : 0;
    }

    const uint64_t new_packed = Device_ExtractPackedPacket(ack.data.data(), ack_len);
    if (!is_vent_mode_ack) {
      if (LIKELY(ack_len <= 13)) {
        ack_changed = (dev->shadow_packed_state != new_packed || dev->last_ack_len != ack_len);
      } else {
        ack_changed = (dev->last_ack_len != ack_len ||
                       memcmp(dev->last_ack_data.data(), ack.data.data(), ack_len) != 0);
      }
      if (ack_changed) {
        dev->shadow_packed_state = new_packed;
        dev->last_ack_len = static_cast<uint8_t>(ack_len);
        memcpy(dev->last_ack_data.data(), ack.data.data(), ack_len);
      }
    } else {
      // 모드 패킷 수신 시 모드 상태 변경 여부 확인하여 브로드캐스트 트리거
      ack_changed = true;
      dev->shadow_packed_state = new_packed;
      dev->last_ack_len = static_cast<uint8_t>(ack_len);
      memcpy(dev->last_ack_data.data(), ack.data.data(), ack_len);
    }
    dev->last_updated_ms = now;
    dev->timeout_count = 0;
    if (!dev->is_online) {
      dev->is_online = true;
      _online_count.fetch_add(1, std::memory_order_relaxed);
    }

    if (ack_changed) {
      copyEntryBounded(dev_snap, *dev);
    }
  } // _cache_mux unlocked (최소 락 윈도우 보장)

  res.updated = true;

  if (ack_changed) {
    bool has_decoded = ControlTemplate_DecodeByDevId(dev_id, ack, &dev_snap, st);
    if (has_decoded) {
      if (dev_id == 0x34) {
        bool ev_state_changed = (st.power != prev_pwr) ||
                                (st.direction != prev_dir) || (st.ho != prev_ho);
        if (ev_state_changed) {
          st.should_broadcast = true;
          Gateway::Foundation::CriticalSeqWriterGuard lock(&_cache_mux, _seqlock);
          auto *mdev = findMutableFast(dev_id, norm_sub1, sub2, h, false);
          if (mdev) {
            mdev->last_current_temp = static_cast<uint8_t>(st.floor);
            mdev->last_target_temp = static_cast<uint8_t>(st.direction);
            mdev->last_ack_data[0] = static_cast<uint8_t>(st.power);
            mdev->last_ack_data[1] = static_cast<uint8_t>(st.ho);
          }
        }
      } else {
        st.should_broadcast = true;
      }
    }
  }

  if (st.should_broadcast) {
    res.should_broadcast = true;
    res.dev_id = dev_id;
    res.sub1 = sub1;
    res.sub2 = sub2;
    res.state = st;
  }
  return res;
}

void DeviceRepository::handlePollingTimeout(uint8_t dev_id, uint8_t sub1,
                                            uint8_t sub2) {
  const uint8_t norm_sub1 = Device_NormSub1(dev_id, sub1);
  const uint8_t h = Device_Hash(dev_id, norm_sub1, sub2);

  Gateway::Foundation::CriticalSeqWriterGuard lock(&_cache_mux, _seqlock);
  auto *mdev = findMutableFast(dev_id, norm_sub1, sub2, h, true);
  if (mdev && mdev->is_online && ++mdev->timeout_count >= 3) {
    mdev->is_online = false;
    if (_online_count.load(std::memory_order_relaxed) > 0) {
      _online_count.fetch_sub(1, std::memory_order_relaxed);
    }
  }
}

size_t DeviceRepository::count() const noexcept {
  return _device_count.load(std::memory_order_relaxed);
}

size_t DeviceRepository::getOnlineCount() const noexcept {
  return _online_count.load(std::memory_order_relaxed);
}

// ── Device_GetSnapshot ────────────────────────────────────────────────────────

void Device_Init() noexcept {
  s_device_repo.initDevices();
  Fcu_Init();
  Bridge_RegisterSlotRxCallback(Fcu_HandleRxStream);
  Bridge_RegisterSlotTickCallback(Fcu_PollTick);
}

void Device_Clear() noexcept {
  s_device_repo.clear();
}

bool Device_GetSnapshot(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                         DeviceStateEntry &out_copy) noexcept {
  return s_device_repo.findCopy(dev_id, sub1, sub2, out_copy);
}

bool Device_GetSnapshot(size_t index, DeviceStateEntry &out_copy) noexcept {
  return s_device_repo.getSnapshot(index, out_copy);
}

bool Device_GetSnapshotAt(size_t index, DeviceStateEntry &out_copy) noexcept {
  return s_device_repo.getSnapshot(index, out_copy);
}

size_t Device_GetSnapshotChunk(size_t start_idx, DeviceStateEntry *out_buf,
                               size_t max_count) noexcept {
  return s_device_repo.getSnapshotChunk(start_idx, out_buf, max_count);
}

size_t Device_GetOnlineCount() noexcept {
  return s_device_repo.getOnlineCount();
}

size_t Device_GetCount() noexcept {
  return s_device_repo.count();
}

uint32_t Device_GetCacheLockTimeouts() noexcept {
  return s_cache_lock_timeouts.load(std::memory_order_relaxed);
}

bool Device_FindCopy(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                     DeviceStateEntry &out_copy) noexcept {
  return s_device_repo.findCopy(dev_id, sub1, sub2, out_copy);
}

bool Device_Exists(uint8_t dev_id, uint8_t sub1, uint8_t sub2) noexcept {
  return s_device_repo.exists(dev_id, sub1, sub2);
}

bool Device_GetPackedState(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                                     uint64_t &out_packed) noexcept {
  return s_device_repo.getPackedState(dev_id, sub1, sub2, out_packed);
}

bool Device_GetMetadata(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                        DeviceMetadata &out_meta) noexcept {
  return s_device_repo.getMetadata(dev_id, sub1, sub2, out_meta);
}

bool Device_GetAtCopy(size_t index, DeviceStateEntry &out_copy) noexcept {
  return s_device_repo.getSnapshot(index, out_copy);
}

bool Device_GetAtMetadata(size_t index, DeviceMetadata &out_meta) noexcept {
  return s_device_repo.getAtMetadata(index, out_meta);
}

void Device_RegisterFcu(uint8_t slot_idx) noexcept {
  s_device_repo.findMutable(Config::FCU::DEV_ID, slot_idx, 0, true);
}

void Device_SyncFcuState(uint8_t slot_idx, uint8_t target_temp, uint8_t room_temp,
                         bool is_online, const uint8_t *raw_pkt, size_t raw_len) noexcept {
  s_device_repo.syncFcuState(slot_idx, target_temp, room_temp, is_online, raw_pkt, raw_len);
}

void Device_HandlePollingTimeout(uint8_t dev_id, uint8_t sub1, uint8_t sub2) noexcept {
  s_device_repo.handlePollingTimeout(dev_id, sub1, sub2);
}

void Device_SetLastStalePollMs(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                              uint32_t ms) noexcept {
  s_device_repo.setLastStalePollMs(dev_id, sub1, sub2, ms);
}

void Device_SetLastStalePollMsByIndex(size_t index, uint32_t ms) noexcept {
  s_device_repo.setLastStalePollMsByIndex(index, ms);
}

// ── Device_UpdateFromBus ──────────────────────────────────────────────────────

DeviceUpdateResult Device_UpdateFromBus(StaticPacket &ack_pkt) noexcept {
  return s_device_repo.updateFromBus(ack_pkt);
}

// ── Device_SetTargetTemp ──────────────────────────────────────────────────────

bool Device_SetTargetTemp(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                           uint8_t temp) noexcept {
  return s_device_repo.setTargetTemp(dev_id, sub1, sub2, temp);
}

// ── Device_CopyVirtualAck ─────────────────────────────────────────────────────

bool Device_CopyVirtualAck(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                             StaticPacket &out_pkt) noexcept {
  return s_device_repo.copyVirtualAck(dev_id, sub1, sub2, out_pkt);
}

// ── Telemetry Event Queue Implementation ──────────────────────────────────────

namespace {
constexpr size_t TELEMETRY_QUEUE_LEN = 16;
static Foundation::SpinlockMpscRingBuffer<TelemetryItem, TELEMETRY_QUEUE_LEN> s_telemetry_queue;
static std::atomic<uint32_t> s_telemetry_drop_count{0};
static std::atomic<uint32_t> s_telemetry_high_watermark{0};

static uint32_t s_last_broadcast_ms = 0;
static uint8_t s_last_broadcast_dev = 0;
static uint8_t s_last_broadcast_sub1 = 0;
static uint8_t s_last_broadcast_sub2 = 0;
} // namespace

bool Telemetry_Enqueue(const TelemetryItem &item) noexcept {
  if (UNLIKELY(!s_telemetry_queue.push(item))) {
    s_telemetry_drop_count.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  const uint32_t waiting = static_cast<uint32_t>(s_telemetry_queue.size());
  uint32_t cur_hw = s_telemetry_high_watermark.load(std::memory_order_relaxed);
  while (waiting > cur_hw &&
         !s_telemetry_high_watermark.compare_exchange_weak(cur_hw, waiting, std::memory_order_relaxed)) {
  }
  return true;
}

bool Telemetry_Dequeue(TelemetryItem &out_item) noexcept {
  return s_telemetry_queue.pop(out_item);
}

void Telemetry_GetStats(uint32_t &drop_count, uint32_t &high_watermark) noexcept {
  drop_count = s_telemetry_drop_count.load(std::memory_order_relaxed);
  high_watermark = s_telemetry_high_watermark.load(std::memory_order_relaxed);
}

void Telemetry_ResetStats() noexcept {
  s_telemetry_drop_count.store(0, std::memory_order_relaxed);
  s_telemetry_high_watermark.store(0, std::memory_order_relaxed);
}

void Device_NotifyElevatorEvent(uint8_t sub1, uint8_t sub2, uint8_t floor,
                                uint8_t ho, uint8_t power,
                                bool is_arrival) noexcept {
  TelemetryItem item{};
  item.type = TelemetryEventType::ELEVATOR;
  item.sub1 = sub1;
  item.sub2 = sub2;
  item.floor = floor;
  item.ho = ho;
  item.power = power;
  item.is_arrival = is_arrival;
  Telemetry_Enqueue(item);
}

void Device_ProcessBusPacket(StaticPacket &ack_pkt) noexcept {
  DeviceUpdateResult res = s_device_repo.updateFromBus(ack_pkt);
  if (res.should_broadcast) {
    const uint32_t now = millis();
    // 50ms throttle window: suppress rapid flapping of identical non-momentary device (bypass elevator 0x34)
    if (res.dev_id != 0x34 &&
        res.dev_id == s_last_broadcast_dev &&
        res.sub1 == s_last_broadcast_sub1 &&
        res.sub2 == s_last_broadcast_sub2 &&
        (now - s_last_broadcast_ms < Config::Timing::DEVICE_BROADCAST_THROTTLE_MS)) {
      return;
    }
    s_last_broadcast_ms = now;
    s_last_broadcast_dev = res.dev_id;
    s_last_broadcast_sub1 = res.sub1;
    s_last_broadcast_sub2 = res.sub2;

    TelemetryItem item{};
    item.type = TelemetryEventType::DEVICE_RESULT;
    item.device_res = res;
    Telemetry_Enqueue(item);
  }
}

void Device_NotifyDoorphoneEvent(bool front_bell, bool lobby_bell) noexcept {
  s_dp_front_bell.store(front_bell, std::memory_order_release);
  s_dp_lobby_bell.store(lobby_bell, std::memory_order_release);
  if (front_bell || lobby_bell) {
    s_dp_last_bell_ms.store(millis(), std::memory_order_release);
  }
  TelemetryItem item{};
  item.type = TelemetryEventType::DOORPHONE;
  item.front_bell = front_bell;
  item.lobby_bell = lobby_bell;
  Telemetry_Enqueue(item);
}

void Device_DoorphoneGetState(bool &out_front_bell, bool &out_lobby_bell,
                              uint32_t &out_last_bell_ms) noexcept {
  out_front_bell = s_dp_front_bell.load(std::memory_order_relaxed);
  out_lobby_bell = s_dp_lobby_bell.load(std::memory_order_relaxed);
  out_last_bell_ms = s_dp_last_bell_ms.load(std::memory_order_relaxed);
}

bool Device_DoorphoneOpen(bool is_lobby) noexcept {
  return Wallpad_DoorphoneOpen(is_lobby);
}

bool Device_DecodeState(uint8_t dev_id,
                        const StaticPacket &ack,
                        const DeviceStateEntry *dev,
                        DecodedDeviceState &out) noexcept {
  return ControlTemplate_DecodeByDevId(dev_id, ack, dev, out);
}

// ── FCU (Air Conditioner) Domain Public Implementation ────────────────────────
bool Device_GetFcuSnapshot(uint8_t slot_idx, FcuDeviceSnapshot &out) noexcept {
  Fcu::SlotRuntime rt{};
  if (!Fcu_GetSlotRuntime(slot_idx, rt)) {
    return false;
  }
  out.power = rt.snap.power;
  out.mode = std::to_underlying(rt.snap.mode);
  out.fan_speed = std::to_underlying(rt.snap.fan_speed);
  out.swing = std::to_underlying(rt.snap.swing);
  out.target_temp = rt.snap.target_temp;
  out.room_temp = rt.snap.room_temp;
  out.is_online = rt.is_online;
  return true;
}

namespace {

struct FcuActionHandler {
  std::string_view action;
  enum class ActionType : uint8_t {
    PowerRestore,
    Power,
    Mode,
    FanSpeed,
    Swing,
    TargetTemp
  } type;
};

// Sorted alphabetically by action name for O(log N) std::lower_bound lookup
constexpr FcuActionHandler kFcuActions[] = {
    {"ac_mode",       FcuActionHandler::ActionType::Mode},
    {"fan_speed",     FcuActionHandler::ActionType::FanSpeed},
    {"mode",          FcuActionHandler::ActionType::Mode},
    {"power",         FcuActionHandler::ActionType::Power},
    {"power_restore", FcuActionHandler::ActionType::PowerRestore},
    {"pwr",           FcuActionHandler::ActionType::Power},
    {"set_temp",      FcuActionHandler::ActionType::TargetTemp},
    {"spd",           FcuActionHandler::ActionType::FanSpeed},
    {"swing",         FcuActionHandler::ActionType::Swing},
    {"temp",          FcuActionHandler::ActionType::TargetTemp},
};

constexpr bool areFcuActionsSorted() noexcept {
  for (size_t i = 1; i < sizeof(kFcuActions) / sizeof(kFcuActions[0]); ++i) {
    if (kFcuActions[i - 1].action >= kFcuActions[i].action) return false;
  }
  return true;
}
static_assert(areFcuActionsSorted(), "kFcuActions must be strictly sorted alphabetically for binary search");

} // namespace

bool Device_ControlFcu(uint8_t slot_idx, std::string_view action, int value,
                       uint16_t mode, uint16_t fan, uint16_t swing,
                       uint8_t temp) noexcept {
  if (action.empty()) return false;

  auto it = std::lower_bound(
      std::begin(kFcuActions), std::end(kFcuActions), action,
      [](const FcuActionHandler &entry, std::string_view key) noexcept {
        return entry.action < key;
      });

  if (it == std::end(kFcuActions) || it->action != action) {
    return false;
  }

  switch (it->type) {
  case FcuActionHandler::ActionType::PowerRestore:
    return Fcu_RestorePower(slot_idx, mode, fan, swing, temp);
  case FcuActionHandler::ActionType::Power:
    return Fcu_SetPower(slot_idx, value == 1);
  case FcuActionHandler::ActionType::Mode:
    return Fcu_SetMode(slot_idx, static_cast<Fcu::Mode>(value));
  case FcuActionHandler::ActionType::FanSpeed:
    return Fcu_SetFanSpeed(slot_idx, static_cast<Fcu::FanSpeed>(value));
  case FcuActionHandler::ActionType::Swing:
    return Fcu_SetSwing(slot_idx, static_cast<Fcu::Swing>(value));
  case FcuActionHandler::ActionType::TargetTemp:
    return Fcu_SetTargetTemp(slot_idx, static_cast<uint8_t>(value));
  }
  return false;
}

#if defined(BENCHMARK_BUILD)
namespace DeviceBenchmark {

bool findCopyDirect(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                    DeviceStateEntry &out_copy) noexcept {
  return s_device_repo.findCopy(dev_id, sub1, sub2, out_copy);
}

bool probeDirectExists(uint8_t dev_id, uint8_t sub1, uint8_t sub2) noexcept {
  return s_device_repo.exists(dev_id, sub1, sub2);
}

portMUX_TYPE *getMuxHandle() noexcept {
  return s_device_repo.getMux();
}

void registerMockDevice(uint8_t dev_id, uint8_t sub1, uint8_t sub2) noexcept {
  Gateway::Foundation::CriticalSeqWriterGuard lock(s_device_repo.getMux(), s_device_repo.getSeqlock());
  DeviceStateEntry *dev = s_device_repo.findMutable(dev_id, sub1, sub2, true);
  if (dev) {
    dev->is_online = true;
    dev->last_updated_ms = millis();
    dev->last_ack_len = 11;
    dev->last_target_temp = 22;
    dev->shadow_packed_state = Device_PackState(dev_id, sub1, 1, 22, 20, 0, 1, sub2);
  }
  Router_RecordRoute(1, -1, dev_id, sub1, sub2);
}

} // namespace DeviceBenchmark
#endif





