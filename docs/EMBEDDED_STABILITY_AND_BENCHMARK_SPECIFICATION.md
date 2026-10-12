# ESP32-S3 Production Stability & Benchmark Specification

> **Version**: v2.0.0  
> **Target Hardware**: M5Stack AtomS3 Lite (ESP32-S3FN8 Dual-Core @ 240 MHz, FreeRTOS, Arduino-ESP32 Core v3.1.x / ESP-IDF v5.3.2)  
> **Standard**: C++23 (`-std=gnu++23`), Exceptions Disabled (`-fno-exceptions`)  
> **Build Isolation**: `#if defined(BENCHMARK_BUILD)` (0 bytes binary overhead in production release)

---

## 0. Objective & Core Principles

Defines the hardware-native benchmark harness and 24/7/365 production runtime stability invariants for the RS-485/TCP smart home gateway firmware.

### Core Principles
1. **On-Chip Execution**: Measurements execute directly on physical ESP32-S3 hardware. PC emulation and synthetic mockups are prohibited.
2. **Zero Measurement Contamination**: Benchmark instrumentation resides strictly behind `BENCHMARK_BUILD` compile-time guards.
3. **Decoupled Root Cause Isolation**: Core 0 vs Core 1 runtimes, FreeRTOS IDLE metrics, codegen regressions, cache stalls, and synchronization costs are profiled independently.

---

## 1. Five Primary Hypotheses for Overhead Profiling

| ID | Physical Factor | Mechanism & Inspection Target | Verification Method |
|:---:|:---|:---|:---|
| **H1** | **Codegen Regression** | ABI mismatch, failed inlining, temporary copies, bounds-checking code in `std::span` / C++23 wrappers | Pointer+length vs `std::span` assembly diff (`objdump`) and cycle profiling |
| **H2** | **Cache & Flash SPI Stalls** | `constexpr` lookup tables and template expansions in Flash `.rodata` causing cache-line misses | Flash execution cycles vs IRAM (`IRAM_ATTR`) and DRAM execution |
| **H3** | **Sync & Memory Ordering** | Redundant DMB barriers from `std::atomic` default (`seq_cst`) or `portENTER_CRITICAL` contention | Cycle profiling across `seq_cst`, `acq_rel`, and `relaxed` memory orders |
| **H4** | **ISR & Context Switching** | Prolonged interrupt disabling or FreeRTOS Queue handoff overhead between ISR and tasks | ISR execution duration, context switch counts, and interrupt mask timers |
| **H5** | **Measurement Artifacts** | Skewed FreeRTOS IDLE runtime calculations, sampling window variance, or Wi-Fi/LwIP background jitter | Independent Core 0 / Core 1 IDLE counters and harness overhead subtraction |

---

## 2. The 12 Production Stability Pillars

```
+==================================================================================================+
|                        ESP32-S3 PRODUCTION-GRADE 12 STABILITY PILLARS                           |
+==================================================================================================+
| [P0]  Measurement Integrity    : Independent Core 0/1 IDLE tracking & probe overhead subtraction |
| [P1]  Non-Blocking / WDT       : Task WDT resets every 10k iterations; Core 0/1 starvation guard |
| [P2]  Zero-Heap & Stack Guard  : 0-Byte heap drift invariant; >= 1.5 KB task stack watermark     |
| [P3]  Cycle-Accurate Timing    : esp_cpu_get_cycle_count() (4.167 ns); 1,000 warm-up runs dropped|
| [P4]  Bus-Isolated Sandbox     : Physical UART/TCP outputs muted (Null Sink); NVS writes diverted|
| [P5]  Frequency & Thermal Guard: 240 MHz lock enforcement; on-chip die temperature telemetry    |
| [P6]  Synchronization & Order  : Spinlock hold cycles bounded; 6 critical-section prohibitions   |
| [P7]  Code Generation / Binary : C++17 vs C++23 objdump diff; .text / .rodata / IRAM inspection  |
| [P8]  Cache & Memory Locality  : Flash cache-miss stall tracking; hot-path IRAM residency        |
| [P9]  Interrupt & ISR Budget   : ISR-to-Task latency budgeted; interrupt disable window <= 2.08µs|
| [P10] Real-Time Determinism    : Full percentiles (P95/P99/P99.9/WCET); hard 300 µs WCET budget  |
| [P11] Queue & Backpressure     : Drop-Head (telemetry) vs Drop-Tail (VIP) buffer saturation test |
+==================================================================================================+
```

### Pillar 0: Measurement Integrity
- **Rule 0.1 (Independent Core Profiling)**: Core 0 (System/Network: IDLE0, TcpReactor, Telnet, Wi-Fi) and Core 1 (Real-Time Communication: IDLE1, `Task_Ch1`, `Task_Ch2Ch3`, `Task_Ch4`) must be profiled as independent runtime windows.
- **Rule 0.2 (Environmental Controls)**: CPU clock (240 MHz), test window (1,000 ms), network state, and background task sets must remain constant across comparisons.
- **Rule 0.3 (Probe Overhead Calibration)**: Cycle count probe overhead (`esp_cpu_get_cycle_count()`, ~1 cycle / ~4.17 ns) is calibrated at Phase 0 and subtracted from raw metrics.

### Pillar 1: Non-Blocking / WDT / Starvation Prevention
- **Rule 1.1 (Task Watchdog Protection)**: Long benchmark runs must invoke `esp_task_wdt_reset()` or `taskYIELD()` every 10,000 iterations.
- **Rule 1.2 (IDLE Task Yielding)**: Call `vTaskDelay(pdMS_TO_TICKS(1))` every 50,000 iterations to service FreeRTOS IDLE and LwIP background threads.

### Pillar 2: Zero-Heap & Stack Watermark Invariant
- **Rule 2.1 (0-Byte Heap Drift)**: Pre- and post-benchmark heap sizes (`esp_get_free_heap_size()`) must match with **0 bytes difference**. Any heap leak triggers immediate benchmark `FAIL`.
- **Rule 2.2 (Stack Headroom)**: Minimum stack high-water mark (`uxTaskGetStackHighWaterMark()`) must remain **$\ge$ 1,536 bytes (1.5 KB)** at all times.

### Pillar 3: Cycle-Accurate Timing & Warmup Invariant
- **Rule 3.1 (Hardware Counter)**: Use hardware cycle counter `esp_cpu_get_cycle_count()` (~4.167 ns per cycle at 240 MHz). OS-level timers (`millis()`, `micros()`) are prohibited.
- **Rule 3.2 (Warm-Up Isolation)**: The first 1,000 iterations are executed as warm-up and discarded to eliminate Flash cache cold-start noise. Cold-start delta is measured separately.
- **Rule 3.3 (Dead-Code Elimination Defense)**: All benchmark parsing outputs must accumulate into an observable sink (e.g., static global accumulator) to prevent compiler optimization removal.

### Pillar 4: Bus-Isolated Sandbox Invariant
- **Rule 4.1 (Physical Bus Silence)**: Hardware UARTs, GPIO pins, and TCP sockets must output zero physical signals during benchmarks. Frames drain into an in-memory Null Sink.
- **Rule 4.2 (NVS Flash Bypass)**: Configuration updates in benchmark vectors must redirect to RAM structures, preventing Flash endurance wear and write-latency spikes.

### Pillar 5: Frequency & Thermal Guard
- **Rule 5.1 (Fixed 240 MHz Frequency)**: Dynamic Frequency Scaling (DFS) is disabled. If CPU clock deviates from 240 MHz, the benchmark run is marked `INVALID`.
- **Rule 5.2 (On-Chip Thermal Logging)**: Die temperature is sampled before and after runs via `temperature_sensor_get_celsius()` to detect thermal throttling drift.

### Pillar 6: Synchronization & Memory Order Invariant
- **Rule 6.1 (Lock Metrics Tracking)**: Measure lock acquire count, spinlock contention count, cumulative hold cycles, and peak hold cycles (`max_hold_cycles`).
- **Rule 6.2 (Six Critical-Section Prohibitions)**: Inside spinlocks or `portENTER_CRITICAL`, the following are strictly prohibited:
  1. Queue blocking (`xQueueSend`, `xQueueReceive`)
  2. Socket / Network I/O
  3. NVS Flash read / write operations
  4. Dynamic heap allocation (`malloc`, `new`)
  5. Function callbacks (deadlock risk)
  6. Variable-length unbounded packet parsing loops
- **Rule 6.3 (Memory Order Auditing)**: Compare `relaxed`, `acq_rel`, and `seq_cst` cycles on atomic paths.
- **Rule 6.4 (Single Consolidated Critical Section)**: Lock churn within a single packet lifecycle is prohibited. Verification, cache update, and buffer sync must execute in a single RAII scoped critical section.

### Pillar 7: Code Generation & Binary Integrity
- **Rule 7.1 (Section Diff)**: Inspect `.text`, `.rodata`, `.bss`, and `IRAM` usage across compiler versions.
- **Rule 7.2 (Assembly Inspection via objdump)**: Disassemble the 8 critical symbols (`Wallpad_ExtractLength`, `Wallpad_ValidatePacket`, `DeviceCatalog::find`, `Firewall`, `Protocol_Router`, `PacketBuilder`, `Queue/RingBuffer`, `CriticalSectionLocker`) to verify inlining and branch efficiency.

### Pillar 8: Cache & Memory Locality Invariant
- **Rule 8.1 (IRAM Residency)**: ISR handlers and real-time hot-path loops must reside in `IRAM_ATTR` to prevent Flash SPI cache-miss stalls.
- **Rule 8.2 (Constant Table Placement)**: Verify whether `constexpr` lookup tables reside in Flash `.rodata` or DRAM to balance memory budget against access latency.

### Pillar 9: Interrupt & ISR Budget Invariant
- **Rule 9.1 (Pipeline Latency Decomposition)**: Profile individual legs: `UART RX ISR` $\rightarrow$ `Queue/RingBuffer` $\rightarrow$ `Task_Ch1 Context Switch`.
- **Rule 9.2 (Interrupt Masking Ceiling)**: Total interrupt disabling time must not exceed **500 cycles (2.08 µs)**.

### Pillar 10: Real-Time Determinism & Jitter Invariant
- **Rule 10.1 (Statistical Percentiles)**: Benchmark reports must include Min, Mean, Median, P95, P99, P99.9, and Max (WCET).
- **Rule 10.2 (Tail Latency Clamp)**: Any spike exceeding 10,000 cycles in P99.9 triggers a jitter defect investigation.
- **Rule 10.3 (Core 1 Hard WCET Budget & Empirical Distribution)**:
  - **Hard Limit**: `Task_Ch1` packet processing WCET must never exceed **300 µs (72,000 cycles @ 240 MHz)**.
  - **Empirical Baseline**:
    - `< 16,384 cyc (< 68.2 µs)`: ~0.2%
    - `< 32,768 cyc (< 136.5 µs)`: **$\ge$ 93.8%** (Primary processing window)
    - `< 65,536 cyc (< 273.0 µs)`: ~5.9% (Complex routing/state transitions)
    - `< 131,072 cyc (< 546.1 µs)`: $\le$ 0.01% (Bus jitter threshold)
  - **Production Peak Ceiling**: Golden target ceiling is **69,822 cycles (290.92 µs)**.

### Pillar 11: Queue & Backpressure Invariant
- **Rule 11.1 (Buffer Operation Profiling)**: Measure push/pop latency across queue implementations.
- **Rule 11.2 (Backpressure Policy Verification)**:
  - Telemetry / Status packets: Drop-Head policy with drop-counter increments.
  - Control (VIP) packets: Drop-Tail rejection with synchronous error return.

---

## 3. 6-Phase Benchmark Harness Architecture

```
┌────────────────────────────────────────────────────────────────────────┐
│ Phase 0: Measurement Validation & Baseline Calibration                 │
│  - Core 0 vs Core 1 IDLE runtime baseline                              │
│  - Hardware cycle counter probe overhead calibration (1 cycle)         │
│  - Frequency lock (240 MHz), zero heap drift, stack watermark          │
└───────────────────────────────────┬────────────────────────────────────┘
                                    │
                                    ▼
┌────────────────────────────────────────────────────────────────────────┐
│ Phase 1: Primitive & Language Feature Micro A/B (50,000 runs)          │
│  - Raw Pointer+Len vs std::span (Pass-by-value vs Reference)           │
│  - std::expected return vs bool + out-param                            │
│  - std::atomic memory order (relaxed vs acq_rel vs seq_cst)            │
│  - Direct Inlined Branch vs Universal Dispatch Engine                  │
└───────────────────────────────────┬────────────────────────────────────┘
                                    │
                                    ▼
┌────────────────────────────────────────────────────────────────────────┐
│ Phase 2: CH1 Hot-Path Packet Flow Decomposition (50,000 runs)          │
│  - 2-A: DeviceCatalog::find() lookup (Seqlock vs Spinlock)             │
│  - 2-B: Fast Hex Formatter (SWAR 32-bit stores)                        │
│  - 2-C: STX/ETX stream framer & length extraction                      │
│  - 2-D: Checksum validation (Legacy loop vs SWAR Slice-by-4)           │
│  - 2-E: Full packet flow end-to-end sandbox                            │
└───────────────────────────────────┬────────────────────────────────────┘
                                    │
                                    ▼
┌────────────────────────────────────────────────────────────────────────┐
│ Phase 3: Synchronization & Inter-Task IPC Profiler (50,000 runs)       │
│  - portENTER_CRITICAL (contention, total/max hold cycles)              │
│  - FreeRTOS Queue (s_ch4_queue) vs LocklessSpscRingBuffer              │
│  - Drop-Head vs Drop-Tail backpressure load simulation                 │
└───────────────────────────────────┬────────────────────────────────────┘
                                    │
                                    ▼
┌────────────────────────────────────────────────────────────────────────┐
│ Phase 4: Code Generation, Cache & Assembly Diff                        │
│  - Assembly inspection: inlining, branches, memory load/store counts   │
│  - Section memory mapping: .text, .rodata, IRAM, DRAM                  │
│  - Cache stall analysis: Flash execution vs IRAM execution             │
└───────────────────────────────────┬────────────────────────────────────┘
                                    │
                                    ▼
┌────────────────────────────────────────────────────────────────────────┐
│ Phase 5: Soak Test & Jitter Analysis (100,000 runs)                    │
│  - Sustained traffic injection at line rate                            │
│  - Real-time percentiles (P95, P99, P99.9, WCET) & Outlier Recorder    │
│  - Core 0 / Core 1 CPU utilization, heap drift, stack high-water mark  │
└────────────────────────────────────────────────────────────────────────┘
```

---

## 4. CH1 Hot-Path Processing Specification

### 4.1 Golden Test Packet Vectors
1. **Query Frame (11 Bytes)**:
   `F7 0B 01 18 01 01 00 00 00 12 EE` (Light 1 Status Query)
2. **ACK Response Frame (11 Bytes)**:
   `F7 0B 01 18 04 01 01 00 00 16 EE` (Light 1 Power ON Ack)
3. **Control Frame (14 Bytes)**:
   `F7 0E 01 28 00 01 01 16 00 00 00 00 3B EE` (Thermostat Setpoint 22°C Command)

### 4.2 Latency Budgets (240 MHz Target)
| Pipeline Step | Target Function | Budget Cycles | Budget Time |
|---|---|:---:|:---:|
| **1. Stream Extract** | STX search / Length extraction | < 240 cyc | < 1.0 µs |
| **2. Packet Validation** | Checksum verification | < 480 cyc | < 2.0 µs |
| **3. Device Decode** | State decode / Catalog lookup | < 720 cyc | < 3.0 µs |
| **4. Route Lookup** | Channel routing table check | < 360 cyc | < 1.5 µs |
| **5. Action Dispatch** | Packet builder & dispatch | < 960 cyc | < 4.0 µs |
| **6. Synchronization** | Critical section / Atomic sync | < 360 cyc | < 1.5 µs |
| **Total Pipeline** | **RX $\rightarrow$ State Update $\rightarrow$ TX Build** | **< 3,120 cyc** | **< 13.0 µs** |

---

## 5. Compile-Time Harness Isolation

- **Production Zero-Overhead Rule**: All benchmark code and test vectors are enclosed in `#if defined(BENCHMARK_BUILD)`.
- **PlatformIO Configuration**:
  ```ini
  [env:m5stack-atoms3-benchmark]
  extends = env:m5stack-atoms3-usb
  build_flags =
      ${env:m5stack-atoms3-usb.build_flags}
      -D BENCHMARK_BUILD=1
  ```

---

## 6. Empirical Benchmark Results & Milestones

Measured on physical ESP32-S3 hardware running firmware `v2.4.7` @ 240 MHz.

### 6.1 Phase 1 & Phase 3 IPC Decomposition
| Benchmark Step | Legacy / Baseline | Modern Optimized | Improvement |
|---|:---:|:---:|:---:|
| **FreeRTOS Queue Push/Pop** | 688 cycles (2.87 µs) | — | Baseline FreeRTOS IPC |
| **`LocklessSpscRingBuffer`** | — | **34 cycles (0.14 µs)** | **20.2x speedup (-95.1%)** |
| **Direct Inlined Branch** | 40 cycles (0.17 µs) | 40 cycles (0.17 µs) | Zero-overhead branch |
| **`std::span` Parameter View** | 64 cycles (0.27 µs) | 61 cycles (0.25 µs) | Comparable to raw ptr |
| **`std::expected` Return** | 20 cycles (0.08 µs) | 17 cycles (0.07 µs) | 3 cycles delta vs out-param |
| **Atomic Memory Orders** | `seq_cst`: 44 cycles | `relaxed`: 43 cycles | Equivalent on Xtensa LX7 |

### 6.2 Phase 2 Pipeline & Catalog Decomposition
| Pipeline Step | Spinlock / Byte Loop | Modern Optimized | Improvement |
|---|:---:|:---:|:---:|
| **Catalog Lookup (`Device_FindCopy`)** | 398 cycles | **249 cycles** (Seqlock) | **-37.4% (-149 cyc)** |
| **Modbus CRC-16 (4 Bytes)** | ~140 cycles | **~35 cycles** (Slice-by-4 SWAR) | **4.0x speedup** |
| **Hex Formatter (2 Bytes)** | ~28 cycles | **~14 cycles** (SWAR 32-bit) | **2.0x speedup** |

### 6.3 Global Stability & Jitter Metrics
| Metric | Pre-Optimization Baseline | Production Target (Current) | Status |
|---|:---:|:---:|:---:|
| **Throughput** | 69,830 pkt/s | **89,768+ pkt/s** | **+28.6%** |
| **P99.9 Latency** | 6,710 cycles | **2,120 cycles** | **-68.4% jitter reduction** |
| **Worst-Case Latency (WCET)** | 139,958 cycles (583.16 µs) | **69,822 cycles (290.92 µs)** | **Pass (< 300 µs limit)** |
| **Dynamic Heap Drift** | 0 Bytes | **0 Bytes** | **Pass (Zero-Heap)** |
| **Minimum Stack Headroom** | > 3.0 KB | **> 4.8 KB** | **Pass ($\ge$ 1.5 KB)** |
| **Static RAM Footprint** | 55.6% (182.2 KB) | **48.7% (159.7 KB)** | **-22.5 KB RAM recovered** |
