// ============================================================================
// DeviceRegistry: Level 3 Physical Device Repository & Cached State Model
// Implementation
// ============================================================================

#include "L3_Routing/Public/Device_Registry.h"
#include "L3_Routing/Private/ControlTemplate.h"
#include "L3_Routing/Private/Wallpad_Protocol.h"

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

class DeviceRepository {
private:
  static constexpr size_t MAX_DEVICES = 48;
  DeviceStateEntry cache[MAX_DEVICES]{};
  int8_t dev_lookup_map[256]{};
  size_t device_count = 0;
  std::atomic<size_t> _online_count{0};
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
  GroupControlTemplate grp{};
  if (g_control_registry.findGroup(dev_id, grp)) {
    if (grp.power_slot.category_val != 0 &&
        grp.power_slot.category_val != 0xFF) {
      if (sub1 == grp.temp_slot.category_val ||
          sub1 == grp.speed_slot.category_val) {
        return grp.power_slot.category_val;
      }
    }
  }
  return sub1;
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

const DeviceStateEntry *DeviceRepository::find(uint8_t dev_id, uint8_t sub1,
                                               uint8_t sub2) const noexcept {
  MutexLocker lock(_cache_mutex);
  return findInternal(dev_id, sub1, sub2);
}

const DeviceStateEntry *DeviceRepository::getAt(size_t index) const noexcept {
  MutexLocker lock(_cache_mutex);
  return (index < device_count) ? &cache[index] : nullptr;
}

DeviceStateEntry *DeviceRepository::getAt(size_t index) noexcept {
  MutexLocker lock(_cache_mutex);
  return (index < device_count) ? &cache[index] : nullptr;
}

bool DeviceRepository::getSnapshot(size_t index,
                                   DeviceStateEntry &out_copy) noexcept {
  MutexLocker lock(_cache_mutex);
  if (index >= device_count)
    return false;
  out_copy.dev_id = cache[index].dev_id;
  out_copy.sub1 = cache[index].sub1;
  out_copy.sub2 = cache[index].sub2;
  uint8_t ack_len = std::min<uint8_t>(cache[index].last_ack_len,
                                      sizeof(out_copy.last_ack_data));
  out_copy.last_ack_len = ack_len;
  if (ack_len > 0)
    memcpy(out_copy.last_ack_data.data(), cache[index].last_ack_data.data(),
           ack_len);
  out_copy.last_updated_ms = cache[index].last_updated_ms;
  out_copy.timeout_count = cache[index].timeout_count;
  out_copy.is_online = cache[index].is_online;
  return true;
}

bool DeviceRepository::setTargetTemp(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                                     uint8_t temp) noexcept {
  MutexLocker lock(_cache_mutex);
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
  MutexLocker lock(_cache_mutex);
  const auto *dev = findMutable(dev_id, sub1, sub2, false);
  if (dev && dev->last_ack_len > 0) {
    out.length = dev->last_ack_len;
    memcpy(out.data.data(), dev->last_ack_data.data(), dev->last_ack_len);
    return true;
  }
  return false;
}

void DeviceRepository::setLastStalePollMs(uint8_t dev_id, uint8_t sub1,
                                          uint8_t sub2, uint32_t ms) noexcept {
  MutexLocker lock(_cache_mutex);
  auto *dev = findMutable(dev_id, sub1, sub2, false);
  if (dev) {
    dev->last_stale_poll_ms = ms;
  }
}

void DeviceRepository::setLastStalePollMsByIndex(size_t index,
                                                 uint32_t ms) noexcept {
  MutexLocker lock(_cache_mutex);
  if (index < device_count) {
    cache[index].last_stale_poll_ms = ms;
  }
}

void DeviceRepository::initDevices() {
  if (!_cache_mutex)
    _cache_mutex = xSemaphoreCreateMutex();
  {
    MutexLocker lock(_cache_mutex);
    memset(dev_lookup_map, -1, sizeof(dev_lookup_map));
    device_count = 0;
    _online_count.store(0, std::memory_order_relaxed);
  }

  // L2.2 ControlTemplate decoupled unit count provider
  ControlTemplateRegistry::setDeviceUnitCountProvider([](uint8_t dev_id) -> size_t {
    size_t units = 0;
    for (size_t i = 0; i < s_device_repo.count() && units < 2; ++i) {
      DeviceStateEntry snap{};
      if (s_device_repo.getSnapshot(i, snap) && snap.dev_id == dev_id)
        ++units;
    }
    return units;
  });

  // L2.1 WallpadProtocol AutoProbingEngine decoupled device hooks
  AutoProbingEngine::setDeviceHooks(
    []() -> size_t {
      return s_device_repo.getOnlineCount();
    },
    [](uint8_t dev_id, uint8_t sub1, uint8_t sub2, const uint8_t **out_ack, size_t *out_len) -> bool {
      const DeviceStateEntry *dev = s_device_repo.find(dev_id, sub1, sub2);
      if (dev && dev->is_online && dev->last_ack_len >= 4) {
        *out_ack = dev->last_ack_data.data();
        *out_len = dev->last_ack_len;
        return true;
      }
      return false;
    },
    [](StaticPacket &ack) {
      s_device_repo.updateFromBus(ack);
    }
  );
}

void DeviceRepository::clear() {
  MutexLocker lock(_cache_mutex);
  memset(dev_lookup_map, -1, sizeof(dev_lookup_map));
  device_count = 0;
  _online_count.store(0, std::memory_order_relaxed);
}

namespace {

static void decodeOutlet(const GroupControlTemplate &grp,
                         const StaticPacket &ack, const DeviceStateEntry *,
                         DecodedDeviceState &out) {
  uint8_t w_off = grp.getWattageOffset(ack.length);
  if (w_off + 1 < ack.length) {
    uint16_t raw_w =
        (static_cast<uint16_t>(ack.data[w_off]) << 8) | ack.data[w_off + 1];
    out.power_w = (raw_w < 50000) ? static_cast<float>(raw_w) : 0.0f;
  }
}

static void decodeSwitch(const GroupControlTemplate &grp,
                         const StaticPacket &ack, const DeviceStateEntry *dev,
                         DecodedDeviceState &out) {
  if (grp.frame_len >= 17) {
    out.dev_class = DeviceClass::OUTLET;
    decodeOutlet(grp, ack, dev, out);
  }
}

static void decodeGas(const GroupControlTemplate &grp, const StaticPacket &ack,
                         const DeviceStateEntry *, DecodedDeviceState &out) {
  uint8_t v_off = grp.getValveStateOffset(ack.length);
  bool is_closed =
      (v_off >= ack.length || ack.data[v_off] == grp.close_slot.off_val);
  snprintf(out.valve_state, sizeof(out.valve_state), "%s",
           is_closed ? "closed" : "open");
}

static void decodeMomentary(const GroupControlTemplate &grp,
                            const StaticPacket &ack,
                            const DeviceStateEntry *dev,
                            DecodedDeviceState &out) {
  out.floor = 15;
  out.direction = 0;
  out.ho = 0;
  if (grp.dev_id == 0x34) {
    out.power =
        (ack.length == 11 && ack.data[4] == 0x04)
            ? ((ack.data[8] == 0x06) ? 1 : 0)
            : ((dev && dev->last_ack_len > 0) ? (dev->last_ack_data[0] & 0x01)
                                              : 0);
  } else if (ack.length >= 6) {
    out.floor = constrain(static_cast<int>(ack.data[5]), 1, 60);
    out.direction = (ack.length >= 7) ? ack.data[6] : 0;
  }
}

static void decodeThermostat(const GroupControlTemplate &grp,
                             const StaticPacket &ack,
                             const DeviceStateEntry *dev,
                             DecodedDeviceState &out) {
  out.target_temp = dev ? dev->last_target_temp : 0;
  out.current_temp = (dev && dev->last_current_temp > 0)
                         ? dev->last_current_temp
                         : out.target_temp;

  uint8_t p_off = grp.getPowerOffset(ack.length);
  if (p_off < ack.length && grp.away_mode_token != 0 &&
      ack.data[p_off] == grp.away_mode_token)
    out.power = 2;

  uint8_t t_off = grp.getTargetTempOffset(ack.length);
  if (t_off < ack.length && ack.data[t_off] >= 5 && ack.data[t_off] <= 35) {
    out.target_temp = ack.data[t_off];
    if (dev)
      const_cast<DeviceStateEntry *>(dev)->last_target_temp = ack.data[t_off];
  }

  uint8_t c_off = grp.getCurrentTempOffset(ack.length);
  if (c_off < ack.length && ack.data[c_off] >= 5 && ack.data[c_off] <= 50) {
    out.current_temp = ack.data[c_off];
    if (dev)
      const_cast<DeviceStateEntry *>(dev)->last_current_temp = ack.data[c_off];
  }
}

static void decodeVent(const GroupControlTemplate &grp, const StaticPacket &ack,
                       const DeviceStateEntry *, DecodedDeviceState &out) {
  uint8_t p_off = grp.getPowerOffset(ack.length);
  out.power = (p_off < ack.length && ack.data[p_off] == 0x01) ? 1 : 0;
  out.fan_speed = 1;
  out.vent_mode = 1;

  uint8_t spd_off = grp.getFanSpeedOffset(ack.length);
  if (spd_off < ack.length)
    out.fan_speed = grp.decodeFanSpeed(ack.data[spd_off]);

  if (out.power == 1 && (ack.length >= 6 && ack.data[5] == 0x43) &&
      p_off < ack.length) {
    uint8_t m = ack.data[p_off];
    if (m >= 1 && m <= 4)
      out.vent_mode = m;
  }
}

static void decodeAircon(const GroupControlTemplate &, const StaticPacket &ack,
                         const DeviceStateEntry *dev, DecodedDeviceState &out) {
  size_t base = (ack.length == 14) ? 7 : 8;
  if (base + 4 >= ack.length)
    return;

  out.power = ((ack.data[base] & 0x7F) == 0x01) ? 1 : 0;
  out.vent_mode = constrain(static_cast<int>(ack.data[base + 1]), 1, 5);
  out.fan_speed = (ack.data[base + 2] >= 1 && ack.data[base + 2] <= 4)
                      ? ack.data[base + 2]
                      : 4;

  uint8_t amb = ack.data[base + 3];
  if (amb >= 5 && amb <= 50) {
    out.current_temp = amb;
    if (dev)
      const_cast<DeviceStateEntry *>(dev)->last_current_temp = amb;
  }
  uint8_t tgt = ack.data[base + 4] & 0x7F;
  if (tgt >= 5 && tgt <= 35) {
    out.target_temp = tgt;
    if (dev)
      const_cast<DeviceStateEntry *>(dev)->last_target_temp = tgt;
  }
}

static void decodeUnknown(const GroupControlTemplate &, const StaticPacket &,
                          const DeviceStateEntry *, DecodedDeviceState &) {}

using ClassDecoderFn = void (*)(const GroupControlTemplate &grp,
                                const StaticPacket &ack,
                                const DeviceStateEntry *dev,
                                DecodedDeviceState &out);

static constexpr ClassDecoderFn kClassDecoders[] = {
    decodeUnknown,    // UNKNOWN = 0
    decodeSwitch,     // SWITCH = 1
    decodeOutlet,     // OUTLET = 2
    decodeGas,        // GAS = 3
    decodeMomentary,  // MOMENTARY = 4
    decodeThermostat, // THERMOSTAT = 5
    decodeVent,       // VENT = 6
    decodeAircon      // AIRCON = 7
};

} // anonymous namespace

void DeviceRepository::decodeDeviceState(const GroupControlTemplate &grp,
                                         const StaticPacket &ack,
                                         const DeviceStateEntry *dev,
                                         DecodedDeviceState &out) {
  out.dev_class = grp.coverage.dev_class;
  out.should_broadcast = false;
  out.power = 0;
  out.target_temp = 0;
  out.current_temp = 0;
  out.fan_speed = 0;
  out.vent_mode = 1;
  out.power_w = 0.0f;
  out.floor = 1;
  out.direction = 0;
  out.ho = 0;
  snprintf(out.valve_state, sizeof(out.valve_state), "closed");

  // 1. 공통 기본 전원 슬롯 디코딩
  uint8_t p_off = grp.getPowerOffset(ack.length);
  if (p_off != 0xFF && p_off < ack.length) {
    out.power = (ack.data[p_off] == grp.power_slot.on_val) ? 1 : 0;
  }

  // 2. 클래스별 디스패치 (특수 전원 및 파라미터 개별 디코딩)
  const size_t idx = static_cast<size_t>(grp.coverage.dev_class);
  if (idx < sizeof(kClassDecoders) / sizeof(kClassDecoders[0])) {
    kClassDecoders[idx](grp, ack, dev, out);
  } else {
    decodeUnknown(grp, ack, dev, out);
  }
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
      MutexLocker lock(mutex);
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

  auto *parser = WallpadParserFactory::getActiveParser();
  if (!parser ||
      !parser->isAckPacket(span<const uint8_t>(ack.data.data(), ack.length)))
    return res;

  uint8_t dev_id = 0, sub1 = 0, sub2 = 0;
  if (!parser->extractDeviceKey(
          span<const uint8_t>(ack.data.data(), ack.length), dev_id, sub1,
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

  {
    MutexLocker lock(_cache_mutex);
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
    GroupControlTemplate grp{};
    bool has_grp = g_control_registry.findGroup(dev_id, grp);
    if (!has_grp && dev_id == 0x34) {
      grp.dev_id = 0x34;
      grp.coverage.dev_class = DeviceClass::MOMENTARY;
      has_grp = true;
    }

    if (has_grp) {
      decodeDeviceState(grp, ack, &dev_snap, st);
      if (dev_id == 0x34) {
        bool ev_state_changed = (st.power != prev_pwr) ||
                                (st.direction != prev_dir) || (st.ho != prev_ho);
        if (ev_state_changed) {
          st.should_broadcast = true;
          MutexLocker lock(_cache_mutex);
          auto *mdev = findMutable(dev_id, sub1, sub2, false);
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

void DeviceRepository::handlePollingTimeout(const DeviceStateEntry *dev) {
  if (!dev)
    return;
  MutexLocker lock(_cache_mutex);
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
  MutexLocker lock(_cache_mutex);
  auto *mdev = findMutable(dev_id, sub1, sub2, true);
  if (mdev && mdev->is_online && ++mdev->timeout_count >= 3) {
    mdev->is_online = false;
    if (_online_count.load(std::memory_order_relaxed) > 0) {
      _online_count.fetch_sub(1, std::memory_order_relaxed);
    }
  }
}

size_t DeviceRepository::count() const noexcept {
  MutexLocker lock(_cache_mutex);
  return device_count;
}

size_t DeviceRepository::getOnlineCount() const noexcept {
  return _online_count.load(std::memory_order_relaxed);
}

// ── Device_GetSnapshot ────────────────────────────────────────────────────────

void Device_Init() noexcept {
  s_device_repo.initDevices();
}

void Device_Clear() noexcept {
  s_device_repo.clear();
}

bool Device_GetSnapshot(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                         DeviceStateEntry &out_copy) noexcept {
  const auto *e = s_device_repo.findEntry(dev_id, sub1, sub2);
  if (!e) return false;
  out_copy = *e;
  return true;
}

bool Device_GetSnapshot(size_t index, DeviceStateEntry &out_copy) noexcept {
  return s_device_repo.getSnapshot(index, out_copy);
}

bool Device_GetSnapshotAt(size_t index, DeviceStateEntry &out_copy) noexcept {
  return s_device_repo.getSnapshot(index, out_copy);
}

size_t Device_GetOnlineCount() noexcept {
  return s_device_repo.getOnlineCount();
}

size_t Device_GetCount() noexcept {
  return s_device_repo.count();
}

const DeviceStateEntry *Device_Find(uint8_t dev_id, uint8_t sub1,
                                    uint8_t sub2) noexcept {
  return s_device_repo.find(dev_id, sub1, sub2);
}

const DeviceStateEntry *Device_GetAt(size_t index) noexcept {
  return s_device_repo.getAt(index);
}

void Device_RegisterFcu(uint8_t slot_idx) noexcept {
  s_device_repo.findMutable(Config::FCU::DEV_ID, slot_idx, 0, true);
}

void Device_SyncFcuState(uint8_t slot_idx, uint8_t target_temp, uint8_t room_temp,
                         bool is_online, const uint8_t *raw_pkt, size_t raw_len) noexcept {
  DeviceStateEntry *dev = s_device_repo.findMutable(Config::FCU::DEV_ID, slot_idx, 0, true);
  if (!dev) return;
  dev->last_ack_len = static_cast<uint8_t>(std::min(raw_len, sizeof(dev->last_ack_data)));
  if (raw_pkt && dev->last_ack_len > 0) {
    memcpy(dev->last_ack_data.data(), raw_pkt, dev->last_ack_len);
  }
  dev->last_target_temp = target_temp;
  dev->last_current_temp = room_temp;
  dev->last_updated_ms = millis();
  dev->is_online = is_online;
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
} // namespace

void Device_RegisterStateListener(DeviceStateListener listener) noexcept {
  s_dev_listener = listener;
}

void Device_RegisterDoorphoneListener(DoorphoneEventListener listener) noexcept {
  s_doorphone_listener = listener;
}

void Device_ProcessBusPacket(StaticPacket &ack_pkt) noexcept {
  DeviceUpdateResult res = s_device_repo.updateFromBus(ack_pkt);
  if (s_dev_listener) {
    s_dev_listener(res);
  }
}

void Device_NotifyDoorphoneEvent(bool front_bell, bool lobby_bell) noexcept {
  if (s_doorphone_listener) {
    s_doorphone_listener(front_bell, lobby_bell);
  }
}

void Device_DecodeState(const struct GroupControlTemplate &grp,
                        const StaticPacket &ack,
                        const DeviceStateEntry *dev,
                        DecodedDeviceState &out) noexcept {
  DeviceRepository::decodeDeviceState(grp, ack, dev, out);
}

void Device_DoorphoneGetState(bool &out_front_bell, bool &out_lobby_bell,
                              uint32_t &out_last_bell_ms) noexcept {
  Wallpad_DoorphoneGetState(out_front_bell, out_lobby_bell, out_last_bell_ms);
}

void Device_DoorphoneGetFraming(FramingStatus &out_status, uint8_t &out_stx,
                                uint8_t &out_etx, uint8_t &out_len) noexcept {
  Wallpad_DoorphoneGetFraming(out_status, out_stx, out_etx, out_len);
}

void Device_DoorphoneClearNvs(const char *nvs_ns) noexcept {
  Wallpad_DoorphoneClearNvs(nvs_ns);
}

bool Device_DoorphoneStartSequence(uint8_t stx, uint8_t etx, uint8_t op_call,
                                   uint8_t op_open, uint8_t op_end) noexcept {
  return Wallpad_DoorphoneStartSequence(stx, etx, op_call, op_open, op_end);
}

bool Device_ExtractKeyFromFrame(const uint8_t *data, size_t len,
                                uint8_t &out_dev_id, uint8_t &out_sub1,
                                uint8_t &out_sub2) noexcept {
  if (!data || len < 5) return false;
  auto *parser = WallpadParserFactory::getActiveParser();
  if (!parser) return false;
  std::span<const uint8_t> frame(data, len);
  parser->extractDeviceKey(frame, out_dev_id, out_sub1, out_sub2);
  return true;
}



