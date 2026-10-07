// ============================================================================
// Benchmark_Harness.cpp — Level 4 System Diagnostics & Performance Profiling
// ESP32-S3 Production-Grade Native Cycle-Accurate Benchmark & Safety Harness
// ============================================================================

#if defined(BENCHMARK_BUILD)

#include "L4_Services/Benchmark_Harness.h"
#include "L0_Foundation/System_Platform.h"
#include "L0_Foundation/System_Buffer.h"
#include "L0_Foundation/System_Config.h"
#include "L3_Protocol/Public/Protocol_Device.h"
#include "L3_Protocol/Public/Protocol_Facade.h"
#include "L3_Protocol/Private/Wallpad_Engine.h"
#include "L4_Services/CLI_Commands.h"

#include <esp_cpu.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include <atomic>
#include <cstring>
#include <algorithm>

namespace Benchmark {

// ── Static Hardware Golden Vectors & Observable Sinks ────────────────────────
static constexpr uint8_t GOLDEN_QUERY[11] = {
    0xF7, 0x0B, 0x01, 0x18, 0x01, 0x01, 0x00, 0x00, 0x00, 0x12, 0xEE};
static constexpr uint8_t GOLDEN_ACK[11] = {
    0xF7, 0x0B, 0x01, 0x18, 0x04, 0x01, 0x01, 0x00, 0x00, 0x16, 0xEE};
static constexpr uint8_t GOLDEN_CONTROL[14] = {
    0xF7, 0x0E, 0x01, 0x28, 0x00, 0x01, 0x01, 0x16, 0x00, 0x00, 0x00, 0x00, 0x3B, 0xEE};

static volatile uint32_t s_observable_sink{0};
static uint8_t s_null_sink[128]{0};
static uint32_t s_probe_overhead_cycles{0};

// ── In-Memory Histogram Accumulator (Pillar 10) ──────────────────────────────
constexpr size_t HIST_BUCKETS = 1000;
constexpr uint32_t BUCKET_WIDTH = 10; // 0..10,000 cycles (10 cycles per bucket)
static uint32_t s_hist[HIST_BUCKETS];

static void HistReset() noexcept {
  std::memset(s_hist, 0, sizeof(s_hist));
}

static inline void HistRecord(uint32_t cycles) noexcept {
  size_t idx = cycles / BUCKET_WIDTH;
  if (idx >= HIST_BUCKETS)
    idx = HIST_BUCKETS - 1;
  s_hist[idx]++;
}

static JitterDistribution HistCompute(uint32_t sample_count, uint32_t min_c,
                                      uint32_t max_c, uint64_t total_c) noexcept {
  JitterDistribution dist{};
  dist.sample_count = sample_count;
  dist.min_cycles = min_c;
  dist.max_cycles = max_c;
  dist.mean_cycles = (sample_count > 0)
                         ? static_cast<uint32_t>(total_c / sample_count)
                         : 0;
  if (sample_count == 0)
    return dist;

  uint32_t p50_target = sample_count * 50 / 100;
  uint32_t p95_target = sample_count * 95 / 100;
  uint32_t p99_target = sample_count * 99 / 100;
  uint32_t p99_9_target = static_cast<uint32_t>(
      static_cast<uint64_t>(sample_count) * 999 / 1000);

  uint32_t accum = 0;
  for (size_t i = 0; i < HIST_BUCKETS; ++i) {
    accum += s_hist[i];
    uint32_t cur_val = static_cast<uint32_t>((i + 1) * BUCKET_WIDTH);
    if (dist.median_cycles == 0 && accum >= p50_target)
      dist.median_cycles = cur_val;
    if (dist.p95_cycles == 0 && accum >= p95_target)
      dist.p95_cycles = cur_val;
    if (dist.p99_cycles == 0 && accum >= p99_target)
      dist.p99_cycles = cur_val;
    if (dist.p99_9_cycles == 0 && accum >= p99_9_target) {
      dist.p99_9_cycles = cur_val;
      break;
    }
  }
  return dist;
}

// ── Probe Overhead Calibration (Pillar 0) ────────────────────────────────────
uint32_t MeasureSelfOverhead() noexcept {
  uint32_t min_diff = UINT32_MAX;
  for (int i = 0; i < 50; ++i) {
    uint32_t t0 = esp_cpu_get_cycle_count();
    uint32_t t1 = esp_cpu_get_cycle_count();
    uint32_t diff = (t1 >= t0) ? (t1 - t0) : 0;
    if (diff < min_diff)
      min_diff = diff;
  }
  s_probe_overhead_cycles = min_diff;
  return min_diff;
}

void Initialize() noexcept {
  MeasureSelfOverhead();
}

static void CaptureSafetyPre(SystemSafetyMetrics &s) noexcept {
  s.start_free_heap = esp_get_free_heap_size();
  s.cpu_freq_mhz = getCpuFrequencyMhz();
  s.start_temp_c = System_ReadTempC();
  s.freq_valid = (s.cpu_freq_mhz == 240);
}

static void CaptureSafetyPost(SystemSafetyMetrics &s) noexcept {
  s.end_free_heap = esp_get_free_heap_size();
  s.heap_delta = static_cast<int32_t>(s.end_free_heap) -
                 static_cast<int32_t>(s.start_free_heap);
  s.heap_valid = (s.heap_delta == 0);

  s.min_stack_headroom = uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t);
  s.stack_valid = (s.min_stack_headroom >= 1536);
  s.end_temp_c = System_ReadTempC();

  SysSnapshot sys_snap;
  HwSnapshot hw_snap;
  StackSnapshot stack_snap;
  PktSnapshot pkt_snap;
  System_TakeSnapshot(sys_snap, hw_snap, stack_snap, pkt_snap);

  s.core0_idle_pct = 100 - hw_snap.cpu0_cur;
  s.core1_idle_pct = 100 - hw_snap.cpu1_cur;
}

// ── Phase 0: Baseline Calibration & Sanity ───────────────────────────────────
BenchmarkReport RunPhase0_BaselineCalibration() noexcept {
  BenchmarkReport r{};
  r.phase_name = "Phase 0: Baseline Calibration";
  r.iterations = 1000;

  CaptureSafetyPre(r.safety);
  uint32_t probe_oh = MeasureSelfOverhead();

  // Test cycle counter stability
  uint32_t t_start = micros();
  uint32_t c_start = esp_cpu_get_cycle_count();
  for (uint32_t i = 0; i < 1000; ++i) {
    uint32_t c0 = esp_cpu_get_cycle_count();
    uint32_t c1 = esp_cpu_get_cycle_count();
    s_observable_sink += (c1 - c0);
  }
  uint32_t c_end = esp_cpu_get_cycle_count();
  uint32_t t_end = micros();

  r.total_duration_us = (t_end >= t_start) ? (t_end - t_start) : 0;
  r.latency.total_pipeline_cycles = probe_oh;

  CaptureSafetyPost(r.safety);
  return r;
}

// ── Phase 1: Primitive & Parser Benchmark (1,000,000 runs) ───────────────────
BenchmarkReport RunPhase1_PrimitiveParser(uint32_t iterations) noexcept {
  BenchmarkReport r{};
  r.phase_name = "Phase 1: Primitive & Parser Benchmark";
  r.iterations = iterations;

  CaptureSafetyPre(r.safety);
  HistReset();

  uint32_t probe_oh = (s_probe_overhead_cycles > 0) ? s_probe_overhead_cycles : MeasureSelfOverhead();
  std::span<const uint8_t> span_pkt(GOLDEN_QUERY, sizeof(GOLDEN_QUERY));

  // 1. Warm-up (1,000 runs discarded)
  for (uint32_t i = 0; i < 1000; ++i) {
    bool v = Wallpad_ValidatePacket(span_pkt);
    int len = Wallpad_ExtractLength(GOLDEN_QUERY, sizeof(GOLDEN_QUERY), 0);
    s_observable_sink += (v ? 1 : 0) ^ len;
  }

  uint32_t t_start = micros();
  uint64_t total_cycles = 0;
  uint32_t min_c = UINT32_MAX;
  uint32_t max_c = 0;

  for (uint32_t i = 0; i < iterations; ++i) {
    uint32_t c0 = esp_cpu_get_cycle_count();

    // Hot-path Parser Operations
    int len = Wallpad_ExtractLength(GOLDEN_QUERY, sizeof(GOLDEN_QUERY), 0);
    bool valid = Wallpad_ValidatePacket(span_pkt);
    bool is_query = Wallpad_IsQueryPacket(span_pkt);

    uint32_t c1 = esp_cpu_get_cycle_count();
    uint32_t diff = (c1 >= c0) ? (c1 - c0) : 0;
    if (diff > probe_oh) diff -= probe_oh;

    s_observable_sink += (valid ? 1 : 0) ^ (is_query ? 2 : 0) ^ len;
    total_cycles += diff;
    if (diff < min_c) min_c = diff;
    if (diff > max_c) max_c = diff;

    HistRecord(diff);

    // Non-blocking WDT Feeder & Yield (Pillar 1)
    if ((i & 0x1FFF) == 0) {
      esp_task_wdt_reset();
      taskYIELD();
    }
    if ((i & 0x7FFF) == 0) {
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }

  uint32_t t_end = micros();
  r.total_duration_us = (t_end >= t_start) ? (t_end - t_start) : 0;
  if (r.total_duration_us > 0) {
    r.throughput_pps = static_cast<uint32_t>(
        (static_cast<uint64_t>(iterations) * 1000000ULL) / r.total_duration_us);
  }

  r.latency.checksum_validate_cycles = static_cast<uint32_t>(total_cycles / iterations);
  r.latency.total_pipeline_cycles = r.latency.checksum_validate_cycles;
  r.jitter = HistCompute(iterations, min_c, max_c, total_cycles);

  CaptureSafetyPost(r.safety);
  return r;
}

// ── Phase 2: CH1 Hot-Path Packet Flow Sandbox (100,000 runs) ─────────────────
BenchmarkReport RunPhase2_CH1HotPathFlow(uint32_t iterations) noexcept {
  BenchmarkReport r{};
  r.phase_name = "Phase 2: CH1 Hot-Path Packet Flow Sandbox";
  r.iterations = iterations;

  CaptureSafetyPre(r.safety);
  HistReset();

  uint32_t probe_oh = (s_probe_overhead_cycles > 0) ? s_probe_overhead_cycles : MeasureSelfOverhead();

  // 1. Warm-up (1,000 runs discarded)
  for (uint32_t i = 0; i < 1000; ++i) {
    std::span<const uint8_t> sp(GOLDEN_QUERY, sizeof(GOLDEN_QUERY));
    Wallpad_ValidatePacket(sp);
    Wallpad_ExtractLength(GOLDEN_QUERY, sizeof(GOLDEN_QUERY), 0);
  }

  uint64_t sum_framing = 0, sum_validate = 0, sum_catalog = 0;
  uint64_t sum_route = 0, sum_dispatch = 0, sum_sync = 0;
  uint64_t total_cycles = 0;
  uint32_t min_c = UINT32_MAX, max_c = 0;

  portMUX_TYPE test_mux = portMUX_INITIALIZER_UNLOCKED;
  uint32_t t_start = micros();

  for (uint32_t i = 0; i < iterations; ++i) {
    uint32_t loop_start = esp_cpu_get_cycle_count();

    // ── Step 1: Stream Framing (STX / Length extraction)
    uint32_t t0 = esp_cpu_get_cycle_count();
    int ext_len = Wallpad_ExtractLength(GOLDEN_QUERY, sizeof(GOLDEN_QUERY), 0);
    uint32_t t1 = esp_cpu_get_cycle_count();
    sum_framing += (t1 >= t0) ? (t1 - t0) : 0;

    // ── Step 2: Packet Validation (Checksum / Framing)
    std::span<const uint8_t> sp(GOLDEN_QUERY, (ext_len > 0) ? ext_len : sizeof(GOLDEN_QUERY));
    t0 = esp_cpu_get_cycle_count();
    bool valid = Wallpad_ValidatePacket(sp);
    t1 = esp_cpu_get_cycle_count();
    sum_validate += (t1 >= t0) ? (t1 - t0) : 0;
    if (!valid) r.checksum_fails++;

    // ── Step 3: Device Catalog Lookup & State Decode
    t0 = esp_cpu_get_cycle_count();
    DeviceStateEntry snap;
    bool found = Device_FindCopy(0x01, 0x01, 0x00, snap);
    t1 = esp_cpu_get_cycle_count();
    sum_catalog += (t1 >= t0) ? (t1 - t0) : 0;
    if (!found) r.catalog_misses++;

    // ── Step 4: Routing Lookup
    t0 = esp_cpu_get_cycle_count();
    uint8_t ch = Protocol_LookupDeviceChannel(0x01, 0x01, 0x00);
    t1 = esp_cpu_get_cycle_count();
    sum_route += (t1 >= t0) ? (t1 - t0) : 0;

    // ── Step 5: Downlink Packet Builder (Null Sink)
    t0 = esp_cpu_get_cycle_count();
    std::memcpy(s_null_sink, GOLDEN_ACK, sizeof(GOLDEN_ACK));
    uint8_t ack_len = sizeof(GOLDEN_ACK);
    t1 = esp_cpu_get_cycle_count();
    sum_dispatch += (t1 >= t0) ? (t1 - t0) : 0;

    // ── Step 6: Synchronization / Critical Section profiling
    t0 = esp_cpu_get_cycle_count();
    portENTER_CRITICAL(&test_mux);
    s_observable_sink += (ch ^ ack_len);
    portEXIT_CRITICAL(&test_mux);
    t1 = esp_cpu_get_cycle_count();
    sum_sync += (t1 >= t0) ? (t1 - t0) : 0;

    uint32_t loop_end = esp_cpu_get_cycle_count();
    uint32_t loop_diff = (loop_end >= loop_start) ? (loop_end - loop_start) : 0;
    if (loop_diff > probe_oh * 6) loop_diff -= (probe_oh * 6);

    total_cycles += loop_diff;
    if (loop_diff < min_c) min_c = loop_diff;
    if (loop_diff > max_c) max_c = loop_diff;

    HistRecord(loop_diff);

    // Outlier capture (> 4,000 cycles)
    if (loop_diff > 4000 && r.outlier_count < r.top_outliers.size()) {
      r.top_outliers[r.outlier_count++] = OutlierEvent{
          .iteration = i,
          .cycles = loop_diff,
          .cmd = GOLDEN_QUERY[3],
          .sub = GOLDEN_QUERY[4]};
    }

    // Non-blocking WDT Feeder & Yield (Pillar 1)
    if ((i & 0x1FFF) == 0) {
      esp_task_wdt_reset();
      taskYIELD();
    }
    if ((i & 0x7FFF) == 0) {
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }

  uint32_t t_end = micros();
  r.total_duration_us = (t_end >= t_start) ? (t_end - t_start) : 0;
  if (r.total_duration_us > 0) {
    r.throughput_pps = static_cast<uint32_t>(
        (static_cast<uint64_t>(iterations) * 1000000ULL) / r.total_duration_us);
  }

  r.latency.stream_framing_cycles = static_cast<uint32_t>(sum_framing / iterations);
  r.latency.checksum_validate_cycles = static_cast<uint32_t>(sum_validate / iterations);
  r.latency.device_catalog_cycles = static_cast<uint32_t>(sum_catalog / iterations);
  r.latency.route_lookup_cycles = static_cast<uint32_t>(sum_route / iterations);
  r.latency.action_dispatch_cycles = static_cast<uint32_t>(sum_dispatch / iterations);
  r.latency.sync_overhead_cycles = static_cast<uint32_t>(sum_sync / iterations);
  r.latency.total_pipeline_cycles = static_cast<uint32_t>(total_cycles / iterations);

  r.jitter = HistCompute(iterations, min_c, max_c, total_cycles);

  CaptureSafetyPost(r.safety);
  return r;
}

// ── Phase 3: Synchronization & Atomic Cost Profiler (500,000 runs) ───────────
BenchmarkReport RunPhase3_SyncAndAtomic(uint32_t iterations) noexcept {
  BenchmarkReport r{};
  r.phase_name = "Phase 3: Synchronization & Atomic Cost Profiler";
  r.iterations = iterations;

  CaptureSafetyPre(r.safety);
  HistReset();

  uint32_t probe_oh = (s_probe_overhead_cycles > 0) ? s_probe_overhead_cycles : MeasureSelfOverhead();

  std::atomic<uint32_t> test_atomic{0};
  portMUX_TYPE test_mux = portMUX_INITIALIZER_UNLOCKED;

  uint64_t total_hold_cycles = 0;
  uint32_t max_hold = 0;
  uint64_t total_cycles = 0;
  uint32_t min_c = UINT32_MAX, max_c = 0;

  uint32_t t_start = micros();

  for (uint32_t i = 0; i < iterations; ++i) {
    uint32_t t0 = esp_cpu_get_cycle_count();

    // 1. portENTER_CRITICAL hold
    portENTER_CRITICAL(&test_mux);
    uint32_t t_inside = esp_cpu_get_cycle_count();
    test_atomic.fetch_add(1, std::memory_order_relaxed);
    uint32_t t_exit = esp_cpu_get_cycle_count();
    portEXIT_CRITICAL(&test_mux);

    uint32_t t1 = esp_cpu_get_cycle_count();

    uint32_t hold = (t_exit >= t_inside) ? (t_exit - t_inside) : 0;
    total_hold_cycles += hold;
    if (hold > max_hold) max_hold = hold;

    uint32_t diff = (t1 >= t0) ? (t1 - t0) : 0;
    if (diff > probe_oh) diff -= probe_oh;

    total_cycles += diff;
    if (diff < min_c) min_c = diff;
    if (diff > max_c) max_c = diff;

    HistRecord(diff);

    // Non-blocking WDT Feeder & Yield (Pillar 1)
    if ((i & 0x1FFF) == 0) {
      esp_task_wdt_reset();
      taskYIELD();
    }
    if ((i & 0x7FFF) == 0) {
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }

  uint32_t t_end = micros();
  r.total_duration_us = (t_end >= t_start) ? (t_end - t_start) : 0;
  if (r.total_duration_us > 0) {
    r.throughput_pps = static_cast<uint32_t>(
        (static_cast<uint64_t>(iterations) * 1000000ULL) / r.total_duration_us);
  }

  r.sync.lock_count = iterations;
  r.sync.total_hold_cycles = total_hold_cycles;
  r.sync.max_hold_cycles = max_hold;
  r.sync.contention_count = 0;

  r.latency.sync_overhead_cycles = static_cast<uint32_t>(total_cycles / iterations);
  r.latency.total_pipeline_cycles = r.latency.sync_overhead_cycles;
  r.jitter = HistCompute(iterations, min_c, max_c, total_cycles);

  CaptureSafetyPost(r.safety);
  return r;
}

// ── 80-Column Unified Report Formatter ────────────────────────────────────────
void FormatReport(AppendBuf &out, const BenchmarkReport &r) noexcept {
  out.append("\r\n");
  out.append(CliFmt::BOX80_EQ);
  out.appendFormat("| %-76s |\r\n", r.phase_name);
  out.append(CliFmt::BOX80_EQ);

  out.appendFormat("Iterations      : %u runs in %u us (%u pkt/s)\r\n",
                   r.iterations, r.total_duration_us, r.throughput_pps);
  out.appendFormat("Probe Overhead  : %u cycles (~%.2f ns)\r\n",
                   s_probe_overhead_cycles, s_probe_overhead_cycles * 4.167f);

  out.append(CliFmt::BOX80_DASH);
  out.append("[PIPELINE LATENCY BREAKDOWN (CYCLES)]\r\n");
  out.append(CliFmt::BOX80_DASH);
  out.appendFormat("1. Stream Framing (STX/Len)  : %6u cycles (%5.2f us)\r\n",
                   r.latency.stream_framing_cycles,
                   r.latency.stream_framing_cycles / 240.0f);
  out.appendFormat("2. Packet Validate (Checksum): %6u cycles (%5.2f us)\r\n",
                   r.latency.checksum_validate_cycles,
                   r.latency.checksum_validate_cycles / 240.0f);
  out.appendFormat("3. Device Catalog Lookup     : %6u cycles (%5.2f us)\r\n",
                   r.latency.device_catalog_cycles,
                   r.latency.device_catalog_cycles / 240.0f);
  out.appendFormat("4. Routing Table Lookup      : %6u cycles (%5.2f us)\r\n",
                   r.latency.route_lookup_cycles,
                   r.latency.route_lookup_cycles / 240.0f);
  out.appendFormat("5. Action Dispatch / Build   : %6u cycles (%5.2f us)\r\n",
                   r.latency.action_dispatch_cycles,
                   r.latency.action_dispatch_cycles / 240.0f);
  out.appendFormat("6. Sync / Spinlock Overhead  : %6u cycles (%5.2f us)\r\n",
                   r.latency.sync_overhead_cycles,
                   r.latency.sync_overhead_cycles / 240.0f);
  out.append(CliFmt::BOX80_DASH);
  out.appendFormat("TOTAL Pipeline per Packet    : %6u cycles (%5.2f us)\r\n",
                   r.latency.total_pipeline_cycles,
                   r.latency.total_pipeline_cycles / 240.0f);

  out.append(CliFmt::BOX80_DASH);
  out.append("[STATISTICAL DISTRIBUTION (JITTER / PILLAR 10)]\r\n");
  out.append(CliFmt::BOX80_DASH);
  out.appendFormat("Min: %u cyc | Mean: %u cyc | Median: %u cyc | P95: %u cyc\r\n",
                   r.jitter.min_cycles, r.jitter.mean_cycles,
                   r.jitter.median_cycles, r.jitter.p95_cycles);
  out.appendFormat("P99: %u cyc | P99.9: %u cyc | Max(WCET): %u cyc (%5.2f us)\r\n",
                   r.jitter.p99_cycles, r.jitter.p99_9_cycles,
                   r.jitter.max_cycles, r.jitter.max_cycles / 240.0f);

  if (r.sync.lock_count > 0) {
    out.append(CliFmt::BOX80_DASH);
    out.append("[SYNCHRONIZATION & CONTENTION / PILLAR 6]\r\n");
    out.append(CliFmt::BOX80_DASH);
    out.appendFormat("Lock Count: %u | Contentions: %u | Max Hold: %u cyc (%5.2f us)\r\n",
                     r.sync.lock_count, r.sync.contention_count,
                     r.sync.max_hold_cycles, r.sync.max_hold_cycles / 240.0f);
  }

  out.append(CliFmt::BOX80_DASH);
  out.append("[SYSTEM HEALTH & SAFETY / PILLARS 0, 2, 5]\r\n");
  out.append(CliFmt::BOX80_DASH);
  out.appendFormat("CPU Frequency   : %u MHz [%s]\r\n",
                   r.safety.cpu_freq_mhz, r.safety.freq_valid ? "PASS" : "INVALID");
  out.appendFormat("Heap Delta      : %d Bytes [%s] (Free: %zu B)\r\n",
                   r.safety.heap_delta, r.safety.heap_valid ? "PASS 0-LEAK" : "FAIL LEAK",
                   r.safety.end_free_heap);
  out.appendFormat("Stack Headroom  : %u Bytes [%s >= 1536 B]\r\n",
                   r.safety.min_stack_headroom, r.safety.stack_valid ? "PASS" : "FAIL OVERFLOW");
  out.appendFormat("Core IDLE (Snap): Core 0: %u%% | Core 1: %u%%\r\n",
                   r.safety.core0_idle_pct, r.safety.core1_idle_pct);
  out.appendFormat("On-Chip Temp    : Start: %d C -> End: %d C\r\n",
                   r.safety.start_temp_c, r.safety.end_temp_c);

  if (r.outlier_count > 0) {
    out.append(CliFmt::BOX80_DASH);
    out.append("[TOP LATENCY OUTLIERS CAPTURED]\r\n");
    for (uint8_t i = 0; i < r.outlier_count; ++i) {
      out.appendFormat(" #%u Iter: %u | Cycles: %u (%.2f us) | Cmd: 0x%02X Sub: 0x%02X\r\n",
                       i + 1, r.top_outliers[i].iteration, r.top_outliers[i].cycles,
                       r.top_outliers[i].cycles / 240.0f,
                       r.top_outliers[i].cmd, r.top_outliers[i].sub);
    }
  }

  out.append(CliFmt::BOX80_EQ);
  out.append("\r\n");
}

} // namespace Benchmark

// ── CLI Command Implementation ───────────────────────────────────────────────
namespace BenchmarkCli {

void cmdBench(CliContext &ctx) {
  int client = ctx.sock;
  int argc = ctx.args.count();

  if (argc <= 1) {
    withScratchBuf(client, [](AppendBuf &out) {
      out.append("\r\nUsage: bench [run <0|1|2|3|all> [runs]] | [health]\r\n");
      out.append("  bench run 0     : Baseline calibration (Measure probe overhead & CPU)\r\n");
      out.append("  bench run 1 [N] : Phase 1 Primitive parser (default 1,000,000 runs)\r\n");
      out.append("  bench run 2 [N] : Phase 2 CH1 Hot-Path Packet Flow (default 100,000 runs)\r\n");
      out.append("  bench run 3 [N] : Phase 3 Sync & Atomic profiler (default 500,000 runs)\r\n");
      out.append("  bench run all   : Run Phase 0, 1, 2, 3 in sequence\r\n");
      out.append("  bench health    : Quick hardware safety & telemetry check\r\n");
    });
    return;
  }

  const char *sub = ctx.args.get(1);

  if (strcasecmp(sub, "health") == 0) {
    withScratchBuf(client, [](AppendBuf &out) {
      Benchmark::BenchmarkReport r = Benchmark::RunPhase0_BaselineCalibration();
      Benchmark::FormatReport(out, r);
    });
    return;
  }

  if (strcasecmp(sub, "run") == 0) {
    const char *phase_arg = (argc > 2) ? ctx.args.get(2) : "0";
    uint32_t custom_runs = 0;
    if (argc > 3) {
      int parsed_runs = 0;
      if (CliFmt::ParseInt(ctx.args.get(3), parsed_runs, 100, 2000000)) {
        custom_runs = static_cast<uint32_t>(parsed_runs);
      }
    }

    if (strcmp(phase_arg, "0") == 0) {
      withScratchBuf(client, [](AppendBuf &out) {
        Benchmark::BenchmarkReport r = Benchmark::RunPhase0_BaselineCalibration();
        Benchmark::FormatReport(out, r);
      });
    } else if (strcmp(phase_arg, "1") == 0) {
      uint32_t runs = (custom_runs > 0) ? custom_runs : 1000000;
      withScratchBuf(client, [runs](AppendBuf &out) {
        Benchmark::BenchmarkReport r = Benchmark::RunPhase1_PrimitiveParser(runs);
        Benchmark::FormatReport(out, r);
      });
    } else if (strcmp(phase_arg, "2") == 0) {
      uint32_t runs = (custom_runs > 0) ? custom_runs : 100000;
      withScratchBuf(client, [runs](AppendBuf &out) {
        Benchmark::BenchmarkReport r = Benchmark::RunPhase2_CH1HotPathFlow(runs);
        Benchmark::FormatReport(out, r);
      });
    } else if (strcmp(phase_arg, "3") == 0) {
      uint32_t runs = (custom_runs > 0) ? custom_runs : 500000;
      withScratchBuf(client, [runs](AppendBuf &out) {
        Benchmark::BenchmarkReport r = Benchmark::RunPhase3_SyncAndAtomic(runs);
        Benchmark::FormatReport(out, r);
      });
    } else if (strcasecmp(phase_arg, "all") == 0) {
      withScratchBuf(client, [](AppendBuf &out) {
        Benchmark::BenchmarkReport r0 = Benchmark::RunPhase0_BaselineCalibration();
        Benchmark::FormatReport(out, r0);
        Benchmark::BenchmarkReport r1 = Benchmark::RunPhase1_PrimitiveParser(100000);
        Benchmark::FormatReport(out, r1);
        Benchmark::BenchmarkReport r2 = Benchmark::RunPhase2_CH1HotPathFlow(50000);
        Benchmark::FormatReport(out, r2);
        Benchmark::BenchmarkReport r3 = Benchmark::RunPhase3_SyncAndAtomic(100000);
        Benchmark::FormatReport(out, r3);
      });
    } else {
      sendTelnetMsg(client, "[ERROR] Unknown phase. Specify 0, 1, 2, 3, or all.\r\n");
    }
    return;
  }

  sendTelnetMsg(client, "[ERROR] Unknown benchmark subcommand. Try 'bench'.\r\n");
}

} // namespace BenchmarkCli

#endif // BENCHMARK_BUILD
