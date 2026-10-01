#pragma once

#include "Base/BufferUtils.h"
#include "Base/SystemConfig.h"
#include "Base/SystemPlatform.h"
#include "System/LockUtils.h"
#include <Arduino.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <esp_system.h>
#include <esp_task_wdt.h>

// ── Snapshot & Metrics Data Structures ──

struct SysSnapshot {
  uint32_t free_heap;
  uint32_t min_free_heap;
  uint32_t total_heap;
  uint32_t sketch_size_kb;
  uint32_t flash_total_kb;
  uint32_t uptime_ms;
  bool wifi_connected;
  int8_t wifi_rssi;
  char wifi_ip[16];
};

struct HwSnapshot {
  uint8_t cpu0_cur, cpu0_15m_avg, cpu0_15m_peak, cpu0_24h_avg, cpu0_24h_peak;
  uint8_t cpu1_cur, cpu1_15m_avg, cpu1_15m_peak, cpu1_24h_avg, cpu1_24h_peak;
  uint16_t ram_cur, ram_15m_avg, ram_15m_peak, ram_24h_avg, ram_24h_peak;
  int8_t temp_cur, temp_15m_avg, temp_15m_peak, temp_24h_avg, temp_24h_peak;
};

struct StackSnapshot {
  uint16_t ch1_stack, ch2_stack, ch3_stack, ch4_stack, net_stack, telnet_stack;
};

struct MetricSample {
  uint8_t cpu0_pct;
  uint8_t cpu1_pct;
  uint16_t ram_kb;
  uint16_t flash_kb;
  int8_t temp_c;
};

struct MetricBucket {
  uint32_t cpu0_sum{0};
  uint32_t cpu1_sum{0};
  uint8_t cpu0_peak{0};
  uint8_t cpu1_peak{0};
  uint32_t ram_sum{0};
  uint16_t ram_peak{0};
  int32_t temp_sum{0};
  int8_t temp_peak{-127};
  uint16_t count{0};
};

struct StatSummary {
  uint8_t cpu0_avg{0}, cpu0_peak{0};
  uint8_t cpu1_avg{0}, cpu1_peak{0};
  uint16_t ram_avg{0}, ram_peak{0};
  uint16_t flash_avg{0}, flash_peak{0};
  int8_t temp_avg{0}, temp_peak{0};
  uint16_t count{0};
};

class SystemMetricsTracker {
public:
  static constexpr size_t SAMPLES_15M = 180;
  static constexpr size_t BUCKETS_24H = 96;

private:
  MetricSample _ring15[SAMPLES_15M]{};
  size_t _ring15_head = 0;
  size_t _ring15_count = 0;

  MetricBucket _ring24[BUCKETS_24H]{};
  size_t _ring24_head = 0;
  size_t _ring24_count = 0;

  MetricBucket _cur_bucket{};
  uint16_t _bucket_sample_count = 0;

  MetricSample _current{};
  uint16_t _cached_flash_kb{0};
  mutable SemaphoreHandle_t _metrics_mutex = nullptr;

public:
  SystemMetricsTracker() noexcept = default;

  void init();
  void reset();
  void addSample(uint8_t cpu0_pct, uint8_t cpu1_pct, uint16_t ram_kb,
                 int8_t temp_c);
  MetricSample getCurrent() const noexcept {
    MutexLocker lock(_metrics_mutex);
    return _current;
  }

  StatSummary get15m() const;
  StatSummary get24h() const;
};

// ── Packet Channel Statistics ──

struct SingleChannelStats {
  std::atomic<uint32_t> rx_pkts{0};
  std::atomic<uint32_t> tx_pkts{0};
  std::atomic<uint32_t> crc_errors{0};
  std::atomic<uint32_t> invalid_frames{0};
  std::atomic<uint32_t> timeouts{0};
  std::atomic<uint32_t> uncached_pkts{0};

  void reset() {
    rx_pkts.store(0, std::memory_order_relaxed);
    tx_pkts.store(0, std::memory_order_relaxed);
    crc_errors.store(0, std::memory_order_relaxed);
    invalid_frames.store(0, std::memory_order_relaxed);
    timeouts.store(0, std::memory_order_relaxed);
    uncached_pkts.store(0, std::memory_order_relaxed);
  }
};

struct TcpSocketStats {
  std::atomic<bool> is_connected{false};
  std::atomic<uint32_t> connection_count{0};
  std::atomic<uint32_t> rx_pkts{0};
  std::atomic<uint32_t> tx_pkts{0};
  std::atomic<uint32_t> dropped_pkts{0};
  std::atomic<uint32_t> uncached_pkts{0};

  void reset() {
    rx_pkts.store(0, std::memory_order_relaxed);
    tx_pkts.store(0, std::memory_order_relaxed);
    dropped_pkts.store(0, std::memory_order_relaxed);
    uncached_pkts.store(0, std::memory_order_relaxed);
  }
};

struct PacketStatistics {
  SingleChannelStats ch1;
  SingleChannelStats ch2;
  SingleChannelStats ch3;
  SingleChannelStats ch4;
  TcpSocketStats ch5;
  TcpSocketStats ch6;

  void resetAll() {
    ch1.reset();
    ch2.reset();
    ch3.reset();
    ch4.reset();
    ch5.reset();
    ch6.reset();
  }
};

struct ChanStats {
  uint32_t rx_pkts{0};
  uint32_t tx_pkts{0};
  uint32_t crc_errors{0};
  uint32_t invalid_frames{0};
  uint32_t timeouts{0};
  uint32_t uncached_pkts{0};

  ChanStats() = default;
  ChanStats(const SingleChannelStats &s) noexcept
      : rx_pkts(s.rx_pkts.load(std::memory_order_relaxed)),
        tx_pkts(s.tx_pkts.load(std::memory_order_relaxed)),
        crc_errors(s.crc_errors.load(std::memory_order_relaxed)),
        invalid_frames(s.invalid_frames.load(std::memory_order_relaxed)),
        timeouts(s.timeouts.load(std::memory_order_relaxed)),
        uncached_pkts(s.uncached_pkts.load(std::memory_order_relaxed)) {}

  ChanStats &operator=(const SingleChannelStats &s) noexcept {
    rx_pkts = s.rx_pkts.load(std::memory_order_relaxed);
    tx_pkts = s.tx_pkts.load(std::memory_order_relaxed);
    crc_errors = s.crc_errors.load(std::memory_order_relaxed);
    invalid_frames = s.invalid_frames.load(std::memory_order_relaxed);
    timeouts = s.timeouts.load(std::memory_order_relaxed);
    uncached_pkts = s.uncached_pkts.load(std::memory_order_relaxed);
    return *this;
  }
};

struct TcpChanStats {
  uint32_t rx_pkts{0};
  uint32_t tx_pkts{0};
  uint32_t dropped_pkts{0};
  uint32_t uncached_pkts{0};
  uint16_t connection_count{0};
  bool is_connected{false};

  TcpChanStats() = default;
  TcpChanStats(const TcpSocketStats &s) noexcept
      : rx_pkts(s.rx_pkts.load(std::memory_order_relaxed)),
        tx_pkts(s.tx_pkts.load(std::memory_order_relaxed)),
        dropped_pkts(s.dropped_pkts.load(std::memory_order_relaxed)),
        uncached_pkts(s.uncached_pkts.load(std::memory_order_relaxed)),
        connection_count(s.connection_count.load(std::memory_order_relaxed)),
        is_connected(s.is_connected.load(std::memory_order_relaxed)) {}

  TcpChanStats &operator=(const TcpSocketStats &s) noexcept {
    is_connected = s.is_connected.load(std::memory_order_relaxed);
    connection_count = s.connection_count.load(std::memory_order_relaxed);
    rx_pkts = s.rx_pkts.load(std::memory_order_relaxed);
    tx_pkts = s.tx_pkts.load(std::memory_order_relaxed);
    dropped_pkts = s.dropped_pkts.load(std::memory_order_relaxed);
    uncached_pkts = s.uncached_pkts.load(std::memory_order_relaxed);
    return *this;
  }
};

struct PktSnapshot {
  ChanStats ch1;
  ChanStats ch2;
  ChanStats ch3;
  ChanStats ch4;
  TcpChanStats ch5;
  TcpChanStats ch6;
};

// ── Watchdog & Task Monitoring ──

struct TaskWdtMetrics {
  std::atomic<uint32_t> last_feed_ms{0};
  std::atomic<uint32_t> max_interval_ms{0};
  std::atomic<uint32_t> feed_count{0};
};

class TaskWdtMonitor {
public:
  static constexpr size_t TASK_COUNT = 6;
  TaskWdtMetrics tasks[TASK_COUNT];

  void feed(size_t index) noexcept;
};

// ── Reboot & Crash Log ──

struct LogEntry {
  uint32_t timestamp;
  char reason[32];
  SysSnapshot stats_snapshot;
  HwSnapshot hw_snapshot;
  StackSnapshot stack_snapshot;
  PktSnapshot packet_stats_snapshot;
};

class LogManager {
public:
  static constexpr size_t MAX_LOG_ENTRIES = 20;
  static void writeRebootLog(const char *reason);
  static size_t getLogCount();
  static bool getLogEntry(size_t index, LogEntry &out_entry);
  static void readRebootLog(char *out_buf, size_t max_len, size_t index = 0);
  static void clearRebootLog();
};

// ── Diagnostics Functions ──

void System_TakeSnapshot(SysSnapshot &sys, HwSnapshot &hw, StackSnapshot &st,
                         PktSnapshot &pkt);
void System_Restart(const char *reason);
void System_ReadCpuPct(uint8_t &cpu0_out, uint8_t &cpu1_out);
int8_t System_ReadTempC();
void System_EnterRescueMode(const char *reason);
void System_CheckOtaHealth();
void System_LogResetReason();
void System_DiagnoseStuck();
void System_CheckCoreDump();
[[nodiscard]] bool System_IsOtaPendingVerify();

extern const char *s_pending_reboot_reason;
extern SystemMetricsTracker g_metrics;
extern TaskWdtMonitor g_wdt_monitor;
extern PacketStatistics g_pkt_stats;
