#include "EngineInternal.h"
#include <algorithm>
#include <cstring>

static inline uint8_t Device_Hash(uint8_t dev_id, uint8_t sub1, uint8_t sub2) noexcept {
  return static_cast<uint8_t>(dev_id + sub1 * 3 + sub2 * 7);
}

static inline uint8_t Device_NormSub1(uint8_t dev_id, uint8_t sub1) noexcept {
  GroupControlTemplate grp{};
  if (g_control_registry.findGroup(dev_id, grp)) {
    if (grp.power_slot.category_val != 0 && grp.power_slot.category_val != 0xFF) {
      if (sub1 == grp.temp_slot.category_val || sub1 == grp.speed_slot.category_val) {
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

const DeviceStateEntry *DeviceRepository::find(uint8_t dev_id, uint8_t sub1,
                                               uint8_t sub2) const noexcept {
  MutexLocker lock(_cache_mutex);
  return const_cast<DeviceRepository *>(this)->findMutable(dev_id, sub1, sub2, false);
}

const DeviceStateEntry *DeviceRepository::getAt(size_t index) const noexcept {
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
                                          uint8_t sub2,
                                          uint32_t ms) noexcept {
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
  MutexLocker lock(_cache_mutex);
  memset(dev_lookup_map, -1, sizeof(dev_lookup_map));
  device_count = 0;
}

void DeviceRepository::clear() {
  MutexLocker lock(_cache_mutex);
  memset(dev_lookup_map, -1, sizeof(dev_lookup_map));
  device_count = 0;
}

namespace {

void decodeOutlet(const GroupControlTemplate &grp, const StaticPacket &ack,
                  const DeviceStateEntry * /*dev*/, DecodedDeviceState &out) {
  uint8_t w_off = grp.getWattageOffset(ack.length);
  if (w_off + 1 < ack.length) {
    uint16_t raw_w = (static_cast<uint16_t>(ack.data[w_off]) << 8) | ack.data[w_off + 1];
    out.power_w = (raw_w < 50000) ? static_cast<float>(raw_w) : 0.0f;
  }
}

void decodeSwitch(const GroupControlTemplate &grp, const StaticPacket &ack,
                  const DeviceStateEntry *dev, DecodedDeviceState &out) {
  if (grp.frame_len >= 17) {
    out.dev_class = DeviceClass::OUTLET;
    decodeOutlet(grp, ack, dev, out);
  }
}

void decodeGas(const GroupControlTemplate &grp, const StaticPacket &ack,
               const DeviceStateEntry * /*dev*/, DecodedDeviceState &out) {
  uint8_t v_off = grp.getValveStateOffset(ack.length);
  bool is_closed = (v_off >= ack.length || ack.data[v_off] == grp.close_slot.off_val);
  snprintf(out.valve_state, sizeof(out.valve_state), "%s", is_closed ? "closed" : "open");
}

void decodeMomentary(const GroupControlTemplate &grp, const StaticPacket &ack,
                     const DeviceStateEntry *dev, DecodedDeviceState &out) {
  out.floor = 15;
  out.direction = 0;
  out.ho = 0;

  if (grp.dev_id == 0x34) {
    out.power = (ack.length == 11 && ack.data[4] == 0x04)
                ? ((ack.data[8] == 0x06) ? 1 : 0)
                : ((dev && dev->last_ack_len > 0) ? (dev->last_ack_data[0] & 0x01) : 0);
  } else if (ack.length >= 6) {
    out.floor = constrain(static_cast<int>(ack.data[5]), 1, 60);
    out.direction = (ack.length >= 7) ? ack.data[6] : 0;
  }
}

void decodeThermostat(const GroupControlTemplate &grp, const StaticPacket &ack,
                      const DeviceStateEntry *dev, DecodedDeviceState &out) {
  out.target_temp = dev ? dev->last_target_temp : 0;
  out.current_temp = (dev && dev->last_current_temp > 0) ? dev->last_current_temp : out.target_temp;

  uint8_t p_off = grp.getPowerOffset(ack.length);
  if (p_off < ack.length && grp.away_mode_token != 0 && ack.data[p_off] == grp.away_mode_token) {
    out.power = 2;
  }

  uint8_t t_off = grp.getTargetTempOffset(ack.length);
  if (t_off < ack.length && ack.data[t_off] >= 5 && ack.data[t_off] <= 35) {
    out.target_temp = ack.data[t_off];
    if (dev) const_cast<DeviceStateEntry *>(dev)->last_target_temp = ack.data[t_off];
  }

  uint8_t c_off = grp.getCurrentTempOffset(ack.length);
  if (c_off < ack.length && ack.data[c_off] >= 5 && ack.data[c_off] <= 50) {
    out.current_temp = ack.data[c_off];
    if (dev) const_cast<DeviceStateEntry *>(dev)->last_current_temp = ack.data[c_off];
  }
}

void decodeVent(const GroupControlTemplate &grp, const StaticPacket &ack,
                const DeviceStateEntry * /*dev*/, DecodedDeviceState &out) {
  uint8_t p_off = grp.getPowerOffset(ack.length);
  out.power = (p_off < ack.length && ack.data[p_off] == 0x01) ? 1 : 0;
  out.fan_speed = 1;
  out.vent_mode = 1;

  uint8_t spd_off = grp.getFanSpeedOffset(ack.length);
  if (spd_off < ack.length) {
    out.fan_speed = grp.decodeFanSpeed(ack.data[spd_off]);
  }

  bool is_mode_pkt = (ack.length >= 6 && ack.data[5] == 0x43);
  if (out.power == 1 && is_mode_pkt && p_off < ack.length) {
    uint8_t m = ack.data[p_off];
    if (m >= 1 && m <= 4) out.vent_mode = m;
  }
}

void decodeAircon(const GroupControlTemplate & /*grp*/, const StaticPacket &ack,
                  const DeviceStateEntry *dev, DecodedDeviceState &out) {
  size_t base = (ack.length == 14) ? 7 : 8;
  if (base + 4 >= ack.length) return;

  out.power = ((ack.data[base] & 0x7F) == 0x01) ? 1 : 0;
  out.vent_mode = constrain(static_cast<int>(ack.data[base + 1]), 1, 5);
  out.fan_speed = (ack.data[base + 2] >= 1 && ack.data[base + 2] <= 4) ? ack.data[base + 2] : 4;

  uint8_t amb = ack.data[base + 3];
  if (amb >= 5 && amb <= 50) {
    out.current_temp = amb;
    if (dev) const_cast<DeviceStateEntry *>(dev)->last_current_temp = amb;
  }
  uint8_t tgt = ack.data[base + 4] & 0x7F;
  if (tgt >= 5 && tgt <= 35) {
    out.target_temp = tgt;
    if (dev) const_cast<DeviceStateEntry *>(dev)->last_target_temp = tgt;
  }
}

void decodeUnknown(const GroupControlTemplate & /*grp*/, const StaticPacket & /*ack*/,
                   const DeviceStateEntry * /*dev*/, DecodedDeviceState & /*out*/) {
}

using ClassDecoderFn = void (*)(const GroupControlTemplate &grp, const StaticPacket &ack,
                                const DeviceStateEntry *dev, DecodedDeviceState &out);

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

void DeviceRepository::updateFromBus(StaticPacket &ack) {
  // 2차 캐시는 CH#1 (물리 서브기기 응답) 및 CH#5 (EW11 스니핑 응답)만 등록 허용 (CH2, CH3, CH4, CH6 금지)
  if (ack.channel_id != 1 && ack.channel_id != 5) {
    return;
  }

  if (UNLIKELY(ack.length < 5))
    return;

  // 구형(Legacy) 34B 난방 브로드캐스트 처리 (Packet[1]==0x22 && Dev==0x18 && Opcode==0x04)
  if (ack.length == 34 && ack.data[3] == 0x18 && ack.data[4] == 0x04) {
    for (uint8_t r = 1; r <= 8; ++r) {
      size_t base = 8 + (r - 1) * 3;
      uint8_t r_state = ack.data[base];
      uint8_t r_amb   = ack.data[base + 1];
      uint8_t r_tgt   = ack.data[base + 2];
      if (r_state == 0x00) continue;

      uint8_t r_sub1 = 0x10 + r;
      int r_pwr = (r_state == 0x01) ? 1 : ((r_state == 0x07) ? 2 : 0);
      {
        MutexLocker lock(_cache_mutex);
        DeviceStateEntry *r_dev = findMutable(0x18, r_sub1, 0, true);
        if (r_dev) {
          r_dev->last_updated_ms = millis();
          r_dev->timeout_count = 0;
          r_dev->is_online = true;
          r_dev->last_current_temp = r_amb;
          r_dev->last_target_temp = r_tgt;
        }
      }
      Mgmt_BroadcastDeviceState(0x18, r_sub1, 0, DeviceClass::THERMOSTAT, r_pwr, r_tgt, r_amb, 0, "closed", 0.0f, 1, 0, 0, 1);
    }
    return;
  }

  auto *parser = WallpadParserFactory::getActiveParser();
  if (!parser)
    return;

  if (!parser->isAckPacket(span<const uint8_t>(ack.data.data(), ack.length)))
    return;

  uint8_t dev_id = 0, sub1 = 0, sub2 = 0;
  if (!parser->extractDeviceKey(span<const uint8_t>(ack.data.data(), ack.length), dev_id, sub1, sub2)) {
    return;
  }

  // 0x2A (신발장 서브 패널 / 원격검침)는 제어 단말기가 아니므로 상태 캐시에서 완전 제외
  if (dev_id == 0x2A) {
    return;
  }

  // 0x81 / NAK / 에러 패킷 필터링 (장비의 명령 거부 응답이 정상 캐시를 오염시키거나 UI를 끄지 않도록 방어)
  if (ack.length >= 7 && (ack.data[5] == 0x81 || ack.data[6] == 0x81)) {
    return;
  }

  DecodedDeviceState st{};

  {
    MutexLocker lock(_cache_mutex);
    DeviceStateEntry *dev = findMutable(dev_id, sub1, sub2, true);
    if (UNLIKELY(!dev)) {
      return;
    }

    // 현대통신 환기(0x2B) 운전 모드(0x43) 패킷의 경우 월패드 가상 응답(0x40 기본 쿼리 응답) 캐시를 덮어쓰지 않음
    bool is_vent_mode_ack = (dev_id == 0x2B && ack.length >= 6 && ack.data[5] == 0x43);
    bool ack_changed = false;

    // 엘리베이터(0x34) 이전 상태 백업: memcpy로 dev->last_ack_data를 덮어쓰기 전에 반드시 수행
    bool prev_pwr = false;
    uint8_t prev_dir = 0;
    uint8_t prev_ho = 0;
    if (dev_id == 0x34) {
      prev_pwr = (dev->last_ack_len > 0) ? (dev->last_ack_data[0] & 0x01) : false;
      prev_dir = dev->last_target_temp;
      prev_ho = (dev->last_ack_len > 1) ? dev->last_ack_data[1] : 0;
    }

    if (!is_vent_mode_ack) {
      ack_changed = (dev->last_ack_len != ack.length || memcmp(dev->last_ack_data.data(), ack.data.data(), ack.length) != 0);
      dev->last_ack_len = ack.length;
      memcpy(dev->last_ack_data.data(), ack.data.data(), ack.length);
    } else {
      // 모드 패킷 수신 시 모드 상태 변경 여부 확인하여 브로드캐스트 트리거
      ack_changed = true;
    }
    dev->last_updated_ms = millis();
    dev->timeout_count = 0;
    dev->is_online = true;

    if (ack_changed) {
      GroupControlTemplate grp{};
      bool has_grp = g_control_registry.findGroup(dev_id, grp);
      if (!has_grp && dev_id == 0x34) {
        grp.dev_id = 0x34;
        grp.coverage.dev_class = DeviceClass::MOMENTARY;
        has_grp = true;
      }

      if (has_grp) {
        decodeDeviceState(grp, ack, dev, st);
        if (dev_id == 0x34) {
          bool ev_state_changed = (st.power != prev_pwr) || (st.direction != prev_dir) || (st.ho != prev_ho);
          if (ev_state_changed) {
            st.should_broadcast = true;
            dev->last_current_temp = static_cast<uint8_t>(st.floor);
            dev->last_target_temp = static_cast<uint8_t>(st.direction);
            dev->last_ack_data[0] = static_cast<uint8_t>(st.power);
            dev->last_ack_data[1] = static_cast<uint8_t>(st.ho);
          }
        } else {
          st.should_broadcast = true;
        }
      }
    }
  } // _cache_mutex unlocked

  if (st.should_broadcast) {
    Mgmt_BroadcastDeviceState(dev_id, sub1, sub2, st.dev_class, st.power, st.target_temp, st.current_temp,
                              st.fan_speed, st.valve_state, st.power_w, st.floor, st.direction, st.ho, st.vent_mode);
  }
}

void DeviceRepository::handlePollingTimeout(const DeviceStateEntry *dev) {
  if (!dev)
    return;
  MutexLocker lock(_cache_mutex);
  auto *mdev = const_cast<DeviceStateEntry *>(dev);
  if (++mdev->timeout_count >= 3)
    mdev->is_online = false;
}

void DeviceRepository::handlePollingTimeout(uint8_t dev_id, uint8_t sub1, uint8_t sub2) {
  MutexLocker lock(_cache_mutex);
  auto *mdev = findMutable(dev_id, sub1, sub2, true);
  if (mdev) {
    if (++mdev->timeout_count >= 3)
      mdev->is_online = false;
  }
}

size_t DeviceRepository::count() const noexcept {
  MutexLocker lock(_cache_mutex);
  return device_count;
}

size_t DeviceRepository::getOnlineCount() const noexcept {
  MutexLocker lock(_cache_mutex);
  size_t online = 0;
  for (size_t i = 0; i < device_count; i++) {
    if (cache[i].is_online && cache[i].last_ack_len > 0)
      online++;
  }
  return online;
}

namespace PacketCodec {
uint8_t calculateChecksum(const uint8_t *data, size_t len) noexcept {
  auto *parser = WallpadParserFactory::getActiveParser();
  return parser ? parser->calculateChecksum(data, len) : 0;
}
} // namespace PacketCodec

namespace PacketBuilder {
void Ch1_BuildQueryPacket(StaticPacket &out, uint8_t dev_id, uint8_t sub1,
                          uint8_t sub2) {
  auto *parser = WallpadParserFactory::getActiveParser();
  if (parser) {
    parser->buildQueryPacket(dev_id, sub1, sub2, out);
  }
}
} // namespace PacketBuilder
static std::atomic<uint32_t> s_last_ch1_tx_ms{0};

void Ch1_RecordTxFinish() {
  s_last_ch1_tx_ms.store(millis(), std::memory_order_release);
}

void Ch1_WaitBusIdle(uint32_t silence_ms) {
  // 1. 연속 제어 명령 간 120ms Guard Interval 보장
  uint32_t last_tx = s_last_ch1_tx_ms.load(std::memory_order_acquire);
  if (last_tx > 0) {
    uint32_t now_tx = millis();
    constexpr uint32_t kGuardIntervalMs = 120;
    if (now_tx - last_tx < kGuardIntervalMs) {
      uint32_t rem_tx = kGuardIntervalMs - (now_tx - last_tx);
      if (rem_tx > 0) {
        vTaskDelay(pdMS_TO_TICKS(rem_tx) > 0 ? pdMS_TO_TICKS(rem_tx) : 1);
      }
    }
  }

  uint32_t last_act = g_ch1_bus_ms.load(std::memory_order_acquire);
  uint32_t now_ms = millis();

  if (now_ms - last_act < silence_ms) {
    uint32_t rem_ms = silence_ms - (now_ms - last_act);
    if (rem_ms > 0) {
      TickType_t delay_ticks = pdMS_TO_TICKS(rem_ms);
      vTaskDelay(delay_ticks > 0 ? delay_ticks : 1);
    }
  }
}

void Ch1_HandleCtrl(const StaticPacket &ctrlPacket) {
  StaticPacket ack_before{};
  uint8_t dev_id = 0, sub1 = 0, sub2 = 0;
  auto *const parser = WallpadParserFactory::getActiveParser();
  bool is_ctrl_query = false;
  if (parser) {
    span<const uint8_t> ctl_span(ctrlPacket.data.data(), ctrlPacket.length);
    is_ctrl_query = parser->isQueryPacket(ctl_span);
    if (parser->extractDeviceKey(ctl_span, dev_id, sub1, sub2)) {
      const auto *cached = g_device_repo.find(dev_id, sub1, sub2);
      if (cached && cached->last_ack_len > 0) {
        ack_before.channel_id = 1;
        ack_before.length = cached->last_ack_len;
        std::copy(cached->last_ack_data.begin(), cached->last_ack_data.begin() + cached->last_ack_len, ack_before.data.begin());
      }
    }
  }

  Ch1_WaitBusIdle(Config::Timing::CH1_INTER_PACKET_DELAY_MS);

  {
    MutexLocker lock(g_uart0_mutex, pdMS_TO_TICKS(100));
    if (!lock.isLocked()) {
      g_pkt_stats.ch1.timeouts.fetch_add(1, std::memory_order_relaxed);
      g_telnet_tracer.trace(
          "[WARN] Dropped CH1 ctrl packet, mutex timed out.\r\n");
      return;
    }

    uart_flush_input(UART_NUM_0);
    uart_write_bytes(UART_NUM_0, ctrlPacket.data.data(), ctrlPacket.length);
    uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(Config::Timing::UART_TX_DONE_TIMEOUT_MS));
    g_ch1_bus_ms.store(millis(), std::memory_order_release);
    Ch1_RecordTxFinish();
    g_pkt_stats.ch1.tx_pkts.fetch_add(1, std::memory_order_relaxed);
  }

  StaticPacket ack;
  if (Uart_RecvPacket(UART_NUM_0, ack,
                      Config::Timing::CH1_POLL_TIMEOUT_MS,
                      nullptr, nullptr,
                      &ctrlPacket) ==
      UartRxStatus::SUCCESS) {
    g_ch1_bus_ms.store(millis(), std::memory_order_release);
    g_telnet_tracer.trace(1, false, TraceType::ACK, ack);
    g_pkt_stats.ch1.rx_pkts.fetch_add(1, std::memory_order_relaxed);
    ack.channel_id = 1;
    g_device_repo.updateFromBus(ack);
    if (dev_id != 0) {
      g_route_registry.recordRoute(1, -1, dev_id, sub1, sub2);
    }
    ack.channel_id = ctrlPacket.channel_id;

    struct WallpadForwardConfig {
      uart_port_t uart_num;
      SemaphoreHandle_t &mutex;
      SingleChannelStats &stats;
    };
    const WallpadForwardConfig wp_cfg[] = {
        {UART_NUM_1, g_uart1_mutex, g_pkt_stats.ch2}, // CH2
        {UART_NUM_2, g_uart2_mutex, g_pkt_stats.ch3}, // CH3
    };

    int wp_idx = static_cast<int>(ctrlPacket.channel_id) - 2; // CH2 → 0, CH3 → 1
    if (wp_idx >= 0 && wp_idx <= 1) {
      const WallpadForwardConfig &cfg = wp_cfg[wp_idx];
      {
        MutexLocker lock(cfg.mutex, pdMS_TO_TICKS(100));
        if (lock.isLocked()) {
          uart_write_bytes(cfg.uart_num, ack.data.data(), ack.length);
          g_telnet_tracer.trace(ctrlPacket.channel_id, true, TraceType::ACK, ack);
          cfg.stats.tx_pkts.fetch_add(1, std::memory_order_relaxed);
        } else {
          g_telnet_tracer.trace("[WARN] UART mutex timeout forwarding ACK\r\n");
        }
      }
    }
  } else {
    g_pkt_stats.ch1.timeouts.fetch_add(1, std::memory_order_relaxed);
    g_telnet_tracer.trace("[WARN] Device did not ACK control packet in time.\r\n");
  }
}
void Ch1_SetState(Ch1State &cur_state, Ch1State new_state) {
  if (cur_state != new_state) {
    Ch1State old = cur_state;
    cur_state = new_state;
    if (new_state == Ch1State::POLL_DEVICE) {
      g_ch1_state_metrics.poll_cnt.fetch_add(1, std::memory_order_relaxed);
    } else if (new_state == Ch1State::VIP_CONTROL) {
      g_ch1_state_metrics.vip_cnt.fetch_add(1, std::memory_order_relaxed);
    } else if (new_state == Ch1State::NORMAL_CONTROL) {
      g_ch1_state_metrics.normal_cnt.fetch_add(1, std::memory_order_relaxed);
    }

    g_ch1_state_metrics.last_from_state.store(old, std::memory_order_relaxed);
    g_ch1_state_metrics.last_to_state.store(new_state,
                                            std::memory_order_relaxed);
    g_ch1_state_metrics.last_transition_ms.store(millis(),
                                                 std::memory_order_relaxed);
  }
}
