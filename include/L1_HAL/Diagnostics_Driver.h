#pragma once

#include "L0_Foundation/System_Buffer.h"
#include "L0_Foundation/System_Config.h"
#include "L0_Foundation/System_Platform.h"
#include <Arduino.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <esp_system.h>
#include <esp_task_wdt.h>

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
  std::atomic<uint32_t> last_activity_ms{0};

  void reset() {
    rx_pkts.store(0, std::memory_order_relaxed);
    tx_pkts.store(0, std::memory_order_relaxed);
    crc_errors.store(0, std::memory_order_relaxed);
    invalid_frames.store(0, std::memory_order_relaxed);
    timeouts.store(0, std::memory_order_relaxed);
    lock_timeouts.store(0, std::memory_order_relaxed);
    uncached_pkts.store(0, std::memory_order_relaxed);
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
void Diag_DiagnoseStuck();
void Diag_CheckCoreDump();
uint32_t Diag_EvaluateCrashCounter(esp_reset_reason_t reason) noexcept;
uint32_t Diag_GetCrashCounter() noexcept;
void Diag_ResetCrashCounter() noexcept;
void Diag_ResetTaskWdtAlive() noexcept;

const char *Diag_GetPendingRebootReason() noexcept;
const char *Diag_ConsumePendingRebootReason() noexcept;
void Diag_SetPendingRebootReason(const char *reason) noexcept;

uint32_t Diag_GetBootTimeMs() noexcept;
void Diag_SetBootTimeMs(uint32_t ms) noexcept;

extern SystemMetricsTracker g_metrics;
void Diagnostics_Init() noexcept;
extern PacketStatistics g_pkt_stats;

struct Ch1StateMetrics {
  std::atomic<uint32_t> poll_cnt{0};
  std::atomic<uint32_t> vip_cnt{0};
  std::atomic<uint32_t> normal_cnt{0};
  std::atomic<uint8_t> last_from_state{0};
  std::atomic<uint8_t> last_to_state{0};
  std::atomic<uint32_t> last_transition_ms{0};
};

extern Ch1StateMetrics g_ch1_state_metrics;

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
