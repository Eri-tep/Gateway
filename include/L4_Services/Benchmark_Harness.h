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
#include "L3_Protocol/Public/Protocol_Device.h"
#include "L4_Services/CLI_Service.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace Benchmark {

// ── 1. Heap Integrity & Memory Safety (User Mandated Model) ───────────────────
struct HeapIntegrityMetrics {
  size_t  benchmark_alloc{0};      // Must be 0
  size_t  benchmark_free{0};       // Must be 0
  size_t  outstanding_alloc{0};    // Must be 0
  size_t  internal_alloc_count{0}; // Must be 0
  size_t  start_global_free{0};
  size_t  end_global_free{0};
  size_t  lowest_ever_free{0};
  size_t  phase_peak_pbuf_drop{0}; // Peak drop during phase
  bool    floor_valid{true};       // Lowest >= 65536
};

// ── 2. Hardware & Platform Safety Metrics ─────────────────────────────────────
struct SystemSafetyMetrics {
  HeapIntegrityMetrics heap;
  uint32_t min_stack_headroom{0};  // Must be >= 1536 bytes
  uint32_t cpu_freq_mhz{0};        // Must be 240
  int8_t   start_temp_c{0};
  int8_t   end_temp_c{0};
  uint8_t  core0_idle_pct{0};
  uint8_t  core1_idle_pct{0};
  bool     freq_valid{true};
  bool     stack_valid{true};
};

// ── 3. Real-Time Jitter & Percentiles (Pillar 10) ──────────────────────────────
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

// ── 4. Outlier Flight Recorder Event (Pillar 10) ───────────────────────────────
struct OutlierEvent {
  uint32_t iteration{0};
  uint32_t cycles{0};
  uint8_t  cmd{0};
  uint8_t  sub{0};
};

// ── Phase 0: Calibration Metrics ──────────────────────────────────────────────
struct Phase0CalibrationMetrics {
  uint32_t probe_overhead_cycles{0};
  uint32_t timer_resolution_ns{0};
  uint32_t loop_overhead_cycles{0}; // B - A (Instrumentation baseline)
  uint32_t wall_clock_vs_cycle_drift_cycles{0};
  uint32_t throughput_ops_sec{0};
};

// ── Phase 1: Primitive & Language Feature Micro A/B Metrics ───────────────────
struct Phase1PrimitiveMetrics {
  uint32_t checksum_cycles{0};        // 1-A: Universal Engine E2E API
  uint32_t checksum_direct_cycles{0}; // 1-A: Direct Inlined Branch
  uint32_t span_cycles{0};            // 1-B: std::span view
  uint32_t ptr_len_cycles{0};        // 1-B: const uint8_t*, size_t equivalent
  uint32_t expected_cycles{0};       // 1-C: std::expected return
  uint32_t bool_cycles{0};           // 1-C: bool return equivalent
  uint32_t atomic_relaxed_cycles{0}; // 1-D: relaxed atomic fetch_add
  uint32_t atomic_acq_rel_cycles{0}; // 1-D: acq_rel atomic exchange
  uint32_t atomic_seq_cst_cycles{0}; // 1-D: seq_cst atomic fetch_add
};

// ── Phase 2: CH1 Hot-Path & Catalog 8-Way Decomposition (2-A ~ 2-H) ───────────
struct Phase2CatalogDecompMetrics {
  uint32_t full_find_copy_cycles{0}; // 2-A: Full Device_FindCopy (mutex + hash + miss + copy)
  uint32_t mutex_only_cycles{0};     // 2-B: Mutex take/give only
  uint32_t pure_lookup_cycles{0};    // 2-C: Hash probe without mutex
  uint32_t hit_lookup_cycles{0};     // 2-D: Existing device hit
  uint32_t miss_lookup_cycles{0};    // 2-E: Nonexistent device miss
  uint32_t copy_snapshot_cycles{0};  // 2-F: 80B copy
  uint32_t direct_ptr_cycles{0};     // 2-G: Direct pointer deref
  uint32_t span_vs_ptr_cycles{0};    // 2-H: span vs ptr+len
  uint32_t stream_framing_cycles{0}; // Framing extraction
  uint32_t checksum_validate_cycles{0}; // Checksum validate
  uint32_t route_lookup_cycles{0};   // Ingress route lookup
  uint32_t dispatch_build_cycles{0}; // Downlink build
  uint32_t unaccounted_cycles{0};    // Wall-clock vs pipeline cycles delta
  uint32_t shadow_dedup_hit_cycles{0};   // 2-I: Warm Path Shadow State Dedup Hit (no delta)
  uint32_t shadow_dedup_delta_cycles{0}; // 2-J: Warm Path Shadow State Delta Emit
  uint32_t e2e_pure_pipeline_cycles{0};  // Pure E2E 1-probe pipeline latency (probe overhead eliminated)
};

// ── Phase 3: Synchronization & Realistic SMP Dual-Core Contention ─────────────
struct Phase3SmpSyncMetrics {
  uint32_t uncontended_lock_cycles{0};       // 3-A: Single-core spinlock
  uint32_t uncontended_atomic_cycles{0};     // 3-A: Single-core atomic
  uint32_t smp_atomic_contended_cycles{0};   // 3-B: Core0 <-> Core1 atomic contention
  uint32_t smp_spinlock_contended_cycles{0}; // 3-B: Core0 <-> Core1 spinlock contention
  uint32_t realistic_smp_cycles{0};          // 3-C: Real shared object contention
  uint32_t max_hold_cycles{0};
  uint32_t contention_count{0};
  uint32_t queue_push_pop_cycles{0};         // 3-D: Warm Path FreeRTOS Queue Push/Pop
  uint32_t ringbuf_push_pop_cycles{0};       // 3-D: Lockless RingBuffer Push/Pop
  uint32_t backpressure_drop_cycles{0};      // 3-E: Warm Path Backpressure Drop-Tail
};

// ── Phase 4: Code Generation & Memory Footprint ───────────────────────────────
struct Phase4CodegenMetrics {
  size_t iram_text_bytes{0};
  size_t flash_text_bytes{0};
  size_t flash_rodata_bytes{0};
  size_t dram_data_bytes{0};
  size_t dram_bss_bytes{0};
};

// ── Phase 5: Real Workload Replay Profiles ─────────────────────────────────────
struct Phase5WorkloadMetrics {
  uint32_t workload_a_hot_hit_cycles{0}; // Normal recurring hit packet
  uint32_t workload_b_mixed_cycles{0};   // CH1 mix: Light, Thermo, Outlet, Gas, Vent
  uint32_t workload_c_stress_cycles{0};  // Mixed with misses, malformed, routing
  float    est_ch1_cpu_pct{0.0f};        // Estimated Task_Ch1 CPU utilization at 20 pkt/s
};

// ── Consolidated Benchmark Report ─────────────────────────────────────────────
struct BenchmarkReport {
  uint8_t  phase_id{0}; // 0..5
  const char *phase_name{"Unknown"};
  uint32_t iterations{0};
  uint32_t total_duration_us{0};
  uint32_t throughput_pps{0};
  uint32_t cold_start_cycles{0};      // Pillar 3: 1st Cold-start iteration cycles
  uint32_t warm_steady_cycles{0};     // Pillar 3: Steady-state cached cycles
  int32_t  cold_warm_delta_cycles{0}; // Cache miss penalty (Cold - Warm)
  SystemSafetyMetrics safety;
  JitterDistribution jitter;
  JitterDistribution core1_isolated_jitter;
  bool has_core1_isolated{false};

  Phase0CalibrationMetrics phase0;
  Phase1PrimitiveMetrics phase1;
  Phase2CatalogDecompMetrics phase2;
  Phase3SmpSyncMetrics phase3;
  Phase4CodegenMetrics phase4;
  Phase5WorkloadMetrics phase5;

  std::array<OutlierEvent, 8> top_outliers{};
  uint8_t outlier_count{0};
};

// ── Public Harness APIs ───────────────────────────────────────────────────────
void Initialize() noexcept;
uint32_t MeasureSelfOverhead() noexcept;

BenchmarkReport RunPhase0_BaselineCalibration() noexcept;
BenchmarkReport RunPhase1_PrimitiveParser(uint32_t iterations = 50'000) noexcept;
BenchmarkReport RunPhase2_CH1HotPathFlow(uint32_t iterations = 50'000) noexcept;
BenchmarkReport RunPhase3_SyncAndAtomic(uint32_t iterations = 50'000) noexcept;
BenchmarkReport RunPhase4_CodegenDiagnostics() noexcept;
BenchmarkReport RunPhase5_RealWorkloadReplay(uint32_t iterations = 50'000) noexcept;

void FormatReport(AppendBuf &out, const BenchmarkReport &report) noexcept;

} // namespace Benchmark

namespace BenchmarkCli {
void cmdBench(CliContext &ctx);
} // namespace BenchmarkCli

#endif // BENCHMARK_BUILD
