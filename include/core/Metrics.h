#pragma once

// ============================================================================
// CONCURRENCY LOCKERS & METRICS TRACKERS (Extracted from Common.h SECTION 4)
// ============================================================================

#include "Config.h"

class CriticalSectionLocker {
private:
  portMUX_TYPE *_mux{nullptr};

public:
  explicit CriticalSectionLocker(portMUX_TYPE *mux) noexcept : _mux(mux) {
    if (_mux)
      portENTER_CRITICAL(_mux);
  }
  explicit CriticalSectionLocker(portMUX_TYPE &mux) noexcept : _mux(&mux) {
    portENTER_CRITICAL(_mux);
  }
  ~CriticalSectionLocker() noexcept {
    if (_mux)
      portEXIT_CRITICAL(_mux);
  }
  CriticalSectionLocker(const CriticalSectionLocker &) = delete;
  CriticalSectionLocker &operator=(const CriticalSectionLocker &) = delete;
  CriticalSectionLocker(CriticalSectionLocker &&) = delete;
  CriticalSectionLocker &operator=(CriticalSectionLocker &&) = delete;
};

class [[nodiscard]] MutexLocker {
private:
  SemaphoreHandle_t _mutex{nullptr};
  bool _locked{false};
  uint32_t _acquired_ms{0};

public:
  explicit MutexLocker(SemaphoreHandle_t mutex,
                       TickType_t timeout = portMAX_DELAY) noexcept
      : _mutex(mutex) {
    if (_mutex) {
      _locked = (xSemaphoreTake(_mutex, timeout) == pdTRUE);
      if (_locked) {
        _acquired_ms = millis();
      }
    }
  }
  ~MutexLocker() noexcept {
    if (_mutex && _locked) {
      uint32_t hold_ms = millis() - _acquired_ms;
      if (hold_ms >= Config::Timing::MAX_LOCK_HOLD_MS) {
        ESP_LOGW("LOCK", "Mutex held for %u ms (>= %u ms threshold)",
                 static_cast<unsigned>(hold_ms),
                 static_cast<unsigned>(Config::Timing::MAX_LOCK_HOLD_MS));
      }
      xSemaphoreGive(_mutex);
    }
  }
  [[nodiscard]] bool isLocked() const noexcept { return _locked; }
  explicit operator bool() const noexcept { return _locked; }
  MutexLocker(const MutexLocker &) = delete;
  MutexLocker &operator=(const MutexLocker &) = delete;
  MutexLocker(MutexLocker &&) = delete;
  MutexLocker &operator=(MutexLocker &&) = delete;
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
  MetricSample _ring15[SAMPLES_15M];
  size_t _ring15_head = 0;
  size_t _ring15_count = 0;

  MetricBucket _ring24[BUCKETS_24H];
  size_t _ring24_head = 0;
  size_t _ring24_count = 0;

  MetricBucket _cur_bucket;
  uint16_t _bucket_sample_count = 0;

  MetricSample _current;
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
