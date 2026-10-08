#pragma once

#include "L0_Foundation/System_Buffer.h"
#include "L0_Foundation/System_Config.h"
#include "L0_Foundation/System_Platform.h"
#include <Arduino.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <esp_cpu.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ── Metrics Data Structures ──

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
  MetricSample getCurrent() const noexcept;

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
  std::atomic<uint32_t> lock_timeouts{0};
  std::atomic<uint32_t> uncached_pkts{0};
  std::atomic<uint32_t> queue_full{0};
  std::atomic<uint32_t> last_activity_ms{0};

  void reset() {
    rx_pkts.store(0, std::memory_order_relaxed);
    tx_pkts.store(0, std::memory_order_relaxed);
    crc_errors.store(0, std::memory_order_relaxed);
    invalid_frames.store(0, std::memory_order_relaxed);
    timeouts.store(0, std::memory_order_relaxed);
    lock_timeouts.store(0, std::memory_order_relaxed);
    uncached_pkts.store(0, std::memory_order_relaxed);
    queue_full.store(0, std::memory_order_relaxed);
    last_activity_ms.store(0, std::memory_order_relaxed);
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

// ── Operational Latency & Log2 Histogram Tracker ──

class LatencyStat {
public:
  // Bucket b: Cycle bit-width b in range [2^(b-1), 2^b). Last bucket is saturated (>= 2^19 = ~2.18ms @ 240MHz)
  static constexpr unsigned kBuckets = 20;

  void record(uint32_t cycles) noexcept {
    constexpr auto rx = std::memory_order_relaxed;
    _count.store(_count.load(rx) + 1, rx);

    if (cycles > _max.load(rx)) {
      _max.store(cycles, rx);
      _maxTick.store(xTaskGetTickCount(), rx);
    }

    const unsigned b = std::min<unsigned>(static_cast<unsigned>(std::bit_width(cycles)), kBuckets - 1);
    _hist[b].store(_hist[b].load(rx) + 1, rx);
  }

  void reset() noexcept {
    constexpr auto rx = std::memory_order_relaxed;
    _count.store(0, rx);
    _max.store(0, rx);
    _maxTick.store(0, rx);
    for (auto &h : _hist) {
      h.store(0, rx);
    }
  }

  void takeSnapshot(LatencySnapshot &snap) const noexcept {
    constexpr auto rx = std::memory_order_relaxed;
    snap.count = _count.load(rx);
    snap.max_cycles = _max.load(rx);
    const uint32_t mt = _maxTick.load(rx);
    snap.max_age_ms = mt > 0 ? (xTaskGetTickCount() - mt) * portTICK_PERIOD_MS : 0;
    for (unsigned b = 0; b < kBuckets; ++b) {
      snap.hist[b] = _hist[b].load(rx);
    }
  }

private:
  std::atomic<uint32_t> _count{0};
  std::atomic<uint32_t> _max{0};
  std::atomic<uint32_t> _maxTick{0};
  std::atomic<uint32_t> _hist[kBuckets]{};
};

#if ATOMIC_INT_LOCK_FREE == 2
static_assert(std::atomic<uint32_t>::is_always_lock_free, "lock-free atomic required");
#else
static_assert(ATOMIC_INT_LOCK_FREE >= 1, "atomic operations supported");
#endif

// ── Snapshot Conversion Helpers ──
inline ChanStats SingleChannelToSnapshot(const SingleChannelStats &s) noexcept {
  ChanStats out{};
  out.rx_pkts = s.rx_pkts.load(std::memory_order_relaxed);
  out.tx_pkts = s.tx_pkts.load(std::memory_order_relaxed);
  out.crc_errors = s.crc_errors.load(std::memory_order_relaxed);
  out.invalid_frames = s.invalid_frames.load(std::memory_order_relaxed);
  out.timeouts = s.timeouts.load(std::memory_order_relaxed);
  out.lock_timeouts = s.lock_timeouts.load(std::memory_order_relaxed);
  out.uncached_pkts = s.uncached_pkts.load(std::memory_order_relaxed);
  out.queue_full = s.queue_full.load(std::memory_order_relaxed);
  out.last_activity_ms = s.last_activity_ms.load(std::memory_order_relaxed);
  return out;
}

inline TcpChanStats TcpSocketToSnapshot(const TcpSocketStats &s) noexcept {
  TcpChanStats out{};
  out.rx_pkts = s.rx_pkts.load(std::memory_order_relaxed);
  out.tx_pkts = s.tx_pkts.load(std::memory_order_relaxed);
  out.dropped_pkts = s.dropped_pkts.load(std::memory_order_relaxed);
  out.uncached_pkts = s.uncached_pkts.load(std::memory_order_relaxed);
  out.connection_count = static_cast<uint16_t>(s.connection_count.load(std::memory_order_relaxed));
  out.is_connected = s.is_connected.load(std::memory_order_relaxed);
  return out;
}

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
  void reset() noexcept;
};

// ── Reboot & Crash Log ──

enum class LogReadError : uint8_t {
  OutOfBounds,
  Empty
};

class LogManager {
public:
  static constexpr size_t MAX_LOG_ENTRIES = 20;
  static void writeRebootLog(const char *reason);
  static size_t getLogCount();
  [[nodiscard]] static std::expected<LogEntry, LogReadError>
  getLogEntry(size_t index) noexcept;
  static bool getLogEntry(size_t index, LogEntry &out_entry) noexcept;
  static void clearRebootLog();
};

// ── Hardware Diagnostics Functions & Rescue HAL ──

struct RescueHwConfig {
  const char *reason{nullptr};
  const char *sta_ssid{nullptr};
  const char *sta_password{nullptr};
};

void Diag_StartRescueAp(const RescueHwConfig &cfg);
void Diag_CheckOtaHealth();
void Diag_LogResetReason();
inline const char *Diag_ResetReasonToString(esp_reset_reason_t rr) noexcept {
  return System_ResetReasonToString(rr);
}
void Diag_DiagnoseStuck();
void Diag_CheckCoreDump();
[[nodiscard]] uint32_t Diag_EvaluateCrashCounter(esp_reset_reason_t reason) noexcept;
[[nodiscard]] uint32_t Diag_GetCrashCounter() noexcept;
void Diag_ResetCrashCounter() noexcept;
void Diag_ResetTaskWdtAlive() noexcept;

[[nodiscard]] const char *Diag_GetPendingRebootReason() noexcept;
[[nodiscard]] const char *Diag_ConsumePendingRebootReason() noexcept;
void Diag_SetPendingRebootReason(const char *reason) noexcept;

[[nodiscard]] uint32_t Diag_GetBootTimeMs() noexcept;
void Diag_SetBootTimeMs(uint32_t ms) noexcept;

void Diagnostics_Init() noexcept;

// ── Packet & Channel Statistics Recording API (100% Encapsulated) ──
[[nodiscard]] SingleChannelStats *Diag_GetChannelStats(uint8_t ch) noexcept;
[[nodiscard]] TcpSocketStats *Diag_GetTcpStats(uint8_t ch) noexcept;

void Diag_RecordChannelTx(uint8_t ch) noexcept;
void Diag_RecordChannelRx(uint8_t ch) noexcept;
void Diag_RecordChannelTimeout(uint8_t ch) noexcept;
void Diag_RecordChannelLockTimeout(uint8_t ch) noexcept;
void Diag_RecordChannelQueueFull(uint8_t ch) noexcept;
void Diag_RecordChannelInvalidFrame(uint8_t ch) noexcept;
void Diag_RecordChannelCrcError(uint8_t ch) noexcept;
void Diag_RecordChannelActivity(uint8_t ch, uint32_t now_ms) noexcept;
[[nodiscard]] uint32_t Diag_GetChannelLastActivityMs(uint8_t ch) noexcept;

void Diag_RecordCh1StateTransition(uint8_t from_state, uint8_t to_state, uint32_t now_ms) noexcept;

void Diag_RecordCh1Latency(uint32_t cycles) noexcept;
void Diag_ResetCh1Latency() noexcept;

class Diag_ScopedCh1Latency {
public:
  Diag_ScopedCh1Latency() noexcept : _t0(esp_cpu_get_cycle_count()) {}
  ~Diag_ScopedCh1Latency() {
    Diag_RecordCh1Latency(static_cast<uint32_t>(esp_cpu_get_cycle_count() - _t0));
  }
  Diag_ScopedCh1Latency(const Diag_ScopedCh1Latency &) = delete;
  Diag_ScopedCh1Latency &operator=(const Diag_ScopedCh1Latency &) = delete;

private:
  uint32_t _t0;
};

// ── Unified System Trace Sink & Shutdown Hooks are canonically in System_Platform.h ──

// ── Task Identifier & Handles (Encapsulated) ──
enum class SystemTaskId : uint8_t {
  CH1 = 0,
  CH2,
  CH3,
  CH4,
  NETWORK,
  TELNET,
  COUNT
};

void System_RegisterTaskHandle(SystemTaskId id, TaskHandle_t handle) noexcept;
[[nodiscard]] TaskHandle_t System_GetTaskHandle(SystemTaskId id) noexcept;
