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

### 0.3 Canonical Clean Architecture Topology (4-Tier + 1 Foundation Soil)

> **Mandatory Architectural Standard (v4.0.0 Canonical & Modularized)**:
> 1. **Foundation Soil (L0 Base Leaf)**: Universal static foundation (`System_Buffer.h`, `System_Config.h`, `System_Platform.h`). Zero upward dependencies; accessible directly by any layer ($L1 \sim L4$).
> 2. **L1 Physical HAL Drivers**: Hardware abstractions (`Uart_Driver`, `NVS_Driver`, `Diagnostics_Driver`, `RTOS_Driver`). Complete information hiding.
> 3. **L2 Transport & Data Link Channels**: Raw frame transport, timeslot scheduling, and socket polling (`RS485_CH`, `TCP_CH`). Pure transport leaves; zero awareness of L3 state or L4 listeners.
> 4. **L3 Routing, Codec & State Hub**: Protocol parsers, state hub SSOT, packet routing, and U-turn bypass (`Wallpad_Parser`, `Wallpad_Protocol`, `Modbus_Parser`, `Modbus_Protocol`, `Device_Registry`, `Packet_Router`).
> 5. **L4 Application Services**: High-level orchestrators (`ST_Service`, `EW11_Service`, `CTL_Service`, `CLI_Service`) with structured submodules (`Console/`, `Remote/`).

```
include/
├── L0_Base/                  [L0: Foundation Soil]
│   ├── System_Buffer.h       (AppendBuf fixed scratch buffers, zero-heap utilities)
│   ├── System_Config.h       (NVS keys, baud rates, timing constants, monadic parsers)
│   └── System_Platform.h     (ESP32-S3 pin mappings, StaticPacket structures)
├── L1_Drivers/               [L1: Physical HAL Drivers]
│   ├── Uart_Driver.h         (Unified HW UART0~2 + Doorphone SW Serial HAL)
│   ├── NVS_Driver.h          (Flash non-volatile key-value storage HAL)
│   ├── RTOS_Driver.h         (FreeRTOS Mutex, Semaphore, and CriticalSection RAII)
│   └── Diagnostics_Driver.h  (Heap/stack watermarks, CPU telemetry, OTA flashing)
├── L2_Channels/              [L2: Transport & Data Link Channels]
│   ├── RS485_CH.h            (Ch1~Ch4 serial channel manager, FreeRTOS timeslots)
│   └── TCP_CH.h              (Core 0 TCP reactor, socket FSM, embedded IPFilter)
├── L3_Routing/               [L3: Routing, Codec & State Hub]
│   ├── Packet_Router.h       (Inter-channel packet dispatch, U-turn bypass orchestration)
│   ├── Device_Registry.h     (SSOT device state repository, desired vs real states)
│   ├── Wallpad_Parser.h      (Hyundai Wallpad packet framing & checksum validation)
│   ├── Wallpad_Protocol.h    (Wallpad packet encoders, decoders, payload builders)
│   ├── Modbus_Parser.h       (Modbus RTU frame boundary and CRC validator)
│   └── Modbus_Protocol.h     (Modbus register mapping, encode/decode routines)
└── L4_Services/              [L4: Application Services]
    ├── ST_Service.h          (SmartThings LAN bridge, asynchronous REST/Webhook push)
    ├── EW11_Service.h        (Virtual RS-485 EW11 TCP client/server session coordinator)
    ├── CTL_Service.h         (Web UI HTTP REST control endpoint handler)
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
│   └── System_Platform.cpp
├── L1_Drivers/
│   ├── Uart_Driver.cpp       (HW UART & SoftwareSerial fully sealed via file-static scope)
│   ├── NVS_Driver.cpp
│   └── Diagnostics_Driver.cpp
├── L2_Channels/
│   ├── RS485_CH.cpp          (Task_Ch1, Task_Ch2Ch3, Task_Ch4 FreeRTOS worker loops)
│   └── TCP_CH.cpp            (Task_TcpCore0 socket polling and IP whitelist filter)
├── L3_Routing/
│   ├── Packet_Router.cpp     (U-turn routing table, horizontal bypass engine)
│   ├── Device_Registry.cpp   (Mutex-protected snapshot API, 0% extern global state leaks)
│   ├── Wallpad_Parser.cpp
│   ├── Wallpad_Protocol.cpp
│   ├── Modbus_Parser.cpp
│   └── Modbus_Protocol.cpp
├── L4_Services/
│   ├── ST_Service.cpp        (SmartThings event transmission loop)
│   ├── EW11_Service.cpp      (EW11 proxy and remote management coordinator)
│   ├── CTL_Service.cpp
│   ├── CLI_Service.cpp       (Serial stream tokenizer and command dispatcher)
│   ├── Console/              [CLI Submodule Implementations]
│   │   ├── CmdConfig.cpp
│   │   ├── CmdDevice.cpp
│   │   ├── CmdSystem.cpp
│   │   └── CmdTrace.cpp
│   └── Remote/               [Remote Submodule Implementations]
│       ├── MgmtRpc.cpp
│       ├── RemoteTelemetry.cpp
│       └── WifiManager.cpp
└── main.cpp                  (Bootstrapping, driver/channel/service init & task launch)
```

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
| `CH#1_IoT` | Core 1 | 19 (High) | `Task_Ch1()` | **Hot Path** | RS-485 physical master polling & device state synchronization |
| `CH#2_WP#1` | Core 1 | 18 (High) | `Task_Ch2Ch3()` | Warm Path | Wallpad #1 RS-485 slave virtual ACK immediate response |
| `CH#3_WP#2` | Core 1 | 18 (High) | `Task_Ch2Ch3()` | Warm Path | Wallpad #2 RS-485 slave virtual ACK immediate response |
| `CH#4_WP#3` | Core 1 | 10 (Med) | `Task_Ch4()` | Warm Path | Doorphone SoftwareSerial bidirectional communication |
| `Network` | Core 0 | 5 (Med) | `TcpReactor::runTask()` (Core 0 Network Reactor) | Warm Path | Wi-Fi connectivity, CH5 EW11 hub client, CH6 Mgmt RPC |
| `Telnet_CLI` | Core 0 | 2 (Low) | `Task_Telnet()` | Cold Path | Telnet CLI diagnostics, packet tracing, administration |

---

## 2. Channel & Port Mapping

| Channel | Physical/Logical Interface | Port / Pins | Role | Description |
| :--- | :--- | :--- | :--- | :--- |
| **CH1** | UART0 (RS-485) | Hardware Default | Sub-device master bus (Lights, Thermostats, Fans) | Real-time physical RS-485 bus master control |
| **CH2** | UART1 (RS-485) | RX: 5, TX: 6 (Configurable) | Main wallpad bridge (Virtual slave) | Immediate virtual ACK frame emission |
| **CH3** | UART2 (RS-485) | RX: 7, TX: 8 (Configurable) | Sub wallpad bridge (Virtual slave) | Immediate virtual ACK frame emission |
| **CH4** | SoftwareSerial | RX: 38, TX: 39 | Doorphone (videophone) serial bus | Call detection and door unlock bridge |
| **CH5** | TCP Client | 8898 (Elevator) / 8891~8894 (FCU) | EW11 multi-hub bridge client | External serial bus bridge over TCP |
| **CH6** | TCP Server | 8900 | SmartThings dedicated JSON-RPC server | Device control ingestion & telemetry push |
| **CLI** | TCP Server | 23 | Telnet administration & diagnostic console | Real-time packet tracer, state dump, NVS config |

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

### Pillar 3: 100% Zero-Heap & Zero-Copy Invariant
> **"Permanently prohibit dynamic heap allocations (malloc/new/String) across all hot paths and long-running runtime loops."**
- In embedded systems running 24/7/365, heap fragmentation is a delayed catastrophic failure.
- Standardise on fixed-size frames (`std::array<uint8_t, N>`, `StaticPacket`), buffer views (`span<const uint8_t>`, `std::string_view`), static ring buffers (`history[8][64]`), and non-allocating utility buffers (`AppendBuf`).

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
