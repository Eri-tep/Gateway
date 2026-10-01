#include "Engine.h"
#include "Core.h"
#include "Protocol.h"
#include "Console.h"
#include "Service.h"

#include "esp_task_wdt.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

// ============================================================================
// Internal Types & Forward Declarations
// ============================================================================

enum class UartRxStatus { SUCCESS, TIMEOUT };
using UartPollCallback = void (*)(void *ctx);

QueueHandle_t Uart_GetEventQueue(uart_port_t u_num);
UartRxStatus Uart_RecvPacket(uart_port_t u_num, StaticPacket &out,
                            uint32_t tout_ms,
                            UartPollCallback on_poll = nullptr,
                            void *poll_ctx = nullptr,
                            const StaticPacket *echo_match = nullptr);

void Ch1_WaitBusIdle(uint32_t silence_ms);
void Ch1_HandleCtrl(const StaticPacket &ctrlPacket);
void Ch1_PollNext(size_t &current_dev_idx);
void Ch1_SetState(Ch1State &cur_state, Ch1State new_state);

namespace PacketBuilder {
void Ch1_BuildQueryPacket(StaticPacket &out, uint8_t dev_id, uint8_t sub1,
                          uint8_t sub2);
}

// ============================================================================
// 1. Hardware Metrics Tracker & Diagnostics (formerly Engine.cpp)
// ============================================================================

namespace {
constexpr uint32_t MIN_SAMPLE_INTERVAL_MS = 100;
constexpr uint32_t CPU0_BASE_LOAD = 3;
constexpr uint32_t CPU0_PPS_DIVISOR = 3;
constexpr uint32_t CPU1_BASE_LOAD = 2;
constexpr uint32_t CPU1_PPS_DIVISOR = 8;
} // namespace

#ifdef __cplusplus
extern "C" {
#endif
float temperatureRead(void);
#ifdef __cplusplus
}
#endif

int8_t System_ReadTempC() { return static_cast<int8_t>(temperatureRead()); }

void System_ReadCpuPct(uint8_t &cpu0_out, uint8_t &cpu1_out) {
  static std::atomic<uint32_t> s_last_time_ms{0}, s_last_ch1{0}, s_last_ch23{0}, s_last_tcp{0};

  uint32_t now_ms = millis();
  uint32_t prev_ms = s_last_time_ms.load(std::memory_order_relaxed);

  uint32_t cur_ch1 = g_pkt_stats.ch1.rx_pkts.load(std::memory_order_relaxed) +
                     g_pkt_stats.ch1.tx_pkts.load(std::memory_order_relaxed);
  uint32_t cur_ch23 = g_pkt_stats.ch2.rx_pkts.load(std::memory_order_relaxed) +
                      g_pkt_stats.ch2.tx_pkts.load(std::memory_order_relaxed) +
                      g_pkt_stats.ch3.rx_pkts.load(std::memory_order_relaxed) +
                      g_pkt_stats.ch3.tx_pkts.load(std::memory_order_relaxed) +
                      g_pkt_stats.ch4.rx_pkts.load(std::memory_order_relaxed) +
                      g_pkt_stats.ch4.tx_pkts.load(std::memory_order_relaxed);
  uint32_t cur_tcp = g_pkt_stats.ch5.rx_pkts.load(std::memory_order_relaxed) +
                     g_pkt_stats.ch5.tx_pkts.load(std::memory_order_relaxed) +
                     g_pkt_stats.ch6.rx_pkts.load(std::memory_order_relaxed) +
                     g_pkt_stats.ch6.tx_pkts.load(std::memory_order_relaxed);

  uint32_t elapsed_ms = now_ms - prev_ms;
  if (!prev_ms || elapsed_ms < MIN_SAMPLE_INTERVAL_MS) {
    if (!prev_ms) {
      s_last_time_ms.store(now_ms, std::memory_order_relaxed);
      s_last_ch1.store(cur_ch1, std::memory_order_relaxed);
      s_last_ch23.store(cur_ch23, std::memory_order_relaxed);
      s_last_tcp.store(cur_tcp, std::memory_order_relaxed);
    }
    cpu0_out = 4;
    cpu1_out = 3;
    return;
  }

  auto get_delta = [](uint32_t cur, std::atomic<uint32_t> &last) {
    uint32_t prev = last.exchange(cur, std::memory_order_relaxed);
    return (cur >= prev) ? (cur - prev) : cur;
  };

  s_last_time_ms.store(now_ms, std::memory_order_relaxed);
  uint32_t delta_tcp = get_delta(cur_tcp, s_last_tcp);
  uint32_t delta_uart = get_delta(cur_ch1, s_last_ch1) + get_delta(cur_ch23, s_last_ch23);

  uint32_t tcp_pps = static_cast<uint32_t>((static_cast<uint64_t>(delta_tcp) * 1000) / elapsed_ms);
  uint32_t load0 = CPU0_BASE_LOAD + (tcp_pps / CPU0_PPS_DIVISOR);
  if (WiFi.isConnected()) load0 += 1;
  if (g_pkt_stats.ch6.is_connected.load(std::memory_order_relaxed)) load0 += 1;

  uint32_t uart_pps = static_cast<uint32_t>((static_cast<uint64_t>(delta_uart) * 1000) / elapsed_ms);
  uint32_t load1 = CPU1_BASE_LOAD + (uart_pps / CPU1_PPS_DIVISOR);

  cpu0_out = static_cast<uint8_t>(std::min(load0, 99U));
  cpu1_out = static_cast<uint8_t>(std::min(load1, 99U));
}

void SystemMetricsTracker::init() {
  if (!_metrics_mutex)
    _metrics_mutex = xSemaphoreCreateMutex();
  _cached_flash_kb = _current.flash_kb = static_cast<uint16_t>(ESP.getSketchSize() / 1024);
  memset(&_cur_bucket, 0, sizeof(_cur_bucket));
}

void SystemMetricsTracker::reset() {
  MutexLocker lock(_metrics_mutex);
  _ring15_head = 0;
  _ring15_count = 0;
  _ring24_head = 0;
  _ring24_count = 0;
  _bucket_sample_count = 0;
  memset(&_cur_bucket, 0, sizeof(_cur_bucket));
}

void SystemMetricsTracker::addSample(uint8_t cpu0_pct, uint8_t cpu1_pct,
                                     uint16_t ram_kb, int8_t temp_c) {
  const uint16_t flash_kb = _cached_flash_kb;
  MutexLocker lock(_metrics_mutex);
  _current = {cpu0_pct, cpu1_pct, ram_kb, flash_kb, temp_c};

  _ring15[_ring15_head] = _current;
  _ring15_head = (_ring15_head + 1) % SAMPLES_15M;
  if (_ring15_count < SAMPLES_15M)
    _ring15_count++;

  _cur_bucket.cpu0_sum += cpu0_pct;
  _cur_bucket.cpu1_sum += cpu1_pct;
  _cur_bucket.ram_sum += ram_kb;
  _cur_bucket.temp_sum += temp_c;

  _cur_bucket.cpu0_peak = std::max(_cur_bucket.cpu0_peak, cpu0_pct);
  _cur_bucket.cpu1_peak = std::max(_cur_bucket.cpu1_peak, cpu1_pct);
  _cur_bucket.ram_peak = std::max(_cur_bucket.ram_peak, ram_kb);
  if (_cur_bucket.count == 0 || temp_c > _cur_bucket.temp_peak) {
    _cur_bucket.temp_peak = temp_c;
  }

  _cur_bucket.count++;
  _bucket_sample_count++;

  if (_bucket_sample_count >= SAMPLES_15M) {
    _ring24[_ring24_head] = _cur_bucket;
    _ring24_head = (_ring24_head + 1) % BUCKETS_24H;
    if (_ring24_count < BUCKETS_24H)
      _ring24_count++;
    memset(&_cur_bucket, 0, sizeof(_cur_bucket));
    _bucket_sample_count = 0;
  }
}

namespace {
struct MetricAccumulator {
  uint32_t cpu0_sum = 0, cpu1_sum = 0, ram_sum = 0, flash_sum = 0;
  int32_t temp_sum = 0;
  uint8_t cpu0_peak = 0, cpu1_peak = 0;
  uint16_t ram_peak = 0, flash_peak = 0;
  int8_t temp_peak = -127;
  uint32_t count = 0;

  void add(uint8_t c0, uint8_t c1, uint16_t ram, uint16_t flash, int8_t temp) {
    cpu0_sum += c0; cpu1_sum += c1; ram_sum += ram; flash_sum += flash; temp_sum += temp;
    count++;
    cpu0_peak = std::max(cpu0_peak, c0);
    cpu1_peak = std::max(cpu1_peak, c1);
    ram_peak = std::max(ram_peak, ram);
    flash_peak = std::max(flash_peak, flash);
    temp_peak = std::max(temp_peak, temp);
  }

  void addBucket(const MetricBucket &b) {
    if (!b.count) return;
    cpu0_sum += b.cpu0_sum; cpu1_sum += b.cpu1_sum; ram_sum += b.ram_sum; temp_sum += b.temp_sum;
    count += b.count;
    cpu0_peak = std::max(cpu0_peak, b.cpu0_peak);
    cpu1_peak = std::max(cpu1_peak, b.cpu1_peak);
    ram_peak = std::max(ram_peak, b.ram_peak);
    temp_peak = std::max(temp_peak, b.temp_peak);
  }

  StatSummary finalize(uint16_t fallback_flash = 0) const {
    StatSummary r = {};
    if (!count) return r;
    r.cpu0_avg = cpu0_sum / count;
    r.cpu0_peak = cpu0_peak;
    r.cpu1_avg = cpu1_sum / count;
    r.cpu1_peak = cpu1_peak;
    r.ram_avg = ram_sum / count;
    r.ram_peak = ram_peak;
    r.flash_avg = flash_sum ? (flash_sum / count) : fallback_flash;
    r.flash_peak = flash_peak ? flash_peak : fallback_flash;
    r.temp_avg = temp_sum / static_cast<int32_t>(count);
    r.temp_peak = temp_peak;
    r.count = static_cast<uint16_t>(std::min<uint32_t>(count, 65535U));
    return r;
  }
};
} // namespace

StatSummary SystemMetricsTracker::get15m() const {
  MutexLocker lock(_metrics_mutex);
  MetricAccumulator acc;
  for (size_t i = 0; i < _ring15_count; i++) {
    const auto &ms = _ring15[i];
    acc.add(ms.cpu0_pct, ms.cpu1_pct, ms.ram_kb, ms.flash_kb, ms.temp_c);
  }
  return acc.finalize();
}

StatSummary SystemMetricsTracker::get24h() const {
  MutexLocker lock(_metrics_mutex);
  MetricAccumulator acc;
  for (size_t i = 0; i < _ring24_count; i++) {
    acc.addBucket(_ring24[i]);
  }
  acc.addBucket(_cur_bucket);
  return acc.finalize(_cached_flash_kb);
}



// ============================================================================
// 2. UART RX Stream Demux & Packet Validation (formerly UartRx.cpp)
// ============================================================================

static inline bool Uart_DrainToStreamBuffer(uart_port_t u_num, uint8_t *stream, size_t &stream_len,
                                            size_t max_stream_buf, uint32_t &last_rx_ms) {
  size_t avail = 0;
  uart_get_buffered_data_len(u_num, &avail);
  if (avail == 0)
    return false;

  uint8_t temp[Config::Packet::UART_READ_CHUNK];
  size_t read_limit = std::min(avail, sizeof(temp));
  int rx = uart_read_bytes(u_num, temp, read_limit, 0);
  if (rx <= 0)
    return false;

  if (stream_len + rx > max_stream_buf) {
    size_t overflow = (stream_len + rx) - max_stream_buf;
    if (overflow < stream_len) {
      memmove(stream, stream + overflow, stream_len - overflow);
      stream_len -= overflow;
    } else {
      stream_len = 0;
    }
  }
  size_t copy_len = std::min(static_cast<size_t>(rx), max_stream_buf - stream_len);
  memcpy(stream + stream_len, temp, copy_len);
  stream_len += copy_len;
  last_rx_ms = millis();
  return true;
}

QueueHandle_t Uart_GetEventQueue(uart_port_t u_num) {
  switch (u_num) {
  case UART_NUM_0: return g_uart0_event_queue;
  case UART_NUM_1: return g_uart1_event_queue;
  case UART_NUM_2: return g_uart2_event_queue;
  default: return nullptr;
  }
}

UartRxStatus Uart_RecvPacket(uart_port_t u_num, StaticPacket &out,
                            uint32_t tout_ms,
                            UartPollCallback on_poll,
                            void *poll_ctx,
                            const StaticPacket *echo_match) {
  uint8_t stream[Config::Packet::MAX_STREAM_BUF];
  size_t stream_len = 0;
  uint32_t start_ms = millis();
  uint32_t last_rx_ms = 0;
  auto *const parser = WallpadParserFactory::getActiveParser();
  const bool is_auto_unlocked = (parser && parser->isAutoMode() && !parser->isLocked());
  const uint8_t stx = parser ? parser->getStx() : PKT_STX;
  QueueHandle_t evt_q = Uart_GetEventQueue(u_num);

  while (millis() - start_ms < tout_ms) {
    esp_task_wdt_reset();
    if (on_poll)
      on_poll(poll_ctx);

    if (stream_len >= 3) {
      if (is_auto_unlocked) {
        if (last_rx_ms > 0 && TimeUtils::isElapsed(last_rx_ms, Config::Timing::WALLPAD_AUTO_IPG_MS)) {
          g_auto_probing_engine.feedFrame(span<const uint8_t>(stream, stream_len));

          if (echo_match && echo_match->length == stream_len &&
              memcmp(echo_match->data.data(), stream, stream_len) == 0) {
            stream_len = 0;
            last_rx_ms = 0;
            continue;
          }

          size_t copy_len = std::min(stream_len, out.data.size());
          out.length = static_cast<uint8_t>(copy_len);
          memcpy(out.data.data(), stream, copy_len);
          stream_len = 0;
          last_rx_ms = 0;
          return UartRxStatus::SUCCESS;
        }
      } else {
        size_t idx = 0;
        while (idx < stream_len) {
          if (stream[idx] != stx) {
            idx++;
            continue;
          }

          int len_res = parser ? parser->extractPacketLength(stream, stream_len, idx) : -1;
          if (len_res == 0) {
            break;
          }
          if (len_res < 0) {
            idx++;
            continue;
          }

          uint8_t pkt_len = static_cast<uint8_t>(len_res);
          uint8_t *pkt = &stream[idx];
          span<const uint8_t> pkt_span(pkt, pkt_len);
          if (!parser->validatePacket(pkt_span)) {
            uint8_t ch = (u_num == UART_NUM_0) ? 1 : (u_num == UART_NUM_1) ? 2 : 3;
            StaticPacket drp_pkt{ch, pkt_len};
            memcpy(drp_pkt.data.data(), pkt, pkt_len);
            g_telnet_tracer.trace(ch, false, TraceType::DRP, drp_pkt);
            idx++;
            continue;
          }

          if (echo_match && echo_match->length == pkt_len &&
              memcmp(echo_match->data.data(), pkt, pkt_len) == 0) {
            idx += pkt_len;
            continue;
          }

          out.length = pkt_len;
          memcpy(out.data.data(), pkt, pkt_len);
          size_t consumed = idx + pkt_len;
          if (consumed < stream_len)
            memmove(stream, stream + consumed, stream_len - consumed);
          stream_len = (consumed < stream_len) ? (stream_len - consumed) : 0;
          return UartRxStatus::SUCCESS;
        }

        if (idx > 0) {
          if (idx < stream_len)
            memmove(stream, stream + idx, stream_len - idx);
          stream_len = (idx < stream_len) ? (stream_len - idx) : 0;
        }
      }
    }

    uint32_t elapsed = millis() - start_ms;
    if (elapsed >= tout_ms)
      break;
    uint32_t rem_ms = tout_ms - elapsed;
    uint32_t wait_ms = std::min<uint32_t>(rem_ms, 5);

    bool received_new_bytes = false;
    if (evt_q) {
      uart_event_t evt;
      if (xQueueReceive(evt_q, &evt, pdMS_TO_TICKS(wait_ms)) == pdTRUE) {
        if (evt.type == UART_DATA) {
          received_new_bytes = Uart_DrainToStreamBuffer(u_num, stream, stream_len, sizeof(stream), last_rx_ms);
        } else if (evt.type == UART_FIFO_OVF || evt.type == UART_BUFFER_FULL) {
          uart_flush_input(u_num);
          xQueueReset(evt_q);
        }
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(std::min<uint32_t>(wait_ms, 2)));
    }

    if (!received_new_bytes) {
      Uart_DrainToStreamBuffer(u_num, stream, stream_len, sizeof(stream), last_rx_ms);
    }
  }

  if (stream_len >= 3 && is_auto_unlocked) {
    g_auto_probing_engine.feedFrame(span<const uint8_t>(stream, stream_len));
    if (!(echo_match && echo_match->length == stream_len &&
          memcmp(echo_match->data.data(), stream, stream_len) == 0)) {
      size_t copy_len = std::min(stream_len, out.data.size());
      out.length = static_cast<uint8_t>(copy_len);
      memcpy(out.data.data(), stream, copy_len);
      return UartRxStatus::SUCCESS;
    }
  }

  return UartRxStatus::TIMEOUT;
}

// ============================================================================
// 3. Device State Repository & CH1 Control Pipeline (formerly Ch1Engine.cpp)
// ============================================================================

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

static void decodeOutlet(const GroupControlTemplate &grp, const StaticPacket &ack,
                         const DeviceStateEntry *, DecodedDeviceState &out) {
  uint8_t w_off = grp.getWattageOffset(ack.length);
  if (w_off + 1 < ack.length) {
    uint16_t raw_w = (static_cast<uint16_t>(ack.data[w_off]) << 8) | ack.data[w_off + 1];
    out.power_w = (raw_w < 50000) ? static_cast<float>(raw_w) : 0.0f;
  }
}

static void decodeSwitch(const GroupControlTemplate &grp, const StaticPacket &ack,
                         const DeviceStateEntry *dev, DecodedDeviceState &out) {
  if (grp.frame_len >= 17) {
    out.dev_class = DeviceClass::OUTLET;
    decodeOutlet(grp, ack, dev, out);
  }
}

static void decodeGas(const GroupControlTemplate &grp, const StaticPacket &ack,
                      const DeviceStateEntry *, DecodedDeviceState &out) {
  uint8_t v_off = grp.getValveStateOffset(ack.length);
  bool is_closed = (v_off >= ack.length || ack.data[v_off] == grp.close_slot.off_val);
  snprintf(out.valve_state, sizeof(out.valve_state), "%s", is_closed ? "closed" : "open");
}

static void decodeMomentary(const GroupControlTemplate &grp, const StaticPacket &ack,
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

static void decodeThermostat(const GroupControlTemplate &grp, const StaticPacket &ack,
                             const DeviceStateEntry *dev, DecodedDeviceState &out) {
  out.target_temp = dev ? dev->last_target_temp : 0;
  out.current_temp = (dev && dev->last_current_temp > 0) ? dev->last_current_temp : out.target_temp;

  uint8_t p_off = grp.getPowerOffset(ack.length);
  if (p_off < ack.length && grp.away_mode_token != 0 && ack.data[p_off] == grp.away_mode_token)
    out.power = 2;

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

static void decodeVent(const GroupControlTemplate &grp, const StaticPacket &ack,
                       const DeviceStateEntry *, DecodedDeviceState &out) {
  uint8_t p_off = grp.getPowerOffset(ack.length);
  out.power = (p_off < ack.length && ack.data[p_off] == 0x01) ? 1 : 0;
  out.fan_speed = 1;
  out.vent_mode = 1;

  uint8_t spd_off = grp.getFanSpeedOffset(ack.length);
  if (spd_off < ack.length)
    out.fan_speed = grp.decodeFanSpeed(ack.data[spd_off]);

  if (out.power == 1 && (ack.length >= 6 && ack.data[5] == 0x43) && p_off < ack.length) {
    uint8_t m = ack.data[p_off];
    if (m >= 1 && m <= 4) out.vent_mode = m;
  }
}

static void decodeAircon(const GroupControlTemplate &, const StaticPacket &ack,
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

static void decodeUnknown(const GroupControlTemplate &, const StaticPacket &,
                          const DeviceStateEntry *, DecodedDeviceState &) {}

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

namespace {
static bool handleLegacyThermostatBroadcast(DeviceRepository &repo, const StaticPacket &ack, SemaphoreHandle_t mutex) {
  if (ack.length != 34 || ack.data[3] != 0x18 || ack.data[4] != 0x04)
    return false;

  for (uint8_t r = 1; r <= 8; ++r) {
    size_t base = 8 + (r - 1) * 3;
    uint8_t r_state = ack.data[base];
    uint8_t r_amb   = ack.data[base + 1];
    uint8_t r_tgt   = ack.data[base + 2];
    if (r_state == 0x00) continue;

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
    Mgmt_BroadcastDeviceState(0x18, r_sub1, 0, DeviceClass::THERMOSTAT, r_pwr, r_tgt, r_amb, 0, "closed", 0.0f, 1, 0, 0, 1);
  }
  return true;
}

static inline void handleElevatorSpecialState(DeviceStateEntry *dev, DecodedDeviceState &st,
                                             bool prev_pwr, uint8_t prev_dir, uint8_t prev_ho) {
  bool ev_state_changed = (st.power != prev_pwr) || (st.direction != prev_dir) || (st.ho != prev_ho);
  if (ev_state_changed) {
    st.should_broadcast = true;
    dev->last_current_temp = static_cast<uint8_t>(st.floor);
    dev->last_target_temp = static_cast<uint8_t>(st.direction);
    dev->last_ack_data[0] = static_cast<uint8_t>(st.power);
    dev->last_ack_data[1] = static_cast<uint8_t>(st.ho);
  }
}
} // namespace

void DeviceRepository::updateFromBus(StaticPacket &ack) {
  // 2차 캐시는 CH#1 (물리 서브기기 응답) 및 CH#5 (EW11 스니핑 응답)만 등록 허용 (CH2, CH3, CH4, CH6 금지)
  if (ack.channel_id != 1 && ack.channel_id != 5) {
    return;
  }

  if (UNLIKELY(ack.length < 5))
    return;

  // 구형(Legacy) 34B 난방 브로드캐스트 처리 (Packet[1]==0x22 && Dev==0x18 && Opcode==0x04)
  if (handleLegacyThermostatBroadcast(*this, ack, _cache_mutex)) {
    return;
  }

  auto *parser = WallpadParserFactory::getActiveParser();
  if (!parser || !parser->isAckPacket(span<const uint8_t>(ack.data.data(), ack.length)))
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
          handleElevatorSpecialState(dev, st, prev_pwr, prev_dir, prev_ho);
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
  uint8_t dev_id = 0, sub1 = 0, sub2 = 0;
  auto *const parser = WallpadParserFactory::getActiveParser();
  if (parser) {
    span<const uint8_t> ctl_span(ctrlPacket.data.data(), ctrlPacket.length);
    parser->extractDeviceKey(ctl_span, dev_id, sub1, sub2);
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

// ============================================================================
// 4. CH1 Polling Master Loop & Task (formerly Ch1Polling.cpp)
// ============================================================================

namespace {
constexpr uint32_t CACHE_CONVERGENCE_STABLE_MS = 1500;
} // namespace

namespace {
static inline int Ch1_ScoreCandidate(const PollingTargetRegistry::PollingCandidate &tgt,
                                     const DeviceStateEntry *cached_dev) {
  constexpr uint8_t CH23_MASK = (1 << 2) | (1 << 3);
  if (tgt.source_channels != 0 && (tgt.source_channels & CH23_MASK) == 0) {
    return 999;
  }

  RouteEndpoint ep;
  if (g_route_registry.lookupRoute(tgt.dev_id, tgt.sub1, tgt.sub2, ep) && ep.channel_id == 5) {
    return 999;
  }

  if (tgt.raw_ack_len == 0 || !cached_dev || cached_dev->last_updated_ms == 0) {
    return 1;
  }
  if (cached_dev->is_online) {
    return 2;
  }
  if (TimeUtils::isElapsed(cached_dev->last_stale_poll_ms, Config::Timing::CH1_STALE_POLL_INTERVAL_MS)) {
    return 3;
  }
  return 999;
}
} // namespace

void Ch1_PollNext(size_t &current_dev_idx) {
  g_polling_targets.sweepExpired(Config::Timing::STALE_DEVICE_THRESHOLD_MS);

  // 불필요한 9.7KB BSS 버퍼를 제거하고 224B 경량 메타데이터 스택 배열 활용
  PollingTargetRegistry::PollingCandidate candidates[PollingTargetRegistry::MAX_TARGETS];
  size_t active_cnt = g_polling_targets.getActiveCandidates(candidates, PollingTargetRegistry::MAX_TARGETS);

  uint8_t poll_dev_id = 0, poll_sub1 = 0, poll_sub2 = 0;
  const uint8_t *poll_raw_ptr = nullptr;
  uint8_t poll_raw_len = 0;
  bool target_selected = false;
  uint32_t now = millis();

  if (!target_selected && active_cnt > 0) {
    int best_prio = 999;
    size_t best_idx = 0;

    for (size_t i = 0; i < active_cnt; i++) {
      size_t idx = (current_dev_idx + i) % active_cnt;
      const auto &tgt = candidates[idx];
      const auto *cached_dev = g_device_repo.find(tgt.dev_id, tgt.sub1, tgt.sub2);
      int score = Ch1_ScoreCandidate(tgt, cached_dev);

      if (score < best_prio) {
        best_prio = score;
        best_idx = idx;
        if (score == 1)
          break; // 최우선 순위 발견 즉시 탐색 중단
      }
    }

    if (best_prio <= 3) {
      const auto &tgt = candidates[best_idx];
      poll_dev_id = tgt.dev_id;
      poll_sub1 = tgt.sub1;
      poll_sub2 = tgt.sub2;
      poll_raw_len = tgt.raw_query_len;
      if (poll_raw_len > 0) {
        g_polling_targets.getQueryData(tgt.entry_idx, poll_raw_ptr, poll_raw_len);
      }

      if (best_prio == 3) {
        g_device_repo.setLastStalePollMs(tgt.dev_id, tgt.sub1, tgt.sub2, now);
        g_ch1_state_metrics.stale_poll_cnt.fetch_add(1, std::memory_order_relaxed);
      }

      current_dev_idx = (best_idx + 1) % active_cnt;
      target_selected = true;
    }
  }

  if (!target_selected) {
    size_t dev_cnt = g_device_repo.count();
    if (dev_cnt > 0) {
      size_t idx = current_dev_idx % dev_cnt;
      auto *dev = g_device_repo.getAt(idx);
      current_dev_idx = (idx + 1) % dev_cnt;
      if (dev && (dev->is_online || dev->last_updated_ms == 0 ||
                  TimeUtils::isElapsed(dev->last_stale_poll_ms, Config::Timing::CH1_STALE_POLL_INTERVAL_MS))) {
        RouteEndpoint ep;
        if (!g_route_registry.lookupRoute(dev->dev_id, dev->sub1, dev->sub2, ep) || ep.channel_id != 5) {
          poll_dev_id = dev->dev_id;
          poll_sub1 = dev->sub1;
          poll_sub2 = dev->sub2;
          if (!dev->is_online)
            g_device_repo.setLastStalePollMsByIndex(idx, now);
          target_selected = true;
        }
      }
    }
  }

  if (target_selected) {
    constexpr uint8_t kMaxRetries = 3;
    constexpr uint32_t kDelayMs = Config::Timing::CH1_INTER_PACKET_DELAY_MS;
    constexpr TickType_t kUartLockTimeout = pdMS_TO_TICKS(5);
    bool sent = false;

    for (uint8_t retry = 0; retry < kMaxRetries; ++retry) {
      Ch1_WaitBusIdle(kDelayMs);

      MutexLocker lock(g_uart0_mutex, kUartLockTimeout);
      if (!lock.isLocked()) {
        // High-priority control transaction in progress on UART0 (Policy A: abort poll cycle)
        return;
      }

      uint32_t last_act = g_ch1_bus_ms.load(std::memory_order_acquire);
      uint32_t elapsed = millis() - last_act;
      if (elapsed < kDelayMs) {
        // TOCTOU: bus became active right before lock acquisition
        continue;
      }

      StaticPacket q_pkt;
      if (poll_raw_len > 0 && poll_raw_ptr) {
        q_pkt.channel_id = 1;
        q_pkt.length = poll_raw_len;
        memcpy(q_pkt.data.data(), poll_raw_ptr, poll_raw_len);
      } else {
        PacketBuilder::Ch1_BuildQueryPacket(q_pkt, poll_dev_id, poll_sub1,
                                             poll_sub2);
      }
      g_telnet_tracer.trace(1, true, TraceType::QRY, q_pkt);

      uart_flush_input(UART_NUM_0);
      uart_write_bytes(UART_NUM_0, q_pkt.data.data(), q_pkt.length);
      uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(Config::Timing::UART_TX_DONE_TIMEOUT_MS));
      g_ch1_bus_ms.store(millis(), std::memory_order_release);
      g_pkt_stats.ch1.tx_pkts.fetch_add(1, std::memory_order_relaxed);

      StaticPacket ack;
      if (Uart_RecvPacket(UART_NUM_0, ack,
                          Config::Timing::CH1_POLL_TIMEOUT_MS,
                          nullptr, nullptr,
                          &q_pkt) ==
          UartRxStatus::SUCCESS) {
        g_ch1_bus_ms.store(millis(), std::memory_order_release);
        g_telnet_tracer.trace(1, false, TraceType::ACK, ack);
        g_pkt_stats.ch1.rx_pkts.fetch_add(1, std::memory_order_relaxed);
        ack.channel_id = 1;
        g_polling_targets.updateResponse(q_pkt.data.data(), q_pkt.length,
                                         ack.data.data(), ack.length);
        g_device_repo.updateFromBus(ack);
        g_polling_targets.markVerified(poll_dev_id, poll_sub1, poll_sub2);
        g_route_registry.recordRoute(1, -1, poll_dev_id, poll_sub1, poll_sub2);
        g_auto_probing_engine.feedOpcodePair(
            span<const uint8_t>(q_pkt.data.data(), q_pkt.length),
            span<const uint8_t>(ack.data.data(), ack.length));
      } else {
        g_pkt_stats.ch1.timeouts.fetch_add(1, std::memory_order_relaxed);
        g_device_repo.handlePollingTimeout(poll_dev_id, poll_sub1, poll_sub2);
      }

      sent = true;
      break;
    }

    if (!sent) {
      // Abort poll cycle due to repeated TOCTOU bus activity
      return;
    }
  }
}

void Task_Ch1(void *pvParameters) {
  esp_task_wdt_add(nullptr);
  if (g_system_event_group) {
    xEventGroupWaitBits(g_system_event_group, SYS_EVT_SYSTEM_RUNNING, pdFALSE, pdFALSE, portMAX_DELAY);
  }
  StaticPacket ctrlPacket;
  size_t current_dev_idx = 0;
  Ch1State current_state = Ch1State::IDLE;

  static uint32_t s_stable_start_ms = 0;
  static size_t s_last_active_tgts = 0;
  static bool s_convergence_done = false;
  uint32_t next_poll_due_ms = millis();

  for (;;) {
    g_wdt_monitor.feed(0);
    if (UNLIKELY(g_ota_in_progress.load(std::memory_order_relaxed))) {
      Ch1_SetState(current_state, Ch1State::IDLE);
      if (g_system_event_group) {
        xEventGroupWaitBits(g_system_event_group, SYS_EVT_OTA_IDLE, pdFALSE, pdFALSE, pdMS_TO_TICKS(1000));
      } else {
        vTaskDelay(pdMS_TO_TICKS(100));
      }
      next_poll_due_ms = millis();
      continue;
    }

    uart_event_t u_evt;
    while (xQueueReceive(g_uart0_event_queue, (void *)&u_evt, 0) == pdTRUE) {
      if (u_evt.type == UART_FIFO_OVF || u_evt.type == UART_BUFFER_FULL) {
        g_pkt_stats.ch1.invalid_frames.fetch_add(1, std::memory_order_relaxed);
        uart_flush_input(UART_NUM_0);
      } else if (u_evt.type == UART_PARITY_ERR ||
                 u_evt.type == UART_FRAME_ERR) {
        g_pkt_stats.ch1.crc_errors.fetch_add(1, std::memory_order_relaxed);
      }
    }

    if (g_probe_convergence_reset.load(std::memory_order_acquire)) {
      g_probe_convergence_reset.store(false, std::memory_order_release);
      s_convergence_done = false;
      s_stable_start_ms = 0;
      s_last_active_tgts = 0;
      g_initial_caching_complete.store(false, std::memory_order_release);
      if (g_system_event_group) {
        xEventGroupClearBits(g_system_event_group, SYS_EVT_CACHE_READY);
      }
      g_telnet_tracer.trace("[AUTO PROBE] Convergence state reset. Re-learning bus offsets...\r\n");
    }

    if (!s_convergence_done) {
      size_t active_tgts = g_polling_targets.activeCount();
      size_t online_devs = g_device_repo.getOnlineCount();

      if (active_tgts != s_last_active_tgts) {
        s_last_active_tgts = active_tgts;
        s_stable_start_ms = millis();
      }

      bool is_all_online = (online_devs >= active_tgts);
      auto *parser = WallpadParserFactory::getActiveParser();
      if (parser && parser->isAutoMode() && !g_auto_probing_engine.isOffsetsLocked()) {
        is_all_online = (g_polling_targets.verifiedCount() >= active_tgts);
      }

      if (active_tgts > 0 && is_all_online) {
        if (s_stable_start_ms == 0) {
          s_stable_start_ms = millis();
        } else if (TimeUtils::isElapsed(s_stable_start_ms, CACHE_CONVERGENCE_STABLE_MS)) { // 1.5초간 신규 기기 증가 멈춤 & 전원 온라인 확인 시 최종 수렴!
          s_convergence_done = true;
          g_initial_caching_complete.store(true, std::memory_order_release);
          if (g_system_event_group) {
            xEventGroupSetBits(g_system_event_group, SYS_EVT_CACHE_READY);
          }
          if (parser && parser->isAutoMode() && !g_auto_probing_engine.isOffsetsLocked()) {
            g_auto_probing_engine.analyzeCacheMatrix();
          }
          g_pkt_stats.resetAll();
          g_polling_targets.resetHits();
          g_metrics.reset();
          g_ch1_state_metrics.normal_cnt.store(0, std::memory_order_relaxed);
          g_ch1_state_metrics.vip_cnt.store(0, std::memory_order_relaxed);
          g_telnet_tracer.trace("[SYSTEM MSG]  ★ 2nd-Tier Cache Converged (Zero Offline). Runtime metrics synchronized.\r\n");
          g_control_registry.synthesizeFromConvergedCache();
          g_telnet_tracer.trace("[CTL] Control template synthesis triggered.\r\n");
        }
      } else {
        s_stable_start_ms = 0;
      }
    }

    size_t active_tgts = g_polling_targets.activeCount();
    const uint32_t poll_interval =
        (s_convergence_done || active_tgts == 0)
            ? g_timing_config.ch1_poll_interval_ms
            : 20;

    uint32_t now = millis();
    uint32_t rem_ms = (now < next_poll_due_ms) ? (next_poll_due_ms - now) : 0;
    TickType_t wait_ticks = (rem_ms > 0) ? pdMS_TO_TICKS(rem_ms) : 1;

    QueueSetMemberHandle_t activated = nullptr;
    if (g_ch1_queue_set) {
      activated = xQueueSelectFromSet(g_ch1_queue_set, wait_ticks);
    } else {
      vTaskDelay(wait_ticks);
    }

    if (g_ch1_vip_queue && xQueueReceive(g_ch1_vip_queue, &ctrlPacket, 0) == pdTRUE) {
      Ch1_SetState(current_state, Ch1State::VIP_CONTROL);
      Ch1_HandleCtrl(ctrlPacket);
      Ch1_SetState(current_state, Ch1State::IDLE);
      continue; // VIP 처리 완료 후 다음 루프로 즉시 재평가
    }

    if (activated == g_ch1_control_queue && g_ch1_control_queue &&
        xQueueReceive(g_ch1_control_queue, &ctrlPacket, 0) == pdTRUE) {
      auto *parser = WallpadParserFactory::getActiveParser();
      span<const uint8_t> frame(ctrlPacket.data.data(), ctrlPacket.length);
      bool is_query = parser && parser->isQueryPacket(frame);

      Ch1_SetState(current_state, is_query ? Ch1State::POLL_DEVICE : Ch1State::NORMAL_CONTROL);
      Ch1_HandleCtrl(ctrlPacket);
      Ch1_SetState(current_state, Ch1State::IDLE);
      continue; // 일반 제어 처리 완료 후 다음 루프로 즉시 재평가
    }

    if (activated == nullptr || now >= next_poll_due_ms) {
      Ch1_SetState(current_state, Ch1State::POLL_DEVICE);
      Ch1_PollNext(current_dev_idx);
      Ch1_SetState(current_state, Ch1State::IDLE);
      next_poll_due_ms = millis() + poll_interval;
    }
  }
}

// ============================================================================
// 5. CH2/CH3 HW Wallpad Slaves & CH4 SW Doorphone (formerly Ch23Engine.cpp)
// ============================================================================

struct TaskAckPollContext {
  TimestampedPacketQueue<8> *ack_q;
  const WallpadChannelConfig *cfg;
  SingleChannelStats *stats;
};

static void Ch2Ch3_DrainVirtualAckQueue(void *arg) {
  auto *ctx = static_cast<TaskAckPollContext *>(arg);
  if (!ctx || !ctx->ack_q || !ctx->cfg || !ctx->stats)
    return;

  StaticPacket next_ack;
  uint32_t next_due = 0;
  uint32_t now = millis();

  while (ctx->ack_q->peek(next_ack, next_due)) {
    if (now < next_due)
      break;
    if (ctx->ack_q->dequeue(next_ack, next_due)) {
      SemaphoreHandle_t u_mux = (ctx->cfg->uart_num == UART_NUM_1) ? g_uart1_mutex : g_uart2_mutex;
      if (u_mux) {
        MutexLocker lock(u_mux, pdMS_TO_TICKS(100));
        if (lock.isLocked()) {
          uart_write_bytes(ctx->cfg->uart_num, next_ack.data.data(), next_ack.length);
        } else {
          ctx->stats->timeouts.fetch_add(1, std::memory_order_relaxed);
          g_telnet_tracer.trace("[WARN] UART mutex timeout on virtual ACK\r\n");
        }
      } else {
        uart_write_bytes(ctx->cfg->uart_num, next_ack.data.data(), next_ack.length);
      }
      g_telnet_tracer.trace(ctx->cfg->channel_id, true, TraceType::ACK, next_ack);
      ctx->stats->tx_pkts.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

void Task_Ch2Ch3(void *pvParameters) {
  auto *cfg = static_cast<WallpadChannelConfig *>(pvParameters);
  if (!cfg)
    return;

  esp_task_wdt_add(nullptr);
  SingleChannelStats *stats = (cfg->channel_id == 2)   ? &g_pkt_stats.ch2
                              : (cfg->channel_id == 3) ? &g_pkt_stats.ch3
                                                       : nullptr;
  if (!stats) {
    esp_task_wdt_delete(nullptr);
    vTaskDelete(nullptr);
    return;
  }

  size_t task_idx = (cfg && cfg->channel_id == 3) ? 2 : 1;
  uart_flush_input(cfg->uart_num);
  TimestampedPacketQueue<8> ack_queue;

  if (g_system_event_group) {
    xEventGroupWaitBits(g_system_event_group, SYS_EVT_SYSTEM_RUNNING, pdFALSE, pdFALSE, portMAX_DELAY);
  }

  TaskAckPollContext poll_ctx{&ack_queue, cfg, stats};

  for (;;) {
    g_wdt_monitor.feed(task_idx);
    if (UNLIKELY(g_ota_in_progress.load(std::memory_order_relaxed))) {
      if (g_system_event_group) {
        xEventGroupWaitBits(g_system_event_group, SYS_EVT_OTA_IDLE, pdFALSE, pdFALSE, pdMS_TO_TICKS(1000));
      } else {
        vTaskDelay(pdMS_TO_TICKS(100));
      }
      continue;
    }

    Ch2Ch3_DrainVirtualAckQueue(&poll_ctx);

    StaticPacket req;
    if (Uart_RecvPacket(cfg->uart_num, req, 100, Ch2Ch3_DrainVirtualAckQueue, &poll_ctx) ==
        UartRxStatus::SUCCESS) {
      stats->rx_pkts.fetch_add(1, std::memory_order_relaxed);
      req.channel_id = cfg->channel_id;

      auto *parser = WallpadParserFactory::getActiveParser();
      span<const uint8_t> frame(req.data.data(), req.length);

      bool is_query = parser->isQueryPacket(frame);
      g_telnet_tracer.trace(cfg->channel_id, false,
                         is_query ? TraceType::QRY : TraceType::CTL, req);

      if (is_query) {
        uint8_t dev_id = 0, sub1 = 0, sub2 = 0;
        parser->extractDeviceKey(frame, dev_id, sub1, sub2);
        g_polling_targets.registerOrTouch(cfg->channel_id, dev_id, sub1, sub2,
                                          req.data.data(), req.length);
        StaticPacket virtual_ack;
        if (g_control_dispatcher.dispatch(req, virtual_ack)) {
          uint32_t delay_ms = (cfg->channel_id == 2)
                                  ? g_timing_config.ch2_cache_delay_ms
                                  : g_timing_config.ch3_cache_delay_ms;
          uint32_t target_due = millis() + delay_ms;
          if (!ack_queue.enqueue(virtual_ack, target_due)) {
            stats->uncached_pkts.fetch_add(1, std::memory_order_relaxed);
            g_telnet_tracer.trace("[WARN] Wallpad virtual ACK queue overflow, "
                               "packet dropped.\r\n");
          }
        } else {
          stats->uncached_pkts.fetch_add(1, std::memory_order_relaxed);
        }
      } else {
        // CH2 및 CH3에서 쿼리가 아닌 제어 패킷이 수신되면 Auto Probing Engine에 학습 전달
        g_auto_probing_engine.feedControlFrame(frame);
        if (parser->isControlPacket(frame)) {
          StaticPacket dummy_ack;
          g_control_dispatcher.dispatch(req, dummy_ack);
        }
      }
    }
  }
}

// ============================================================================
// 6. Channel 4 Sub-Wallpad Passthrough & Doorphone Bridge Engine
// ============================================================================

static inline void Ch4_SendPassthrough(const StaticPacket &pkt, StaticPacket &last_tx_pkt,
                                       uint32_t &last_tx_ms, char *cur_dp_ns) {
  if (pkt.length >= 3) {
    Config::Doorphone::FramingTracker::getNvsNamespace(g_config.wallpad_profile, cur_dp_ns, 16);
    g_doorphone_tracker.processFrame(pkt.data[0], pkt.data[pkt.length - 1], pkt.length, cur_dp_ns);
  }
  g_telnet_tracer.trace(4, true, TraceType::RMT, pkt);
  last_tx_pkt = pkt; // Correctly recorded in all code paths to avoid echo reflection misinterpretation
  g_doorphone_serial.write(pkt.data.data(), pkt.length);
  last_tx_ms = millis();
  g_pkt_stats.ch4.tx_pkts.fetch_add(1, std::memory_order_relaxed);
}

static inline void Ch4_HandleDoorphoneEvent(const StaticPacket &packet, StaticPacket &last_pkt,
                                            uint32_t &last_pkt_ms, uint32_t now) {
  bool is_debounce = (packet.length == last_pkt.length &&
                      memcmp(packet.data.data(), last_pkt.data.data(), packet.length) == 0 &&
                      !TimeUtils::isElapsed(last_pkt_ms, Config::Timing::DOORPHONE_DEBOUNCE_MS));
  if (is_debounce) return;

  last_pkt = packet;
  last_pkt_ms = now;

  if (packet.length >= 2) {
    uint8_t opcode = packet.data[1];
    bool state_changed = false;
    uint8_t pkt_stx = packet.data[0];
    uint8_t pkt_etx = packet.data[packet.length - 1];
    const DoorphoneSpec *dp_prof =
        ProfileMatcher::matchDoorphone(pkt_stx, pkt_etx, packet.length);

    uint8_t bell_front = dp_prof ? dp_prof->bell_front : 0xB5;
    uint8_t bell_lobby = dp_prof ? dp_prof->bell_lobby : 0x5A;
    uint8_t end_front  = dp_prof ? dp_prof->end_front  : 0xB8;
    uint8_t end_lobby  = dp_prof ? dp_prof->end_lobby  : 0x60;

    if (opcode == bell_front) { // 현관 벨 호출
      g_doorphone_state.front_bell.store(true, std::memory_order_release);
      g_doorphone_state.last_bell_ms.store(now, std::memory_order_release);
      state_changed = true;
    } else if (opcode == end_front || opcode == 0xB6) { // 현관 무응답/통화 종료
      g_doorphone_state.front_bell.store(false, std::memory_order_release);
      state_changed = true;
    } else if (opcode == bell_lobby || opcode == 0x5F) { // 로비 벨/호출
      g_doorphone_state.lobby_bell.store(true, std::memory_order_release);
      g_doorphone_state.last_bell_ms.store(now, std::memory_order_release);
      state_changed = true;
    } else if (opcode == end_lobby) { // 로비 통화 종료
      g_doorphone_state.lobby_bell.store(false, std::memory_order_release);
      state_changed = true;
    }

    if (state_changed) {
      bool f = g_doorphone_state.front_bell.load(std::memory_order_relaxed);
      bool l = g_doorphone_state.lobby_bell.load(std::memory_order_relaxed);
      Mgmt_BroadcastDoorphoneEvent(f, l);
    }
  }

  g_telnet_tracer.trace(4, false, TraceType::RMT, packet);
  g_pkt_stats.ch4.rx_pkts.fetch_add(1, std::memory_order_relaxed);
}

static inline void Ch4_DropInvalidFrame(const uint8_t *data, size_t len) {
  StaticPacket drp_pkt{4, static_cast<uint8_t>(std::min<size_t>(len, 16))};
  memcpy(drp_pkt.data.data(), data, drp_pkt.length);
  g_telnet_tracer.trace(4, false, TraceType::DRP, drp_pkt);
  g_pkt_stats.ch4.invalid_frames.fetch_add(1, std::memory_order_relaxed);
}

void Task_Ch4(void *pvParameters) {
  esp_task_wdt_add(nullptr);
  StaticPacket packet_to_tx;

  uint8_t buf[128] = {0};
  size_t buf_len = 0;
  uint32_t last_byte_ms = 0;  // 마지막 수신 바이트 타임스탬프
  StaticPacket last_tx_pkt{};
  uint32_t last_tx_ms = 0;
  StaticPacket last_pkt{};
  uint32_t last_pkt_ms = 0;
  char cur_dp_ns[16];
  Config::Doorphone::FramingTracker::getNvsNamespace(g_config.wallpad_profile, cur_dp_ns, sizeof(cur_dp_ns));

  if (g_system_event_group) {
    xEventGroupWaitBits(g_system_event_group, SYS_EVT_SYSTEM_RUNNING, pdFALSE, pdFALSE, portMAX_DELAY);
  }

  if (!g_initial_caching_complete.load(std::memory_order_acquire)) {
    if (g_system_event_group) {
      xEventGroupWaitBits(g_system_event_group, SYS_EVT_CACHE_READY, pdFALSE, pdFALSE,
                          pdMS_TO_TICKS(Config::Timing::INITIAL_CACHING_GRACE_PERIOD_MS));
    } else {
      vTaskDelay(pdMS_TO_TICKS(Config::Timing::INITIAL_CACHING_GRACE_PERIOD_MS));
    }
    g_wdt_monitor.feed(3);
  }

  for (;;) {
    g_wdt_monitor.feed(3);
    if (UNLIKELY(g_ota_in_progress.load(std::memory_order_relaxed))) {
      if (g_system_event_group) {
        xEventGroupWaitBits(g_system_event_group, SYS_EVT_OTA_IDLE, pdFALSE, pdFALSE, pdMS_TO_TICKS(1000));
      } else {
        vTaskDelay(pdMS_TO_TICKS(100));
      }
      continue;
    }

    if (xQueueReceive(g_ch4_passthrough_queue, &packet_to_tx, 0) == pdTRUE) {
      Ch4_SendPassthrough(packet_to_tx, last_tx_pkt, last_tx_ms, cur_dp_ns);
    }

    const uint32_t ib_timeout = Config::Timing::getDoorphoneInterByteTimeoutMs(g_config.doorphone_baud_rate);
    uint32_t burst_spin_total = 0;
    while (g_doorphone_serial.available() > 0) {
      uint8_t byte = static_cast<uint8_t>(g_doorphone_serial.read());
      uint32_t now = millis();

      if (buf_len > 0 && last_byte_ms > 0 && TimeUtils::isElapsed(last_byte_ms, ib_timeout)) {
        buf_len = 0;
      }

      if (buf_len < sizeof(buf)) {
        buf[buf_len++] = byte;
      } else {
        memmove(buf, buf + 1, buf_len - 1);
        buf_len--;
        buf[buf_len++] = byte;
      }
      last_byte_ms = now;

      if (burst_spin_total < 20) {
        uint32_t drain_start = millis();
        while (g_doorphone_serial.available() == 0 && (millis() - drain_start < 6)) {
          esp_rom_delay_us(100);
        }
        burst_spin_total += (millis() - drain_start);
      }
    }

    Config::Doorphone::FramingStatus cur_status = g_doorphone_tracker.status.load(std::memory_order_relaxed);
    if (cur_status == Config::Doorphone::FramingStatus::LOCKED && buf_len >= 3) {
      uint8_t target_stx = g_doorphone_tracker.candidate_stx.load(std::memory_order_relaxed);
      uint8_t target_etx = g_doorphone_tracker.candidate_etx.load(std::memory_order_relaxed);
      uint8_t target_len = g_doorphone_tracker.candidate_len.load(std::memory_order_relaxed);

      size_t p = 0;
      while (p < buf_len) {
        if (buf[p] != target_stx) {
          p++;
          continue;
        }

        bool frame_found = false;
        size_t found_len = 0;

        if (target_len >= 3 && target_len <= 64) {
          if (buf_len - p >= target_len) {
            if (buf[p + target_len - 1] == target_etx) {
              frame_found = true;
              found_len = target_len;
            } else {
              Ch4_DropInvalidFrame(&buf[p], buf_len - p);
              p++;
              continue;
            }
          } else {
            break;
          }
        } else {
          for (size_t i = p + 2; i < buf_len && (i - p + 1) <= 64; ++i) {
            if (buf[i] == target_etx) {
              frame_found = true;
              found_len = (i - p) + 1;
              break;
            }
          }
        }

        if (frame_found) {
          if (last_tx_pkt.length == found_len &&
              memcmp(last_tx_pkt.data.data(), &buf[p], found_len) == 0 &&
              last_tx_ms > 0 && !TimeUtils::isElapsed(last_tx_ms, 250)) {
            p += found_len;
            last_byte_ms = 0;
            continue;
          }

          StaticPacket packet{4, static_cast<uint8_t>(found_len)};
          memcpy(packet.data.data(), &buf[p], found_len);

          Ch4_HandleDoorphoneEvent(packet, last_pkt, last_pkt_ms, millis());

          p += found_len;
          last_byte_ms = 0;
        } else {
          if (buf_len - p >= 64) {
            Ch4_DropInvalidFrame(&buf[p], buf_len - p);
            p++;
            continue;
          }
          break;
        }
      }

      if (p > 0) {
        if (p < buf_len) {
          memmove(buf, buf + p, buf_len - p);
          buf_len -= p;
        } else {
          buf_len = 0;
        }
      }
    }

    if (last_byte_ms > 0 &&
        TimeUtils::isElapsed(last_byte_ms, Config::Timing::DOORPHONE_IPG_MS)) {

      if (cur_status == Config::Doorphone::FramingStatus::LOCKED) {
        buf_len = 0;
      } else {
        if (buf_len >= 3) {
          StaticPacket packet{4, static_cast<uint8_t>(buf_len)};
          memcpy(packet.data.data(), buf, buf_len);

          uint8_t pkt_stx = packet.data[0];
          uint8_t pkt_etx = packet.data[packet.length - 1];

          Config::Doorphone::FramingStatus prev_status = g_doorphone_tracker.status.load(std::memory_order_relaxed);
          
          // 프로파일 카탈로그에 일치하는 도어폰 규격이 있으면 즉시 영구 잠금(LOCKED)
          const DoorphoneSpec *dp_spec = ProfileMatcher::matchDoorphone(pkt_stx, pkt_etx, 5);
          char cur_dp_ns_local[16];
          Config::Doorphone::FramingTracker::getNvsNamespace(g_config.wallpad_profile, cur_dp_ns_local, sizeof(cur_dp_ns_local));

          if (dp_spec && pkt_stx == dp_spec->stx && pkt_etx == dp_spec->etx && packet.length >= 5) {
            if (prev_status != Config::Doorphone::FramingStatus::LOCKED) {
              g_doorphone_tracker.setFixedLock(dp_spec->stx, dp_spec->etx, dp_spec->len);
              g_doorphone_tracker.saveToNvs(cur_dp_ns_local);
            }
          } else {
            g_doorphone_tracker.processFrame(pkt_stx, pkt_etx, packet.length, cur_dp_ns_local);
            Config::Doorphone::FramingStatus status = g_doorphone_tracker.status.load(std::memory_order_relaxed);
            if (prev_status != Config::Doorphone::FramingStatus::LOCKED &&
                status == Config::Doorphone::FramingStatus::LOCKED) {
              g_doorphone_tracker.saveToNvs(cur_dp_ns_local);
            }
          }

          Ch4_HandleDoorphoneEvent(packet, last_pkt, last_pkt_ms, millis());
        }
        buf_len = 0;
      }
      last_byte_ms = 0;
    }

    uint32_t wait_ms = (buf_len > 0) ? 1 : 5;
    if (last_byte_ms > 0) {
      uint32_t elapsed = millis() - last_byte_ms;
      if (elapsed < Config::Timing::DOORPHONE_IPG_MS) {
        wait_ms = (buf_len > 0) ? 1 : (Config::Timing::DOORPHONE_IPG_MS - elapsed);
      } else {
        wait_ms = 1;
      }
    }
    wait_ms = std::max<uint32_t>(wait_ms, 1);

    if (xQueueReceive(g_ch4_passthrough_queue, &packet_to_tx, pdMS_TO_TICKS(wait_ms)) == pdTRUE) {
      Ch4_SendPassthrough(packet_to_tx, last_tx_pkt, last_tx_ms, cur_dp_ns);
    }
  }
}
