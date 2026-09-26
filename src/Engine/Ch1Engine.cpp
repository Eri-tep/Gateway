#include "EngineInternal.h"
#include <algorithm>
#include <cstring>

static inline uint8_t Device_Hash(uint8_t dev_id, uint8_t sub1, uint8_t sub2) noexcept {
  return static_cast<uint8_t>(dev_id + sub1 * 3 + sub2 * 7);
}

static inline uint8_t Device_NormSub1(uint8_t dev_id, uint8_t sub1) noexcept {
  const GroupControlTemplate *grp = g_control_registry.findGroup(dev_id);
  if (grp && grp->power_slot.category_val != 0 && grp->power_slot.category_val != 0xFF) {
    if (sub1 == grp->temp_slot.category_val || sub1 == grp->speed_slot.category_val) {
      return grp->power_slot.category_val;
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
  if (index >= device_count)
    return false;
  MutexLocker lock(_cache_mutex);
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

inline void parseThermostatState(const GroupControlTemplate *grp, const StaticPacket &ack,
                                 DeviceStateEntry *dev, int b_pwr,
                                 int &b_t_temp, int &b_c_temp) {
  b_t_temp = dev->last_target_temp > 0 ? dev->last_target_temp : 22;
  b_c_temp = dev->last_current_temp > 0 ? dev->last_current_temp : b_t_temp;

  uint8_t target_off = grp->getTargetTempOffset(ack.length);
  uint8_t current_off = grp->getCurrentTempOffset(ack.length);

  if (target_off == 0xFF && current_off == 0xFF) {
    uint8_t cat_val = 0;
    if (grp->temp_slot.category_offset != 0xFF && grp->temp_slot.category_offset < ack.length) {
      cat_val = ack.data[grp->temp_slot.category_offset];
    } else if (grp->power_slot.category_offset != 0xFF && grp->power_slot.category_offset < ack.length) {
      cat_val = ack.data[grp->power_slot.category_offset];
    }

    if (grp->temp_slot.category_val != 0 && cat_val == grp->temp_slot.category_val) {
      target_off = (grp->temp_slot.ack_target_offset != 0xFF) ? grp->temp_slot.ack_target_offset : grp->ack_slots.target_temp_offset;
      current_off = (grp->temp_slot.ack_telemetry_offset != 0xFF) ? grp->temp_slot.ack_telemetry_offset : grp->ack_slots.current_temp_offset;
    } else if (grp->power_slot.category_val != 0 && cat_val == grp->power_slot.category_val) {
      target_off = (grp->power_slot.ack_target_offset != 0xFF) ? grp->power_slot.ack_target_offset : 0xFF;
      current_off = (grp->power_slot.ack_telemetry_offset != 0xFF) ? grp->power_slot.ack_telemetry_offset : grp->ack_slots.current_temp_offset;
    }
  }

  if (b_pwr != 2 && target_off != 0xFF && target_off < ack.length) {
    uint8_t b = ack.data[target_off];
    if (b >= 5 && b <= 35) {
      b_t_temp = b;
      dev->last_target_temp = b;
    }
  }

  if (current_off != 0xFF && current_off < ack.length) {
    uint8_t b = ack.data[current_off];
    if (b >= 5 && b <= 50) {
      b_c_temp = b;
      dev->last_current_temp = b;
    }
  }
}

inline void parseVentState(const GroupControlTemplate *grp, const StaticPacket &ack, int b_pwr, int &b_spd, int &b_vent_mode) {
  b_spd = 1;
  b_vent_mode = 1;
  uint8_t spd_off = grp->getFanSpeedOffset(ack.length);
  if (spd_off != 0xFF && spd_off < ack.length) {
    uint8_t raw_b = ack.data[spd_off];
    b_spd = grp->decodeFanSpeed(raw_b);
  }
  // 전원이 켜져 있고 0x43 전용 모드 패킷이거나 모드 슬롯이 유효한 경우에만 모드 파싱
  bool is_mode_pkt = (ack.length >= 6 && ack.data[5] == 0x43);
  if (b_pwr == 1 && is_mode_pkt) {
    uint8_t p_off = grp->getPowerOffset(ack.length);
    if (p_off != 0xFF && p_off < ack.length) {
      uint8_t raw_mode = ack.data[p_off];
      if (raw_mode >= 1 && raw_mode <= 4) {
        b_vent_mode = raw_mode;
      }
    }
  }
}

inline void parseGasState(const GroupControlTemplate *grp, const StaticPacket &ack, const char *&b_v_state) {
  uint8_t v_off = grp->getValveStateOffset(ack.length);
  if (v_off != 0xFF && v_off < ack.length) {
    b_v_state = (ack.data[v_off] == grp->close_slot.off_val) ? "closed" : "open";
  }
}

inline void parseOutletState(const GroupControlTemplate *grp, const StaticPacket &ack, float &b_power_w) {
  uint8_t w_off = grp->getWattageOffset(ack.length);
  if (w_off != 0xFF && w_off + 1 < ack.length) {
    uint16_t raw_w = (static_cast<uint16_t>(ack.data[w_off]) << 8) | ack.data[w_off + 1];
    if (raw_w < 50000) b_power_w = static_cast<float>(raw_w);
  }
}

inline void parseMomentaryState(uint8_t dev_id, const StaticPacket &ack, DeviceStateEntry *dev,
                                int &b_pwr, int &b_floor, int &b_direction, int &b_ho) {
  if (dev_id == 0x34) {
    if (ack.length == 11 && ack.data[4] == 0x04) {
      // 11바이트 상태 ACK (Byte #8: 0x06 = 호출 중, 0x00 = 대기/종료)
      b_pwr = (ack.data[8] == 0x06) ? 1 : 0;
      b_direction = 0;
      b_floor = 15;
      b_ho = 0;
    } else {
      // 그 외 13바이트 브로드캐스트 등은 상태 변경 없이 기존 상태 유지
      b_pwr = dev->last_ack_data[0] & 0x01;
      b_direction = 0;
      b_floor = 15;
      b_ho = 0;
    }
  } else if (ack.length >= 6) {
    b_floor = ack.data[5];
    if (b_floor < 1 || b_floor > 60) b_floor = 1;
    if (ack.length >= 7) b_direction = ack.data[6];
  }
}

} // anonymous namespace

void DeviceRepository::updateFromBus(StaticPacket &ack) {
  // 2차 캐시는 CH#1 (물리 서브기기 응답) 및 CH#5 (EW11 스니핑 응답)만 등록 허용 (CH2, CH3, CH4, CH6 금지)
  if (ack.channel_id != 1 && ack.channel_id != 5) {
    return;
  }

  if (UNLIKELY(ack.length < 5))
    return;
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

  bool should_broadcast = false;
  DeviceClass b_dev_class = DeviceClass::SWITCH;
  int b_pwr = 0, b_t_temp = 0, b_c_temp = 0, b_spd = 0, b_vent_mode = 1, b_floor = 1, b_direction = 0, b_ho = 0;
  float b_power_w = 0.0f;
  const char *b_v_state = "closed";

  {
    MutexLocker lock(_cache_mutex);
    DeviceStateEntry *dev = findMutable(dev_id, sub1, sub2, true);
    if (UNLIKELY(!dev)) {
      return;
    }

    // 현대통신 환기(0x2B) 운전 모드(0x43) 패킷의 경우 월패드 가상 응답(0x40 기본 쿼리 응답) 캐시를 덮어쓰지 않음
    bool is_vent_mode_ack = (dev_id == 0x2B && ack.length >= 6 && ack.data[5] == 0x43);
    bool ack_changed = false;

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
      if (dev_id == 0x34) {
        parseMomentaryState(dev_id, ack, dev, b_pwr, b_floor, b_direction, b_ho);
        // 엘리베이터 실질적 상태 변화가 있을 때만 브로드캐스트 (전원 변경, 방향/도착 변경, 호기 변경)
        // last_target_temp를 이전 direction, last_current_temp를 이전 floor, last_stale_poll_ms를 이전 pwr/ho로 활용
        bool prev_pwr = (dev->last_ack_len > 0) ? (dev->last_ack_data[0] & 0x01) : 0;
        uint8_t prev_dir = dev->last_target_temp;
        uint8_t prev_ho = (dev->last_ack_len > 1) ? dev->last_ack_data[1] : 0;

        bool ev_state_changed = (b_pwr != prev_pwr) || (b_direction != prev_dir) || (b_ho != prev_ho);
        if (ev_state_changed) {
          should_broadcast = true;
          b_dev_class = DeviceClass::MOMENTARY;
          dev->last_current_temp = static_cast<uint8_t>(b_floor);
          dev->last_target_temp = static_cast<uint8_t>(b_direction);
          dev->last_ack_data[0] = static_cast<uint8_t>(b_pwr);
          dev->last_ack_data[1] = static_cast<uint8_t>(b_ho);
        }
      } else {
        const GroupControlTemplate *grp = g_control_registry.findGroup(dev_id);
        if (grp) {
          should_broadcast = true;
          b_dev_class = grp->coverage.dev_class;

          bool is_outlet = (grp->coverage.dev_class == DeviceClass::OUTLET) ||
                           (grp->coverage.dev_class == DeviceClass::SWITCH && grp->frame_len >= 17);

          uint8_t p_off = grp->getPowerOffset(ack.length);
          if (p_off != 0xFF && p_off < ack.length) {
            uint8_t b = ack.data[p_off];
            if (grp->coverage.dev_class == DeviceClass::VENT) {
              b_pwr = (b == 0x01) ? 1 : 0;
            } else if (b == grp->power_slot.on_val) {
              b_pwr = 1;
            } else if (b == grp->power_slot.off_val || b == 0) {
              b_pwr = 0;
            } else if (grp->coverage.dev_class == DeviceClass::THERMOSTAT && grp->away_mode_token != 0 && b == grp->away_mode_token) {
              b_pwr = 2;
            } else {
              b_pwr = 0;
            }
          }

          switch (grp->coverage.dev_class) {
            case DeviceClass::THERMOSTAT:
              parseThermostatState(grp, ack, dev, b_pwr, b_t_temp, b_c_temp);
              break;
            case DeviceClass::VENT:
              parseVentState(grp, ack, b_pwr, b_spd, b_vent_mode);
              break;
            case DeviceClass::GAS:
              parseGasState(grp, ack, b_v_state);
              break;
            case DeviceClass::MOMENTARY:
              parseMomentaryState(dev_id, ack, dev, b_pwr, b_floor, b_direction, b_ho);
              break;
            case DeviceClass::AIRCON:
              break;
            case DeviceClass::OUTLET:
              parseOutletState(grp, ack, b_power_w);
              break;
            default:
              if (is_outlet) {
                b_dev_class = DeviceClass::OUTLET;
                parseOutletState(grp, ack, b_power_w);
              }
              break;
          }
        }
      }
    }
  } // _cache_mutex unlocked

  if (should_broadcast) {
    Mgmt_BroadcastDeviceState(dev_id, sub1, sub2, b_dev_class, b_pwr, b_t_temp, b_c_temp, b_spd, b_v_state, b_power_w, b_floor, b_direction, b_ho, b_vent_mode);
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
void Ch1_WaitBusIdle(uint32_t silence_ms) {
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
