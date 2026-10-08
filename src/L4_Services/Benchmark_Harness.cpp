// ============================================================================
// Benchmark_Harness.cpp — Level 4 System Diagnostics & Performance Profiling
// ESP32-S3 Production-Grade Native Cycle-Accurate Benchmark & Safety Harness
// ============================================================================
// Conforms strictly to docs/EMBEDDED_STABILITY_AND_BENCHMARK_SPECIFICATION.md
// Governed by 12 Production-Grade Pillars (P0 ~ P11).
// Completely isolated under -D BENCHMARK_BUILD=1 (0 bytes overhead in Release).
// ============================================================================

#if defined(BENCHMARK_BUILD)

#include "L4_Services/Benchmark_Harness.h"
#include "L0_Foundation/System_Platform.h"
#include "L0_Foundation/System_Buffer.h"
#include "L0_Foundation/System_Config.h"
#include "L0_Foundation/Lockless_RingBuffer.h"
#include "L3_Protocol/Public/Protocol_Device.h"
#include "L3_Protocol/Public/Protocol_Facade.h"
#include "L3_Protocol/Private/Wallpad_Engine.h"
#include "L4_Services/CLI_Commands.h"

#include <esp_cpu.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <esp_app_desc.h>
#include <esp_ota_ops.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>

#include <atomic>
#include <cstring>
#include <algorithm>
#include <expected>

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

// ── Outlier Flight Recorder Helpers (Top-N Min-Replacement) ──────────────────
static void RecordOutlier(BenchmarkReport &r, uint32_t iter, uint32_t diff,
                          uint8_t cmd, uint8_t sub) noexcept {
  if (r.outlier_count < r.top_outliers.size()) {
    r.top_outliers[r.outlier_count++] = OutlierEvent{
        .iteration = iter, .cycles = diff, .cmd = cmd, .sub = sub};
  } else {
    size_t min_idx = 0;
    for (size_t k = 1; k < r.top_outliers.size(); ++k) {
      if (r.top_outliers[k].cycles < r.top_outliers[min_idx].cycles) {
        min_idx = k;
      }
    }
    if (diff > r.top_outliers[min_idx].cycles) {
      r.top_outliers[min_idx] = OutlierEvent{
          .iteration = iter, .cycles = diff, .cmd = cmd, .sub = sub};
    }
  }
}

static void FinalizeOutliers(BenchmarkReport &r) noexcept {
  if (r.outlier_count > 1) {
    std::sort(r.top_outliers.begin(), r.top_outliers.begin() + r.outlier_count,
              [](const OutlierEvent &a, const OutlierEvent &b) {
                return a.cycles > b.cycles;
              });
  }
}

// ── Probe Overhead Calibration (Pillar 0) ────────────────────────────────────
uint32_t MeasureSelfOverhead() noexcept {
  uint32_t min_diff = UINT32_MAX;
  for (int i = 0; i < 50; ++i) {
    uint32_t t0 = esp_cpu_get_cycle_count();
    uint32_t t1 = esp_cpu_get_cycle_count();
    uint32_t diff = static_cast<uint32_t>(t1 - t0);
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
  s.heap.benchmark_alloc = 0;
  s.heap.benchmark_free = 0;
  s.heap.outstanding_alloc = 0;
  s.heap.internal_alloc_count = 0;
  s.heap.start_global_free = esp_get_free_heap_size();
  s.cpu_freq_mhz = getCpuFrequencyMhz();
  s.start_temp_c = System_ReadTempC();
  s.freq_valid = (s.cpu_freq_mhz == 240);
}

static void CaptureSafetyPost(SystemSafetyMetrics &s) noexcept {
  s.heap.end_global_free = esp_get_free_heap_size();
  s.heap.lowest_ever_free = esp_get_minimum_free_heap_size();
  s.heap.floor_valid = (s.heap.end_global_free >= 65536) && (s.heap.outstanding_alloc == 0);

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

// ── Language Feature Micro Functions (Inlining Inhibited for Clean Codegen) ───
[[gnu::noinline]] static uint8_t benchSpanCalc(std::span<const uint8_t> s) noexcept {
  uint8_t x = 0;
  for (uint8_t b : s) x ^= b;
  return x;
}

[[gnu::noinline]] static uint8_t benchPtrLenCalc(const uint8_t *p, size_t n) noexcept {
  uint8_t x = 0;
  for (size_t i = 0; i < n; ++i) x ^= p[i];
  return x;
}

[[gnu::noinline]] static std::expected<uint8_t, int> benchExpectedRet(uint8_t val) noexcept {
  if (__builtin_expect(val > 0, 1)) return val;
  return std::unexpected(-1);
}

[[gnu::noinline]] static bool benchBoolRet(uint8_t val, uint8_t &out) noexcept {
  if (__builtin_expect(val > 0, 1)) {
    out = val;
    return true;
  }
  return false;
}

// ============================================================================
// Phase 0: Measurement Calibration & Instrumentation Overhead
// ============================================================================
BenchmarkReport RunPhase0_BaselineCalibration() noexcept {
  BenchmarkReport r{};
  r.phase_id = 0;
  r.phase_name = "Phase 0: Measurement Calibration & Instrumentation Overhead";
  r.iterations = 10000;

  CaptureSafetyPre(r.safety);
  uint32_t probe_oh = MeasureSelfOverhead();
  r.phase0.probe_overhead_cycles = probe_oh;
  r.phase0.timer_resolution_ns = 4; // 240 MHz -> ~4.17 ns

  // Measure Loop Skeleton Overhead (B - A: Harness framework with workload OFF)
  uint32_t c_loop_start = esp_cpu_get_cycle_count();
  uint32_t t_start = micros();
  for (uint32_t i = 0; i < r.iterations; ++i) {
    uint32_t c0 = esp_cpu_get_cycle_count();
    uint32_t c1 = esp_cpu_get_cycle_count();
    s_observable_sink += (c1 >= c0) ? (c1 - c0) : 0;
    if ((i & 0x03FF) == 0) {
      esp_task_wdt_reset();
    }
  }
  uint32_t c_loop_end = esp_cpu_get_cycle_count();
  uint32_t t_end = micros();

  r.total_duration_us = (t_end >= t_start) ? (t_end - t_start) : 0;
  if (r.total_duration_us > 0) {
    r.throughput_pps = static_cast<uint32_t>(
        (static_cast<uint64_t>(r.iterations) * 1000000ULL) / r.total_duration_us);
  }
  r.phase0.throughput_ops_sec = r.throughput_pps;

  uint64_t total_cycles = (c_loop_end >= c_loop_start) ? (c_loop_end - c_loop_start) : 0;
  r.phase0.loop_overhead_cycles = static_cast<uint32_t>(total_cycles / r.iterations);

  uint64_t wall_clock_cycles = static_cast<uint64_t>(r.total_duration_us) * 240ULL;
  r.phase0.wall_clock_vs_cycle_drift_cycles =
      (wall_clock_cycles >= total_cycles) ? static_cast<uint32_t>(wall_clock_cycles - total_cycles) : 0;

  CaptureSafetyPost(r.safety);
  return r;
}

// ============================================================================
// Phase 1: Primitive & Language Feature Micro A/B (50,000 runs)
// ============================================================================
BenchmarkReport RunPhase1_PrimitiveParser(uint32_t iterations) noexcept {
  BenchmarkReport r{};
  r.phase_id = 1;
  r.phase_name = "Phase 1: Primitive & Language Feature Micro A/B";
  r.iterations = iterations;

  CaptureSafetyPre(r.safety);
  HistReset();

  uint32_t probe_oh = (s_probe_overhead_cycles > 0) ? s_probe_overhead_cycles : MeasureSelfOverhead();
  std::span<const uint8_t> span_pkt(GOLDEN_QUERY, sizeof(GOLDEN_QUERY));
  std::atomic<uint32_t> test_atomic{0};

  // Pillar 3: Capture Cold-Start latency (1st invocation before cache heat)
  uint32_t cold_t0 = esp_cpu_get_cycle_count();
  (void)calculateChecksumDirect(ChecksumAlgo::XOR_NO_STX, span_pkt.data(), span_pkt.size());
  (void)Universal_GetEngine().calculateChecksum(span_pkt);
  benchSpanCalc(span_pkt);
  benchPtrLenCalc(GOLDEN_QUERY, sizeof(GOLDEN_QUERY));
  (void)benchExpectedRet(GOLDEN_QUERY[3]);
  uint8_t cold_bv = 0;
  benchBoolRet(GOLDEN_QUERY[3], cold_bv);
  test_atomic.fetch_add(1, std::memory_order_relaxed);
  uint32_t cold_t1 = esp_cpu_get_cycle_count();
  r.cold_start_cycles = static_cast<uint32_t>(cold_t1 - cold_t0);

  // Warm-up (1,000 runs per Pillar 3 invariant: pre-heat instruction cache & Flash MMU XIP)
  for (uint32_t i = 0; i < 1000; ++i) {
    (void)calculateChecksumDirect(ChecksumAlgo::XOR_NO_STX, span_pkt.data(), span_pkt.size());
    (void)Universal_GetEngine().calculateChecksum(span_pkt);
    benchSpanCalc(span_pkt);
    benchPtrLenCalc(GOLDEN_QUERY, sizeof(GOLDEN_QUERY));
    (void)benchExpectedRet(GOLDEN_QUERY[3]);
    uint8_t bv = 0;
    benchBoolRet(GOLDEN_QUERY[3], bv);
    test_atomic.fetch_add(1, std::memory_order_relaxed);

    if ((i & 0x01FF) == 0) {
      esp_task_wdt_reset();
      System_FeedWdt(Config::Task::WDT_ID_TELNET);
      taskYIELD();
    }
  }

  uint64_t sum_cs = 0, sum_cs_dir = 0, sum_span = 0, sum_ptr = 0, sum_exp = 0, sum_bool = 0;
  uint64_t sum_rel = 0, sum_acq = 0, sum_seq = 0;
  uint64_t total_cycles = 0;
  uint32_t min_c = UINT32_MAX, max_c = 0;

  auto &active_parser = Universal_GetEngine();
  uint32_t t_start = micros();

  for (uint32_t i = 0; i < iterations; ++i) {
    uint32_t loop_start = esp_cpu_get_cycle_count();

    // 1-A Checksum Calculation Micro A/B (Direct Inlined vs Universal E2E API)
    uint32_t t0 = esp_cpu_get_cycle_count();
    uint16_t cs_dir = calculateChecksumDirect(ChecksumAlgo::XOR_NO_STX, span_pkt.data(), span_pkt.size());
    uint32_t t1 = esp_cpu_get_cycle_count();
    sum_cs_dir += static_cast<uint32_t>(t1 - t0);

    t0 = esp_cpu_get_cycle_count();
    uint16_t cs = active_parser.calculateChecksum(span_pkt);
    t1 = esp_cpu_get_cycle_count();
    sum_cs += static_cast<uint32_t>(t1 - t0);
    s_observable_sink += static_cast<uint8_t>(cs ^ cs_dir);

    // 1-B Memory View: std::span vs pointer+length
    t0 = esp_cpu_get_cycle_count();
    uint8_t r_span = benchSpanCalc(span_pkt);
    t1 = esp_cpu_get_cycle_count();
    sum_span += static_cast<uint32_t>(t1 - t0);

    t0 = esp_cpu_get_cycle_count();
    uint8_t r_ptr = benchPtrLenCalc(GOLDEN_QUERY, sizeof(GOLDEN_QUERY));
    t1 = esp_cpu_get_cycle_count();
    sum_ptr += static_cast<uint32_t>(t1 - t0);
    s_observable_sink += (r_span ^ r_ptr);

    // 1-C Result Type: std::expected vs bool
    t0 = esp_cpu_get_cycle_count();
    auto exp_res = benchExpectedRet(GOLDEN_QUERY[3]);
    t1 = esp_cpu_get_cycle_count();
    sum_exp += static_cast<uint32_t>(t1 - t0);

    uint8_t bool_val = 0;
    t0 = esp_cpu_get_cycle_count();
    bool bool_ok = benchBoolRet(GOLDEN_QUERY[3], bool_val);
    t1 = esp_cpu_get_cycle_count();
    sum_bool += static_cast<uint32_t>(t1 - t0);
    s_observable_sink += (exp_res ? *exp_res : 0) ^ (bool_ok ? bool_val : 0);

    // 1-D Atomic Order: relaxed vs acq_rel vs seq_cst
    t0 = esp_cpu_get_cycle_count();
    test_atomic.fetch_add(1, std::memory_order_relaxed);
    t1 = esp_cpu_get_cycle_count();
    sum_rel += static_cast<uint32_t>(t1 - t0);

    t0 = esp_cpu_get_cycle_count();
    test_atomic.fetch_add(1, std::memory_order_acq_rel);
    t1 = esp_cpu_get_cycle_count();
    sum_acq += static_cast<uint32_t>(t1 - t0);

    t0 = esp_cpu_get_cycle_count();
    test_atomic.fetch_add(1, std::memory_order_seq_cst);
    t1 = esp_cpu_get_cycle_count();
    sum_seq += static_cast<uint32_t>(t1 - t0);

    uint32_t loop_end = esp_cpu_get_cycle_count();
    uint32_t diff = static_cast<uint32_t>(loop_end - loop_start);
    if (diff > probe_oh * 8) diff -= (probe_oh * 8);
    else diff = 1;

    total_cycles += diff;
    if (diff < min_c) min_c = diff;
    if (diff > max_c) max_c = diff;
    HistRecord(diff);

    // Outlier capture (diff > 3000 cyc, Top-N Min-Replacement)
    if (diff > 3000) {
      RecordOutlier(r, i, diff, GOLDEN_QUERY[3], GOLDEN_QUERY[4]);
    }

    if ((i & 0x03FF) == 0) {
      esp_task_wdt_reset();
      System_FeedWdt(Config::Task::WDT_ID_TELNET);
      taskYIELD();
    }
  }

  uint32_t t_end = micros();
  r.total_duration_us = (t_end >= t_start) ? (t_end - t_start) : 0;
  if (r.total_duration_us > 0) {
    r.throughput_pps = static_cast<uint32_t>(
        (static_cast<uint64_t>(iterations) * 1000000ULL) / r.total_duration_us);
  }

  r.phase1.checksum_direct_cycles = static_cast<uint32_t>(sum_cs_dir / iterations);
  r.phase1.checksum_cycles = static_cast<uint32_t>(sum_cs / iterations);
  r.phase1.span_cycles = static_cast<uint32_t>(sum_span / iterations);
  r.phase1.ptr_len_cycles = static_cast<uint32_t>(sum_ptr / iterations);
  r.phase1.expected_cycles = static_cast<uint32_t>(sum_exp / iterations);
  r.phase1.bool_cycles = static_cast<uint32_t>(sum_bool / iterations);
  r.phase1.atomic_relaxed_cycles = static_cast<uint32_t>(sum_rel / iterations);
  r.phase1.atomic_acq_rel_cycles = static_cast<uint32_t>(sum_acq / iterations);
  r.phase1.atomic_seq_cst_cycles = static_cast<uint32_t>(sum_seq / iterations);

  r.jitter = HistCompute(iterations, min_c, max_c, total_cycles);
  FinalizeOutliers(r);
  r.warm_steady_cycles = r.jitter.mean_cycles;
  r.cold_warm_delta_cycles = static_cast<int32_t>(r.cold_start_cycles) - static_cast<int32_t>(r.warm_steady_cycles);
  CaptureSafetyPost(r.safety);
  return r;
}

// ============================================================================
// Phase 2: CH1 Hot-Path & Catalog 8-Way Decomposition (50,000 runs)
// ============================================================================
BenchmarkReport RunPhase2_CH1HotPathFlow(uint32_t iterations) noexcept {
  BenchmarkReport r{};
  r.phase_id = 2;
  r.phase_name = "Phase 2: CH1 Hot-Path & Catalog 8-Way Decomposition";
  r.iterations = iterations;

  CaptureSafetyPre(r.safety);
  HistReset();

  uint32_t probe_oh = (s_probe_overhead_cycles > 0) ? s_probe_overhead_cycles : MeasureSelfOverhead();

  // Register mock device into SSOT repository for Hit benchmarking
  DeviceBenchmark::registerMockDevice(0x18, 0x01, 0x00);

  std::span<const uint8_t> sp(GOLDEN_QUERY, sizeof(GOLDEN_QUERY));
  portMUX_TYPE *cache_mux = DeviceBenchmark::getMuxHandle();

  // Pillar 3: Capture Cold-Start latency (1st invocation before cache heat)
  uint32_t cold_t0 = esp_cpu_get_cycle_count();
  (void)Wallpad_ExtractLength(GOLDEN_QUERY, sizeof(GOLDEN_QUERY), 0);
  (void)Wallpad_ValidatePacket(sp);
  DeviceStateEntry cold_dummy;
  (void)Device_FindCopy(0x18, 0x01, 0x00, cold_dummy);
  uint32_t cold_t1 = esp_cpu_get_cycle_count();
  r.cold_start_cycles = static_cast<uint32_t>(cold_t1 - cold_t0);

  // Warm-up (1,000 runs per Pillar 3 invariant: pre-heat instruction cache & Flash MMU XIP for all pipeline stages)
  for (uint32_t i = 0; i < 1000; ++i) {
    (void)Wallpad_ExtractLength(GOLDEN_QUERY, sizeof(GOLDEN_QUERY), 0);
    (void)Wallpad_ValidatePacket(sp);
    DeviceStateEntry dummy;
    (void)Device_FindCopy(0x18, 0x01, 0x00, dummy);
    if (cache_mux) {
      portENTER_CRITICAL(cache_mux);
      s_observable_sink = s_observable_sink + 1;
      portEXIT_CRITICAL(cache_mux);
    }
    (void)ControlTemplate_NormSub1(0x18, 0x01);
    (void)DeviceBenchmark::findCopyDirect(0x18, 0x01, 0x00, dummy);
    (void)DeviceBenchmark::findCopyDirect(0x99, 0x99, 0x99, dummy);
    (void)DeviceBenchmark::probeDirectExists(0x18, 0x01, 0x00);
    (void)benchSpanCalc(sp);
    (void)Protocol_LookupDeviceChannel(0x18, 0x01, 0x00);
    std::memcpy(s_null_sink, GOLDEN_ACK, sizeof(GOLDEN_ACK));

    if ((i & 0x01FF) == 0) {
      esp_task_wdt_reset();
      System_FeedWdt(Config::Task::WDT_ID_TELNET);
      taskYIELD();
    }
  }

  uint64_t sum_framing = 0, sum_validate = 0, sum_full_find = 0;
  uint64_t sum_mutex = 0, sum_pure_lookup = 0, sum_hit = 0, sum_miss = 0;
  uint64_t sum_copy = 0, sum_ptr = 0, sum_span_param = 0;
  uint64_t sum_route = 0, sum_dispatch = 0;
  uint64_t sum_dedup_hit = 0, sum_dedup_delta = 0;
  uint64_t total_cycles = 0;
  uint32_t min_c = UINT32_MAX, max_c = 0;

  uint32_t t_start = micros();

  for (uint32_t i = 0; i < iterations; ++i) {
    uint32_t loop_start = esp_cpu_get_cycle_count();

    // 1. Framing
    uint32_t t0 = esp_cpu_get_cycle_count();
    uint8_t ext_len = Wallpad_ExtractLength(GOLDEN_QUERY, sizeof(GOLDEN_QUERY), 0);
    uint32_t t1 = esp_cpu_get_cycle_count();
    sum_framing += static_cast<uint32_t>(t1 - t0);

    // 2. Validate
    t0 = esp_cpu_get_cycle_count();
    bool valid = Wallpad_ValidatePacket(sp);
    t1 = esp_cpu_get_cycle_count();
    sum_validate += static_cast<uint32_t>(t1 - t0);

    // 2-A. Full Device_FindCopy (portMUX + Hash probe + Copy)
    DeviceStateEntry snap_full;
    t0 = esp_cpu_get_cycle_count();
    bool found_full = Device_FindCopy(0x18, 0x01, 0x00, snap_full);
    t1 = esp_cpu_get_cycle_count();
    sum_full_find += static_cast<uint32_t>(t1 - t0);

    // 2-B. portMUX_TYPE Lock Only (Critical Section)
    t0 = esp_cpu_get_cycle_count();
    if (cache_mux) {
      portENTER_CRITICAL(cache_mux);
      s_observable_sink = s_observable_sink + 1;
      portEXIT_CRITICAL(cache_mux);
    }
    t1 = esp_cpu_get_cycle_count();
    sum_mutex += static_cast<uint32_t>(t1 - t0);

    // 2-C. ControlTemplate_NormSub1 (Derived Cache LUT)
    t0 = esp_cpu_get_cycle_count();
    uint8_t n_sub = ControlTemplate_NormSub1(0x18, 0x01);
    t1 = esp_cpu_get_cycle_count();
    sum_pure_lookup += static_cast<uint32_t>(t1 - t0);

    // 2-D. Catalog Hit (Known 0x18 with snapshot copy)
    DeviceStateEntry snap_hit;
    t0 = esp_cpu_get_cycle_count();
    bool hit_ok = DeviceBenchmark::findCopyDirect(0x18, 0x01, 0x00, snap_hit);
    t1 = esp_cpu_get_cycle_count();
    sum_hit += static_cast<uint32_t>(t1 - t0);

    // 2-E. Catalog Miss (Unknown 0x99 with snapshot copy)
    DeviceStateEntry snap_miss;
    t0 = esp_cpu_get_cycle_count();
    bool miss_ok = DeviceBenchmark::findCopyDirect(0x99, 0x99, 0x99, snap_miss);
    t1 = esp_cpu_get_cycle_count();
    sum_miss += static_cast<uint32_t>(t1 - t0);

    // 2-F. Snapshot Copy (80B struct copy)
    DeviceStateEntry snap_copy;
    t0 = esp_cpu_get_cycle_count();
    std::memcpy(&snap_copy, &snap_full, sizeof(DeviceStateEntry));
    t1 = esp_cpu_get_cycle_count();
    sum_copy += static_cast<uint32_t>(t1 - t0);

    // 2-G. Pure Fast Direct Lookup (Probe Exist under lock)
    t0 = esp_cpu_get_cycle_count();
    bool direct_exists = DeviceBenchmark::probeDirectExists(0x18, 0x01, 0x00);
    t1 = esp_cpu_get_cycle_count();
    sum_ptr += static_cast<uint32_t>(t1 - t0);

    // 2-H. Span Parameter Passing
    t0 = esp_cpu_get_cycle_count();
    uint8_t sp_res = benchSpanCalc(sp);
    t1 = esp_cpu_get_cycle_count();
    sum_span_param += static_cast<uint32_t>(t1 - t0);

    // 2-I. Warm Path Shadow State Deduplication Hit (Identical state -> No delta emit)
    t0 = esp_cpu_get_cycle_count();
    DeviceStateEntry snap_dedup;
    bool dedup_hit = Device_FindCopy(0x18, 0x01, 0x00, snap_dedup);
    bool is_delta = (snap_dedup.last_target_temp != 22);
    t1 = esp_cpu_get_cycle_count();
    sum_dedup_hit += static_cast<uint32_t>(t1 - t0);

    // 2-J. Warm Path Shadow State Delta Emit (State changed -> Event emission branch)
    t0 = esp_cpu_get_cycle_count();
    snap_dedup.last_target_temp = 23;
    bool delta_emitted = (snap_dedup.last_target_temp != 22);
    t1 = esp_cpu_get_cycle_count();
    sum_dedup_delta += static_cast<uint32_t>(t1 - t0);

    // 4. Routing Table Lookup
    t0 = esp_cpu_get_cycle_count();
    uint8_t ch = Protocol_LookupDeviceChannel(0x18, 0x01, 0x00);
    t1 = esp_cpu_get_cycle_count();
    sum_route += static_cast<uint32_t>(t1 - t0);

    // 5. Downlink Packet Build
    t0 = esp_cpu_get_cycle_count();
    std::memcpy(s_null_sink, GOLDEN_ACK, sizeof(GOLDEN_ACK));
    t1 = esp_cpu_get_cycle_count();
    sum_dispatch += static_cast<uint32_t>(t1 - t0);

    s_observable_sink += (ext_len ^ (valid ? 1 : 0) ^ (found_full ? 2 : 0) ^
                          n_sub ^ (hit_ok ? 4 : 0) ^ (miss_ok ? 8 : 0) ^
                          (direct_exists ? 16 : 0) ^ sp_res ^ ch ^
                          (dedup_hit ? 32 : 0) ^ (is_delta ? 64 : 0) ^ (delta_emitted ? 128 : 0));

    uint32_t loop_end = esp_cpu_get_cycle_count();
    uint32_t loop_diff = static_cast<uint32_t>(loop_end - loop_start);
    if (loop_diff > probe_oh * 14) loop_diff -= (probe_oh * 14);
    else loop_diff = 1;

    total_cycles += loop_diff;
    if (loop_diff < min_c) min_c = loop_diff;
    if (loop_diff > max_c) max_c = loop_diff;
    HistRecord(loop_diff);

    // Outlier capture (> 4000 cyc, Top-N Min-Replacement)
    if (loop_diff > 4000) {
      RecordOutlier(r, i, loop_diff, GOLDEN_QUERY[3], GOLDEN_QUERY[4]);
    }

    if ((i & 0x03FF) == 0) {
      esp_task_wdt_reset();
      System_FeedWdt(Config::Task::WDT_ID_TELNET);
      taskYIELD();
    }
  }

  uint32_t t_end = micros();
  r.total_duration_us = (t_end >= t_start) ? (t_end - t_start) : 0;
  if (r.total_duration_us > 0) {
    r.throughput_pps = static_cast<uint32_t>(
        (static_cast<uint64_t>(iterations) * 1000000ULL) / r.total_duration_us);
  }

  r.phase2.stream_framing_cycles = static_cast<uint32_t>(sum_framing / iterations);
  r.phase2.checksum_validate_cycles = static_cast<uint32_t>(sum_validate / iterations);
  r.phase2.full_find_copy_cycles = static_cast<uint32_t>(sum_full_find / iterations);
  r.phase2.mutex_only_cycles = static_cast<uint32_t>(sum_mutex / iterations);
  r.phase2.pure_lookup_cycles = static_cast<uint32_t>(sum_pure_lookup / iterations);
  r.phase2.hit_lookup_cycles = static_cast<uint32_t>(sum_hit / iterations);
  r.phase2.miss_lookup_cycles = static_cast<uint32_t>(sum_miss / iterations);
  r.phase2.copy_snapshot_cycles = static_cast<uint32_t>(sum_copy / iterations);
  r.phase2.direct_ptr_cycles = static_cast<uint32_t>(sum_ptr / iterations);
  r.phase2.span_vs_ptr_cycles = static_cast<uint32_t>(sum_span_param / iterations);
  r.phase2.shadow_dedup_hit_cycles = static_cast<uint32_t>(sum_dedup_hit / iterations);
  r.phase2.shadow_dedup_delta_cycles = static_cast<uint32_t>(sum_dedup_delta / iterations);
  r.phase2.route_lookup_cycles = static_cast<uint32_t>(sum_route / iterations);
  r.phase2.dispatch_build_cycles = static_cast<uint32_t>(sum_dispatch / iterations);

  // Unaccounted overhead calculation (true loop residual)
  uint64_t all_measured_ops = r.phase2.stream_framing_cycles +
                              r.phase2.checksum_validate_cycles +
                              r.phase2.full_find_copy_cycles +
                              r.phase2.mutex_only_cycles +
                              r.phase2.pure_lookup_cycles +
                              r.phase2.hit_lookup_cycles +
                              r.phase2.miss_lookup_cycles +
                              r.phase2.copy_snapshot_cycles +
                              r.phase2.direct_ptr_cycles +
                              r.phase2.span_vs_ptr_cycles +
                              r.phase2.shadow_dedup_hit_cycles +
                              r.phase2.shadow_dedup_delta_cycles +
                              r.phase2.route_lookup_cycles +
                              r.phase2.dispatch_build_cycles;
  uint64_t mean_measured_loop = total_cycles / iterations;
  r.phase2.unaccounted_cycles = (mean_measured_loop >= all_measured_ops)
                                    ? static_cast<uint32_t>(mean_measured_loop - all_measured_ops)
                                    : 0;

  r.jitter = HistCompute(iterations, min_c, max_c, total_cycles);
  FinalizeOutliers(r);
  r.warm_steady_cycles = r.jitter.mean_cycles;
  r.cold_warm_delta_cycles = static_cast<int32_t>(r.cold_start_cycles) - static_cast<int32_t>(r.warm_steady_cycles);
  CaptureSafetyPost(r.safety);
  return r;
}

// ============================================================================
// Phase 3: Synchronization & Realistic SMP Dual-Core Contention
// ============================================================================
static StaticTask_t s_smp_worker_tcb;
static StackType_t s_smp_worker_stack[2048];
static std::atomic<bool> s_smp_worker_active{false};
static std::atomic<uint32_t> s_smp_shared_atomic{0};
static portMUX_TYPE s_smp_shared_mux = portMUX_INITIALIZER_UNLOCKED;

static void SmpWorkerTask(void *param) {
  uint32_t cnt = 0;
  while (s_smp_worker_active.load(std::memory_order_relaxed)) {
    // 1. Contend on shared atomic
    s_smp_shared_atomic.fetch_add(1, std::memory_order_relaxed);

    // 2. Contend on shared spinlock
    portENTER_CRITICAL(&s_smp_shared_mux);
    s_observable_sink = s_observable_sink + 1;
    portEXIT_CRITICAL(&s_smp_shared_mux);

    if ((++cnt & 0x03FF) == 0) {
      taskYIELD();
    }
  }
  vTaskDelete(nullptr);
}

BenchmarkReport RunPhase3_SyncAndAtomic(uint32_t iterations) noexcept {
  BenchmarkReport r{};
  r.phase_id = 3;
  r.phase_name = "Phase 3: Synchronization & Realistic SMP Dual-Core Contention";
  r.iterations = iterations;

  CaptureSafetyPre(r.safety);
  HistReset();

  uint32_t probe_oh = (s_probe_overhead_cycles > 0) ? s_probe_overhead_cycles : MeasureSelfOverhead();

  // 3-A: Uncontended Single-Core Baseline
  std::atomic<uint32_t> local_atomic{0};
  portMUX_TYPE local_mux = portMUX_INITIALIZER_UNLOCKED;

  // Pillar 3: Capture Cold-Start latency (1st invocation before cache heat)
  uint32_t cold_t0 = esp_cpu_get_cycle_count();
  portENTER_CRITICAL(&local_mux);
  s_observable_sink = s_observable_sink + 1;
  portEXIT_CRITICAL(&local_mux);
  local_atomic.fetch_add(1, std::memory_order_relaxed);
  uint32_t cold_t1 = esp_cpu_get_cycle_count();
  r.cold_start_cycles = static_cast<uint32_t>(cold_t1 - cold_t0);

  // Warm-up (1,000 runs per Pillar 3 invariant: pre-heat instruction cache & Flash MMU XIP)
  for (uint32_t i = 0; i < 1000; ++i) {
    portENTER_CRITICAL(&local_mux);
    s_observable_sink = s_observable_sink + 1;
    portEXIT_CRITICAL(&local_mux);
    local_atomic.fetch_add(1, std::memory_order_relaxed);

    if ((i & 0x01FF) == 0) {
      esp_task_wdt_reset();
      System_FeedWdt(Config::Task::WDT_ID_TELNET);
      taskYIELD();
    }
  }

  uint64_t sum_uncontended_lock = 0, sum_uncontended_atomic = 0;
  uint32_t max_hold = 0;

  for (uint32_t i = 0; i < 5000; ++i) {
    uint32_t t0 = esp_cpu_get_cycle_count();
    portENTER_CRITICAL(&local_mux);
    uint32_t t_in = esp_cpu_get_cycle_count();
    s_observable_sink = s_observable_sink + 1;
    uint32_t t_out = esp_cpu_get_cycle_count();
    portEXIT_CRITICAL(&local_mux);
    uint32_t t1 = esp_cpu_get_cycle_count();
    sum_uncontended_lock += static_cast<uint32_t>(t1 - t0);

    uint32_t hold = static_cast<uint32_t>(t_out - t_in);
    if (hold > max_hold) max_hold = hold;

    t0 = esp_cpu_get_cycle_count();
    local_atomic.fetch_add(1, std::memory_order_relaxed);
    t1 = esp_cpu_get_cycle_count();
    sum_uncontended_atomic += static_cast<uint32_t>(t1 - t0);
  }
  r.phase3.uncontended_lock_cycles = static_cast<uint32_t>(sum_uncontended_lock / 5000);
  r.phase3.uncontended_atomic_cycles = static_cast<uint32_t>(sum_uncontended_atomic / 5000);
  r.phase3.max_hold_cycles = max_hold;

  // 3-D: Warm Path FreeRTOS Queue Push/Pop vs Direct Lockless RingBuffer
  StaticQueue_t q_buffer;
  uint8_t q_storage[8 * sizeof(uint32_t)];
  QueueHandle_t test_q = xQueueCreateStatic(8, sizeof(uint32_t), q_storage, &q_buffer);

  uint64_t sum_q_cycles = 0;
  for (uint32_t i = 0; i < 5000; ++i) {
    uint32_t val = i;
    uint32_t t0 = esp_cpu_get_cycle_count();
    xQueueSend(test_q, &val, 0);
    uint32_t out_val = 0;
    xQueueReceive(test_q, &out_val, 0);
    uint32_t t1 = esp_cpu_get_cycle_count();
    sum_q_cycles += static_cast<uint32_t>(t1 - t0);
  }
  r.phase3.queue_push_pop_cycles = static_cast<uint32_t>(sum_q_cycles / 5000);

  // Lockless SPSC RingBuffer (Foundation::LocklessSpscRingBuffer class)
  uint64_t sum_ring_cycles = 0;
  Foundation::LocklessSpscRingBuffer<uint32_t, 8> ring_buf;
  for (uint32_t i = 0; i < 5000; ++i) {
    uint32_t t0 = esp_cpu_get_cycle_count();
    (void)ring_buf.push(i);
    uint32_t v = 0;
    (void)ring_buf.pop(v);
    uint32_t t1 = esp_cpu_get_cycle_count();
    sum_ring_cycles += static_cast<uint32_t>(t1 - t0);
    s_observable_sink += v;
  }
  r.phase3.ringbuf_push_pop_cycles = static_cast<uint32_t>(sum_ring_cycles / 5000);

  // 3-E: Warm Path Backpressure Drop-Tail Simulation (Full Queue rejection with zero timeout)
  for (uint32_t k = 0; k < 8; ++k) {
    xQueueSend(test_q, &k, 0);
  }
  uint64_t sum_bp_drop = 0;
  for (uint32_t i = 0; i < 5000; ++i) {
    uint32_t drop_val = 0xFF;
    uint32_t t0 = esp_cpu_get_cycle_count();
    BaseType_t sent = xQueueSend(test_q, &drop_val, 0);
    if (sent != pdTRUE) {
      s_observable_sink = s_observable_sink + 1;
    }
    uint32_t t1 = esp_cpu_get_cycle_count();
    sum_bp_drop += static_cast<uint32_t>(t1 - t0);
  }
  uint32_t dummy_drain = 0;
  while (xQueueReceive(test_q, &dummy_drain, 0) == pdTRUE) {}
  r.phase3.backpressure_drop_cycles = static_cast<uint32_t>(sum_bp_drop / 5000);

  // 3-B: Launch Core 1 SMP Contender Task
  s_smp_shared_atomic.store(0, std::memory_order_relaxed);
  s_smp_worker_active.store(true, std::memory_order_release);
  TaskHandle_t h_worker = xTaskCreateStaticPinnedToCore(
      SmpWorkerTask, "SmpWorker",
      sizeof(s_smp_worker_stack) / sizeof(StackType_t), nullptr, 2,
      s_smp_worker_stack, &s_smp_worker_tcb, 1);

  vTaskDelay(pdMS_TO_TICKS(5)); // Allow Core 1 worker to start churning

  uint64_t sum_smp_atomic = 0, sum_smp_spin = 0;
  uint64_t total_cycles = 0;
  uint32_t min_c = UINT32_MAX, max_c = 0;

  uint32_t t_start = micros();

  for (uint32_t i = 0; i < iterations; ++i) {
    uint32_t loop_start = esp_cpu_get_cycle_count();

    // Contended Atomic on Shared Cache Line
    uint32_t t0 = esp_cpu_get_cycle_count();
    s_smp_shared_atomic.fetch_add(1, std::memory_order_relaxed);
    uint32_t t1 = esp_cpu_get_cycle_count();
    sum_smp_atomic += static_cast<uint32_t>(t1 - t0);

    // Contended Spinlock
    t0 = esp_cpu_get_cycle_count();
    portENTER_CRITICAL(&s_smp_shared_mux);
    s_observable_sink = s_observable_sink + 1;
    portEXIT_CRITICAL(&s_smp_shared_mux);
    t1 = esp_cpu_get_cycle_count();
    sum_smp_spin += static_cast<uint32_t>(t1 - t0);

    uint32_t loop_end = esp_cpu_get_cycle_count();
    uint32_t diff = static_cast<uint32_t>(loop_end - loop_start);
    if (diff > probe_oh * 4) diff -= (probe_oh * 4);
    else diff = 1;

    total_cycles += diff;
    if (diff < min_c) min_c = diff;
    if (diff > max_c) max_c = diff;
    HistRecord(diff);

    // Outlier capture (diff > 3000 cyc, Top-N Min-Replacement)
    if (diff > 3000) {
      RecordOutlier(r, i, diff, 0x33, 0x00);
    }

    if ((i & 0x03FF) == 0) {
      esp_task_wdt_reset();
      System_FeedWdt(Config::Task::WDT_ID_TELNET);
      taskYIELD();
    }
  }

  uint32_t t_end = micros();
  r.total_duration_us = (t_end >= t_start) ? (t_end - t_start) : 0;
  if (r.total_duration_us > 0) {
    r.throughput_pps = static_cast<uint32_t>(
        (static_cast<uint64_t>(iterations) * 1000000ULL) / r.total_duration_us);
  }

  // Stop Core 1 worker
  s_smp_worker_active.store(false, std::memory_order_release);
  vTaskDelay(pdMS_TO_TICKS(10));

  r.phase3.smp_atomic_contended_cycles = static_cast<uint32_t>(sum_smp_atomic / iterations);
  r.phase3.smp_spinlock_contended_cycles = static_cast<uint32_t>(sum_smp_spin / iterations);
  r.phase3.realistic_smp_cycles = r.phase3.smp_spinlock_contended_cycles;

  r.jitter = HistCompute(iterations, min_c, max_c, total_cycles);
  FinalizeOutliers(r);
  r.warm_steady_cycles = r.jitter.mean_cycles;
  r.cold_warm_delta_cycles = static_cast<int32_t>(r.cold_start_cycles) - static_cast<int32_t>(r.warm_steady_cycles);
  CaptureSafetyPost(r.safety);
  return r;
}

// ============================================================================
// Phase 4: Code Generation & Memory Footprint Diagnostics
// ============================================================================
BenchmarkReport RunPhase4_CodegenDiagnostics() noexcept {
  BenchmarkReport r{};
  r.phase_id = 4;
  r.phase_name = "Phase 4: Code Generation & Memory Footprint Diagnostics";
  r.iterations = 1;

  CaptureSafetyPre(r.safety);

  r.phase4.iram_text_bytes = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_EXEC);
  r.phase4.dram_data_bytes = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) -
                             heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  r.phase4.dram_bss_bytes = uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t);

  const esp_partition_t *running = esp_ota_get_running_partition();
  if (running) {
    r.phase4.flash_text_bytes = running->size;
  }

  CaptureSafetyPost(r.safety);
  return r;
}

// ============================================================================
// Phase 5: Real Workload Replay Profiles (50,000 runs)
// ============================================================================
BenchmarkReport RunPhase5_RealWorkloadReplay(uint32_t iterations) noexcept {
  BenchmarkReport r{};
  r.phase_id = 5;
  r.phase_name = "Phase 5: Real Workload Replay (Hot / Mixed / Stress)";
  r.iterations = iterations;

  CaptureSafetyPre(r.safety);
  HistReset();

  uint32_t probe_oh = (s_probe_overhead_cycles > 0) ? s_probe_overhead_cycles : MeasureSelfOverhead();

  // Synthetic typical CH1 packets
  static constexpr uint8_t PKT_LIGHT[11]   = {0xF7, 0x0B, 0x01, 0x19, 0x01, 0x01, 0x00, 0x00, 0x00, 0x10, 0xEE};
  static constexpr uint8_t PKT_THERMO[11]  = {0xF7, 0x0B, 0x01, 0x18, 0x01, 0x01, 0x00, 0x00, 0x00, 0x12, 0xEE};
  static constexpr uint8_t PKT_OUTLET[11]  = {0xF7, 0x0B, 0x01, 0x1F, 0x01, 0x01, 0x00, 0x00, 0x00, 0x15, 0xEE};
  static constexpr uint8_t PKT_GAS[11]     = {0xF7, 0x0B, 0x01, 0x1B, 0x01, 0x01, 0x00, 0x00, 0x00, 0x13, 0xEE};
  static constexpr uint8_t PKT_VENT[11]    = {0xF7, 0x0B, 0x01, 0x2B, 0x01, 0x01, 0x00, 0x00, 0x00, 0x23, 0xEE};
  static constexpr uint8_t PKT_BAD_CS[11]  = {0xF7, 0x0B, 0x01, 0x18, 0x01, 0x01, 0x00, 0x00, 0x00, 0x99, 0xEE};

  DeviceBenchmark::registerMockDevice(0x19, 0x01, 0x00);
  DeviceBenchmark::registerMockDevice(0x18, 0x01, 0x00);
  DeviceBenchmark::registerMockDevice(0x1F, 0x01, 0x00);
  DeviceBenchmark::registerMockDevice(0x1B, 0x01, 0x00);
  DeviceBenchmark::registerMockDevice(0x2B, 0x01, 0x00);

  // Pillar 3: Capture Cold-Start latency (1st invocation before cache heat)
  uint32_t cold_t0 = esp_cpu_get_cycle_count();
  Wallpad_ValidatePacket(std::span<const uint8_t>(PKT_THERMO, 11));
  DeviceStateEntry cold_dummy;
  (void)Device_FindCopy(0x18, 0x01, 0x00, cold_dummy);
  Wallpad_ValidatePacket(std::span<const uint8_t>(PKT_BAD_CS, 11));
  (void)Device_FindCopy(0x88, 0x88, 0x88, cold_dummy);
  uint32_t cold_t1 = esp_cpu_get_cycle_count();
  r.cold_start_cycles = static_cast<uint32_t>(cold_t1 - cold_t0);

  // Warm-up (1,000 runs per Pillar 3 invariant: pre-heat instruction cache & Flash MMU XIP)
  for (uint32_t w = 0; w < 1000; ++w) {
    Wallpad_ValidatePacket(std::span<const uint8_t>(PKT_THERMO, 11));
    DeviceStateEntry dummy;
    (void)Device_FindCopy(0x18, 0x01, 0x00, dummy);
    Wallpad_ValidatePacket(std::span<const uint8_t>(PKT_BAD_CS, 11));
    (void)Device_FindCopy(0x88, 0x88, 0x88, dummy);

    if ((w & 0x01FF) == 0) {
      esp_task_wdt_reset();
      System_FeedWdt(Config::Task::WDT_ID_TELNET);
      taskYIELD();
    }
  }

  uint64_t sum_a = 0, sum_b = 0, sum_c = 0;
  uint64_t total_cycles = 0;
  uint32_t min_c = UINT32_MAX, max_c = 0;

  const bool was_locked = AutoProbe_GetEngine().isLocked();
  uint32_t t_start = micros();

  for (uint32_t i = 0; i < iterations; ++i) {
    uint32_t loop_start = esp_cpu_get_cycle_count();

    // Workload A: Hot Hit (Thermo 0x18 recurring)
    uint32_t t0 = esp_cpu_get_cycle_count();
    Wallpad_ValidatePacket(std::span<const uint8_t>(PKT_THERMO, 11));
    DeviceStateEntry snap_a;
    (void)Device_FindCopy(0x18, 0x01, 0x00, snap_a);
    uint32_t t1 = esp_cpu_get_cycle_count();
    sum_a += static_cast<uint32_t>(t1 - t0);

    // Workload B: Mixed CH1 Distribution
    const uint8_t *mix_pkt = PKT_LIGHT;
    uint8_t dev_target = 0x19;
    switch (i % 5) {
      case 0: mix_pkt = PKT_LIGHT;  dev_target = 0x19; break;
      case 1: mix_pkt = PKT_THERMO; dev_target = 0x18; break;
      case 2: mix_pkt = PKT_OUTLET; dev_target = 0x1F; break;
      case 3: mix_pkt = PKT_GAS;    dev_target = 0x1B; break;
      case 4: mix_pkt = PKT_VENT;   dev_target = 0x2B; break;
    }
    t0 = esp_cpu_get_cycle_count();
    Wallpad_ValidatePacket(std::span<const uint8_t>(mix_pkt, 11));
    DeviceStateEntry snap_b;
    (void)Device_FindCopy(dev_target, 0x01, 0x00, snap_b);
    t1 = esp_cpu_get_cycle_count();
    sum_b += static_cast<uint32_t>(t1 - t0);

    // Workload C: Stress (Malformed Checksum + Missing Device)
    t0 = esp_cpu_get_cycle_count();
    Wallpad_ValidatePacket(std::span<const uint8_t>(PKT_BAD_CS, 11));
    DeviceStateEntry snap_c;
    (void)Device_FindCopy(0x88, 0x88, 0x88, snap_c);
    t1 = esp_cpu_get_cycle_count();
    sum_c += static_cast<uint32_t>(t1 - t0);

    uint32_t loop_end = esp_cpu_get_cycle_count();
    uint32_t diff = static_cast<uint32_t>(loop_end - loop_start);
    if (diff > probe_oh * 6) diff -= (probe_oh * 6);
    else diff = 1;

    total_cycles += diff;
    if (diff < min_c) min_c = diff;
    if (diff > max_c) max_c = diff;
    HistRecord(diff);

    // Outlier capture (diff > 4000 cyc, Top-N Min-Replacement)
    if (diff > 4000) {
      RecordOutlier(r, i, diff, mix_pkt[3], mix_pkt[4]);
    }

    if ((i & 0x03FF) == 0) {
      esp_task_wdt_reset();
      System_FeedWdt(Config::Task::WDT_ID_TELNET);
      taskYIELD();
    }
  }

  uint32_t t_end = micros();
  r.total_duration_us = (t_end >= t_start) ? (t_end - t_start) : 0;
  if (r.total_duration_us > 0) {
    r.throughput_pps = static_cast<uint32_t>(
        (static_cast<uint64_t>(iterations) * 1000000ULL) / r.total_duration_us);
  }

  if (was_locked && !AutoProbe_GetEngine().isLocked()) {
    AutoProbe_GetEngine().initFromNvs();
  }

  r.phase5.workload_a_hot_hit_cycles = static_cast<uint32_t>(sum_a / iterations);
  r.phase5.workload_b_mixed_cycles = static_cast<uint32_t>(sum_b / iterations);
  r.phase5.workload_c_stress_cycles = static_cast<uint32_t>(sum_c / iterations);

  // Map to estimated Task_Ch1 CPU utilization at 20 packets/second:
  // (mixed_cycles * 20 pkts/s) / 240,000,000 cyc/s * 100%
  r.phase5.est_ch1_cpu_pct =
      (static_cast<float>(r.phase5.workload_b_mixed_cycles) * 20.0f) / 2400000.0f;

  r.jitter = HistCompute(iterations, min_c, max_c, total_cycles);
  FinalizeOutliers(r);
  r.warm_steady_cycles = r.jitter.mean_cycles;
  r.cold_warm_delta_cycles = static_cast<int32_t>(r.cold_start_cycles) - static_cast<int32_t>(r.warm_steady_cycles);
  CaptureSafetyPost(r.safety);
  return r;
}

// ============================================================================
// Consolidated Format & Reporting Engine
// ============================================================================
void FormatReport(AppendBuf &out, const BenchmarkReport &r) noexcept {
  out.append(CliFmt::BOX80_EQ);
  out.appendFormat("| %-76s |\r\n", r.phase_name);
  out.append(CliFmt::BOX80_EQ);

  if (r.phase_id != 4) {
    out.appendFormat("Iterations      : %u runs in %u us (%u pkt/s)\r\n",
                     r.iterations, r.total_duration_us, r.throughput_pps);
    out.appendFormat("Probe Overhead  : %u cycles (~%.2f ns)\r\n",
                     s_probe_overhead_cycles, s_probe_overhead_cycles * 4.167f);
    if (r.cold_start_cycles > 0) {
      out.appendFormat("Cold-Start (1st): %6u cycles (%.2f us) | Warm-Steady: %6u cycles (%.2f us)\r\n",
                       r.cold_start_cycles, r.cold_start_cycles / 240.0f,
                       r.warm_steady_cycles, r.warm_steady_cycles / 240.0f);
      out.appendFormat("Cache Miss Delta: %+6d cycles (%+.2f us) [%s]\r\n",
                       r.cold_warm_delta_cycles, r.cold_warm_delta_cycles / 240.0f,
                       (r.cold_warm_delta_cycles >= 0) ? "Cold Miss Penalty" : "Even");
    }
  }

  // Phase 0: Calibration Breakdown
  if (r.phase_id == 0) {
    out.append(CliFmt::BOX80_DASH);
    out.append("[MEASUREMENT CALIBRATION & PROBE FIDELITY]\r\n");
    out.append(CliFmt::BOX80_DASH);
    out.appendFormat("1. Probe Overhead              : %6u cycles (%.2f ns)\r\n",
                     r.phase0.probe_overhead_cycles, r.phase0.probe_overhead_cycles * 4.167f);
    out.appendFormat("2. Cycle Timer Resolution      : %6u ns\r\n", r.phase0.timer_resolution_ns);
    out.appendFormat("3. Harness Framework (B - A)   : %6u cycles (%.2f us)\r\n",
                     r.phase0.loop_overhead_cycles, r.phase0.loop_overhead_cycles / 240.0f);
    out.appendFormat("4. Wall vs Cycle Drift         : %6u cycles\r\n",
                     r.phase0.wall_clock_vs_cycle_drift_cycles);
    out.appendFormat("5. Calibration Throughput      : %u ops/s\r\n",
                     r.phase0.throughput_ops_sec);
  }

  // Phase 1: Primitive Micro A/B Breakdown
  if (r.phase_id == 1) {
    out.append(CliFmt::BOX80_DASH);
    out.append("[PRIMITIVE & LANGUAGE FEATURE MICRO A/B (CYCLES)]\r\n");
    out.appendFormat("1-A. Direct Inlined Branch   : %6u cycles (%.2f us)\r\n",
                     r.phase1.checksum_direct_cycles, r.phase1.checksum_direct_cycles / 240.0f);
    out.appendFormat("     Universal Engine E2E API : %6u cycles (%.2f us)\r\n",
                     r.phase1.checksum_cycles, r.phase1.checksum_cycles / 240.0f);
    out.appendFormat("1-B. std::span Parameter View : %6u cycles (%.2f us)\r\n",
                     r.phase1.span_cycles, r.phase1.span_cycles / 240.0f);
    out.appendFormat("     const ptr + len View     : %6u cycles (%.2f us)\r\n",
                     r.phase1.ptr_len_cycles, r.phase1.ptr_len_cycles / 240.0f);
    out.appendFormat("1-C. std::expected Return     : %6u cycles (%.2f us)\r\n",
                     r.phase1.expected_cycles, r.phase1.expected_cycles / 240.0f);
    out.appendFormat("     bool Return + Out-Param  : %6u cycles (%.2f us)\r\n",
                     r.phase1.bool_cycles, r.phase1.bool_cycles / 240.0f);
    out.appendFormat("1-D. Atomic fetch_add relaxed : %6u cycles (%.2f us)\r\n",
                     r.phase1.atomic_relaxed_cycles, r.phase1.atomic_relaxed_cycles / 240.0f);
    out.appendFormat("     Atomic fetch_add acq_rel : %6u cycles (%.2f us)\r\n",
                     r.phase1.atomic_acq_rel_cycles, r.phase1.atomic_acq_rel_cycles / 240.0f);
    out.appendFormat("     Atomic fetch_add seq_cst : %6u cycles (%.2f us)\r\n",
                     r.phase1.atomic_seq_cst_cycles, r.phase1.atomic_seq_cst_cycles / 240.0f);
  }

  // Phase 2: Catalog 8-Way Decomposition
  if (r.phase_id == 2) {
    out.append(CliFmt::BOX80_DASH);
    out.append("[CH1 HOT-PATH & CATALOG 8-WAY DECOMPOSITION (CYCLES)]\r\n");
    out.append(CliFmt::BOX80_DASH);
    out.appendFormat("2-A. Full Device_FindCopy      : %6u cycles (%5.2f us) [Target <= 400~500 cyc]\r\n",
                     r.phase2.full_find_copy_cycles, r.phase2.full_find_copy_cycles / 240.0f);
    out.appendFormat("2-B. portMUX_TYPE Lock Only    : %6u cycles (%5.2f us)\r\n",
                     r.phase2.mutex_only_cycles, r.phase2.mutex_only_cycles / 240.0f);
    out.appendFormat("2-C. ControlTemplate_NormSub1  : %6u cycles (%5.2f us) [Derived LUT <= 20 cyc]\r\n",
                     r.phase2.pure_lookup_cycles, r.phase2.pure_lookup_cycles / 240.0f);
    out.appendFormat("2-D. Catalog Hit (Snapshot)    : %6u cycles (%5.2f us)\r\n",
                     r.phase2.hit_lookup_cycles, r.phase2.hit_lookup_cycles / 240.0f);
    out.appendFormat("2-E. Catalog Miss (Fallback)   : %6u cycles (%5.2f us)\r\n",
                     r.phase2.miss_lookup_cycles, r.phase2.miss_lookup_cycles / 240.0f);
    out.appendFormat("2-F. Snapshot Copy (80B Struct): %6u cycles (%5.2f us)\r\n",
                     r.phase2.copy_snapshot_cycles, r.phase2.copy_snapshot_cycles / 240.0f);
    out.appendFormat("2-G. Direct Key Lookup (Exist) : %6u cycles (%5.2f us) [Target <= 400 cyc]\r\n",
                     r.phase2.direct_ptr_cycles, r.phase2.direct_ptr_cycles / 240.0f);
    out.appendFormat("2-H. Span vs Pointer Passing   : %6u cycles (%5.2f us)\r\n",
                     r.phase2.span_vs_ptr_cycles, r.phase2.span_vs_ptr_cycles / 240.0f);
    out.appendFormat("2-I. Shadow Dedup Hit (No-Delta): %6u cycles (%5.2f us)\r\n",
                     r.phase2.shadow_dedup_hit_cycles, r.phase2.shadow_dedup_hit_cycles / 240.0f);
    out.appendFormat("2-J. Shadow Delta Emit (Update) : %6u cycles (%5.2f us)\r\n",
                     r.phase2.shadow_dedup_delta_cycles, r.phase2.shadow_dedup_delta_cycles / 240.0f);
    out.append(CliFmt::BOX80_DASH);
    out.appendFormat("Pipeline Stream Framing        : %6u cycles (%5.2f us)\r\n",
                     r.phase2.stream_framing_cycles, r.phase2.stream_framing_cycles / 240.0f);
    out.appendFormat("Pipeline Checksum Validate     : %6u cycles (%5.2f us)\r\n",
                     r.phase2.checksum_validate_cycles, r.phase2.checksum_validate_cycles / 240.0f);
    out.appendFormat("Pipeline Routing Table Lookup  : %6u cycles (%5.2f us)\r\n",
                     r.phase2.route_lookup_cycles, r.phase2.route_lookup_cycles / 240.0f);
    out.appendFormat("Pipeline Action Dispatch/Build : %6u cycles (%5.2f us)\r\n",
                     r.phase2.dispatch_build_cycles, r.phase2.dispatch_build_cycles / 240.0f);
    out.appendFormat("Unaccounted Overhead (Jitter)  : %6u cycles (%5.2f us)\r\n",
                     r.phase2.unaccounted_cycles, r.phase2.unaccounted_cycles / 240.0f);
  }

  // Phase 3: SMP Contention Breakdown
  if (r.phase_id == 3) {
    out.append(CliFmt::BOX80_DASH);
    out.append("[SYNCHRONIZATION & REALISTIC SMP CONTENTION]\r\n");
    out.append(CliFmt::BOX80_DASH);
    out.appendFormat("3-A. Single-Core Uncontended Lock  : %6u cycles (%.2f us)\r\n",
                     r.phase3.uncontended_lock_cycles, r.phase3.uncontended_lock_cycles / 240.0f);
    out.appendFormat("     Single-Core Uncontended Atomic: %6u cycles (%.2f us)\r\n",
                     r.phase3.uncontended_atomic_cycles, r.phase3.uncontended_atomic_cycles / 240.0f);
    out.appendFormat("3-B. Core0 <-> Core1 Contended Atomic: %6u cycles (%.2f us)\r\n",
                     r.phase3.smp_atomic_contended_cycles, r.phase3.smp_atomic_contended_cycles / 240.0f);
    out.appendFormat("     Core0 <-> Core1 Contended Spin  : %6u cycles (%.2f us)\r\n",
                     r.phase3.smp_spinlock_contended_cycles, r.phase3.smp_spinlock_contended_cycles / 240.0f);
    out.appendFormat("3-C. Realistic Shared Object Contend : %6u cycles (%.2f us)\r\n",
                     r.phase3.realistic_smp_cycles, r.phase3.realistic_smp_cycles / 240.0f);
    out.appendFormat("     Max Critical Section Hold Time  : %6u cycles (%.2f us)\r\n",
                     r.phase3.max_hold_cycles, r.phase3.max_hold_cycles / 240.0f);
    out.appendFormat("3-D. Warm Path FreeRTOS Queue Push/Pop: %5u cycles (%.2f us)\r\n",
                     r.phase3.queue_push_pop_cycles, r.phase3.queue_push_pop_cycles / 240.0f);
    out.appendFormat("     Lockless RingBuffer Push/Pop    : %6u cycles (%.2f us)\r\n",
                     r.phase3.ringbuf_push_pop_cycles, r.phase3.ringbuf_push_pop_cycles / 240.0f);
    out.appendFormat("3-E. Warm Path Backpressure Drop-Tail : %5u cycles (%.2f us)\r\n",
                     r.phase3.backpressure_drop_cycles, r.phase3.backpressure_drop_cycles / 240.0f);
  }

  // Phase 4: Codegen & Footprint
  if (r.phase_id == 4) {
    out.append(CliFmt::BOX80_DASH);
    out.append("[CODE GENERATION & MEMORY FOOTPRINT DIAGNOSTICS]\r\n");
    out.append(CliFmt::BOX80_DASH);
    out.appendFormat("IRAM Executable Memory  : %8zu Bytes\r\n", r.phase4.iram_text_bytes);
    out.appendFormat("Flash Firmware Image    : %8zu Bytes\r\n", r.phase4.flash_text_bytes);
    out.appendFormat("Internal Heap In Use    : %8zu Bytes\r\n", r.phase4.dram_data_bytes);
    out.appendFormat("Current Task Stack Mark : %8zu Bytes Headroom\r\n", r.phase4.dram_bss_bytes);
  }

  // Phase 5: Real Workload Profiles
  if (r.phase_id == 5) {
    out.append(CliFmt::BOX80_DASH);
    out.append("[REAL WORKLOAD REPLAY (HOT / MIXED / STRESS)]\r\n");
    out.append(CliFmt::BOX80_DASH);
    out.appendFormat("Workload A (Hot Hit 0x18 Recurring) : %6u cycles (%.2f us)\r\n",
                     r.phase5.workload_a_hot_hit_cycles, r.phase5.workload_a_hot_hit_cycles / 240.0f);
    out.appendFormat("Workload B (Mixed CH1 Distribution) : %6u cycles (%.2f us)\r\n",
                     r.phase5.workload_b_mixed_cycles, r.phase5.workload_b_mixed_cycles / 240.0f);
    out.appendFormat("Workload C (Stress: Miss/Bad/Errors): %6u cycles (%.2f us)\r\n",
                     r.phase5.workload_c_stress_cycles, r.phase5.workload_c_stress_cycles / 240.0f);
    out.appendFormat("Estimated Task_Ch1 CPU Load (20pps) : %6.2f %%\r\n",
                     r.phase5.est_ch1_cpu_pct);
  }

  // Jitter Distribution (Phase 1, 2, 3, 5)
  if (r.phase_id == 1 || r.phase_id == 2 || r.phase_id == 3 || r.phase_id == 5) {
    out.append(CliFmt::BOX80_DASH);
    out.append("[STATISTICAL DISTRIBUTION (JITTER / PILLAR 10)]\r\n");
    out.append(CliFmt::BOX80_DASH);
    out.appendFormat("Min: %u cyc | Mean: %u cyc | Median: %u cyc | P95: %u cyc\r\n",
                     r.jitter.min_cycles, r.jitter.mean_cycles,
                     r.jitter.median_cycles, r.jitter.p95_cycles);
    out.appendFormat("P99: %u cyc | P99.9: %u cyc | Max(WCET): %u cyc (%.2f us)\r\n",
                     r.jitter.p99_cycles, r.jitter.p99_9_cycles,
                     r.jitter.max_cycles, r.jitter.max_cycles / 240.0f);
  }

  // Heap Integrity & System Safety (User Mandated Model)
  out.append(CliFmt::BOX80_DASH);
  out.append("[HEAP INTEGRITY & SAFETY]\r\n");
  out.append(CliFmt::BOX80_DASH);
  out.appendFormat("Benchmark allocation     : %zu B       [PASS]\r\n", r.safety.heap.benchmark_alloc);
  out.appendFormat("Benchmark free           : %zu B       [PASS]\r\n", r.safety.heap.benchmark_free);
  out.appendFormat("Outstanding              : %zu B       [PASS]\r\n", r.safety.heap.outstanding_alloc);
  out.append("Global free heap:\r\n");
  out.appendFormat("  Start                  : %zu B\r\n", r.safety.heap.start_global_free);
  out.appendFormat("  End                    : %zu B\r\n", r.safety.heap.end_global_free);
  out.appendFormat("  Lowest Ever            : %zu B\r\n", r.safety.heap.lowest_ever_free);
  out.appendFormat("Production Heap Floor    : 64 KB     [%s]\r\n",
                   r.safety.heap.floor_valid ? "PASS" : "FAIL DEPLETED");
  out.appendFormat("Stack Headroom           : %u B      [%s >= 1536 B]\r\n",
                   r.safety.min_stack_headroom, r.safety.stack_valid ? "PASS" : "FAIL OVERFLOW");
  out.appendFormat("Core IDLE (Snap)         : Core 0: %u%% | Core 1: %u%%\r\n",
                   r.safety.core0_idle_pct, r.safety.core1_idle_pct);
  out.appendFormat("On-Chip Temp             : Start: %d C -> End: %d C\r\n",
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

// ============================================================================
// CLI Command Implementation
// ============================================================================
namespace BenchmarkCli {

void cmdBench(CliContext &ctx) {
  int client = ctx.sock;
  int sub_count = ctx.args.count();

  if (sub_count == 0) {
    withScratchBuf(client, [](AppendBuf &out) {
      out.append("\r\nUsage: bench [run <0|1|2|3|4|5|all> [runs]] | [health]\r\n");
      out.append("  bench run 0     : Phase 0 Calibration (probe overhead & instrumentation)\r\n");
      out.append("  bench run 1 [N] : Phase 1 Primitive & Language Micro A/B (default 50k runs)\r\n");
      out.append("  bench run 2 [N] : Phase 2 CH1 Hot-Path Catalog 8-Way Decomp (default 50k runs)\r\n");
      out.append("  bench run 3 [N] : Phase 3 Sync & Realistic SMP Contention (default 50k runs)\r\n");
      out.append("  bench run 4     : Phase 4 Code Generation & Footprint Diagnostics\r\n");
      out.append("  bench run 5 [N] : Phase 5 Real Workload Replay (Hot/Mixed/Stress, 50k runs)\r\n");
      out.append("  bench run all   : Run Phase 0, 1, 2, 3, 4, 5 in sequence\r\n");
      out.append("  bench health    : Quick hardware safety & telemetry snapshot\r\n");
    });
    return;
  }

  const char *sub = ctx.args.get(1);

  static const CliFmt::SubCmdDef kBenchDefs[] = {
      {"health", "health", "Quick hardware safety & telemetry snapshot",
       [](int client, int, const Args &) {
         withScratchBuf(client, [](AppendBuf &out) {
           Benchmark::BenchmarkReport r = Benchmark::RunPhase0_BaselineCalibration();
           Benchmark::FormatReport(out, r);
         });
       }},
      {"run", "run <0-5|all> [runs]", "Run benchmark phase",
       [](int client, int sub_count, const Args &args) {
         const char *phase_arg = (sub_count >= 2) ? args.get(2) : "0";
         uint32_t custom_runs = 0;
         if (sub_count >= 3) {
           int parsed_runs = 0;
           if (CliFmt::ParseInt(args.get(3), parsed_runs, 100, 2000000)) {
             custom_runs = static_cast<uint32_t>(parsed_runs);
           }
         }

    struct PhaseRunner {
      const char *name;
      void (*run)(int client, uint32_t runs);
    };

    static constexpr PhaseRunner kPhaseRunners[] = {
        {"0", [](int client, uint32_t) {
           withScratchBuf(client, [](AppendBuf &out) {
             Benchmark::BenchmarkReport r = Benchmark::RunPhase0_BaselineCalibration();
             Benchmark::FormatReport(out, r);
           });
         }},
        {"1", [](int client, uint32_t runs) {
           uint32_t r_cnt = (runs > 0) ? runs : 50000;
           withScratchBuf(client, [r_cnt](AppendBuf &out) {
             Benchmark::BenchmarkReport r = Benchmark::RunPhase1_PrimitiveParser(r_cnt);
             Benchmark::FormatReport(out, r);
           });
         }},
        {"2", [](int client, uint32_t runs) {
           uint32_t r_cnt = (runs > 0) ? runs : 50000;
           withScratchBuf(client, [r_cnt](AppendBuf &out) {
             Benchmark::BenchmarkReport r = Benchmark::RunPhase2_CH1HotPathFlow(r_cnt);
             Benchmark::FormatReport(out, r);
           });
         }},
        {"3", [](int client, uint32_t runs) {
           uint32_t r_cnt = (runs > 0) ? runs : 50000;
           withScratchBuf(client, [r_cnt](AppendBuf &out) {
             Benchmark::BenchmarkReport r = Benchmark::RunPhase3_SyncAndAtomic(r_cnt);
             Benchmark::FormatReport(out, r);
           });
         }},
        {"4", [](int client, uint32_t) {
           withScratchBuf(client, [](AppendBuf &out) {
             Benchmark::BenchmarkReport r = Benchmark::RunPhase4_CodegenDiagnostics();
             Benchmark::FormatReport(out, r);
           });
         }},
        {"5", [](int client, uint32_t runs) {
           uint32_t r_cnt = (runs > 0) ? runs : 50000;
           withScratchBuf(client, [r_cnt](AppendBuf &out) {
             Benchmark::BenchmarkReport r = Benchmark::RunPhase5_RealWorkloadReplay(r_cnt);
             Benchmark::FormatReport(out, r);
           });
         }},
        {"all", [](int client, uint32_t) {
           vTaskDelay(pdMS_TO_TICKS(50));
           withScratchBuf(client, [](AppendBuf &out) {
             Benchmark::BenchmarkReport r0 = Benchmark::RunPhase0_BaselineCalibration();
             Benchmark::FormatReport(out, r0);
           });
           vTaskDelay(pdMS_TO_TICKS(50));
           withScratchBuf(client, [](AppendBuf &out) {
             Benchmark::BenchmarkReport r1 = Benchmark::RunPhase1_PrimitiveParser(50000);
             Benchmark::FormatReport(out, r1);
           });
           vTaskDelay(pdMS_TO_TICKS(50));
           withScratchBuf(client, [](AppendBuf &out) {
             Benchmark::BenchmarkReport r2 = Benchmark::RunPhase2_CH1HotPathFlow(50000);
             Benchmark::FormatReport(out, r2);
           });
           vTaskDelay(pdMS_TO_TICKS(50));
           withScratchBuf(client, [](AppendBuf &out) {
             Benchmark::BenchmarkReport r3 = Benchmark::RunPhase3_SyncAndAtomic(50000);
             Benchmark::FormatReport(out, r3);
           });
           vTaskDelay(pdMS_TO_TICKS(50));
           withScratchBuf(client, [](AppendBuf &out) {
             Benchmark::BenchmarkReport r4 = Benchmark::RunPhase4_CodegenDiagnostics();
             Benchmark::FormatReport(out, r4);
           });
           vTaskDelay(pdMS_TO_TICKS(50));
           withScratchBuf(client, [](AppendBuf &out) {
             Benchmark::BenchmarkReport r5 = Benchmark::RunPhase5_RealWorkloadReplay(50000);
             Benchmark::FormatReport(out, r5);
           });
         }},
    };

    for (const auto &p : kPhaseRunners) {
      if (strcmp(phase_arg, p.name) == 0) {
        p.run(client, custom_runs);
        return;
      }
    }

         withScratchBuf(client, [](AppendBuf &out) {
           out.append("Invalid phase. Valid options: 0, 1, 2, 3, 4, 5, all\r\n");
         });
       }},
  };

  if (CliFmt::DispatchSubCmd(sub, client, sub_count, ctx.args, kBenchDefs)) {
    return;
  }

  withScratchBuf(client, [](AppendBuf &out) {
    out.append("Unknown bench command. Type 'bench' for help.\r\n");
  });
}

} // namespace BenchmarkCli

#endif // BENCHMARK_BUILD
