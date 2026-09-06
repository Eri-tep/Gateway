#include "Common.h"
#include "MgmtRpc.h"
#include "TelnetCli.h"
#include "WallpadParser.h"
#include "ControlTemplate.h"
#include "esp_timer.h"
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif
float temperatureRead(void);
#ifdef __cplusplus
}
#endif

// ============================================================================
// SECTION 1: HARDWARE METRICS & SYSTEM COLLECTORS
// ============================================================================

int8_t System_ReadTempC() { return static_cast<int8_t>(temperatureRead()); }

void System_ReadCpuPct(uint8_t &cpu0_out, uint8_t &cpu1_out) {
  static std::atomic<uint32_t> s_last_time_ms{0};
  static std::atomic<uint32_t> s_last_ch1_pkts{0};
  static std::atomic<uint32_t> s_last_ch23_pkts{0};
  static std::atomic<uint32_t> s_last_tcp_pkts{0};

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

  if (!prev_ms) {
    s_last_time_ms.store(now_ms, std::memory_order_relaxed);
    s_last_ch1_pkts.store(cur_ch1, std::memory_order_relaxed);
    s_last_ch23_pkts.store(cur_ch23, std::memory_order_relaxed);
    s_last_tcp_pkts.store(cur_tcp, std::memory_order_relaxed);
    cpu0_out = 4;
    cpu1_out = 3;
    return;
  }

  uint32_t elapsed_ms = now_ms - prev_ms;
  if (elapsed_ms < Config::Metrics::MIN_SAMPLE_INTERVAL_MS) {
    cpu0_out = 4;
    cpu1_out = 3;
    return;
  }

  uint32_t last_ch1 = s_last_ch1_pkts.load(std::memory_order_relaxed);
  uint32_t last_ch23 = s_last_ch23_pkts.load(std::memory_order_relaxed);
  uint32_t last_tcp = s_last_tcp_pkts.load(std::memory_order_relaxed);

  uint32_t delta_ch1 = (cur_ch1 >= last_ch1) ? (cur_ch1 - last_ch1) : cur_ch1;
  uint32_t delta_ch23 = (cur_ch23 >= last_ch23) ? (cur_ch23 - last_ch23) : cur_ch23;
  uint32_t delta_tcp = (cur_tcp >= last_tcp) ? (cur_tcp - last_tcp) : cur_tcp;

  s_last_time_ms.store(now_ms, std::memory_order_relaxed);
  s_last_ch1_pkts.store(cur_ch1, std::memory_order_relaxed);
  s_last_ch23_pkts.store(cur_ch23, std::memory_order_relaxed);
  s_last_tcp_pkts.store(cur_tcp, std::memory_order_relaxed);

  // Core 0 (Network, TCP, WiFi, Telnet):
  uint32_t tcp_pps = (elapsed_ms > 0)
                         ? static_cast<uint32_t>((static_cast<uint64_t>(delta_tcp) * 1000) / elapsed_ms)
                         : 0;
  uint32_t load0 = Config::Metrics::CPU0_BASE_LOAD + (tcp_pps / Config::Metrics::CPU0_PPS_DIVISOR);
  if (WiFi.isConnected()) load0 += 1;
  if (g_pkt_stats.ch6.is_connected.load(std::memory_order_relaxed)) load0 += 1;

  // Core 1 (UART Master, Wallpad Slaves, RS-485 timing):
  uint32_t uart_pps = (elapsed_ms > 0)
                          ? static_cast<uint32_t>((static_cast<uint64_t>(delta_ch1 + delta_ch23) * 1000) / elapsed_ms)
                          : 0;
  uint32_t load1 = Config::Metrics::CPU1_BASE_LOAD + (uart_pps / Config::Metrics::CPU1_PPS_DIVISOR);

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

StatSummary SystemMetricsTracker::get15m() const {
  StatSummary r = {};
  MutexLocker lock(_metrics_mutex);
  size_t cnt = _ring15_count;
  if (!cnt)
    return r;

  uint32_t c0s = 0, c1s = 0, rs = 0, fs = 0;
  int32_t ts = 0;
  uint8_t c0peak = 0, c1peak = 0;
  uint16_t rpeak = 0, fpeak = 0;
  int8_t tpeak = -127;

  for (size_t i = 0; i < cnt; i++) {
    const auto &ms = _ring15[i];
    c0s += ms.cpu0_pct;
    c1s += ms.cpu1_pct;
    rs += ms.ram_kb;
    fs += ms.flash_kb;
    ts += ms.temp_c;
    c0peak = std::max(c0peak, ms.cpu0_pct);
    c1peak = std::max(c1peak, ms.cpu1_pct);
    rpeak = std::max(rpeak, ms.ram_kb);
    fpeak = std::max(fpeak, ms.flash_kb);
    tpeak = std::max(tpeak, ms.temp_c);
  }

  r.cpu0_avg = c0s / cnt;
  r.cpu0_peak = c0peak;
  r.cpu1_avg = c1s / cnt;
  r.cpu1_peak = c1peak;
  r.ram_avg = rs / cnt;
  r.ram_peak = rpeak;
  r.flash_avg = fs / cnt;
  r.flash_peak = fpeak;
  r.temp_avg = ts / static_cast<int32_t>(cnt);
  r.temp_peak = tpeak;
  r.count = static_cast<uint16_t>(cnt);
  return r;
}

StatSummary SystemMetricsTracker::get24h() const {
  StatSummary r = {};
  MutexLocker lock(_metrics_mutex);
  size_t cnt24 = _ring24_count;

  uint32_t c0s = 0, c1s = 0, rs = 0, total_samples = 0;
  int32_t ts = 0;
  uint8_t c0peak = 0, c1peak = 0;
  uint16_t rpeak = 0;
  int8_t tpeak = -127;

  auto acc = [&](const MetricBucket &b) {
    if (!b.count)
      return;
    c0s += b.cpu0_sum;
    c1s += b.cpu1_sum;
    rs += b.ram_sum;
    ts += b.temp_sum;
    total_samples += b.count;
    c0peak = std::max(c0peak, b.cpu0_peak);
    c1peak = std::max(c1peak, b.cpu1_peak);
    rpeak = std::max(rpeak, b.ram_peak);
    tpeak = std::max(tpeak, b.temp_peak);
  };

  for (size_t i = 0; i < cnt24; i++)
    acc(_ring24[i]);
  acc(_cur_bucket);

  if (total_samples > 0) {
    r.cpu0_avg = c0s / total_samples;
    r.cpu0_peak = c0peak;
    r.cpu1_avg = c1s / total_samples;
    r.cpu1_peak = c1peak;
    r.ram_avg = rs / total_samples;
    r.ram_peak = rpeak;
    r.flash_avg = _cached_flash_kb;
    r.flash_peak = _cached_flash_kb;
    r.temp_avg = ts / static_cast<int32_t>(total_samples);
    r.temp_peak = tpeak;
    r.count = static_cast<uint16_t>(std::min<uint32_t>(total_samples, 65535U));
  }
  return r;
}

namespace Fmt {

void FormatHwMetrics(AppendBuf &out, const HwSnapshot &hw) {
  out.append(DIV80);
  out.appendFormat("%-16s %11s  %11s  %11s  %11s  %11s\r\n", "Resource / Core", "Current",
                   "15m Avg", "15m Peak", "24h Avg", "24h Peak");
  out.append(DIV80);

  struct HwRow {
    const char *name;
    uint16_t cur, a15, p15, a24, p24;
    const char *suffix;
  };
  const HwRow rows[] = {
      {"CPU0 (Net/WiFi)", hw.cpu0_cur, hw.cpu0_15m_avg, hw.cpu0_15m_peak, hw.cpu0_24h_avg, hw.cpu0_24h_peak, "%"},
      {"CPU1 (RS485/IO)", hw.cpu1_cur, hw.cpu1_15m_avg, hw.cpu1_15m_peak, hw.cpu1_24h_avg, hw.cpu1_24h_peak, "%"},
      {"RAM Used", hw.ram_cur, hw.ram_15m_avg, hw.ram_15m_peak, hw.ram_24h_avg, hw.ram_24h_peak, " KB"},
      {"Temp", static_cast<uint16_t>(hw.temp_cur), static_cast<uint16_t>(hw.temp_15m_avg), static_cast<uint16_t>(hw.temp_15m_peak), static_cast<uint16_t>(hw.temp_24h_avg), static_cast<uint16_t>(hw.temp_24h_peak), " C"},
  };

  for (const auto &r : rows) {
    char c[5][16];
    snprintf(c[0], sizeof(c[0]), "%u%s", static_cast<unsigned>(r.cur), r.suffix);
    snprintf(c[1], sizeof(c[1]), "%u%s", static_cast<unsigned>(r.a15), r.suffix);
    snprintf(c[2], sizeof(c[2]), "%u%s", static_cast<unsigned>(r.p15), r.suffix);
    snprintf(c[3], sizeof(c[3]), "%u%s", static_cast<unsigned>(r.a24), r.suffix);
    snprintf(c[4], sizeof(c[4]), "%u%s", static_cast<unsigned>(r.p24), r.suffix);
    out.appendFormat("%-16s %11s  %11s  %11s  %11s  %11s\r\n", r.name, c[0], c[1], c[2], c[3], c[4]);
  }
}

void FormatNetworkStats(AppendBuf &out, const PktSnapshot &pkt) {
  out.append(DIV80);
  out.appendFormat("%-10s %-6s %-13s %-7s %-11s %-11s %-8s %s\r\n", "Channel", "Port",
                   "Status", "Conn", "RX Pkts", "TX Pkts", "Dropped", "Uncache");
  out.append(DIV80);

  const TcpChanStats *t_st[] = {&pkt.ch5, &pkt.ch6};
  const char *tn[] = {"CH#5_Hub#2", "CH#6_Hub#1"};
  const uint16_t tp[] = {Config::TCP::DOORPHONE_PORT, Config::TCP::HUB_PORT};

  for (int i = 0; i < 2; ++i) {
    bool is_conn = t_st[i]->is_connected;
    uint32_t rx = t_st[i]->rx_pkts;
    uint32_t tx = t_st[i]->tx_pkts;
    const char *status_str = !is_conn               ? "Disconnected"
                             : (rx == 0 && tx == 0) ? "Idle"
                                                     : "Connected";
    out.appendFormat("%-10s %-6u %-14s %3u%12u%12u%10u%10u\r\n", tn[i], tp[i], status_str,
                     static_cast<unsigned>(t_st[i]->connection_count), static_cast<unsigned>(rx), static_cast<unsigned>(tx),
                     static_cast<unsigned>(t_st[i]->dropped_pkts), static_cast<unsigned>(t_st[i]->uncached_pkts));
  }
}

void FormatRs485Stats(AppendBuf &out, const PktSnapshot &pkt) {
  out.append(DIV80);
  out.appendFormat("%-10s %10s %12s %15s %10s %9s %8s\r\n", "Channel", "RX Pkts", "TX Pkts",
                   "CRC Err", "Inv Frm", "Timeouts", "Uncache");
  out.append(DIV80);

  const char *rs_n[] = {"CH#1_IoT", "CH#2_WP#1", "CH#3_WP#2", "CH#4_WP#3"};
  const ChanStats *rs_st[] = {&pkt.ch1, &pkt.ch2, &pkt.ch3, &pkt.ch4};
  for (int i = 0; i < 4; ++i) {
    uint32_t rx = rs_st[i]->rx_pkts, crc = rs_st[i]->crc_errors;
    char r_str[24];
    snprintf(r_str, sizeof(r_str), "%u (%.2f%%)", static_cast<unsigned>(crc),
             rx ? (static_cast<float>(crc) / rx) * 100.0f : 0.0f);
    out.appendFormat("%-10s %10u %12u %15s %10u %9u %8u\r\n", rs_n[i], static_cast<unsigned>(rx),
                     static_cast<unsigned>(rs_st[i]->tx_pkts), r_str, static_cast<unsigned>(rs_st[i]->invalid_frames),
                     static_cast<unsigned>(rs_st[i]->timeouts), static_cast<unsigned>(rs_st[i]->uncached_pkts));
  }
}

void FormatTaskStacks(AppendBuf &out, const StackSnapshot &st, const TaskWdtMonitor &wdt) {
  auto gtag = [](uint16_t b) {
    return b >= 1000 ? "SAFE" : b >= 500 ? "WARN" : "CRIT";
  };

  const uint16_t stacks[6] = {st.ch1_stack, st.ch2_stack, st.ch3_stack,
                              st.ch4_stack, st.net_stack, st.telnet_stack};
  const char *names[6] = {"CH#1_IoT",  "CH#2_WP#1", "CH#3_WP#2",
                          "CH#4_WP#3", "Network",   "Telnet_CLI"};
  const char *scopes[6] = {"IoT Master Comm",    "Wallpad#1 HW Slave",
                           "Wallpad#2 HW Slave", "Wallpad#3 SW Slave",
                           "WiFi & TCP Manager", "Telnet CLI Server"};

  out.append(DIV80);
  out.appendFormat("%-11s %-12s %-10s %-12s %-8s %-18s\r\n", "Task Name", "Min Stack",
                   "Last Feed", "Peak Intvl", "Status", "Task Scope");
  out.append(DIV80);

  uint32_t now = millis();
  for (size_t i = 0; i < 6; ++i) {
    uint32_t last_feed = wdt.tasks[i].last_feed_ms.load(std::memory_order_relaxed);
    uint32_t elapsed = (last_feed > 0 && now >= last_feed) ? (now - last_feed) : 0;
    uint32_t peak = wdt.tasks[i].max_interval_ms.load(std::memory_order_relaxed);

    out.appendFormat("%-11s %5u Bytes  %5u ms     %5u ms       %-7s %-18s\r\n", names[i],
                     stacks[i], static_cast<unsigned>(elapsed), static_cast<unsigned>(peak), gtag(stacks[i]), scopes[i]);
  }
}

} // namespace Fmt

// ============================================================================
// SECTION 2: DEVICE REPOSITORY & RS-485 CONTROL ENGINE
// ============================================================================

static inline uint8_t Device_Hash(uint8_t dev_id, uint8_t sub1, uint8_t sub2) noexcept {
  return static_cast<uint8_t>(dev_id + sub1 * 3 + sub2 * 7);
}

static inline uint8_t Device_NormSub1(uint8_t dev_id, uint8_t sub1) noexcept {
  // [의도적 설계] 온도조절기 0x18/sub1=0x45: 전원-ON 직후 버스에서 0x45가 관측되나
  // 실제 상태 조회 sub1=0x46과 동일 장치이므로 0x46으로 정규화 (DevRepo 중복 방지)
  if (dev_id == Config::Devices::DEV_THERMOSTAT && sub1 == 0x45)
    return 0x46;
  // [의도적 설계] 전열교환기 0x2B: 제어 응답(sub1=0x42)과 상태 조회(sub1=0x40)가
  // 같은 물리 장치를 가리키므로 DevRepo에서 동일 키로 관리.
  // CH6 ACK 변환(0x42→0x40)과 짝을 이루며, 스마트싱스가 단일 장치로 인식하도록 설계.
  // ※ 이 함수는 extractDeviceKey()가 이미 오프셋을 해석한 후 dev_id/sub1을 받으므로
  //    하드코딩된 오프셋과 무관하게 올바르게 동작함.
  if (dev_id == Config::Devices::DEV_HEAT_EXCHANGER &&
      sub1 == Config::Devices::SUB_HEAT_EXCHANGER_CTRL_ACK) {
    return Config::Devices::SUB_HEAT_EXCHANGER_QUERY;
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

  dev->last_ack_len = ack.length;
  memcpy(dev->last_ack_data.data(), ack.data.data(), ack.length);
  dev->last_updated_ms = millis();
  dev->timeout_count = 0;
  dev->is_online = true;
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

bool ControlDispatcher::dispatch(StaticPacket &req,
                                 StaticPacket &virtual_ack_out) {
  if (UNLIKELY(req.length < 5))
    return false;
  auto *parser = WallpadParserFactory::getActiveParser();
  span<const uint8_t> frame(req.data.data(), req.length);
  if (parser->isQueryPacket(frame)) {
    virtual_ack_out.channel_id = req.channel_id;
    uint8_t dev_id = 0, sub1 = 0, sub2 = 0;
    if (!parser->extractDeviceKey(frame, dev_id, sub1, sub2)) {
      return false;
    }
    return g_device_repo.copyVirtualAck(dev_id, sub1, sub2, virtual_ack_out);
  }

  if (parser->isControlPacket(frame)) {
    // CH6(앱) / 월패드 제어 명령: 가상 응답 없이 실제 장치로 명령 전달
    QueueHandle_t q = (req.channel_id == 6) ? g_ch1_vip_queue : g_ch1_control_queue;
    if (!Queue_EnqueueDropHead(q, req)) {
      return false;
    }
    g_telnet_tracer.trace(1, true, TraceType::CTL, req);
    return false;
  }
  return false;
}

static inline void Ch1_WaitBusIdle(uint32_t silence_ms) {
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

enum class UartRxStatus { SUCCESS, TIMEOUT };

using UartPollCallback = void (*)(void *ctx);

static inline QueueHandle_t Uart_GetEventQueue(uart_port_t u_num) {
  switch (u_num) {
  case UART_NUM_0: return g_uart0_event_queue;
  case UART_NUM_1: return g_uart1_event_queue;
  case UART_NUM_2: return g_uart2_event_queue;
  default: return nullptr;
  }
}

// [1] 통합 스트림 파서 (Event-Driven UART Engine)
static UartRxStatus Uart_RecvPacket(uart_port_t u_num, StaticPacket &out,
                                   uint32_t tout_ms,
                                   UartPollCallback on_poll = nullptr,
                                   void *poll_ctx = nullptr,
                                   const StaticPacket *echo_match = nullptr) {
  uint8_t temp[64], stream[128];
  size_t stream_len = 0;
  uint32_t start_ms = millis();
  uint32_t last_rx_ms = 0;
  auto *parser = WallpadParserFactory::getActiveParser();
  QueueHandle_t evt_q = Uart_GetEventQueue(u_num);

  while (millis() - start_ms < tout_ms) {
    esp_task_wdt_reset();
    if (on_poll)
      on_poll(poll_ctx);

    // 1. 남아있는 스트림 버퍼에서 즉시 유효 패킷 파싱 시도
    if (stream_len >= 3) {
      if (parser && parser->isAutoMode() && !parser->isLocked()) {
        // [Auto Mode Initial Learning: Silence (IPG) Framing]
        if (last_rx_ms > 0 && TimeUtils::isElapsed(last_rx_ms, Config::Timing::WALLPAD_AUTO_IPG_MS)) {
          g_auto_probing_engine.feedFrame(span<const uint8_t>(stream, stream_len));

          if (echo_match && echo_match->length == stream_len &&
              memcmp(echo_match->data.data(), stream, stream_len) == 0) {
            stream_len = 0;
            last_rx_ms = 0;
            continue;
          }

          out.length = static_cast<uint8_t>(stream_len);
          memcpy(out.data.data(), stream, stream_len);
          stream_len = 0;
          last_rx_ms = 0;
          return UartRxStatus::SUCCESS;
        }
      } else {
        uint8_t stx = parser ? parser->getStx() : PKT_STX;
        size_t idx = 0;
        while (idx < stream_len) {
          if (stream[idx] != stx) {
            idx++;
            continue;
          }

          int len_res = parser ? parser->extractPacketLength(stream, stream_len, idx) : -1;
          if (len_res == 0) {
            // 불완전 패킷 (추가 바이트 대기 필요)
            break;
          }
          if (len_res < 0) {
            // 프레이밍 불일치/헤더 오류 → 다음 바이트로 이동
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

          // [순수 범용 에코 필터링] 송신 패킷과 100% 동일한 바이트인 경우 스킵
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

    // 2. 남은 타임아웃 계산 및 하드웨어 이벤트 큐 블로킹 대기 (최대 5ms 정밀 슬라이스)
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
          size_t avail = 0;
          uart_get_buffered_data_len(u_num, &avail);
          if (avail > 0) {
            size_t read_limit = std::min(avail, sizeof(temp));
            int rx = uart_read_bytes(u_num, temp, read_limit, 0);
            if (rx > 0) {
              if (stream_len + rx > sizeof(stream)) {
                size_t overflow = (stream_len + rx) - sizeof(stream);
                if (overflow < stream_len) {
                  memmove(stream, stream + overflow, stream_len - overflow);
                  stream_len -= overflow;
                } else {
                  stream_len = 0;
                }
              }
              size_t copy_len = std::min(static_cast<size_t>(rx), sizeof(stream) - stream_len);
              memcpy(stream + stream_len, temp, copy_len);
              stream_len += copy_len;
              received_new_bytes = true;
              last_rx_ms = millis();
            }
          }
        } else if (evt.type == UART_FIFO_OVF || evt.type == UART_BUFFER_FULL) {
          uart_flush_input(u_num);
          xQueueReset(evt_q);
        }
      }
    } else {
      // 이벤트 큐가 없는 경우 fallback: 2ms 슬립 대기
      vTaskDelay(pdMS_TO_TICKS(std::min<uint32_t>(wait_ms, 2)));
    }

    // 3. 큐 이벤트와 무관하게 버퍼에 잔여 데이터가 있는 경우 드레인
    if (!received_new_bytes) {
      size_t avail = 0;
      uart_get_buffered_data_len(u_num, &avail);
      if (avail > 0) {
        size_t read_limit = std::min(avail, sizeof(temp));
        int rx = uart_read_bytes(u_num, temp, read_limit, 0);
        if (rx > 0) {
          if (stream_len + rx > sizeof(stream)) {
            size_t overflow = (stream_len + rx) - sizeof(stream);
            if (overflow < stream_len) {
              memmove(stream, stream + overflow, stream_len - overflow);
              stream_len -= overflow;
            } else {
              stream_len = 0;
            }
          }
          size_t copy_len = std::min(static_cast<size_t>(rx), sizeof(stream) - stream_len);
          memcpy(stream + stream_len, temp, copy_len);
          stream_len += copy_len;
          last_rx_ms = millis();
        }
      }
    }
  }

  // 타임아웃 발생 시에도 Auto 모드 미잠금 상태에서 유효 바이트가 있으면 프레임 처리
  if (stream_len >= 3 && parser && parser->isAutoMode() && !parser->isLocked()) {
    g_auto_probing_engine.feedFrame(span<const uint8_t>(stream, stream_len));
    if (!(echo_match && echo_match->length == stream_len &&
          memcmp(echo_match->data.data(), stream, stream_len) == 0)) {
      out.length = static_cast<uint8_t>(stream_len);
      memcpy(out.data.data(), stream, stream_len);
      return UartRxStatus::SUCCESS;
    }
  }

  return UartRxStatus::TIMEOUT;
}

// [2] 제어 패킷 전송 및 투명 중계
static void Ch1_HandleCtrl(const StaticPacket &ctrlPacket) {
  StaticPacket ack_before{};
  auto *parser = WallpadParserFactory::getActiveParser();
  if (parser) {
    uint8_t dev_id = 0, sub1 = 0, sub2 = 0;
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
    uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(20));
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
    g_auto_probing_engine.feedControlPair(
        span<const uint8_t>(ctrlPacket.data.data(), ctrlPacket.length),
        span<const uint8_t>(ack.data.data(), ack.length));
    if (!parser || !parser->isQueryPacket(span<const uint8_t>(ctrlPacket.data.data(), ctrlPacket.length))) {
      g_control_registry.onControlTransaction(ctrlPacket, ack_before, ack);
    }
    ack.channel_id = ctrlPacket.channel_id;

    // ★ [추가] 스마트싱스/앱(CH6) 전송용 주소 변환 (0x42 -> 0x40 및 Checksum 재계산)
    // 오프셋 학습 완료 시 동적 오프셋 사용, 미완료 시 기본값(3,5) fallback
    StaticPacket ch6_ack = ack;
    if (ch6_ack.length >= 7 && ch6_ack.data[0] == 0xF7) {
      auto ad = g_auto_probing_engine.getDescriptor();
      // ACK DevType 위치: swap 구조일 때 gw_addr_offset, 아닐 때 dev_id_offset
      uint8_t ack_dev_off  = (ad.offsets_locked && ad.is_swapped_addr)
                                 ? ad.gw_addr_offset : (ad.offsets_locked ? ad.dev_id_offset : 3);
      uint8_t ack_sub1_off = ad.offsets_locked ? ad.sub1_offset : 5;

      if (ack_dev_off < ch6_ack.length && ack_sub1_off < ch6_ack.length &&
          ch6_ack.data[ack_dev_off] == Config::Devices::DEV_HEAT_EXCHANGER &&
          ch6_ack.data[ack_sub1_off] == Config::Devices::SUB_HEAT_EXCHANGER_CTRL_ACK) {
        ch6_ack.data[ack_sub1_off] = Config::Devices::SUB_HEAT_EXCHANGER_QUERY;

        ch6_ack.data[ch6_ack.length - 2] =
            PacketCodec::calculateChecksum(ch6_ack.data.data(), ch6_ack.length);
      }
    }

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
      Ch6_SendAck(ch6_ack);
    } else if (ctrlPacket.channel_id == 6) {
      Ch6_SendAck(ch6_ack);
    }
  } else {
    g_pkt_stats.ch1.timeouts.fetch_add(1, std::memory_order_relaxed);
    g_telnet_tracer.trace("[WARN] Device did not ACK control packet in time.\r\n");
  }
}

// [3] 장치 폴링
static void Ch1_PollNext(size_t &current_dev_idx) {
  g_polling_targets.sweepExpired(Config::Timing::STALE_DEVICE_THRESHOLD_MS);

  static PollingTargetEntry s_active_targets[PollingTargetRegistry::MAX_TARGETS];
  size_t active_cnt = g_polling_targets.getActiveTargets(s_active_targets, PollingTargetRegistry::MAX_TARGETS);

  uint8_t poll_dev_id = 0, poll_sub1 = 0, poll_sub2 = 0;
  uint8_t poll_raw_len = 0;
  uint8_t poll_raw_data[64] = {0};
  bool target_selected = false;
  uint32_t now = millis();

  if (active_cnt > 0) {
    // 0. Super-Priority: Fresh uncached targets (last_updated_ms == 0 or !cached_dev)
    for (size_t i = 0; i < active_cnt; i++) {
      size_t idx = (current_dev_idx + i) % active_cnt;
      const auto &tgt = s_active_targets[idx];
      const auto *cached_dev = g_device_repo.find(tgt.dev_id, tgt.sub1, tgt.sub2);

      if (tgt.raw_ack_len == 0 || !cached_dev || cached_dev->last_updated_ms == 0) {
        poll_dev_id = tgt.dev_id;
        poll_sub1 = tgt.sub1;
        poll_sub2 = tgt.sub2;
        poll_raw_len = tgt.raw_query_len;
        if (poll_raw_len > 0)
          memcpy(poll_raw_data, tgt.raw_query_data.data(), poll_raw_len);
        current_dev_idx = (idx + 1) % active_cnt;
        target_selected = true;
        break;
      }
    }

    // 1. Regular Priority: Online targets
    if (!target_selected) {
      for (size_t i = 0; i < active_cnt; i++) {
        size_t idx = (current_dev_idx + i) % active_cnt;
        const auto &tgt = s_active_targets[idx];
        const auto *cached_dev = g_device_repo.find(tgt.dev_id, tgt.sub1, tgt.sub2);

        if (cached_dev && cached_dev->is_online) {
          poll_dev_id = tgt.dev_id;
          poll_sub1 = tgt.sub1;
          poll_sub2 = tgt.sub2;
          poll_raw_len = tgt.raw_query_len;
          if (poll_raw_len > 0)
            memcpy(poll_raw_data, tgt.raw_query_data.data(), poll_raw_len);
          current_dev_idx = (idx + 1) % active_cnt;
          target_selected = true;
          break;
        }
      }
    }

    // 2. If no online target ready, check if any stale target is due for retry (10s backoff)
    if (!target_selected) {
      for (size_t i = 0; i < active_cnt; i++) {
        size_t idx = (current_dev_idx + i) % active_cnt;
        const auto &tgt = s_active_targets[idx];
        const auto *cached_dev = g_device_repo.find(tgt.dev_id, tgt.sub1, tgt.sub2);
        if (cached_dev && TimeUtils::isElapsed(cached_dev->last_stale_poll_ms, Config::Timing::CH1_STALE_POLL_INTERVAL_MS)) {
          poll_dev_id = tgt.dev_id;
          poll_sub1 = tgt.sub1;
          poll_sub2 = tgt.sub2;
          poll_raw_len = tgt.raw_query_len;
          if (poll_raw_len > 0)
            memcpy(poll_raw_data, tgt.raw_query_data.data(), poll_raw_len);
          g_device_repo.setLastStalePollMs(tgt.dev_id, tgt.sub1, tgt.sub2, now);
          g_ch1_state_metrics.stale_poll_cnt.fetch_add(1, std::memory_order_relaxed);
          current_dev_idx = (idx + 1) % active_cnt;
          target_selected = true;
          break;
        }
      }
    }
  }

  // 3. Fallback: If no 1st-tier targets active, poll g_device_repo if any devices exist
  if (!target_selected) {
    size_t dev_cnt = g_device_repo.count();
    if (dev_cnt > 0) {
      size_t idx = current_dev_idx % dev_cnt;
      auto *dev = g_device_repo.getAt(idx);
      current_dev_idx = (idx + 1) % dev_cnt;
      if (dev && (dev->is_online || dev->last_updated_ms == 0 ||
                  TimeUtils::isElapsed(dev->last_stale_poll_ms, Config::Timing::CH1_STALE_POLL_INTERVAL_MS))) {
        poll_dev_id = dev->dev_id;
        poll_sub1 = dev->sub1;
        poll_sub2 = dev->sub2;
        if (!dev->is_online)
          g_device_repo.setLastStalePollMsByIndex(idx, now);
        target_selected = true;
      }
    }
  }

  if (target_selected) {
    MutexLocker lock(g_uart0_mutex, pdMS_TO_TICKS(100));
    if (lock.isLocked()) {
      StaticPacket q_pkt;
      if (poll_raw_len > 0) {
        // ★ [1차 캐시 직접 투과] 월패드/앱에서 수신된 실제 Raw 쿼리 패킷을 100% 그대로 CH1으로 송신!
        q_pkt.channel_id = 1;
        q_pkt.length = poll_raw_len;
        memcpy(q_pkt.data.data(), poll_raw_data, poll_raw_len);
      } else {
        PacketBuilder::Ch1_BuildQueryPacket(q_pkt, poll_dev_id, poll_sub1,
                                             poll_sub2);
      }
      g_telnet_tracer.trace(1, true, TraceType::QRY, q_pkt);
      Ch1_WaitBusIdle(Config::Timing::CH1_INTER_PACKET_DELAY_MS);

      uart_flush_input(UART_NUM_0);
      uart_write_bytes(UART_NUM_0, q_pkt.data.data(), q_pkt.length);
      uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(20));
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
        g_auto_probing_engine.feedOpcodePair(
            span<const uint8_t>(q_pkt.data.data(), q_pkt.length),
            span<const uint8_t>(ack.data.data(), ack.length));
      } else {
        g_pkt_stats.ch1.timeouts.fetch_add(1, std::memory_order_relaxed);
        g_device_repo.handlePollingTimeout(poll_dev_id, poll_sub1, poll_sub2);
      }
    }
  }
}

static void Ch1_SetState(Ch1State &cur_state, Ch1State new_state) {
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
// SECTION 3: FREERTOS CORE1 TASKS
// ============================================================================

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

    g_control_registry.processActiveLearning();

    // ★ wallpad reset 신호 처리: s_convergence_done을 리셋하여 재수렴·재락 허용
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

    // 2차 캐싱 100% 수렴 완료 판정
    if (!s_convergence_done) {
      size_t active_tgts = g_polling_targets.activeCount();
      size_t online_devs = g_device_repo.getOnlineCount();

      // 신규 기기 유입 중이면 1.5초 안정 타이머 리셋 (성급한 리셋 방지)
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
        } else if (TimeUtils::isElapsed(s_stable_start_ms, Config::Timing::CACHE_CONVERGENCE_STABLE_MS)) { // 1.5초간 신규 기기 증가 멈춤 & 전원 온라인 확인 시 최종 수렴!
          s_convergence_done = true;
          g_initial_caching_complete.store(true, std::memory_order_release);
          if (g_system_event_group) {
            xEventGroupSetBits(g_system_event_group, SYS_EVT_CACHE_READY);
          }
          if (parser && parser->isAutoMode() && !g_auto_probing_engine.isOffsetsLocked()) {
            MutexLocker u0_lock(g_uart0_mutex, pdMS_TO_TICKS(100));
            Ch1_WaitBusIdle(Config::Timing::CH1_INTER_PACKET_DELAY_MS);
            g_auto_probing_engine.analyzeCacheMatrix();
          }
          // ★ 2차 캐싱 100% 수렴 완료! 초기 웜업 노이즈(Uncache, 웜업 제어/폴링 수) 일괄 리셋
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

    // 다음 폴링 시점까지 남은 시간 계산
    uint32_t now = millis();
    uint32_t rem_ms = (now < next_poll_due_ms) ? (next_poll_due_ms - now) : 0;
    TickType_t wait_ticks = (rem_ms > 0) ? pdMS_TO_TICKS(rem_ms) : 1;

    QueueSetMemberHandle_t activated = nullptr;
    if (g_ch1_queue_set) {
      activated = xQueueSelectFromSet(g_ch1_queue_set, wait_ticks);
    } else {
      vTaskDelay(wait_ticks);
    }

    // 1. VIP 우선순위 철저 보장: 큐셋이 어느 큐 때문에 깨어났든 VIP 큐를 non-blocking(0)으로 최우선 검사
    if (g_ch1_vip_queue && xQueueReceive(g_ch1_vip_queue, &ctrlPacket, 0) == pdTRUE) {
      Ch1_SetState(current_state, Ch1State::VIP_CONTROL);
      Ch1_HandleCtrl(ctrlPacket);
      Ch1_SetState(current_state, Ch1State::IDLE);
      continue; // VIP 처리 완료 후 다음 루프로 즉시 재평가
    }

    // 2. 일반 제어 큐 처리
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

    // 3. 타임아웃 만료 (activated == nullptr) = 폴링 시점 도달
    if (activated == nullptr || now >= next_poll_due_ms) {
      Ch1_SetState(current_state, Ch1State::POLL_DEVICE);
      Ch1_PollNext(current_dev_idx);
      Ch1_SetState(current_state, Ch1State::IDLE);
      next_poll_due_ms = millis() + poll_interval;
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

    struct TaskAckPollContext {
      TimestampedPacketQueue<8> *ack_q;
      const WallpadChannelConfig *cfg;
      SingleChannelStats *stats;
    };
    TaskAckPollContext poll_ctx{&ack_queue, cfg, stats};

    auto poll_ack = [](void *arg) {
      auto *ctx = static_cast<TaskAckPollContext *>(arg);
      if (!ctx || !ctx->ack_q || !ctx->cfg || !ctx->stats)
        return;

      StaticPacket next_ack;
      uint32_t next_due = 0;
      uint32_t now = millis();

      while (ctx->ack_q->peek(next_ack, next_due)) {
        if (now >= next_due) {
          if (ctx->ack_q->dequeue(next_ack, next_due)) {
            SemaphoreHandle_t u_mux =
                (ctx->cfg->uart_num == UART_NUM_1)   ? g_uart1_mutex
                : (ctx->cfg->uart_num == UART_NUM_2) ? g_uart2_mutex
                                                     : nullptr;
            if (u_mux) {
              MutexLocker lock(u_mux, pdMS_TO_TICKS(100));
              if (lock.isLocked()) {
                uart_write_bytes(ctx->cfg->uart_num, next_ack.data.data(),
                                 next_ack.length);
              } else {
                ctx->stats->timeouts.fetch_add(1, std::memory_order_relaxed);
                g_telnet_tracer.trace(
                    "[WARN] UART mutex timeout on virtual ACK\r\n");
              }
            } else {
              uart_write_bytes(ctx->cfg->uart_num, next_ack.data.data(),
                               next_ack.length);
            }
            g_telnet_tracer.trace(ctx->cfg->channel_id, true, TraceType::ACK,
                               next_ack);
            ctx->stats->tx_pkts.fetch_add(1, std::memory_order_relaxed);
          }
        } else {
          break;
        }
      }
    };

    poll_ack(&poll_ctx);

    StaticPacket req;
    if (Uart_RecvPacket(cfg->uart_num, req, 100, poll_ack, &poll_ctx) ==
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
      } else if (parser->isControlPacket(frame)) {
        StaticPacket dummy_ack;
        g_control_dispatcher.dispatch(req, dummy_ack);
      }
    }
  }
}

void Task_Ch4(void *pvParameters) {
  esp_task_wdt_add(nullptr);
  StaticPacket packet_to_tx;

  // 범용 인터패킷 갭(IPG) 기반 패킷화 엔진
  // STX/ETX에 무관하게 어떤 제조사 도어폰이든 25ms 침묵을 1프레임 종료로 판정
  static uint8_t buf[64] = {0};
  static size_t buf_len = 0;
  static uint32_t last_byte_ms = 0;  // 마지막 수신 바이트 타임스탬프
  static StaticPacket last_tx_pkt{};
  static uint32_t last_tx_ms = 0;
  static StaticPacket last_pkt{};
  static uint32_t last_pkt_ms = 0;

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

    // TX: 앱/허브에서 도어폰으로 보내는 제어 패킷 (PassThrough)
    if (xQueueReceive(g_ch4_passthrough_queue, &packet_to_tx, 0) == pdTRUE) {
      if (packet_to_tx.length >= 3) {
        g_doorphone_tracker.processFrame(packet_to_tx.data[0], packet_to_tx.data[packet_to_tx.length - 1], packet_to_tx.length);
      }
      g_telnet_tracer.trace(4, true, TraceType::RMT, packet_to_tx);
      last_tx_pkt = packet_to_tx;
      g_doorphone_serial.write(packet_to_tx.data.data(), packet_to_tx.length);
      last_tx_ms = millis();
      g_pkt_stats.ch4.tx_pkts.fetch_add(1, std::memory_order_relaxed);
    }

    // RX: 도어폰 하드웨어에서 들어오는 바이트를 스트림 버퍼에 누적
    const uint32_t ib_timeout = Config::Timing::getDoorphoneInterByteTimeoutMs(g_config.doorphone_baud_rate);
    while (g_doorphone_serial.available() > 0) {
      uint8_t byte = static_cast<uint8_t>(g_doorphone_serial.read());
      uint32_t now = millis();

      // 바이트 간 연속성 검증: 직전 바이트와의 간격이 보레이트 기준 허용치를 초과하면 비연속 노이즈 조각으로 판단하여 버퍼 초기화
      if (buf_len > 0 && last_byte_ms > 0 &&
          TimeUtils::isElapsed(last_byte_ms, ib_timeout)) {
        buf_len = 0;
      }

      if (buf_len < sizeof(buf)) {
        buf[buf_len++] = byte;
      } else {
        // 버퍼 가득 참 → 앞 1바이트 버리고 시프트 (노이즈 회복)
        memmove(buf, buf + 1, buf_len - 1);
        buf_len--;
        buf[buf_len++] = byte;
      }
      last_byte_ms = now;
    }

    // [월패드급 슬라이딩 윈도우 스트림 파서]
    Config::Doorphone::FramingStatus cur_status = g_doorphone_tracker.status.load(std::memory_order_relaxed);
    if (cur_status == Config::Doorphone::FramingStatus::LOCKED && buf_len >= 3) {
      uint8_t target_stx = g_doorphone_tracker.candidate_stx.load(std::memory_order_relaxed);
      uint8_t target_etx = g_doorphone_tracker.candidate_etx.load(std::memory_order_relaxed);
      uint8_t target_len = g_doorphone_tracker.candidate_len.load(std::memory_order_relaxed);

      size_t p = 0;
      while (p < buf_len) {
        // 1. STX 탐색 (앞선 노이즈 바이트 자동 스킵)
        if (buf[p] != target_stx) {
          p++;
          continue;
        }

        // 2. 가변/고정 패킷 길이 슬라이싱 탐색
        bool frame_found = false;
        size_t found_len = 0;

        if (target_len >= 3 && target_len <= 64) {
          // 고정 길이 우선 검사
          if (buf_len - p >= target_len) {
            if (buf[p + target_len - 1] == target_etx) {
              frame_found = true;
              found_len = target_len;
            } else {
              // STX는 일치하나 고정 길이 위치의 바이트가 ETX와 불일치 (노이즈/변칙 프레임)
              // 현재 STX를 노이즈로 간주하고 즉시 스킵하여 버퍼에서 폐기(Drop)
              StaticPacket drp_pkt{4, static_cast<uint8_t>(std::min<size_t>(buf_len - p, 16))};
              memcpy(drp_pkt.data.data(), &buf[p], drp_pkt.length);
              g_telnet_tracer.trace(4, false, TraceType::DRP, drp_pkt);
              g_pkt_stats.ch4.invalid_frames.fetch_add(1, std::memory_order_relaxed);
              p++;
              continue;
            }
          } else {
            // 패킷 바이트 추가 수신 대기
            break;
          }
        } else {
          // 고정 길이가 아니거나 미설정된 경우, 가변 ETX 탐색 (3B ~ 64B)
          for (size_t i = p + 2; i < buf_len && (i - p + 1) <= 64; ++i) {
            if (buf[i] == target_etx) {
              frame_found = true;
              found_len = (i - p) + 1;
              break;
            }
          }
        }

        if (frame_found) {
          // [순수 범용 에코 필터링] 직전 100ms 이내 송신 패킷과 100% 바이트 단위 일치 시 에코로 폐기
          if (last_tx_pkt.length == found_len &&
              memcmp(last_tx_pkt.data.data(), &buf[p], found_len) == 0 &&
              last_tx_ms > 0 && !TimeUtils::isElapsed(last_tx_ms, 150)) {
            p += found_len;
            last_byte_ms = 0;
            continue;
          }

          StaticPacket packet{4, static_cast<uint8_t>(found_len)};
          memcpy(packet.data.data(), &buf[p], found_len);

          uint32_t now = millis();
          bool is_debounce = (packet.length == last_pkt.length &&
                              memcmp(packet.data.data(), last_pkt.data.data(), packet.length) == 0 &&
                              now - last_pkt_ms < Config::Timing::DOORPHONE_DEBOUNCE_MS);

          if (!is_debounce) {
            last_pkt = packet;
            last_pkt_ms = now;
            xQueueSend(g_ch4_to_tcp_queue, &packet, 0);
            g_telnet_tracer.trace(4, false, TraceType::RMT, packet);
            g_pkt_stats.ch4.rx_pkts.fetch_add(1, std::memory_order_relaxed);
          }

          p += found_len;
          last_byte_ms = 0;
        } else {
          if (buf_len - p >= 64) {
            // 가변 탐색에서 64바이트 이내에 ETX를 못 찾으면 STX 폐기
            StaticPacket drp_pkt{4, static_cast<uint8_t>(std::min<size_t>(buf_len - p, 16))};
            memcpy(drp_pkt.data.data(), &buf[p], drp_pkt.length);
            g_telnet_tracer.trace(4, false, TraceType::DRP, drp_pkt);
            g_pkt_stats.ch4.invalid_frames.fetch_add(1, std::memory_order_relaxed);
            p++;
            continue;
          }
          // 불완전 패킷 (추가 데이터 수신 대기)
          break;
        }
      }

      // 파싱 완료된 바이트 슬라이딩 시프트
      if (p > 0) {
        if (p < buf_len) {
          memmove(buf, buf + p, buf_len - p);
          buf_len -= p;
        } else {
          buf_len = 0;
        }
      }
    }

    // 미학습/학습 초기 상태 인터패킷 갭(IPG) 감지 및 피딩
    if (last_byte_ms > 0 &&
        TimeUtils::isElapsed(last_byte_ms, Config::Timing::DOORPHONE_IPG_MS)) {

      if (cur_status == Config::Doorphone::FramingStatus::LOCKED) {
        // ★ LOCKED 상태: IPG 만료 시 스트림 파서가 정상 패킷을 처리하고 남긴 단순 꼬리 잔여 찌꺼기만 조용히 플러시
        buf_len = 0;
      } else {
        // ★ 미학습(WAITING/LEARNING) 상태: IPG로 패킷 프레임 수집 & 동적 학습
        if (buf_len >= 3) {
          StaticPacket packet{4, static_cast<uint8_t>(buf_len)};
          memcpy(packet.data.data(), buf, buf_len);

          uint8_t pkt_stx = packet.data[0];
          uint8_t pkt_etx = packet.data[packet.length - 1];

          Config::Doorphone::FramingStatus prev_status = g_doorphone_tracker.status.load(std::memory_order_relaxed);
          g_doorphone_tracker.processFrame(pkt_stx, pkt_etx, packet.length);
          Config::Doorphone::FramingStatus status = g_doorphone_tracker.status.load(std::memory_order_relaxed);

          if (prev_status != Config::Doorphone::FramingStatus::LOCKED &&
              status == Config::Doorphone::FramingStatus::LOCKED) {
            g_doorphone_tracker.saveToNvs();
          }

          // 미학습 상태에서도 모니터링/테스트를 위해 전달
          uint32_t now = millis();
          bool is_debounce = (packet.length == last_pkt.length &&
                              memcmp(packet.data.data(), last_pkt.data.data(), packet.length) == 0 &&
                              now - last_pkt_ms < Config::Timing::DOORPHONE_DEBOUNCE_MS);

          if (!is_debounce) {
            last_pkt = packet;
            last_pkt_ms = now;
            xQueueSend(g_ch4_to_tcp_queue, &packet, 0);
            g_telnet_tracer.trace(4, false, TraceType::RMT, packet);
            g_pkt_stats.ch4.rx_pkts.fetch_add(1, std::memory_order_relaxed);
          }
        }
        buf_len = 0;
      }
      last_byte_ms = 0;
    }

    // Event-Driven 블로킹: IPG 잔여 시간에 맞춘 정밀 커널 큐 대기 (최소 2ms 보장하여 IDLE/슬레이브 태스크 CPU 양보)
    uint32_t wait_ms = 5;
    if (last_byte_ms > 0) {
      uint32_t elapsed = millis() - last_byte_ms;
      if (elapsed < Config::Timing::DOORPHONE_IPG_MS) {
        wait_ms = Config::Timing::DOORPHONE_IPG_MS - elapsed;
      } else {
        wait_ms = 2;
      }
    }
    wait_ms = std::max<uint32_t>(wait_ms, 2);

    // TX 큐 블로킹 수신: wait_ms 동안 커널 레벨 Blocked 대기하므로 CPU 점유율 0% 유지
    if (xQueueReceive(g_ch4_passthrough_queue, &packet_to_tx, pdMS_TO_TICKS(wait_ms)) == pdTRUE) {
      if (packet_to_tx.length >= 3) {
        g_doorphone_tracker.processFrame(packet_to_tx.data[0], packet_to_tx.data[packet_to_tx.length - 1], packet_to_tx.length);
      }
      g_telnet_tracer.trace(4, true, TraceType::RMT, packet_to_tx);
      g_doorphone_serial.write(packet_to_tx.data.data(), packet_to_tx.length);
      last_tx_ms = millis();
      g_pkt_stats.ch4.tx_pkts.fetch_add(1, std::memory_order_relaxed);
    }
  }
}