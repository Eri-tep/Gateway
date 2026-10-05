# GW Home Gateway Architecture, Specifications & Design Philosophy

This document defines the system specifications, runtime topology, channel mappings, deadlock-free concurrency/locking hierarchies, and the **Canonical 4+1 Layer Architecture & Implementation Standards**.

---

### 0. Migration Post-Mortem & Anti-Pattern Analysis (Architectural Integrity Mandate)

> [!CAUTION]
> **Post-Mortem Review & Mandatory Corrective Action (Recorded 2026-10-04)**
> During the 4+1 Layer Clean Architecture migration, an over-reliance on transitional alias headers and direct bulk relocation led to critical architectural compromises. To ensure zero recurrence, the root causes, anti-patterns, and binding corrective rules are permanently codified here.

#### 0.1 Root Causes of Recent Architectural Regressions
1. **"Relocation Without Modular Decomposition" (The God File Renaming Fallacy)**:
   - The monolithic `EngineTask.cpp` (~1,370 lines) contained responsibilities spanning three distinct layers: L2 frame transmission, L3 packet parsing & device state mutation (`DeviceRegistry`), and L4 event dispatching (SmartThings/Doorphone listeners).
   - Rather than decomposing these responsibilities into their designated layers, the entire file body was simply moved into `src/Channels/RS485_CH.cpp`. Renaming a God File without decomposing its internal responsibilities violated Single Responsibility and clean layering.
2. **Upward Dependency Leak in L2 Transport**:
   - Because `RS485_CH.cpp` retained L3 state mutation and L4 listener dispatch logic, `include/Channels/RS485_CH.h` required `#include "Routing/DeviceRegistry.h"`, and `src/Channels/RS485_CH.cpp` required `#include "Services/EngineTask.h"`.
   - This directly violated the **Strict Downlink Hierarchy ($L4 \rightarrow L3 \rightarrow L2 \rightarrow L1$)** and the **Zero Upward Include** rule.
3. **Spurious File Proliferation via Unresolved Scaffolding**:
   - Intermediate forwarding headers and legacy files (`DeviceRegistry.cpp` alongside `Device_Registry.cpp`, `NetworkRouter.cpp` alongside `Packet_Router.cpp`, and unmerged trackers) were retained concurrently rather than undergoing full in-place canonical absorption.
4. **The False Remedy of Forced Monolithic Merging**:
   - Attempting to reduce file counts by collapsing legitimately separated modules (`Wallpad_Parser`, `Wallpad_Protocol`, `Device_Registry`, `AutoProbingEngine`) into a single file directly contradicts modular cohesion and recreates massive monolithic debt. Legitimate submodules (such as `Console/` commands and `Remote/` handlers) must remain modularized while eliminating transitional scaffolding.

#### 0.2 Canonical Corrective Principles
- **L2 Pure Transport Leaf Invariant**: `RS485_CH` and `TCP_CH` must act strictly as L2 Transport Leaves. They manage physical UART/Socket I/O, ring buffers, timeslots, and hardware tasks. They possess **0% awareness of L3 device state registries or L4 listeners**. All packet boundary validation and state updates belong strictly in L3.
- **Top-Down Downlink / Pull-Pop Pipeline**: Downlink requests flow strictly $L4 \rightarrow L3 \rightarrow L2 \rightarrow L1$. Uplink reception operates via top-down polling/pulling from thread-safe channel queues without upward callback hooks.

---

### 0.3 Canonical Clean Architecture Topology (4-Tier + L0 Foundation Soil & L3 Shell-Core)

> **Mandatory Architectural Standard (v4.1.0 Canonical Shell-Core Standard)**:
> 1. **Foundation Soil (L0 Base Leaf)**: Universal static foundation (`System_Buffer.h`, `System_Config.h`, `System_Platform.h`). Zero upward dependencies; accessible directly by any layer ($L1 \sim L4$). Must remain a **pure foundation leaf** with **zero domain-specific hooks or callbacks**.
> 2. **L1 Physical HAL Drivers**: Hardware abstractions (`Uart_Driver`, `Diagnostics_Driver`, `OTA_Driver`). Complete information hiding. Exclusively tracks physical driver health and UART channel state metrics.
> 3. **L2 Transport & Data Link Channels**: Raw frame transport, timeslot scheduling, and socket polling (`RS485_CH`, `TCP_CH`). Pure transport leaves; zero awareness of L3 device state, scheduler strategies, or L4 listeners.
> 4. **L3 Routing, Subsystem & Shell-Core Engine**:
>    - **L3 Public Shell (External Boundary Gateways)**:
>      - `Packet_Router` : **The ONLY bidirectional packet gateway** between L3 and L2 (`Router_EnqueueDownlink`, `Router_BuildNextPoll`).
>      - `Device_Registry`: SSOT device repository and decoupled control/state ingress.
>      - `ProtocolDiagnostics`: Decoupled read-only diagnostic snapshots and inspection facade for L4 (strictly zero runtime framing engines or mutated state).
>    - **L3 Private Core (100% Encapsulated Engines)**:
>      - `Wallpad_Protocol`: Internal protocol FSM and doorphone state machine (`FramingTracker` sealed here).
>      - `Wallpad_Parser`: Binary frame parser and checksum validation.
>      - `PollingRegistry`: Dynamic polling target registry, warm cache, and **sole owner of internal `stale_poll_cnt`**.
>      - `AutoProbingEngine`: Runtime heuristic protocol matrix solver.
>      - `ControlTemplate`: Device capability blueprints and action slot decoders.
> 5. **L4 Application Services**: High-level orchestrators (`ST_Service`, `EW11_Service`, `CLI_Service`, `RemoteRpc`, `RemoteTelemetry`). Interacts strictly with L3 Public; possesses 0% access to L3 Private.

```
include/
├── L0_Base/                  [L0: Pure Foundation Soil Leaf]
│   ├── System_Buffer.h       (AppendBuf fixed scratch buffers, zero-heap utilities)
│   ├── System_Config.h       (NVS keys, baud rates, timing constants, monadic parsers)
│   └── System_Platform.h     (ESP32-S3 pin mappings, StaticPacket, System_TracePacket/Message)
├── L1_Drivers/               [L1: Physical HAL Drivers]
│   ├── Uart_Driver.h         (Unified HW UART0~2 + Doorphone SW Serial HAL)
│   ├── Diagnostics_Driver.h  (Heap/stack watermarks, CPU telemetry, Ch1StateMetrics)
│   └── OTA_Driver.h          (Dual-partition rollback, rescue AP recovery)
├── L2_Channels/              [L2: Transport & Data Link Channels]
│   ├── RS485_CH.h            (Ch1~Ch4 serial channel manager, FreeRTOS timeslot loops)
│   └── TCP_CH.h              (Core 0 TCP reactor, socket FSM, embedded IPFilter)
├── L3_Routing/               [L3: Routing, Subsystem & Shell-Core Hub]
│   ├── Public/               [L3 Public Shell: External Gateways for L4 & L2]
│   │   ├── Packet_Router.h   (Sole L3 ↔ L2 bidirectional packet gateway & downlink egress)
│   │   ├── Device_Registry.h (SSOT device state repository & control ingress API)
│   │   ├── Protocol_Diagnostics.h (Thread-safe read-only diagnostic snapshots & facade)
│   │   └── Modbus_Codec.h    (FCU Modbus RTU byte stream encoder, decoder & CRC-16)
│   └── Private/              [L3 Private Core: 100% Internal Subsystem Engines]
│       ├── Wallpad_Engine.h  (Internal protocol FSM & Doorphone FramingTracker engine)
│       ├── Wallpad_Parser.h  (Hyundai Wallpad packet framing & checksum algorithms)
│       ├── Polling_Registry.h(1st-tier dynamic polling targets & internal stale_poll_cnt)
│       ├── Auto_Probing.h    (Runtime automatic matrix solver & profile discovery)
│       └── Control_Registry.h(Control blueprints, slot coverage & frame synthesis)
└── L4_Services/              [L4: Application Services]
    ├── ST_Service.h          (SmartThings LAN bridge, asynchronous REST/Webhook push)
    ├── EW11_Service.h        (Virtual RS-485 EW11 TCP client/server session coordinator)
    ├── CLI_Service.h         (UART0 serial diagnostic/administration console REPL)
    ├── Console/              [CLI Submodules - Domain Modularization]
    │   ├── ConsoleFmt.h      (ANSI styling and tabular text formatting utilities)
    │   ├── CmdConfig.h       (NVS configuration and Wi-Fi parameter commands)
    │   ├── CmdDevice.h       (Device control and real-time state query commands)
    │   ├── CmdSystem.h       (FreeRTOS tasks, heap, stack, and mutex diagnostics)
    │   └── CmdTrace.h        (Channel-specific real-time packet sniffer commands)
    └── Remote/               [Remote & EW11 Submodules]
        ├── RemoteInternal.h  (Internal session types and remote context definitions)
        ├── MgmtRpc.h         (Remote JSON-RPC parser and management command handlers)
        ├── RemoteTelemetry.h (Periodic system metric payload builders)
        └── WifiManager.h     (Wi-Fi state machine and automatic reconnect logic)

src/
├── L0_Base/
│   ├── System_Config.cpp
│   └── System_Platform.cpp   (Platform synchronization & decoupled trace message/packet sinks)
├── L1_Drivers/
│   ├── Uart_Driver.cpp       (HW UART & SoftwareSerial fully sealed via file-static scope)
│   ├── Diagnostics_Driver.cpp(System metrics, task watchdogs, hardware crash telemetry)
│   └── OTA_Driver.cpp
├── L2_Channels/
│   ├── RS485_CH.cpp          (Task_Ch1, Task_Ch2Ch3, Task_Ch4 FreeRTOS worker loops)
│   └── TCP_CH.cpp            (Task_TcpCore0 socket polling and IP whitelist filter)
├── L3_Routing/
│   ├── Public/
│   │   ├── Packet_Router.cpp (Downlink queue dispatch & horizontal bus routing)
│   │   ├── Device_Registry.cpp(Mutex-protected snapshot API, 0% extern global state leaks)
│   │   ├── Protocol_Diagnostics.cpp(Facade query methods & snapshot mapping)
│   │   └── Modbus_Codec.cpp  (FCU Modbus RTU byte stream encoder & CRC-16 implementation)
│   └── Private/
│       ├── Wallpad_Engine.cpp(Doorphone FSM, guard delays, framing engine implementation)
│       ├── Wallpad_Parser.cpp  (Zero-copy span packet parsers)
│       ├── Polling_Registry.cpp (Dynamic polling targets, warm cache & stale_poll_cnt)
│       ├── Auto_Probing.cpp  (Matrix solver & convergence detection)
│       └── Control_Registry.cpp (Blueprint synthesis & action execution)
├── L4_Services/
│   ├── ST_Service.cpp
│   ├── EW11_Service.cpp      (EW11 proxy coordinator with self-contained frame metadata)
│   ├── CLI_Service.cpp       (Telnet virtual stream diagnostic console REPL / TCP Port 23; strictly network-only)
│   ├── Console/              (CmdConfig.cpp, CmdDevice.cpp, CmdSystem.cpp, CmdTrace.cpp)
│   └── Remote/               (MgmtRpc.cpp, RemoteTelemetry.cpp, WifiManager.cpp)
└── main.cpp                  (Bootstrapping, dependency injection & task launches)
```

#### 0.3.1 Binding Architectural Invariants (Non-Negotiable)

1. **Total Shell-Core Model (100% Information Hiding)**:
   - All external ingress into L3 (from L4) must target **L3 Public headers exclusively**.
   - All external egress from L3 (to L2) must traverse **`Packet_Router::Router_EnqueueDownlink()` exclusively**.
   - L3 Private headers (`Wallpad_Protocol.h`, `PollingRegistry.h`, `ControlTemplate.h`, etc.) are strictly forbidden from being included by L4 Services or L2 Channels.
2. **Zero Upward Dependencies & Zero Layer Skipping**:
   - Upward includes ($L_M \rightarrow L_N$ where $M < N$) are strictly prohibited.
   - Vertical runtime calls must follow the strictly adjacent hierarchy: $L4 \rightarrow L3 \rightarrow L2 \rightarrow L1$. Layer skipping ($L3 \rightarrow L1$) is prohibited.
3. **No Middle-Man Semantic Leakage**:
   - L3 scheduler decisions (such as "stale device polling") belong strictly inside L3 (`PollingRegistry::_stale_poll_cnt`). L3 must never pass scheduler semantics down to L2 channels as parameters or delegate metric increments to lower layers.
4. **No Overreaching Metric Invasions**:
   - L3 protocol convergence (`Wallpad_CheckConvergence`) must only stabilize its own cache and signal system milestone `SYS_EVT_CACHE_READY`. It is strictly forbidden for L3 to wipe or reset L1 hardware metrics (`g_pkt_stats`, `g_metrics`).
5. **L0 Foundation Soil Purity**:
   - L0 Base (`System_Platform.h/cpp`) is a universal, static leaf. It must remain completely free of application- or channel-specific callback hooks.
6. **Framing & State Ownership**:
   - Framing engines (`FramingTracker`) belong strictly to their operational domain (`Wallpad_Protocol` for Doorphone CH4). Services such as `EW11_Service` must encapsulate their own framing parameters without coupling to L3 core engines.
7. **CLI Virtual Stream & Dedicated UART0 Invariant**:
   - `CLI_Service` is strictly an L4 Telnet network stream service (TCP Port 23 / `Task_Telnet`). It must never be designated as or multiplexed with a UART0 serial console. Hardware UART0 is 100% dedicated to CH1 RS-485 bus master communication.
8. **Headless Safety & Silent Booting Guard**:
   - Because no physical serial console exists during normal operation:
     - **Core Dump to Flash**: `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=1` and the dedicated `coredump` flash partition (384KB) capture panic/WDT backtraces for post-boot Telnet inspection (`coredump` command).
     - **RS-485 Silent Boot Guard**: Early boot logging must never leak onto hardware UART0 pins. Output is muted or restricted to internal memory/CDC until RS-485 port drivers are cleanly initialized.
9. **Task Worker Execution Context (Synchronous Vertical Pipeline)**:
   - FreeRTOS tasks (`Task_Ch1`, `Task_Ch2Ch3`) are execution workers, not layer definitions. Workers execute adjacent synchronous calls: $L3 \rightarrow L2 \rightarrow L1$. L2 Channels (`RS485_CH`, `TCP_CH`) remain pure I/O leaves (buffer & UART management), with zero knowledge of device states or routing tables.
10. **Static Buffer Backpressure & Drop Policy**:
    - Under bus traffic bursts or network disconnects:
      - State and polling queues utilize **Drop-Head** (discard oldest stale frames, preserve newest state).
      - VIP and control command queues utilize **Drop-Tail** with synchronous error reporting (reject new command with error code to prompt immediate client retry).
11. **Flash Endurance Protection via RTC SRAM**:
    - Dynamic polling cache, probing matrix, and volatile runtime tracking are preserved across soft resets and WDT reboots in RTC Fast/Slow SRAM (`RTC_NOINIT_ATTR`). Flash NVS commits are strictly debounced and executed only upon cache convergence (`SYS_EVT_CACHE_READY`) or explicit shutdown.

---

### 0.4 Framework & Toolchain Environment Specifications
- **Target Hardware**: M5Stack AtomS3 Lite (ESP32-S3FN8, 240MHz Dual-Core, 320KB SRAM, 8MB Flash)
- **Framework**: `framework-arduinoespressif32 @ 3.1.3` (Arduino-ESP32 Core v3.1.x)
- **Underlying SDK / ESP-IDF**: **ESP-IDF v5.3.2** (`ESP_IDF_VERSION_VAL(5, 3, 2)`)
- **Toolchain**: `xtensa-esp-elf-gcc / g++ 13.2.0 (crosstool-NG esp-13.2.0_20240530)`
- **C++ Standard**: **C++23** (`-std=gnu++23`)

---

## 1. System Architecture & Immutable Constraints

### 1.1 Runtime Reliability Principles
- **24/7 Uninterrupted Operation**: Maintain zero ESP32 Task Watchdog Timer (WDT) resets across all operational conditions.
- **Zero Real-time Loop Dynamic Allocation**: Heap allocations (`new`, `malloc`, dynamic `String`) are strictly forbidden in real-time execution paths (`Task_Ch1`, `Task_Ch2Ch3`, `TcpReactor::runTask`). Use stack-allocated or fixed-size buffers (`StaticPacket`, `AppendBuf`, `std::array`).
- **LOCKED Device Push Isolation**: To prevent propagation of noisy or unverified bus packets, real-time push to SmartThings (CH6 / TCP 8900) is strictly restricted to verified state changes from `LOCKED` devices.

### 1.2 FreeRTOS Task Topology (Immutable)

| Task Name | Core Affinity | Priority | Entry Function | Path Classification | Responsibility |
| :--- | :---: | :---: | :--- | :--- | :--- |
| `CH#1_IoT` | Core 1 | 19 (High) | `Task_Ch1()` | **Hot Path** | RS-485 master execution worker (drives L2 transport, performs L3 polling & device state sync) |
| `CH#2_WP#1` | Core 1 | 18 (High) | `Task_Ch2Ch3()` | Warm Path | Wallpad #1 RS-485 slave virtual ACK immediate response |
| `CH#3_WP#2` | Core 1 | 18 (High) | `Task_Ch2Ch3()` | Warm Path | Wallpad #2 RS-485 slave virtual ACK immediate response |
| `CH#4_WP#3` | Core 1 | 10 (Med) | `Task_Ch4()` | Warm Path | Doorphone SoftwareSerial bidirectional communication |
| `Network` | Core 0 | 5 (Med) | `TcpReactor::runTask()` (Core 0 Network Reactor) | Warm Path | Wi-Fi connectivity, CH5 EW11 hub client, CH6 Mgmt RPC |
| `Telnet_CLI` | Core 0 | 2 (Low) | `Task_Telnet()` | Cold Path | Telnet CLI diagnostics, packet tracing, administration |

---

## 2. Channel & Port Mapping

| Channel | Physical/Logical Interface | Port / Pins | Role | Description |
| :--- | :--- | :--- | :--- | :--- |
| **CH1** | UART0 (RS-485) | Hardware Default | Sub-device master bus (Lights, Thermostats, Fans) | Real-time physical RS-485 bus master control (Exclusively dedicated; NO console serial multiplexing) |
| **CH2** | UART1 (RS-485) | RX: 5, TX: 6 (Configurable) | Main wallpad bridge (Virtual slave) | Immediate virtual ACK frame emission |
| **CH3** | UART2 (RS-485) | RX: 7, TX: 8 (Configurable) | Sub wallpad bridge (Virtual slave) | Immediate virtual ACK frame emission |
| **CH4** | SoftwareSerial | RX: 38, TX: 39 | Doorphone (videophone) serial bus | Call detection and door unlock bridge |
| **CH5** | TCP Client | 8898 (Elevator) / 8891~8894 (FCU) | EW11 multi-hub bridge client | External serial bus bridge over TCP |
| **CH6** | TCP Server | 8900 | SmartThings dedicated JSON-RPC server | Device control ingestion & telemetry push |
| **CLI** | TCP Server | 23 | Telnet administration & diagnostic console | Real-time packet tracer, state dump, NVS config (100% Virtual network stream; NO UART) |

---

## 3. Concurrency & Lock Hierarchy (Deadlock-Free Standard)

1. **Spinlocks & Critical Sections (`portMUX_TYPE`)**:
   - Keep critical section duration (`taskENTER_CRITICAL(&mux)` / `CriticalSectionLocker`) strictly **under tens of microseconds (µs)**.
   - Strictly prohibit I/O operations (`Serial.print`), memory allocations, NVS access, and blocking function calls inside critical sections.
   - High-speed asynchronous timers (`BurstTxFsm` via `esp_timer`) must be guarded strictly with isolated spinlocks.
2. **Mutexes (`SemaphoreHandle_t`, `MutexLocker`)**:
   - Use dedicated mutexes (internal channel mutexes, etc.) for TCP socket transmissions and shared buffer synchronization.
   - Lock acquisition timeout must never exceed `Config::Timing::MAX_LOCK_HOLD_MS`.
   - **Non-reentrant Socket Lock Standard**: Never invoke socket transmission functions directly from inside a packet reception callback or parser while holding locks. Instead, enqueue the request into a pending buffer (`pending_cmd_buf`) and dispatch sequentially in the main loop to completely eliminate self-deadlocks.
3. **Read/Write Shared State (`std::shared_mutex`)**:
   - For global configurations (`g_config`) with frequent multi-task reads and rare writes, standardise on `std::shared_mutex` (`std::shared_lock` vs `std::unique_lock`).
4. **NVS Persistence Debouncing**:
   - Debounce runtime template updates and configuration writes (`WARM_CACHE_NVS_DEBOUNCE_MS`) to protect Flash endurance.
5. **Flash Wear Leveling & RTC SRAM Retention**:
   - Volatile runtime caching, auto-probing matrix state, and dynamic polling registries reside in RTC Fast/Slow SRAM (`RTC_NOINIT_ATTR`). Flash writes (`WARM_CACHE_NVS_DEBOUNCE_MS`) are strictly debounced and committed only upon complete cache convergence (`SYS_EVT_CACHE_READY`) or explicit shutdown hook, shielding SPI Flash from endurance fatigue.

---

## 4. The 7 Core Architectural & Refactoring Pillars

These principles represent the engineering standard established across the `Protocol.cpp`, `Console.cpp`, and `Bridge.cpp` rebuilds.

### Pillar 1: Domain Isolation & Single Responsibility Principle (SRP)
> **"Every file, class, and namespace must encapsulate exactly one architectural layer."**
- Never intermix L4 socket transport, L7 stream demuxing/framing, protocol codecs, and application business logic in a single function or global scope.
- Enforce strict domain segregation via internal namespaces (`ModbusRtu`, `Ew11Manager`, `Fcu`).

### Pillar 2: Explicit Finite State Machine (FSM First)
> **"Replace distributed if-else flags and ad-hoc time comparisons with explicit, prioritized state machines."**
- Dispersed `bool` flags coupled with `millis()` checks across loops introduce state leaks and race conditions.
- Consolidate sequential workflows into explicit enum-based FSMs (e.g. Telnet ANSI stream parser) or prioritized sequential loops (e.g. FCU 5-step FSM loop).

### Pillar 3: 100% Zero-Heap, Zero-Copy & Backpressure Invariant
> **"Permanently prohibit dynamic heap allocations (malloc/new/String) across all hot paths, and establish deterministic queue drop semantics."**
- In embedded systems running 24/7/365, heap fragmentation is a delayed catastrophic failure.
- Standardise on fixed-size frames (`std::array<uint8_t, N>`, `StaticPacket`), buffer views (`span<const uint8_t>`, `std::string_view`), static ring buffers (`history[8][64]`), and non-allocating utility buffers (`AppendBuf`).
- **Deterministic Queue Backpressure & Drop Policy**:
  - **Drop-Head (State & Polling Caches)**: When static queues saturate under bus traffic bursts, the oldest frame is discarded to preserve immediate temporal freshness.
  - **Drop-Tail with Synchronous Error (VIP & Control Commands)**: Saturated control queues reject new inbound commands with an immediate error response, preventing silent command drop and prompting upstream retransmission.

### Pillar 4: Pipeline Unification & Table-Driven Dispatch
> **"Consolidate repetitive procedural operations into unified template pipelines and elevate multi-branch conditions into constexpr lookup tables."**
- Never copy-paste boilerplate code across multiple setters or frame builders.
- Eliminate chained `strcasecmp` calls by normalizing strings once into lowercase and dispatching via sorted `constexpr` command tables (`ConsoleCommandEntry[]`).
- Unify multi-attribute setters via generic dispatchers (e.g. `executeRegisterWrite()`).

### Pillar 5: Hardware-Aware Defensive Timing
> **"Refactoring is an evolution of code structure, not a change in protocol behavior. Physical hardware timing must be preserved down to the millisecond."**
- Software optimizations must never compromise hardware constraints of external transceivers, wallpads, or HVAC blower/flap motors.
- Strictly maintain inter-packet bus idle gaps (50ms Guard Interval), two-phase power-on sequencing (delayed target temperature dispatch), and flap motor origin calibration compensation (automatic swing restore).

### Pillar 6: Deadlock-Free Concurrency & Spinlock Isolation
> **"Lock acquisitions must proceed unidirectionally. Interrupt/timer contexts must remain strictly isolated via spinlocks."**
- Never acquire blocking mutexes from inside ISRs or microsecond-level timer callbacks.
- Prevent recursive mutex deadlocks by isolating socket responses through stop-and-wait command queues.

### Pillar 7: Optimistic State Synchronization & Full Observability
> **"User-facing interfaces must respond instantaneously, physical bus responses confirm ground truth, and every transaction must be measurable."**
- Optimistically reflect device state changes in `g_device_repo` upon dispatch to deliver sub-100ms response times in mobile apps, then reconcile with physical bus ACK feedback.
- Instrument every RX, TX, and DROP event with atomic counters (`g_pkt_stats`) and non-allocating CLI tracers (`g_telnet_tracer`).

---

## 5. Subsystem Evolution & Modularity Milestones

| Architectural Subsystem | Canonical Modules | Core Architectural Enhancements | Quantifiable Results |
|---|---|---|---|
| **L3 Protocol Engine** | `WallpadParser.cpp`, `PollingRegistry.cpp`, `AutoProbingEngine.cpp` | Codec, 48-slot cache, and ML probing engine segregation; `span` zero-copy validation | 0 CRC errors, 0 dropped frames, automatic protocol probing, **3 files @ 400~800L** |
| **L4 Console & CLI** | `ConsoleCommands.cpp`, `CmdSystem.cpp`, `CmdConfig.cpp`, `CmdDevice.cpp`, `CmdTrace.cpp` | ANSI Telnet FSM; 8-slot ring-buffer command history (↑/↓); Tab completion; unified `CliFmt` & table dispatch | **-5.7KB Flash reduction**, Zero-Heap CLI, **4 domain packs @ 700~800L** |
| **L4 Remote Services** | `RemoteService.cpp`, `MgmtRpc.cpp`, `RemoteTelemetry.cpp`, `WifiManager.cpp` | SmartThings JSON-RPC, Port 8900 session reactor, 15s fallback guard FSM | Real-time push, zero deadlocks, **3 modules @ 100~880L** |
| **L2 Transport & Core** | `TcpReactor.cpp`, `NetworkRouter.cpp`, `FramingTracker.cpp`, `DoorphoneTracker.cpp` | Dedicated Core 0 non-blocking select reactor, framing FSM promoted to L2 | Single-threaded Core 0 socket loops, 0 race conditions |

---

## 6. Architecture Anti-Patterns vs. Modern Standards

| Area | Legacy Anti-Pattern | Modern Architecture Standard (GW Standard) |
|---|---|---|
| **String & Command Matching** | Chained `if (strcasecmp(...) == 0) else if (...)` | One-shot lowercase normalization + `constexpr` table dispatch |
| **Packet Buffering** | Ad-hoc `uint8_t buf[256]` with manual index math | Type-safe `std::array<uint8_t, N>`, `StaticPacket`, `span` views |
| **Stream Sliding** | Pointer offset math + scattered `memmove` calls | Unified `consumeRxBuffer()` safe boundary sliding helper |
| **Device Control Setters** | Duplicated validation & raw send loops per attribute | Unified template pipeline (`executeRegisterWrite()`) |
| **Delay & Timing Logic** | Dispersed `bool` flags and scattered `millis()` checks | Explicit, prioritized 5-step sequential FSM execution loop |
| **Dynamic Memory** | Use of `new`, `malloc`, and Arduino `String` | **100% Zero-Heap**: Exclusively stack and static allocation |
