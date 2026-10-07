#pragma once

// ============================================================================
// Benchmark_Harness.h — Level 4 System Diagnostics & Performance Profiling
// ESP32-S3 Production-Grade Native Cycle-Accurate Benchmark & Safety Harness
// ============================================================================
// Conforms strictly to docs/EMBEDDED_STABILITY_AND_BENCHMARK_SPECIFICATION.md
// Governed by 12 Production-Grade Pillars (P0 ~ P11).
// Completely isolated under -D BENCHMARK_BUILD=1 (0 bytes overhead in Release).
// ============================================================================

#if defined(BENCHMARK_BUILD)

#include "L0_Foundation/System_Buffer.h"
#include "L0_Foundation/System_Config.h"
#include "L0_Foundation/System_Platform.h"
#include "L4_Services/CLI_Service.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace Benchmark {

// ── Pipeline Latency Metrics (Cycles) ─────────────────────────────────────────
struct PipelineLatencyMetrics {
  uint32_t stream_framing_cycles{0};    // 1. STX/Framer
  uint32_t checksum_validate_cycles{0}; // 2. Checksum Validate
  uint32_t device_catalog_cycles{0};    // 3. Catalog & State Decode
  uint32_t route_lookup_cycles{0};      // 4. Ingress Routing Lookup
  uint32_t action_dispatch_cycles{0};   // 5. Downlink Packet Build
  uint32_t sync_overhead_cycles{0};     // 6. Lock/Atomic overhead
  uint32_t total_pipeline_cycles{0};    // Sum per packet
};

// ── Real-Time Jitter & Percentiles (Pillar 10) ────────────────────────────────
struct JitterDistribution {
  uint32_t sample_count{0};
  uint32_t mean_cycles{0};
  uint32_t median_cycles{0};
  uint32_t p95_cycles{0};
  uint32_t p99_cycles{0};
  uint32_t p99_9_cycles{0};
  uint32_t min_cycles{0};
  uint32_t max_cycles{0}; // WCET (Worst-Case Execution Time)
};

// ── Synchronization & Contention Metrics (Pillar 6) ───────────────────────────
struct SyncContentionMetrics {
  uint32_t lock_count{0};
  uint32_t contention_count{0};
  uint64_t total_hold_cycles{0};
  uint32_t max_hold_cycles{0};
};

// ── Hardware & Resource Safety Metrics (Pillar 0, 2, 5) ───────────────────────
struct SystemSafetyMetrics {
  size_t   start_free_heap{0};
  size_t   end_free_heap{0};
  int32_t  heap_delta{0};          // Must be 0
  uint32_t min_stack_headroom{0};  // Must be >= 1536 bytes
  uint32_t cpu_freq_mhz{0};        // Must be 240
  int8_t   start_temp_c{0};
  int8_t   end_temp_c{0};
  uint8_t  core0_idle_pct{0};
  uint8_t  core1_idle_pct{0};
  bool     freq_valid{true};
  bool     heap_valid{true};
  bool     stack_valid{true};
};

// ── Outlier Flight Recorder Event (Pillar 10) ─────────────────────────────────
struct OutlierEvent {
  uint32_t iteration{0};
  uint32_t cycles{0};
  uint8_t  cmd{0};
  uint8_t  sub{0};
};

// ── Consolidated Benchmark Report ─────────────────────────────────────────────
struct BenchmarkReport {
  const char* phase_name{"Unknown"};
  uint32_t iterations{0};
  uint32_t total_duration_us{0};
  uint32_t throughput_pps{0};
  PipelineLatencyMetrics latency;
  JitterDistribution jitter;
  SyncContentionMetrics sync;
  SystemSafetyMetrics safety;
  uint32_t parse_errors{0};
  uint32_t checksum_fails{0};
  uint32_t catalog_misses{0};
  std::array<OutlierEvent, 8> top_outliers{};
  uint8_t outlier_count{0};
};

// ── Core Lifecycle & Harness APIs ─────────────────────────────────────────────
void Initialize() noexcept;
uint32_t MeasureSelfOverhead() noexcept;

BenchmarkReport RunPhase0_BaselineCalibration() noexcept;
BenchmarkReport RunPhase1_PrimitiveParser(uint32_t iterations = 1'000'000) noexcept;
BenchmarkReport RunPhase2_CH1HotPathFlow(uint32_t iterations = 100'000) noexcept;
BenchmarkReport RunPhase3_SyncAndAtomic(uint32_t iterations = 500'000) noexcept;

void FormatReport(AppendBuf &out, const BenchmarkReport &report) noexcept;

} // namespace Benchmark

namespace BenchmarkCli {
void cmdBench(CliContext &ctx);
} // namespace BenchmarkCli

#endif // BENCHMARK_BUILD
