# GW Home Gateway Architecture, Specifications & Design Philosophy

This document defines the system specifications, runtime topology, channel mappings, deadlock-free concurrency/locking hierarchies, and the **Canonical 4+1 Layer Architecture & Implementation Standards**.

---

---

### 0. Canonical Clean Architecture Topology (4-Tier + L0 Foundation Soil & L3 Shell-Core)

> **Mandatory Architectural Standard (v4.2.0 Canonical Shell-Core & DIP Standard - Firmware v1.9.7)**:
- **L2 Pure Transport Leaf Invariant**: `RS485_CH` and `TCP_CH` act strictly as L2 Transport Leaves. They manage physical UART/Socket I/O, ring buffers, timeslots, and hardware tasks. They possess **0% awareness of L3 device state registries or L4 listeners**. All packet boundary validation and state updates belong strictly in L3.
- **Top-Down Downlink / Pull-Pop Pipeline**: Downlink requests flow strictly $L4 \rightarrow L3 \rightarrow L2 \rightarrow L1$. Uplink reception operates via top-down polling/pulling from thread-safe channel queues without upward callback hooks.
- **Zero Upward Include**: Higher layers must never be included by lower layers ($L_M \rightarrow L_N$ where $M < N$ is strictly forbidden).

#### 0.1 Layer Hierarchy & Responsibilities
> 1. **Foundation Soil (L0 Base Leaf)**: Universal static foundation (`System_Buffer.h`, `System_Config.h`, `System_Platform.h`). Zero upward dependencies; accessible directly by any layer ($L1 \sim L4$). Acts as the **universal pure foundation leaf** defining global types, synchronization primitives, and **abstract platform service contracts (`System_*`)**.
> 2. **L1 Physical HAL Drivers**: Hardware abstractions (`Uart_Driver`, `Diagnostics_Driver`, `OTA_Driver`). Complete information hiding. Exclusively implements physical driver health, dual-partition OTA engine, NVS/RTC reboot logs, and hardware telemetry.
> 3. **L2 Transport & Data Link Channels**: Raw frame transport, timeslot scheduling, and socket polling (`RS485_CH`, `TCP_CH`). Pure transport leaves; zero awareness of L3 device state, scheduler strategies, or L4 listeners.
> 4. **L3 Routing, Subsystem & Shell-Core Engine**:
>    - **L3 Public Shell (External Boundary Gateways)**:
>      - `Packet_Router` : **The ONLY bidirectional packet gateway** between L3 and L2 (`Router_EnqueueDownlink`, `Router_BuildNextPoll`).
>      - `Device_Registry`: SSOT device repository and decoupled control/state ingress.
>      - `Protocol_Diagnostics`: Pure protocol & routing diagnostics facade for L4 (strictly zero hardware HAL includes; zero non-protocol state).
>      - `Modbus_Codec`: FCU Modbus RTU byte stream encoder, decoder & CRC-16.
>    - **L3 Private Core (100% Encapsulated Engines)**:
>      - `Wallpad_Engine`: Internal protocol FSM and doorphone state machine (`FramingTracker` sealed here).
>      - `Wallpad_Parser`: Binary frame parser and checksum validation.
>      - `Polling_Registry`: Dynamic polling target registry, warm cache, and **sole owner of internal `stale_poll_cnt`**.
>      - `Auto_Probing`: Runtime heuristic protocol matrix solver.
>      - `Control_Registry`: Device capability blueprints and action slot decoders.
> 5. **L4 Application Services**: High-level orchestrators (`Mgmt_Service`, `EW11_Service`, `CLI_Service`). Interacts strictly with L3 Public for protocol needs, and consumes cross-cutting platform capabilities directly via L0 `System_Platform.h`. Possesses 0% access to L3 Private or L2 Channels.

```
include/
├── L0_Foundation/                  [L0: Pure Foundation Soil Leaf]
│   ├── System_Buffer.h       (AppendBuf fixed scratch buffers, zero-heap utilities)
│   ├── System_Config.h       (NVS keys, baud rates, timing constants, monadic parsers)
│   └── System_Platform.h     (Abstract System_* platform contracts, StaticPacket, trace sinks)
├── L1_HAL/                         [L1: Physical HAL Drivers]
│   ├── Uart_Driver.h         (Unified HW UART0~2 + Doorphone SW Serial HAL)
│   ├── Diagnostics_Driver.h  (Heap/stack watermarks, NVS LogManager, Ch1StateMetrics)
│   └── OTA_Driver.h          (Dual-partition rollback, rescue AP recovery, HttpOtaState)
├── L2_Transport/                   [L2: Transport & Data Link Channels]
│   ├── RS485_CH.h            (Ch1~Ch4 serial channel manager, FreeRTOS timeslot loops)
│   └── TCP_CH.h              (Core 0 TCP reactor, socket FSM, embedded IPFilter)
├── L3_Protocol/                    [L3: Routing, Subsystem & Shell-Core Hub]
│   ├── Public/               [L3 Public Shell: External Gateways for L4 & L2]
│   │   ├── Packet_Router.h   (Sole L3 ↔ L2 bidirectional packet gateway & downlink egress)
│   │   ├── Device_Registry.h (SSOT device state repository & control ingress API)
│   │   ├── Protocol_Diagnostics.h (Thread-safe read-only protocol diagnostic snapshots & facade)
│   │   └── Modbus_Codec.h    (FCU Modbus RTU byte stream encoder, decoder & CRC-16)
│   └── Private/              [L3 Private Core: 100% Internal Subsystem Engines]
│       ├── Wallpad_Engine.h  (Internal protocol FSM & Doorphone FramingTracker engine)
│       ├── Wallpad_Parser.h  (Hyundai Wallpad packet framing & checksum algorithms)
│       ├── Polling_Registry.h(1st-tier dynamic polling targets & internal stale_poll_cnt)
│       ├── Auto_Probing.h    (Runtime automatic matrix solver & profile discovery)
│       └── Control_Registry.h(Control blueprints, slot coverage & frame synthesis)
└── L4_Services/                    [L4: Application Services]
    ├── Mgmt_Service.h        (Port 8900 JSON-RPC remote bridge & session coordinator)
    ├── EW11_Service.h        (Virtual RS-485 EW11 TCP client/server session coordinator)
    ├── CLI_Service.h         (Telnet virtual stream diagnostic console REPL / TCP Port 23)
    ├── Console/              [CLI Submodules - Domain Modularization]
    │   ├── Console_Commands.h(Unified command table dispatch definition)
    │   └── Console_Fmt.h     (ANSI styling and tabular text formatting utilities)
    └── Mgmt/                 [Mgmt & Remote Submodules]
        └── Mgmt_Internal.h   (Internal session types, mutexes, and fallback guards)

src/
├── L0_Foundation/
│   ├── System_Buffer.cpp     (Fast Hex LUT, elapsed time utilities)
│   ├── System_Config.cpp     (NVS configuration loader/writer & defaults)
│   └── System_Platform.cpp   (Platform synchronization & decoupled trace message/packet sinks)
├── L1_HAL/
│   ├── Uart_Driver.cpp       (HW UART & SoftwareSerial fully sealed via file-static scope)
│   ├── Diagnostics_Driver.cpp(System metrics, task watchdogs, hardware crash telemetry, NVS LogManager)
│   └── OTA_Driver.cpp        (Background HTTP/HTTPS OTA task, dual-slot validation)
├── L2_Transport/
│   ├── RS485_CH.cpp          (Task_Ch1, Task_Ch2Ch3, Task_Ch4 FreeRTOS worker loops)
│   └── TCP_CH.cpp            (Task_TcpCore0 socket polling and IP whitelist filter)
├── L3_Protocol/
│   ├── Public/
│   │   ├── Packet_Router.cpp (Downlink queue dispatch & horizontal bus routing)
│   │   ├── Device_Registry.cpp(Mutex-protected snapshot API, 0% extern global state leaks)
│   │   ├── Protocol_Diagnostics.cpp(Facade query methods & protocol snapshot mapping)
│   │   └── Modbus_Codec.cpp  (FCU Modbus RTU byte stream encoder & CRC-16 implementation)
│   └── Private/
│       ├── Wallpad_Engine.cpp(Doorphone FSM, guard delays, framing engine implementation)
│       ├── Wallpad_Parser.cpp(Zero-copy span packet parsers)
│       ├── Polling_Registry.cpp(Dynamic polling targets, warm cache & stale_poll_cnt)
│       ├── Auto_Probing.cpp  (Matrix solver & convergence detection)
│       └── Control_Registry.cpp(Blueprint synthesis & action execution)
├── L4_Services/
│   ├── Mgmt_Service.cpp
│   ├── EW11_Service.cpp      (EW11 proxy coordinator with self-contained frame metadata)
│   ├── CLI_Service.cpp       (Telnet virtual stream diagnostic console REPL / TCP Port 23; strictly network-only)
│   ├── Console/              (Console_Commands.cpp, CmdConfig.cpp, CmdDevice.cpp, CmdSystem.cpp, CmdTrace.cpp)
│   └── Mgmt/                 (Mgmt_Rpc.cpp, Mgmt_Telemetry.cpp, Wifi_Manager.cpp)
└── main.cpp                  (Bootstrapping, dependency injection & task launches)
```

#### 0.3.1 Binding Architectural Invariants (Non-Negotiable)

1. **Total Shell-Core Model (100% Information Hiding)**:
   - All external ingress into L3 (from L4) must target **L3 Public headers exclusively**.
   - All external egress from L3 (to L2) must traverse **`Packet_Router::Router_EnqueueDownlink()` exclusively**.
   - L3 Private headers (`Wallpad_Engine.h`, `Polling_Registry.h`, `Control_Registry.h`, etc.) are strictly forbidden from being included by L4 Services or L2 Channels.
2. **Zero Upward Dependencies & Zero Layer Skipping (Strict Pipeline)**:
   - Upward includes ($L_M \rightarrow L_N$ where $M < N$) are strictly prohibited.
   - Vertical runtime calls must follow the strictly adjacent hierarchy: $L4 \rightarrow L3 \rightarrow L2 \rightarrow L1$. Layer skipping ($L3 \rightarrow L1$ or $L4 \rightarrow L2$) is prohibited.
3. **No Middle-Man Semantic Leakage**:
   - L3 scheduler decisions (such as "stale device polling") belong strictly inside L3 (`Polling_Registry::_stale_poll_cnt`). L3 must never pass scheduler semantics down to L2 channels as parameters or delegate metric increments to lower layers.
4. **No Overreaching Metric Invasions**:
   - L3 protocol convergence (`Wallpad_CheckConvergence`) must only stabilize its own cache and signal system milestone `SYS_EVT_CACHE_READY`. It is strictly forbidden for L3 to wipe or reset L1 hardware metrics (`g_pkt_stats`, `g_metrics`).
5. **L0 Base Foundation Soil & Cross-Cutting Platform DIP (Rule 17)**:
   - L0 Foundation (`System_Platform.h`, `System_Config.h`, `System_Buffer.h`) is the universal "Foundation Soil" accessible directly by all tiers ($L1 \sim L4$).
   - Cross-cutting platform concerns (Task WDT, HTTP OTA, Reboot Log in RTC/NVS, CPU/Temp metrics, Network/Transport traffic counters, Trace Sinks, Shutdown Hooks) are declared as abstract C++ contracts in `include/L0_Foundation/System_Platform.h` (`System_*`), implemented in L1 HAL (`Diagnostics_Driver.cpp`, `OTA_Driver.cpp`), and consumed directly by L4 Services.
   - **Zero HAL Pollution in L3**: $L3$ Protocol must never include $L1$ HAL headers. Artificial middle-man passthrough wrappers in $L3$ (`ProtocolDiag_GetSystemSnapshot`, `ProtocolDiag_TaskWdtFeed`, `ProtocolDiag_StartHttpOta`, etc.) are permanently eradicated.
6. **Framing & State Ownership**:
   - Framing engines (`FramingTracker`) belong strictly to their operational domain (`Wallpad_Engine` for Doorphone CH4). Services such as `EW11_Service` must encapsulate their own framing parameters without coupling to L3 core engines.
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

These principles represent the engineering standard established across the Canonical 4+1 Layer architecture rebuild.

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
| **L4 Services (CLI & Mgmt)** | `CLI_Service.cpp`, `Console_Commands.cpp`, `CmdSystem.cpp`, `CmdConfig.cpp`, `CmdDevice.cpp`, `CmdTrace.cpp`, `Mgmt_Service.cpp`, `Mgmt_Rpc.cpp`, `Mgmt_Telemetry.cpp`, `Wifi_Manager.cpp`, `EW11_Service.cpp` | ANSI Telnet FSM; 8-slot ring history; Port 8900 JSON-RPC reactor; Virtual EW11 bridge; zero direct L1/L2 dependencies | **Zero-Heap CLI**, 0 deadlock, direct L0 platform DIP integration |
| **L3 Protocol Engine** | `Packet_Router.cpp`, `Device_Registry.cpp`, `Protocol_Diagnostics.cpp`, `Modbus_Codec.cpp`, `Wallpad_Engine.cpp`, `Wallpad_Parser.cpp`, `Polling_Registry.cpp`, `Auto_Probing.cpp`, `Control_Registry.cpp` | Public Shell / Private Core segregation; `span` zero-copy codecs; dynamic 48-slot polling matrix; 100% pure protocol logic (**0% L1 HAL pollution**) | 0 CRC error, 0 dropped frame, auto protocol matrix solver |
| **L2 Transport & Data Link** | `RS485_CH.cpp`, `TCP_CH.cpp` | Ch1~Ch4 FreeRTOS dedicated timeslot loops; Core 0 non-blocking TCP select reactor; embedded IP whitelist | Strict L2 transport leaves, 0 L3/L4 semantic awareness |
| **L1 Physical HAL Drivers** | `Uart_Driver.cpp`, `Diagnostics_Driver.cpp`, `OTA_Driver.cpp` | Unified HW UART0~2 + SoftwareSerial; NVS `LogManager`; Task WDT monitor; dual-partition rollback engine | Complete HW information hiding, atomic driver metrics |
| **L0 Foundation Soil** | `System_Buffer.cpp`, `System_Config.cpp`, `System_Platform.cpp` | Universal static leaf; `FixedBuf`/`AppendBuf` zero-heap builders; `System_*` abstract platform contracts | Accessible by all layers ($L1 \sim L4$), zero upward dependencies |

---

## 6. Architecture Anti-Patterns vs. Modern Standards (C++23 Standards)

### 6.1 Anti-Patterns & Modern Equivalents Matrix

| Domain | Legacy Anti-Pattern (STRICTLY FORBIDDEN) | Modern Standard (C++23 / GW Canonical Standard) | Rationale & Prevention |
|---|---|---|---|
| **Architecture** | **Upward Include ($L_M \rightarrow L_N, M < N$)**: 하위 계층이 상위 헤더 참조 | **Strict Downlink ($L4 \rightarrow L3 \rightarrow L2 \rightarrow L1$)**: L2는 L3/L4를 일체 모름 | 상위 계층 변경 시 하위 계층 리빌드 방지 및 순환 참조 원천 차단 |
| **Architecture** | **Middle-Man Pass-Through**: L0 Foundation Soil을 L2, L3가 단순 포워딩 래핑 | **Direct Leaf Access**: 모든 계층($L1 \sim L4$)에서 L0 Base 직접 참조 | 무의미한 래퍼 보일러플레이트 제거, 컴파일 최적화 |
| **Architecture** | **Extern State Leak**: 소켓, 채널 뮤텍스, 큐를 `extern`으로 헤더에 노출 | **100% Information Hiding**: `.cpp` 내부 `static` 봉인 후 Snapshot API 제공 | 스레드 경합(Race Condition) 차단, 불변성 보장 |
| **Memory / Hot Path** | **Heap in Hot Path**: 패킷 수신/송신 시 `new`, `malloc`, `String` 사용 | **100% Zero-Heap**: `std::array`, `std::span`, 고정 링버퍼, `AppendBuf` | 24/7/365 가동 시 힙 단편화(Fragmentation)에 의한 패닉/크래시 원천 방지 |
| **Memory / Hot Path** | **Silent Command Drop**: 큐 포화 시 중요 제어 명령을 조용히 누락 | **Deterministic Drop Semantics**: 상태/폴링=Drop-Head, VIP 제어=Drop-Tail + 동기 에러 | 스마트싱스 앱과의 상태 불일치 방지 및 재전송 유도 |
| **Memory / Hot Path** | **Frequent Flash Write**: 런타임 상태 변경마다 NVS 플래시 직기록 | **RTC SRAM Retention + 30s Debounce**: `RTC_NOINIT_ATTR` 캐시 후 지연 커밋 | SPI 플래시 쓰기 수명(Flash Wear) 보존 |
| **Control Flow** | **Chained `if-else` / `strcasecmp`**: 선형 순차 문자열 비교 | **One-shot Lowercase + `constexpr` Table Dispatch**: 정렬 테이블 기반 이진 탐색 | $O(N)$ 문자열 비교 오버헤드 제거, $O(\log N)$ 디스패치 |
| **Control Flow** | **Raw Enum Cast**: `static_cast<uint8_t>(e)` 남발 | **`std::to_underlying(e)` (C++23)** | 가독성 및 타입 변환 안전성 확보 |
| **Control Flow** | **Manual Byte Shift**: `((b[0]<<8)\|b[1])` 또는 비표준 매크로 | **`std::byteswap()` (C++23)** | 하드웨어 가속 내장 함수를 통한 엔디안 변환 최적화 |
| **Control Flow** | **Dummy Return in Unreachable**: default 레이블의 무의미한 더미 리턴 | **`std::unreachable()` (C++23)** | 컴파일러에게 분기 미도달 힌트를 제공하여 불필요한 코드 생성 억제 |
| **Control Flow** | **Ad-hoc Flags & `millis()`**: 분산된 `bool` 플래그와 산발적 시간 비교 | **Explicit FSM**: 명시적 상태 전이 루프로 단일화 | 비결정론적 레이스 컨디션 및 복잡도 제거 |
| **Concurrency** | **Reentrant Lock Trap**: 락 보유 중 소켓 송신 또는 외부 콜백 직접 호출 | **Pending Buffer Queue**: 락 해제 후 대기 버퍼에 적재하여 메인 루프 순차 처리 | Self-Deadlock(자기 교착 상태) 완전 박멸 |
| **Concurrency** | **Single Mutex Monopoly**: 다중 태스크 읽기에도 단일 독점 뮤텍스 사용 | **`std::shared_mutex` (C++17/23)**: 빈번한 읽기=`shared_lock`, 쓰기=`unique_lock` | 태스크 간 불필요한 블로킹 제거 및 처리량 극대화 |
| **Concurrency** | **Infinite Mutex Wait (`portMAX_DELAY`)**: 무제한 락 대기 | **Defensive Timeout**: `MAX_LOCK_HOLD_MS`(최대 50ms) 타임아웃 강제 | 버스 정체 시 시스템 전체 행(Hang) 방지 |

---

### 6.2 Decision Matrix: Table-Driven (테이블화) vs `switch-case` 우선순위 가이드라인

게이트웨이 펌웨어에서는 데이터와 동작의 성격에 따라 테이블화와 `switch-case`의 사용 우선순위를 엄격히 구분한다.

```
                           [분기 설계 선택 기준]
                                     │
                 ┌───────────────────┴───────────────────┐
                 ▼                                       ▼
       "데이터 매핑인가,                        "순차적 FSM 상태 전이인가,
    외부 입력(문자열/패킷)의               컴파일 타임 전수 검사(-Wswitch)가
      핸들러 디스패치인가?"                         핵심인 열거형인가?"
                 │                                       │
                 ▼                                       ▼
    ★ 1순위: Table-Driven (테이블화)             ★ 1순위: switch-case
```

#### 🥇 Table-Driven (테이블화)을 1순위로 사용하는 경우
1. **문자열 기반 커맨드 매칭 (CLI, JSON-RPC)**:
   - C++은 문자열 대상 `switch`가 불가능하므로, **`constexpr` 정렬 테이블 + `std::string_view` 이진 탐색(`std::lower_bound`)**을 표준으로 한다.
   - 예: `ConsoleCommandEntry g_cmd_table[] = { {"clear", ...}, {"info", ...} };`
2. **패킷 헤더 / 커맨드 바이트별 라우팅 (Packet Dispatcher)**:
   - 프로토콜 명령 바이트(`0x31`, `0x41`, `0x42` 등)별로 전담 처리 함수를 호출할 때:
   - 거대 `switch` 대신 **함수 포인터 테이블(`using PacketHandler = void(*)(span<const uint8_t>);`)**을 사용하여, 새 명령 추가 시 기존 코드를 수정하지 않고 테이블에 항목만 추가(개방-폐쇄 원칙 OCP 준수).
3. **하드웨어 핀 / 다차원 설정 매핑 (Configuration Matrix)**:
   - 채널별 GPIO, UART 보드 레이트, 타이머 인터벌 등은 코드 분기가 아닌 `constexpr ChannelConfig g_channel_table[NUM_CH]`로 데이터화.

#### 🥇 `switch-case`를 1순위로 사용하는 경우
1. **유한 상태 머신 (FSM State Transitions)**:
   - 통신 프로토콜 프레임 수신 단계(`WAIT_HEADER` $\rightarrow$ `READ_LEN` $\rightarrow$ `READ_PAYLOAD` $\rightarrow$ `VERIFY_CRC`).
   - Telnet ANSI 이스케이프 시퀀스 파서.
   - **사유**: 테이블화 시 람다/함수 포인터 간 스택 컨텍스트 전달 비용이 발생하지만, `switch-case`는 함수 내부 지역 변수를 직접 접근하며 컴파일러가 최적의 단일 사이클 점프 테이블(`O(1)`)을 생성함.
2. **컴파일 타임 전수 검사가 필요한 열거형 (Exhaustive Enum Check)**:
   - `-Wswitch -Werror=switch` 컴파일러 플래그와 연계하여, 새 열거형 값이 추가되었을 때 미구현 분기를 **컴파일 에러**로 즉시 검출.
3. **순수 Enum $\leftrightarrow$ Name 변환 함수 (Enum to `std::string_view`)**:
   - `[[nodiscard]] constexpr std::string_view getDeviceTypeName(DeviceType type) noexcept`
   - 플래시 메모리에 포인터 배열을 유지하는 것보다 `switch-case` + `std::unreachable()`을 인라인하는 것이 캐시 히트율과 바이너리 크기 면에서 최적.

> **[골든 룰 (Golden Rule)]**:
> - **동작(함수)을 외부 입력과 결합하거나 데이터를 정형화할 때는 테이블화(Table-Driven)**를 우선한다.
> - **순차적 FSM 상태 흐름을 다루거나 Enum의 완전성을 컴파일러로 강제할 때는 `switch-case`**를 우선한다.
> - **단, `case` 내부에 수십 줄 이상의 복잡한 비즈니스 로직을 인라인하는 "God Switch"는 엄격히 금지**하며, 반드시 개별 핸들러 함수로 분리 후 호출한다.


