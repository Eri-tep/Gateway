#include "Common.h"
#include <cstdio>
#include <cstring>
#include <atomic>

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

  // CH6 SmartThings & Mgmt JSON-RPC (8900)
  {
    bool is_conn = pkt.ch6.is_connected;
    uint32_t rx = pkt.ch6.rx_pkts;
    uint32_t tx = pkt.ch6.tx_pkts;
    const char *status_str = !is_conn               ? "Disconnected"
                             : (rx == 0 && tx == 0) ? "Idle"
                                                     : "Connected";
    out.appendFormat("%-10s %-6u %-14s %3u%12u%12u%10u%10u\r\n", "CH#6_Mgmt", Config::TCP::MGMT_PORT, status_str,
                     static_cast<unsigned>(pkt.ch6.connection_count), static_cast<unsigned>(rx), static_cast<unsigned>(tx),
                     static_cast<unsigned>(pkt.ch6.dropped_pkts), static_cast<unsigned>(pkt.ch6.uncached_pkts));
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

  // CH5 EW11 TCP Clients (Slot 0: 8898, Slot 1~4: 8891~8894)
  for (int s = 0; s < Config::TCP::MAX_EW11_SLOTS; s++) {
    auto &slot = g_ew11_slots[s];
    if (!slot.enabled && strlen(slot.target_ip) == 0 && slot.target_port == 0) continue;

    char chan_name[16];
    snprintf(chan_name, sizeof(chan_name), "CH#5_%u", slot.target_port ? slot.target_port : Config::TCP::EW11_SLOT_PORTS[s]);

    uint32_t drp = slot.dropped_pkts;
    char drp_str[24];
    snprintf(drp_str, sizeof(drp_str), "%u", static_cast<unsigned>(drp));

    out.appendFormat("%-10s %10u %12u %15s %10u %9u %8u\r\n",
                     chan_name,
                     static_cast<unsigned>(slot.rx_pkts),
                     static_cast<unsigned>(slot.tx_pkts),
                     drp > 0 ? drp_str : "0 (0.00%)",
                     0u, 0u, 0u);
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

