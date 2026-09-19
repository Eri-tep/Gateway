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
    e.state_len = 0;
    e.last_target_temp = 0;
    e.last_ack_len = 0;
    e.last_updated_ms = 0;
    e.last_stale_poll_ms = 0;
    e.timeout_count = 0;
    e.is_online = false;
    memset(e.state_data.data(), 0, sizeof(e.state_data));
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
  uint8_t len =
      std::min<uint8_t>(cache[index].state_len, sizeof(out_copy.state_data));
  out_copy.state_len = len;
  memcpy(out_copy.state_data.data(), cache[index].state_data.data(), len);
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

void DeviceRepository::updateFromBus(StaticPacket &ack) {
  if (UNLIKELY(ack.length < 5))
    return;
  auto *parser = WallpadParserFactory::getActiveParser();
  if (!parser)
    return;

  // ★ ACK 패킷만 DevRepo에 등록 - 쿼리(0x01)/제어(0x02)가 섞여서
  // 오프셋 LEARNING 중에 23개 장치가 46개로 2배 등록되는 버그 수정
  if (!parser->isAckPacket(span<const uint8_t>(ack.data.data(), ack.length)))
    return;

  uint8_t dev_id = 0, sub1 = 0, sub2 = 0;
  if (!parser->extractDeviceKey(span<const uint8_t>(ack.data.data(), ack.length), dev_id, sub1, sub2)) {
    return;
  }

  MutexLocker lock(_cache_mutex);
  DeviceStateEntry *dev = findMutable(dev_id, sub1, sub2, true);
  if (UNLIKELY(!dev)) {
    return;
  }

  bool ack_changed = (dev->last_ack_len != ack.length || memcmp(dev->last_ack_data.data(), ack.data.data(), ack.length) != 0);
  dev->last_ack_len = ack.length;
  memcpy(dev->last_ack_data.data(), ack.data.data(), ack.length);
  dev->last_updated_ms = millis();
  dev->timeout_count = 0;
  dev->is_online = true;

  if (ack_changed) {
    const GroupControlTemplate *grp = g_control_registry.findGroup(dev_id);
    if (grp && grp->status == GroupControlTemplate::Status::LOCKED) {
      const char *cls_str = "switch";
      switch (grp->coverage.dev_class) {
        case DeviceClass::THERMOSTAT: cls_str = "thermostat"; break;
        case DeviceClass::VENT:       cls_str = "vent"; break;
        case DeviceClass::GAS:        cls_str = "gas"; break;
        case DeviceClass::MOMENTARY:  cls_str = "momentary"; break;
        case DeviceClass::AIRCON:     cls_str = "aircon"; break;
        default:                      cls_str = "switch"; break;
      }

      bool is_outlet = (grp->coverage.dev_class == DeviceClass::SWITCH) &&
                       (strcasestr(grp->group_name, "Outlet") != nullptr || grp->frame_len >= 17);
      if (is_outlet) {
        cls_str = "outlet";
      }

      int pwr = 0;
      int t_temp = 0, c_temp = 0, spd = 0;
      float power_w = 0.0f;
      int floor = 1;
      int direction = 0;
      const char *v_state = "closed";

      if (grp->ack_slots.power_offset != 0xFF && grp->ack_slots.power_offset < ack.length) {
        uint8_t b = ack.data[grp->ack_slots.power_offset];
        pwr = (b == grp->power_slot.on_val) ? 1 : ((grp->coverage.dev_class == DeviceClass::THERMOSTAT && grp->away_mode_token != 0 && b == grp->away_mode_token) ? 2 : 0);
      } else if (grp->power_slot.discovered && grp->power_slot.ack_state_offset != 0xFF && grp->power_slot.ack_state_offset < ack.length) {
        uint8_t b = ack.data[grp->power_slot.ack_state_offset];
        pwr = (b == grp->power_slot.on_val) ? 1 : ((grp->coverage.dev_class == DeviceClass::THERMOSTAT && grp->away_mode_token != 0 && b == grp->away_mode_token) ? 2 : 0);
      }

      if (grp->coverage.dev_class == DeviceClass::THERMOSTAT) {
        t_temp = dev->last_target_temp > 0 ? dev->last_target_temp : 22;
        c_temp = dev->last_current_temp > 0 ? dev->last_current_temp : t_temp;

        // 컨텍스트 채널 분기별 독립 슬롯 참조 (Zero-Hardcoding / Blueprint Driven)
        uint8_t cat_val = 0;
        if (grp->temp_slot.category_offset != 0xFF && grp->temp_slot.category_offset < ack.length) {
          cat_val = ack.data[grp->temp_slot.category_offset];
        } else if (grp->power_slot.category_offset != 0xFF && grp->power_slot.category_offset < ack.length) {
          cat_val = ack.data[grp->power_slot.category_offset];
        }

        uint8_t target_off = 0xFF;
        uint8_t current_off = 0xFF;

        if (grp->temp_slot.category_val != 0 && cat_val == grp->temp_slot.category_val) {
          // [TEMP Context: 0x45 등] 온도 제어 응답 채널
          target_off = (grp->temp_slot.ack_target_offset != 0xFF) ? grp->temp_slot.ack_target_offset : grp->ack_slots.target_temp_offset;
          current_off = (grp->temp_slot.ack_telemetry_offset != 0xFF) ? grp->temp_slot.ack_telemetry_offset : grp->ack_slots.current_temp_offset;
        } else if (grp->power_slot.category_val != 0 && cat_val == grp->power_slot.category_val) {
          // [POWER Context: 0x46 등] 전원/상태 주기적 응답 채널
          target_off = (grp->power_slot.ack_target_offset != 0xFF) ? grp->power_slot.ack_target_offset : 0xFF;
          current_off = (grp->power_slot.ack_telemetry_offset != 0xFF) ? grp->power_slot.ack_telemetry_offset : grp->ack_slots.current_temp_offset;
        } else {
          // 기본 fallback 슬롯
          target_off = grp->ack_slots.target_temp_offset;
          current_off = grp->ack_slots.current_temp_offset;
        }

        // 외출 모드(pwr == 2) 시 외출 고정 온도로 왜곡되지 않도록 보호
        if (pwr != 2 && target_off != 0xFF && target_off < ack.length) {
          uint8_t b = ack.data[target_off];
          if (b >= 5 && b <= 35) {
            t_temp = b;
            dev->last_target_temp = b;
          }
        }

        if (current_off != 0xFF && current_off < ack.length) {
          uint8_t b = ack.data[current_off];
          if (b >= 5 && b <= 50) {
            c_temp = b;
            dev->last_current_temp = b;
          }
        }
      } else if (grp->coverage.dev_class == DeviceClass::VENT) {
        spd = 1;
        if (grp->ack_slots.fan_speed_offset != 0xFF && grp->ack_slots.fan_speed_offset < ack.length) {
          uint8_t b = ack.data[grp->ack_slots.fan_speed_offset];
          if (b >= 1 && b <= 3) spd = b;
        }
      } else if (grp->coverage.dev_class == DeviceClass::GAS) {
        if (grp->ack_slots.valve_state_offset != 0xFF && grp->ack_slots.valve_state_offset < ack.length) {
          v_state = (ack.data[grp->ack_slots.valve_state_offset] == grp->close_slot.off_val) ? "closed" : "open";
        }
      } else if (is_outlet && ack.length >= 11) {
        uint16_t raw_w = (static_cast<uint16_t>(ack.data[9]) << 8) | ack.data[10];
        if (raw_w < 50000) power_w = static_cast<float>(raw_w) / 10.0f;
      } else if (grp->coverage.dev_class == DeviceClass::MOMENTARY && ack.length >= 6) {
        floor = ack.data[5];
        if (floor < 1 || floor > 60) floor = 1;
        if (ack.length >= 7) direction = ack.data[6];
      }

      Mgmt_BroadcastDeviceState(dev_id, sub1, sub2, cls_str, pwr, t_temp, c_temp, spd, v_state, power_w, floor, direction);
    }
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
  auto *parser = WallpadParserFactory::getActiveParser();
  if (parser) {
    span<const uint8_t> ctl_span(ctrlPacket.data.data(), ctrlPacket.length);
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
    g_device_repo.updateFromBus(ack);
    if (dev_id != 0) {
      g_route_registry.recordRoute(1, -1, dev_id, sub1, sub2);
    }
    g_auto_probing_engine.feedControlPair(
        span<const uint8_t>(ctrlPacket.data.data(), ctrlPacket.length),
        span<const uint8_t>(ack.data.data(), ack.length));
    if (!parser || !parser->isQueryPacket(span<const uint8_t>(ctrlPacket.data.data(), ctrlPacket.length))) {
      // 위저드가 현재 어떤 조작을 기다리는지 semantic hint를 먼저 읽은 후 전달
      AckSlotHint hint = g_telnet_manager.peekWizardHint(dev_id);
      g_control_registry.onControlTransaction(ctrlPacket, ack_before, ack, hint);
      if (dev_id != 0) {
        g_telnet_manager.notifyControlTransaction(dev_id);
      }
    }
    ack.channel_id = ctrlPacket.channel_id;

    // CH2/CH3 포워딩 설정 테이블화
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
        if (lock.isLocked())
          uart_write_bytes(cfg.uart_num, ack.data.data(), ack.length);
      }
      g_telnet_tracer.trace(ctrlPacket.channel_id, true, TraceType::ACK, ack);
      cfg.stats.tx_pkts.fetch_add(1, std::memory_order_relaxed);
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
