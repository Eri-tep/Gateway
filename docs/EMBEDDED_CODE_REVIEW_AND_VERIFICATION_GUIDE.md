# Embedded Systems Code Review, Architectural Decision & Verification Standard

---

## 1. Overview & Core Philosophy

Embedded communication gateways and IoT edge systems operating mission-critical 24/7/365 services face physical constraints fundamentally different from desktop or cloud software. CPU clock cycles, RAM, Flash ROM, and battery/RTC-backed retention memory are strictly limited. A single crash, unexpected panic, or a continuous leak of just a few bytes per hour will eventually brick hardware or trigger critical system lockups in field deployments.

This document formalizes a universal, domain-agnostic standard for code review, architectural decision-making, and phased verification for embedded and real-time firmware engineers and autonomous AI agents.

### 🛡️ Three Invariant Laws
1. **Zero Dynamic Allocation in Hot Paths**:
   No `malloc`, `free`, `new`, `delete`, or dynamic string structures (`std::string`, Arduino `String`) are permitted in continuous RX/TX polling, packet framing, parsing, or dispatch loops. Use stack allocations, pre-allocated static/slot buffers, and compile-time fixed spans.
2. **Defensive Boundaries & Invariant Contracts**:
   All data ingested from external physical buses (RS-485, CAN, UART, SPI, I2C) or network sockets (TCP, UDP) must be treated as tainted. No buffer indexing, array subscripting, or pointer arithmetic is allowed prior to explicit length, type, and boundary validation.
3. **Atomic Phased Verification**:
   Refactoring and optimizations must never be applied in unsegmented massive changes. All modifications must proceed through sequential gated milestones [Critical Bugs → Hot-Path & RAM Recovery → Low-Level Memory & Hardening → External Contract Alignment], verified with byte-level delta tracking and 0-error/0-warning compiler compliance.

---

## 2. The 8-Domain Embedded Audit Framework

Every code review must systematically audit the codebase across eight independent engineering domains:

```
┌────────────────────────────────────────────────────────────────────────┐
│                   8-Domain Embedded Audit Framework                    │
├───────────────────────────────────┬────────────────────────────────────┤
│ 1. Runtime Defensiveness & Bounds │ 5. System Resilience & Watchdogs   │
│ 2. Multi-Tier Memory & Leak Audit │ 6. Dead Code & Legacy Buffers      │
│ 3. Hot-Path & Latency Audit       │ 7. Trust Boundaries & Sanitization │
│ 4. Concurrency & Lock Hierarchy   │ 8. Client-Server Contract Harmony  │
└───────────────────────────────────┴────────────────────────────────────┘
```

---

### Domain 1: Runtime Defensiveness & Boundary Safety

* **Buffer Overflow & Sliding Window Overrun Protection**:
  * When guarding stream accumulators (`if (s->len + len > sizeof(s->buffer))`), never rely solely on resetting the accumulator (`s->len = 0`). If an oversized frame where `len > sizeof(s->buffer)` arrives, a subsequent copy will still overflow.
  * Always clamp ingestion length using `std::min(len, sizeof(dest) - current_len)` and drop malformed frames before copying.
* **Parser Delimiter & Token Boundaries**:
  * String and token parsers (JSON, CSV, HTTP, CLI) must never scan past buffer limits looking for terminating delimiters (`:`, `"`, `,`, `\r\n`). If a starting delimiter is absent or broken, immediately reject the token instead of scanning into uninitialized memory.
* **Signedness & Type Truncation**:
  * Disallow implicit conversions between signed types (`int`, `int8_t`, `long`) and unsigned sizes (`size_t`, `uint8_t`, `uint16_t`). Negative values converted to unsigned indices cause immediate memory corruption. Enforce strict range checks before `static_cast`.

---

### Domain 2: Multi-Tier Memory Architecture & Leak Audit

Embedded platforms utilize distinct memory tiers. Each tier has strict sizing, lifetime, and access patterns:

```
[Stack] ────── Prevents Task Stack Overflow (Eliminates Large Local Buffers)
[Heap] ─────── Prevents Heap Fragmentation & FreeRTOS Task Deletion RAII Traps
[BSS/Data] ─── Minimizes Static Footprint & Eliminates Inter-Session Pollution
[Retention] ── Minimizes RTC Fast SRAM / Battery-Backed RAM Footprint
[Flash] ────── Optimizes NVS Footprint & Respects Flash Sector Alignment
```

1. **Stack Memory (Task High-Watermark Protection)**:
   * Declaring large local arrays (`uint8_t temp[1024]`) inside an 8KB FreeRTOS task frame consumes significant stack headroom and risks immediate stack overflow under nested function calls or ISR preemption. Chunk buffers must be limited to small sizes (128–256 bytes) or point directly into task-owned static slots.
2. **Heap Memory (Zero-Heap & Destruction Traps)**:
   * If dynamic allocation (`strdup`, `new`, `malloc`) is passed across task creation boundaries, a failure in task spawning (`xTaskCreate != pdPASS`) creates an irrecoverable heap leak unless an explicit failure cleanup branch exists.
   * FreeRTOS `vTaskDelete(nullptr)` aborts execution without triggering C++ stack unwinding or RAII destructors. Any managed smart pointers (`std::unique_ptr`) must be explicitly released (`.reset()`) prior to task termination.
3. **BSS & Static Data (Session Isolation & Footprint Trim)**:
   * Function-scope `static` variables retain state across calls. If a connection terminates, static trackers must be re-initialized (`resetTrackers()`) upon new client connection to prevent stale timestamps from polluting subsequent sessions.
   * Idle background utilities must not permanently hold multi-kilobyte static scratchpads in BSS; scratchpads must be sized to exact peak requirements or shared safely.
4. **Retention SRAM (RTC / Battery-Backed RAM)**:
   * Retention memory surviving deep sleep and soft WDT resets is extremely scarce (often 4–8KB). Never size persistent cache entries to worst-case theoretical maximums if real-world payloads are small. Sizing structures to actual operational payloads preserves safety margins.

---

### Domain 3: Hot-Path Execution Latency & Optimization

* **Strict Physical Separation of Hot and Cold Paths**:
  * Packet receiving, framing, and routing loops executing tens to hundreds of times per second constitute the Hot Path. Management RPC, CLI formatting, OTA orchestration, and human configuration interfaces belong to the Cold Path.
* **Elimination of Parasitic String Computations**:
  * Calling O(N) string algorithms (`strcasestr`, `strcmp`, `strlen`) inside hot packet loops to classify devices or commands wastes excessive CPU cycles.
  * **Classification Hoisting**: Determine device classes, channel roles, or routing targets once during registration or configuration (Cold Path), cache the result as an O(1) integer enumeration (`enum class`), and use integer equality in the Hot Path.
* **Intermediate Copy Reduction**:
  * Eliminate unnecessary staging buffers between stream reception and dispatch. Transition multi-stage pipelines to single-copy or zero-copy abstractions (`span<const uint8_t>`).

---

### Domain 4: Concurrency, Lock Hierarchy & Interlocks

* **Lock Hierarchy Enforcement**:
  * When multiple mutexes are required, establish a universal, system-wide acquisition order (e.g., `Repository Lock` → `Network Lock`). Never acquire locks in reverse order across different tasks, which causes priority inversion and deadlocks.
* **Minimizing Lock Hold Time**:
  * Never perform blocking delays (`vTaskDelay`), physical flash writes, or blocking network socket I/O while holding a shared mutex. Capture a local snapshot inside the critical section, release the lock immediately, and perform I/O outside.
* **Integrity on Lock Timeout**:
  * When acquiring locks with timeouts (`MutexLocker lock(mutex, timeout)`), execution branches handling acquisition failure must not execute subsequent state transitions, increment transmission statistics (`tx_pkts++`), or assume transactions completed.

---

### Domain 5: System Resilience, Timing & Watchdogs

* **Hierarchical Multi-Tier Watchdogs**:
  * Layer Hardware WDTs, FreeRTOS Task WDTs (TWDT), and software task heartbeat monitors. Ensure blocking socket operations specify timeouts (e.g., `SO_RCVTIMEO`) significantly smaller than WDT trigger periods.
* **Large Stream Ingestion Interlocks (OTA / Bulk Transfers)**:
  * When downloading large payloads over HTTP/TCP, stream stalls can delay loop iterations. Explicit WDT resets (`esp_task_wdt_reset()`) paired with micro-yields (`vTaskDelay(pdMS_TO_TICKS(5))`) guarantee RTOS scheduler fairness and prevent spurious watchdog triggers.
* **Crash-Loop Recovery & Automatic Rollback**:
  * Implement non-volatile boot counters. If repeated crashes occur before achieving operational convergence, the bootloader or early init phase must automatically invalidate the active partition, trigger fallback rollback, or boot into a minimal rescue state.

---

### Domain 6: Dead Code & Legacy Buffer Deprecation

* **Consolidation of Superseded Structures**:
  * When evolving data structures (e.g., transitioning from single-frame snapshots to a multi-cycle rolling transaction history), audit and excise obsolete intermediate buffers. Leaving legacy arrays intact causes duplicate copies and wastes static RAM.
* **Elimination of Phantom Fields**:
  * Structures often accumulate fields that are cleared during initialization and copied across assignments, but never queried or transmitted. Identify and purge these phantom fields to reclaim memory across array instances.

---

### Domain 7: Trust Boundaries & Input Sanitization

* **Parameter Range & Index Bounds Validation**:
  * All command arguments received over external communication endpoints must be checked against valid bounds before being cast or used as array indices. Disallow unbounded IDs, channels, or offsets.
* **Brute-Force & Denial-of-Service Defense**:
  * Enforce cooldown periods and failure tracking per IP or session for management interfaces. Ensure connection drops do not reset lockout penalties.

---

### Domain 8: Client-Server Protocol & Contract Consistency

A firmware gateway operates as a server to external clients (cloud connectors, mobile apps, local edge drivers). Protocol consistency is a universal engineering contract:

1. **Protocol Contract Invariance**:
   * Any change to firmware endpoint configurations, TCP ports, channel designations, or serialization schemas requires atomic, synchronized updates to client-side mapping constants.
2. **Default & Fallback Equivalence**:
   * **Anti-Pattern**: A client omitting an endpoint configuration parameter falls back to a hardcoded legacy value that contradicts the firmware’s algorithmic default rules.
   * **Invariant**: Client-side fallback logic must mirror the server’s canonical initialization rules (e.g., `default_port = (id == 0) ? PORT_A : (BASE_PORT + id)`) or reject ambiguous configurations outright.
3. **State Synchronization Integrity**:
   * Asynchronous push telemetry and synchronous polling query responses must share identical capability definitions, value scales, and state representations.

---

## 3. Architectural Decision & Prioritization Matrix

All findings during audit and review must be categorized using this objective impact matrix:

| Severity | Definition | Representative Examples | Action Required |
|:---:|---|---|---|
| **🔴 Critical** | System crash, memory corruption, memory leak, security bypass | Accumulator overflow when `len > sizeof(buf)`, leaked allocation on task spawn fail, unchecked buffer indexing | Halt pipeline; immediate fix required prior to any release |
| **🟠 High** | Hot-Path CPU waste, session cross-contamination, metrics distortion | O(N) `strcasestr` in packet loop, stale static trackers across sessions, counting packets on mutex timeout | Core performance and behavioral defect; mandatory priority fix |
| **🟡 Medium** | RAM/SRAM inefficiency, dead code, lock scope bloat | Unused legacy struct fields, unneeded shadow buffers, minor delays held inside critical sections | Planned memory reclamation and structural hygiene |

### ⚖️ Universal Embedded Trade-off Rules
1. **Stack vs. Static vs. Heap**:
   * Size $\le 256$ bytes: Prefer stack allocation.
   * Size $> 256$ bytes (Task-Specific): Prefer task-owned static buffers.
   * Temporary Large Payloads: Permit dynamic allocation exclusively in Cold Paths with strict RAII lifetimes, freeing immediately upon completion.
2. **Pass-by-Value vs. Pass-by-Reference (Zero-Copy)**:
   * Data crossing concurrency/thread boundaries: Enforce value-copy snapshots to eliminate race conditions.
   * Data pipelines within a single task context: Enforce zero-copy reference abstractions (`span<const uint8_t>`).

---

## 4. Phased Verification Pipeline

All structural improvements and refactoring must execute through four gated milestones:

```
[Phase 1: Defect Isolation] ──> [Phase 2: Hot-Path & RAM Recovery] ──> [Phase 3: Retention & Security] ──> [Phase 4: Contract Sync & Build]
 (Crashes, Leaks, Boundary)       (O(1) Enums, Dead Buffers Purged)     (Retention SRAM, Range Guards)     (External Clients, Full Image)
```

### Milestone Exit Gates
* **Phase 1 (Critical Defects)**: Eliminate buffer overflow points, dynamic allocation leaks, and concurrency accounting bugs. Build with 0 errors and 0 warnings.
* **Phase 2 (Hot-Path & RAM Recovery)**: Replace string lookups in hot paths with O(1) enums; remove unused legacy struct fields. Measure and report static RAM reduction.
* **Phase 3 (Retention & Hardening)**: Optimize retention/RTC SRAM layouts to maximize headroom; establish boundary checks on all external endpoints.
* **Phase 4 (Contract Sync & Verification)**: Synchronize client/driver mapping constants; perform complete compilation and binary image generation.

---

## 5. Diagnostic Audit Tooling & Commands

Perform systematic codebase audits using standard command-line regular expressions:

```bash
# 1. Audit dynamic heap allocations across the codebase
grep -rn "new \|malloc\|strdup\|realloc\|make_unique" src/ include/

# 2. Audit parasitic string scanning in hot paths (engines, parsers)
grep -rn "strcasestr\|strstr\|strcmp\|strlen" src/Engine/ src/Parser/

# 3. Audit large local stack buffers (>= 100 bytes)
grep -rn "uint8_t\s\+[a-zA-Z0-9_]*\[[0-9]\{3,\}\]" src/

# 4. Audit unbounded lock acquisitions (portMAX_DELAY)
grep -rn "xSemaphoreTake.*portMAX_DELAY\|MutexLocker" src/

# 5. Audit persistent static local variables across functions
grep -rn "static\s\+\(uint8_t\|char\|int\|struct\)\s\+.*\[" src/

# 6. Verify zero-error and zero-warning compilation
~/.platformio/penv/bin/pio run
```

---

## 6. Pre-Flight Review Checklist

Before signing off on any code modification, verify that every item evaluates to **YES**:

- [ ] Does the hot communication path contain zero dynamic allocations (`new`, `malloc`, `String`)?
- [ ] Are stream ingestion buffers completely protected against overflow even if `incoming_len > sizeof(buffer)`?
- [ ] Are task stack frames free of local buffers exceeding 256 bytes?
- [ ] Are all resources safely released if task spawning fails or if a task deletes itself (`vTaskDelete`)?
- [ ] Have all iterative string comparisons (`strcasestr`, `strcmp`) in packet processing loops been hoisted to O(1) integer enumerations?
- [ ] Are packet and transaction statistics protected from false increments when lock acquisition times out?
- [ ] Have legacy shadow arrays and duplicate history buffers been purged?
- [ ] Do all struct fields serve an active, live read/write purpose with no phantom fields?
- [ ] Is retention/RTC SRAM minimized to ensure adequate safety margins?
- [ ] Do external client/driver fallback defaults strictly match canonical firmware initialization rules?
- [ ] Does the final build compile with **0 errors and 0 warnings**, with documented memory deltas?
