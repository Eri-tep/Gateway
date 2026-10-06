// ============================================================================
// DeviceRegistry: Level 3 Physical Device Repository & Cached State Model
// Implementation
// ============================================================================

#include "L3_Protocol/Public/Device_Registry.h"
#include "L3_Protocol/Private/Fcu_Protocol.h"
#include "L2_Transport/Bridge_CH.h"

#include <Arduino.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

// ============================================================================
// DeviceClass String Conversion Implementations (Absorbed from ProtocolTypes)
// ============================================================================

const char *DeviceClassToName(DeviceClass cls) noexcept {
  static constexpr const char *kNames[] = {"Unknown", "Light",    "Outlet",
                                           "Gas",     "Elevator", "Thermo",
                                           "Vent",    "Aircon"};
  const size_t idx = static_cast<size_t>(cls);
  return (idx < sizeof(kNames) / sizeof(kNames[0])) ? kNames[idx] : "Unknown";
}

const char *DeviceClassToCliString(DeviceClass cls) noexcept {
  static constexpr const char *kCliNames[] = {"UNKNOWN", "SWITCH", "OUTLET",
                                              "GAS",     "MOMENT", "THERMO",
                                              "VENT",    "AIRCON"};
  const size_t idx = static_cast<size_t>(cls);
  return (idx < sizeof(kCliNames) / sizeof(kCliNames[0])) ? kCliNames[idx]
                                                          : "UNKNOWN";
}

const char *DeviceClassToTelemetryString(DeviceClass cls) noexcept {
  static constexpr const char *kTeleNames[] = {
      "unknown",   "switch",     "outlet", "gas",
      "momentary", "thermostat", "vent",   "aircon"};
  const size_t idx = static_cast<size_t>(cls);
  return (idx < sizeof(kTeleNames) / sizeof(kTeleNames[0])) ? kTeleNames[idx]
                                                            : "unknown";
}

namespace {

DeviceAckPacketCheckFn s_ack_packet_check = nullptr;
DeviceKeyExtractorFn s_key_extractor = nullptr;
DeviceStateDecoderFn s_state_decoder = nullptr;
DeviceNormSub1Fn s_norm_sub1_fn = nullptr;
DoorphoneOpenHandler s_dp_open_handler = nullptr;

std::atomic<bool> s_dp_front_bell{false};
std::atomic<bool> s_dp_lobby_bell{false};
std::atomic<uint32_t> s_dp_last_bell_ms{0};
static std::atomic<uint32_t> s_cache_lock_timeouts{0};

class DeviceRepository {
private:
  static constexpr size_t MAX_DEVICES = 48;
  static constexpr TickType_t kCacheLockTimeout = pdMS_TO_TICKS(10);
  DeviceStateEntry cache[MAX_DEVICES]{};
  int8_t dev_lookup_map[256]{};
  size_t device_count = 0;
  std::atomic<size_t> _online_count{0};
  mutable StaticSemaphore_t _cache_mutex_buf{};
  mutable SemaphoreHandle_t _cache_mutex = nullptr;

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
  [[nodiscard]] bool findCopy(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                              DeviceStateEntry &out_copy) const noexcept;
  [[nodiscard]] bool getSnapshot(size_t index,
                                 DeviceStateEntry &out_copy) const noexcept;
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
  void handlePollingTimeout(const DeviceStateEntry *dev);
  void handlePollingTimeout(uint8_t dev_id, uint8_t sub1, uint8_t sub2);
  [[nodiscard]] bool copyVirtualAck(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                                    StaticPacket &out) noexcept;
  void syncFcuState(uint8_t slot_idx, uint8_t target_temp, uint8_t room_temp,
                    bool is_online, const uint8_t *raw_pkt, size_t raw_len) noexcept;
};

// ── Static Sealed Repository Instance (Rule 17) ──
static DeviceRepository s_device_repo;

} // namespace

// ============================================================================
// Device State Repository & CH1 Control Pipeline
// ============================================================================

static inline uint8_t Device_Hash(uint8_t dev_id, uint8_t sub1,
                                  uint8_t sub2) noexcept {
  return static_cast<uint8_t>(dev_id + sub1 * 3 + sub2 * 7);
}

static inline uint8_t Device_NormSub1(uint8_t dev_id, uint8_t sub1) noexcept {
  return s_norm_sub1_fn ? s_norm_sub1_fn(dev_id, sub1) : sub1;
}

DeviceStateEntry *DeviceRepository::findMutable(uint8_t dev_id, uint8_t sub1,
                                                uint8_t sub2,
                                                bool auto_create) noexcept {
  sub1 = Device_NormSub1(dev_id, sub1);
  uint8_t h = Device_Hash(dev_id, sub1, sub2);
  size_t attempts = 0;

  while (attempts < MAX_DEVICES) {
    int8_t idx = dev_lookup_map[h];
    if (idx == -1)
      break;
    if (idx >= 0 && static_cast<size_t>(idx) < device_count &&
        cache[idx].dev_id == dev_id && cache[idx].sub1 == sub1 &&
        cache[idx].sub2 == sub2) {
      return &cache[idx];
    }
    h = (h + 1) & 0xFF;
    attempts++;
  }

  if (auto_create && device_count < MAX_DEVICES) {
    size_t idx = device_count++;
    auto &e = cache[idx];
    e.dev_id = dev_id;
    e.sub1 = sub1;
    e.sub2 = sub2;
    e.last_target_temp = 0;
    e.last_ack_len = 0;
    e.last_updated_ms = 0;
    e.last_stale_poll_ms = 0;
    e.timeout_count = 0;
    e.is_online = false;
    memset(e.last_ack_data.data(), 0, sizeof(e.last_ack_data));

    uint8_t map_h = Device_Hash(dev_id, sub1, sub2);
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

const DeviceStateEntry *
DeviceRepository::findInternal(uint8_t dev_id, uint8_t sub1,
                               uint8_t sub2) const noexcept {
  sub1 = Device_NormSub1(dev_id, sub1);
  uint8_t h = Device_Hash(dev_id, sub1, sub2);
  size_t attempts = 0;

  while (attempts < MAX_DEVICES) {
    int8_t idx = dev_lookup_map[h];
    if (idx == -1)
      break;
    if (idx >= 0 && static_cast<size_t>(idx) < device_count &&
        cache[idx].dev_id == dev_id && cache[idx].sub1 == sub1 &&
        cache[idx].sub2 == sub2) {
      return &cache[idx];
    }
    h = (h + 1) & 0xFF;
    attempts++;
  }
  return nullptr;
}

DeviceStateEntry *DeviceRepository::findInternal(uint8_t dev_id, uint8_t sub1,
                                                 uint8_t sub2) noexcept {
  return findMutable(dev_id, sub1, sub2, false);
}

bool DeviceRepository::findCopy(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                                DeviceStateEntry &out_copy) const noexcept {
  MutexLocker lock(_cache_mutex, kCacheLockTimeout);
  if (!lock.isLocked())
    return false;
  const DeviceStateEntry *e = findInternal(dev_id, sub1, sub2);
  if (!e)
    return false;
  out_copy = *e;
  return true;
}

bool DeviceRepository::getSnapshot(size_t index,
                                   DeviceStateEntry &out_copy) const noexcept {
  MutexLocker lock(_cache_mutex, kCacheLockTimeout);
  if (!lock.isLocked() || index >= device_count)
    return false;
  out_copy = cache[index];
  return true;
}

size_t DeviceRepository::getSnapshotChunk(size_t start_idx,
                                          DeviceStateEntry *out_buf,
                                          size_t max_count) const noexcept {
  if (!out_buf || max_count == 0)
    return 0;
  MutexLocker lock(_cache_mutex, kCacheLockTimeout);
  if (!lock.isLocked() || start_idx >= device_count)
    return 0;
  size_t to_copy = std::min(max_count, device_count - start_idx);
  for (size_t i = 0; i < to_copy; ++i) {
    out_buf[i] = cache[start_idx + i];
  }
  return to_copy;
}

bool DeviceRepository::setTargetTemp(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                                     uint8_t temp) noexcept {
  MutexLocker lock(_cache_mutex, kCacheLockTimeout);
  if (!lock.isLocked()) {
    s_cache_lock_timeouts.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  auto *dev = findMutable(dev_id, sub1, sub2, false);
  if (dev) {
    dev->last_target_temp = temp;
    return true;
  }
  return false;
}

bool DeviceRepository::copyVirtualAck(uint8_t dev_id, uint8_t sub1,
                                      uint8_t sub2,
                                      StaticPacket &out) noexcept {
  MutexLocker lock(_cache_mutex, kCacheLockTimeout);
  if (!lock.isLocked())
    return false;
  const auto *dev = findMutable(dev_id, sub1, sub2, false);
  if (dev && dev->last_ack_len > 0) {
    out.length = dev->last_ack_len;
    memcpy(out.data.data(), dev->last_ack_data.data(), dev->last_ack_len);
    return true;
  }
  return false;
}

void DeviceRepository::syncFcuState(uint8_t slot_idx, uint8_t target_temp, uint8_t room_temp,
                                    bool is_online, const uint8_t *raw_pkt, size_t raw_len) noexcept {
  MutexLocker lock(_cache_mutex, kCacheLockTimeout);
  if (!lock.isLocked()) {
    s_cache_lock_timeouts.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  DeviceStateEntry *dev = findMutable(Config::FCU::DEV_ID, slot_idx, 0, true);
  if (!dev) return;

  dev->last_ack_len = static_cast<uint8_t>(std::min(raw_len, sizeof(dev->last_ack_data)));
  if (raw_pkt && dev->last_ack_len > 0) {
    memcpy(dev->last_ack_data.data(), raw_pkt, dev->last_ack_len);
  }
  dev->last_target_temp = target_temp;
  dev->last_current_temp = room_temp;
  dev->last_updated_ms = millis();

  if (is_online && !dev->is_online) {
    _online_count.fetch_add(1, std::memory_order_relaxed);
  } else if (!is_online && dev->is_online && _online_count.load(std::memory_order_relaxed) > 0) {
    _online_count.fetch_sub(1, std::memory_order_relaxed);
  }
  dev->is_online = is_online;
}

void DeviceRepository::setLastStalePollMs(uint8_t dev_id, uint8_t sub1,
                                          uint8_t sub2, uint32_t ms) noexcept {
  MutexLocker lock(_cache_mutex, kCacheLockTimeout);
  if (!lock.isLocked()) {
    s_cache_lock_timeouts.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  auto *dev = findMutable(dev_id, sub1, sub2, false);
  if (dev) {
    dev->last_stale_poll_ms = ms;
  }
}

void DeviceRepository::setLastStalePollMsByIndex(size_t index,
                                                 uint32_t ms) noexcept {
  MutexLocker lock(_cache_mutex, kCacheLockTimeout);
  if (!lock.isLocked()) {
    s_cache_lock_timeouts.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (index < device_count) {
    cache[index].last_stale_poll_ms = ms;
  }
}

void DeviceRepository::initDevices() {
  if (!_cache_mutex)
    _cache_mutex = xSemaphoreCreateMutexStatic(&_cache_mutex_buf);
  {
    MutexLocker lock(_cache_mutex, kCacheLockTimeout);
    if (!lock.isLocked()) {
      s_cache_lock_timeouts.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    memset(dev_lookup_map, -1, sizeof(dev_lookup_map));
    device_count = 0;
    _online_count.store(0, std::memory_order_relaxed);
  }
}

void DeviceRepository::clear() {
  MutexLocker lock(_cache_mutex, kCacheLockTimeout);
  if (!lock.isLocked()) {
    s_cache_lock_timeouts.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  memset(dev_lookup_map, -1, sizeof(dev_lookup_map));
  device_count = 0;
  _online_count.store(0, std::memory_order_relaxed);
}

namespace {
static bool handleLegacyThermostatBroadcast(DeviceRepository &repo,
                                            const StaticPacket &ack,
                                            SemaphoreHandle_t mutex,
                                            DeviceUpdateResult &out_res) {
  if (ack.length != 34 || ack.data[3] != 0x18 || ack.data[4] != 0x04)
    return false;

  out_res.updated = true;
  out_res.should_broadcast = true;
  out_res.dev_id = 0x18;
  out_res.extra_count = 0;

  for (uint8_t r = 1; r <= 8; ++r) {
    size_t base = 8 + (r - 1) * 3;
    uint8_t r_state = ack.data[base];
    uint8_t r_amb = ack.data[base + 1];
    uint8_t r_tgt = ack.data[base + 2];
    if (r_state == 0x00)
      continue;

    uint8_t r_sub1 = 0x10 + r;
    int r_pwr = (r_state == 0x01) ? 1 : ((r_state == 0x07) ? 2 : 0);
    {
      MutexLocker lock(mutex, pdMS_TO_TICKS(10));
      if (!lock.isLocked()) {
        s_cache_lock_timeouts.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      DeviceStateEntry *r_dev = repo.findMutable(0x18, r_sub1, 0, true);
      if (r_dev) {
        r_dev->last_updated_ms = millis();
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
  if (handleLegacyThermostatBroadcast(*this, ack, _cache_mutex, res)) {
    return res;
  }

  if (!s_ack_packet_check ||
      !s_ack_packet_check(std::span<const uint8_t>(ack.data.data(), ack.length)))
    return res;

  uint8_t dev_id = 0, sub1 = 0, sub2 = 0;
  if (!s_key_extractor ||
      !s_key_extractor(std::span<const uint8_t>(ack.data.data(), ack.length),
                       dev_id, sub1, sub2)) {
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

  {
    MutexLocker lock(_cache_mutex, kCacheLockTimeout);
    if (!lock.isLocked()) {
      s_cache_lock_timeouts.fetch_add(1, std::memory_order_relaxed);
      return res;
    }
    DeviceStateEntry *dev = findMutable(dev_id, sub1, sub2, true);
    if (UNLIKELY(!dev)) {
      return res;
    }

    // 현대통신 환기(0x2B) 운전 모드(0x43) 패킷의 경우 월패드 가상 응답(0x40
    // 기본 쿼리 응답) 캐시를 덮어쓰지 않음
    bool is_vent_mode_ack =
        (dev_id == 0x2B && ack.length >= 6 && ack.data[5] == 0x43);

    // 엘리베이터(0x34) 이전 상태 백업: memcpy로 dev->last_ack_data를 덮어쓰기
    // 전에 반드시 수행
    if (dev_id == 0x34) {
      prev_pwr =
          (dev->last_ack_len > 0) ? (dev->last_ack_data[0] & 0x01) : false;
      prev_dir = dev->last_target_temp;
      prev_ho = (dev->last_ack_len > 1) ? dev->last_ack_data[1] : 0;
    }

    if (!is_vent_mode_ack) {
      ack_changed =
          (dev->last_ack_len != ack.length ||
           memcmp(dev->last_ack_data.data(), ack.data.data(), ack.length) != 0);
      dev->last_ack_len = ack.length;
      memcpy(dev->last_ack_data.data(), ack.data.data(), ack.length);
    } else {
      // 모드 패킷 수신 시 모드 상태 변경 여부 확인하여 브로드캐스트 트리거
      ack_changed = true;
    }
    dev->last_updated_ms = millis();
    dev->timeout_count = 0;
    if (!dev->is_online) {
      dev->is_online = true;
      _online_count.fetch_add(1, std::memory_order_relaxed);
    }

    if (ack_changed) {
      dev_snap = *dev;
    }
  } // _cache_mutex unlocked (최소 락 윈도우 보장)

  res.updated = true;

  if (ack_changed) {
    bool has_decoded = s_state_decoder && s_state_decoder(dev_id, ack, &dev_snap, st);
    if (has_decoded) {
      if (dev_id == 0x34) {
        bool ev_state_changed = (st.power != prev_pwr) ||
                                (st.direction != prev_dir) || (st.ho != prev_ho);
        if (ev_state_changed) {
          st.should_broadcast = true;
          MutexLocker lock(_cache_mutex, kCacheLockTimeout);
          if (lock.isLocked()) {
            auto *mdev = findMutable(dev_id, sub1, sub2, false);
            if (mdev) {
              mdev->last_current_temp = static_cast<uint8_t>(st.floor);
              mdev->last_target_temp = static_cast<uint8_t>(st.direction);
              mdev->last_ack_data[0] = static_cast<uint8_t>(st.power);
              mdev->last_ack_data[1] = static_cast<uint8_t>(st.ho);
            }
          } else {
            s_cache_lock_timeouts.fetch_add(1, std::memory_order_relaxed);
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

void DeviceRepository::handlePollingTimeout(const DeviceStateEntry *dev) {
  if (!dev)
    return;
  MutexLocker lock(_cache_mutex, kCacheLockTimeout);
  if (!lock.isLocked()) {
    s_cache_lock_timeouts.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  auto *mdev = const_cast<DeviceStateEntry *>(dev);
  if (mdev->is_online && ++mdev->timeout_count >= 3) {
    mdev->is_online = false;
    if (_online_count.load(std::memory_order_relaxed) > 0) {
      _online_count.fetch_sub(1, std::memory_order_relaxed);
    }
  }
}

void DeviceRepository::handlePollingTimeout(uint8_t dev_id, uint8_t sub1,
                                            uint8_t sub2) {
  MutexLocker lock(_cache_mutex, kCacheLockTimeout);
  if (!lock.isLocked()) {
    s_cache_lock_timeouts.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  auto *mdev = findMutable(dev_id, sub1, sub2, true);
  if (mdev && mdev->is_online && ++mdev->timeout_count >= 3) {
    mdev->is_online = false;
    if (_online_count.load(std::memory_order_relaxed) > 0) {
      _online_count.fetch_sub(1, std::memory_order_relaxed);
    }
  }
}

size_t DeviceRepository::count() const noexcept {
  MutexLocker lock(_cache_mutex, kCacheLockTimeout);
  return lock.isLocked() ? device_count : 0;
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

bool Device_GetAtCopy(size_t index, DeviceStateEntry &out_copy) noexcept {
  return s_device_repo.getSnapshot(index, out_copy);
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

// ── Listener Subscription Implementation ──────────────────────────────────────

namespace {
DeviceStateListener s_dev_listener = nullptr;
DoorphoneEventListener s_doorphone_listener = nullptr;
ElevatorEventListener s_elevator_listener = nullptr;
} // namespace

void Device_RegisterStateListener(DeviceStateListener listener) noexcept {
  s_dev_listener = listener;
}

void Device_RegisterDoorphoneListener(DoorphoneEventListener listener) noexcept {
  s_doorphone_listener = listener;
}

void Device_RegisterElevatorListener(ElevatorEventListener listener) noexcept {
  s_elevator_listener = listener;
}

void Device_NotifyElevatorEvent(uint8_t sub1, uint8_t sub2, uint8_t floor,
                                uint8_t ho, uint8_t power,
                                bool is_arrival) noexcept {
  if (s_elevator_listener) {
    s_elevator_listener(sub1, sub2, floor, ho, power, is_arrival);
  }
}

void Device_ProcessBusPacket(StaticPacket &ack_pkt) noexcept {
  DeviceUpdateResult res = s_device_repo.updateFromBus(ack_pkt);
  if (s_dev_listener) {
    s_dev_listener(res);
  }
}

void Device_NotifyDoorphoneEvent(bool front_bell, bool lobby_bell) noexcept {
  s_dp_front_bell.store(front_bell, std::memory_order_release);
  s_dp_lobby_bell.store(lobby_bell, std::memory_order_release);
  if (front_bell || lobby_bell) {
    s_dp_last_bell_ms.store(millis(), std::memory_order_release);
  }
  if (s_doorphone_listener) {
    s_doorphone_listener(front_bell, lobby_bell);
  }
}

void Device_DoorphoneGetState(bool &out_front_bell, bool &out_lobby_bell,
                              uint32_t &out_last_bell_ms) noexcept {
  out_front_bell = s_dp_front_bell.load(std::memory_order_relaxed);
  out_lobby_bell = s_dp_lobby_bell.load(std::memory_order_relaxed);
  out_last_bell_ms = s_dp_last_bell_ms.load(std::memory_order_relaxed);
}

bool Device_DoorphoneOpen(bool is_lobby) noexcept {
  return s_dp_open_handler ? s_dp_open_handler(is_lobby) : false;
}

void Device_RegisterParserHooks(DeviceAckPacketCheckFn ack_check,
                                DeviceKeyExtractorFn key_extract) noexcept {
  s_ack_packet_check = ack_check;
  s_key_extractor = key_extract;
}

void Device_RegisterStateDecoder(DeviceStateDecoderFn fn) noexcept {
  s_state_decoder = fn;
}

void Device_RegisterNormSub1Hook(DeviceNormSub1Fn fn) noexcept {
  s_norm_sub1_fn = fn;
}

void Device_RegisterDoorphoneOpenHandler(DoorphoneOpenHandler handler) noexcept {
  s_dp_open_handler = handler;
}

bool Device_DecodeState(uint8_t dev_id,
                        const StaticPacket &ack,
                        const DeviceStateEntry *dev,
                        DecodedDeviceState &out) noexcept {
  return s_state_decoder ? s_state_decoder(dev_id, ack, dev, out) : false;
}

// ── FCU (Air Conditioner) Domain Public Implementation ────────────────────────
bool Device_GetFcuSnapshot(uint8_t slot_idx, FcuDeviceSnapshot &out) noexcept {
  Fcu::SlotRuntime rt{};
  if (!Fcu_GetSlotRuntime(slot_idx, rt)) {
    return false;
  }
  out.power = rt.snap.power;
  out.mode = static_cast<uint16_t>(rt.snap.mode);
  out.fan_speed = static_cast<uint16_t>(rt.snap.fan_speed);
  out.swing = static_cast<uint16_t>(rt.snap.swing);
  out.target_temp = rt.snap.target_temp;
  out.room_temp = rt.snap.room_temp;
  out.is_online = rt.is_online;
  return true;
}

bool Device_ControlFcu(uint8_t slot_idx, const char *action, int value,
                       uint16_t mode, uint16_t fan, uint16_t swing,
                       uint8_t temp) noexcept {
  if (!action) return false;
  std::string_view sv{action};

  if (sv == "power_restore") {
    return Fcu_RestorePower(slot_idx, mode, fan, swing, temp);
  }
  if (sv == "power" || sv == "pwr") {
    return Fcu_SetPower(slot_idx, value == 1);
  }
  if (sv == "mode" || sv == "ac_mode") {
    return Fcu_SetMode(slot_idx, static_cast<Fcu::Mode>(value));
  }
  if (sv == "fan_speed" || sv == "spd") {
    return Fcu_SetFanSpeed(slot_idx, static_cast<Fcu::FanSpeed>(value));
  }
  if (sv == "swing") {
    return Fcu_SetSwing(slot_idx, static_cast<Fcu::Swing>(value));
  }
  if (sv == "set_temp" || sv == "temp") {
    return Fcu_SetTargetTemp(slot_idx, static_cast<uint8_t>(value));
  }
  return false;
}





