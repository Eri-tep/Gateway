# GW Home Gateway Architecture, Specifications & Design Philosophy

This document defines the system specifications, runtime topology, channel mappings, deadlock-free concurrency/locking hierarchies, and the **Canonical 4+1 Layer Architecture & Implementation Standards**.

---

---

### 0. Canonical Clean Architecture Topology (4-Tier + L0 Foundation Soil & L3 Shell-Core)

> **Mandatory Architectural Standard (v4.2.0 Canonical Shell-Core & DIP Standard - Firmware v2.2.1)**:
- **L2 Pure Transport Leaf Invariant**: `RS485_CH` and `TCP_CH` act strictly as L2 Transport Leaves. They manage physical UART/Socket I/O, ring buffers, timeslots, and hardware tasks. They possess **0% awareness of L3 device state registries or L4 listeners**. All packet boundary validation and state updates belong strictly in L3.
- **Top-Down Downlink / Pull-Pop Pipeline**: Downlink requests flow strictly $L4 \rightarrow L3 \rightarrow L2 \rightarrow L1$. Uplink reception operates via top-down polling/pulling from thread-safe channel queues without upward callback hooks.
- **Zero Upward Include**: Higher layers must never be included by lower layers ($L_M \rightarrow L_N$ where $M < N$ is strictly forbidden).
- **Embedded Stability & Benchmark Specification**: Detailed ESP32-S3 cycle-accurate benchmark harness, 12 stability pillars (P0~P11), 6-phase harness topology, and CPU optimization specifications are canonically governed in [`docs/EMBEDDED_STABILITY_AND_BENCHMARK_SPECIFICATION.md`](EMBEDDED_STABILITY_AND_BENCHMARK_SPECIFICATION.md).

#### 0.1 Layer Hierarchy & Responsibilities
> 1. **Foundation Soil (L0 Base Leaf)**: Universal static foundation (`System_Buffer.h`, `System_Config.h`, `System_Platform.h`). Zero upward dependencies; accessible directly by any layer ($L1 \sim L4$). Acts as the **universal pure foundation leaf** defining global types, synchronization primitives, abstract platform service contracts (`System_*`), and sealed system state accessors (`System_MarkStage`, `System_IsRescueMode`, `System_IsRollbackDetected`, `System_SetRollbackDetected`). All higher-domain hooks (e.g. protocol convergence flags) are permanently eradicated from L0.
> 2. **L1 Physical HAL Drivers**: Hardware abstractions (`Uart_Driver`, `Diagnostics_Driver`, `OTA_Driver`, `Wifi_Driver`). Complete information hiding. Exclusively implements physical driver health, dual-partition OTA engine, NVS/RTC reboot logs, Wi-Fi connectivity/reconnect FSM (bound to L0 `System_Wifi*`), and hardware telemetry. 4 canonical drivers maintain strict cohesion and 1:1 header-source parity.
> 3. **L2 Transport & Data Link Channels**: Raw frame transport, timeslot scheduling, and socket polling (`RS485_CH`, `TCP_CH`). Pure transport leaves; zero awareness of L3 device state, scheduler strategies, or L4 listeners. Consumes upper-layer requests strictly via injected raw DI callbacks (`onTakeRelearnRequest`, `onCheckConvergence`, `onBuildPoll`).
> 4. **L3 Routing, Subsystem & Shell-Core Engine**:
>    - **L3 Public Shell (External Boundary Gateways & Facades)**:
>      - `Protocol_Device`: SSOT device repository, decoupled control/state ingress, and event dispatch.
>      - `Protocol_Facade`: Pure protocol & routing facade for L4 CLI/RPC and L2 transport (strictly zero hardware HAL includes; zero non-protocol state; read-only snapshots, route lookups, dispatch & `ProtocolDiag_RequestRelearn()`).
>    - **L3 Private Core (100% Encapsulated Engines)**:
>      - `Routing_Engine`: Sole internal packet routing, address remapping, and bus forwarding engine.
>      - `Wallpad_Engine`: Internal protocol FSM, zero-copy packet parser (`std::span` hygiene), and doorphone state machine (`FramingTracker` sealed here; sole owner of protocol convergence & relearn state `s_relearn_requested`).
>      - `Wallpad_Learning`: Collocated learning & registration SSOT engine (dynamic polling targets, warm cache, auto-probing heuristic matrix solver, control blueprints/coverage, and sole owner of `stale_poll_cnt`).
>      - `Fcu_Engine`: Dedicated FCU Modbus-RTU protocol engine & register handler with C++23 Endian abstractions.
> 5. **L4 Application Services**: High-level orchestrators (`CLI_Service`, `Mgmt_Service`). Interacts strictly with L3 Public for protocol needs, and consumes cross-cutting platform capabilities directly via L0 `System_Platform.h`. Possesses 0% access to L3 Private or L2 Channels.

```
include/
├── L0_Foundation/                  [L0: Pure Foundation Soil Leaf]
│   ├── System_Buffer.h       (AppendBuf fixed scratch buffers, Endian abstractions, zero-heap utilities)
│   ├── System_Config.h       (NVS keys, baud rates, timing constants, monadic parsers)
│   └── System_Platform.h     (Abstract System_* platform contracts, StaticPacket, trace sinks)
├── L1_HAL/                         [L1: Physical HAL Drivers]
│   ├── Uart_Driver.h         (Unified HW UART0~2 + Doorphone SW Serial HAL, strongly-typed toEnum)
│   ├── Diagnostics_Driver.h  (Heap/stack watermarks, NVS LogManager, Ch1StateMetrics)
│   ├── OTA_Driver.h          (Dual-partition rollback, rescue AP recovery, HttpOtaState)
│   └── Wifi_Driver.h         (Physical Wi-Fi HAL driver & System_Wifi* platform binding)
├── L2_Transport/                   [L2: Transport & Data Link Channels]
│   ├── RS485_CH.h            (Ch1~Ch4 serial channel manager, FreeRTOS timeslot loops, std::span callbacks)
│   ├── TCP_CH.h              (Core 0 TCP reactor, socket FSM, embedded IPFilter)
│   └── Bridge_CH.h           (Ch5/Ch6 EW11 TCP bridge channels, atomic slot online & socket proxy)
├── L3_Protocol/                    [L3: Routing, Subsystem & Shell-Core Hub]
│   ├── Public/               [L3 Public Shell: External Gateways for L4 & L2]
│   │   ├── Protocol_Device.h (SSOT device state repository, constexpr kFcuActions binary search table)
│   │   └── Protocol_Facade.h (Thread-safe read-only protocol diagnostic snapshots, routing facade & entry point)
│   └── Private/              [L3 Private Core: 100% Internal Subsystem Engines]
│       ├── Routing_Engine.h  (Internal device routing table, forward dispatch & channel lookup)
│       ├── Wallpad_Engine.h  (Internal protocol FSM, framing parser & Doorphone engine, std::span hygiene)
│       ├── Wallpad_Learning.h(Polling registry, warm cache, auto-probing matrix solver & control blueprints)
│       └── Fcu_Engine.h      (FCU Modbus-RTU protocol engine & register handler, C++23 Endian)
└── L4_Services/                    [L4: Application Services]
    ├── CLI_Commands.h        (Unified CLI command declarations, std::span<const SubCmdDef> dispatcher)
    ├── CLI_Service.h         (Telnet virtual stream diagnostic console REPL / TCP Port 23 & tracer)
    └── Mgmt_Service.h        (Port 8900 JSON-RPC remote bridge, telemetry & session coordinator)

src/
├── L0_Foundation/
│   ├── System_Buffer.cpp     (Fast Hex LUT, Endian verify static_asserts, elapsed time utilities)
│   ├── System_Config.cpp     (NVS configuration loader/writer & defaults)
│   └── System_Platform.cpp   (Platform synchronization & decoupled trace message/packet sinks)
├── L1_HAL/
│   ├── Uart_Driver.cpp       (HW UART & SoftwareSerial fully sealed via file-static scope)
│   ├── Diagnostics_Driver.cpp(Trace/shutdown hooks, metrics tracker, task handles, Task WDT, NVS reboot log)
│   ├── OTA_Driver.cpp        (Background HTTP/HTTPS OTA task, dual-slot validation, URL trust policy)
│   └── Wifi_Driver.cpp       (WiFi event handler, reconnect FSM & System_Wifi* binding)
├── L2_Transport/
│   ├── RS485_CH.cpp          (Task_Ch1, Task_Ch2Ch3, Task_Ch4 FreeRTOS worker loops, std::span packet slices)
│   ├── TCP_CH.cpp            (Task_TcpCore0 socket polling and IP whitelist filter)
│   └── Bridge_CH.cpp         (Ch5/Ch6 socket connections & non-allocating bridge I/O)
├── L3_Protocol/
│   ├── Public/
│   │   ├── Protocol_Device.cpp(Mutex-protected snapshot API, 0% extern global state leaks)
│   │   └── Protocol_Facade.cpp(Facade query methods & protocol snapshot mapping, single entry point)
│   └── Private/
│       ├── Routing_Engine.cpp(Downlink queue dispatch & horizontal bus routing, exhaustive enum switch)
│       ├── Wallpad_Engine.cpp(Doorphone FSM, guard delays, framing engine & zero-copy parser)
│       ├── Wallpad_Learning.cpp(Unified dynamic polling targets, matrix solver & blueprint execution)
│       └── Fcu_Engine.cpp    (FCU Modbus-RTU frame processing & register snapshot management)
└── L4_Services/
    ├── CLI_Service.cpp       (Telnet session engine, command routing table & 5KB scratch buffer)
    ├── Cli/
    │   ├── Cli_CmdConfig.cpp (Modularized config management subcommands)
    │   ├── Cli_CmdDevice.cpp (Modularized device/blueprint control subcommands)
    │   ├── Cli_CmdSystem.cpp (Modularized system status/reboot/OTA subcommands)
    │   └── Cli_CmdTrace.cpp  (Modularized real-time packet trace subcommands)
    ├── Mgmt_Service.cpp      (HTTP/WS/JSON-RPC management coordinator & WebServer loop)
    └── Mgmt/
        ├── Mgmt_Rpc.cpp      (JSON-RPC 2.0 command parser & action dispatch)
        └── Mgmt_Telemetry.cpp(Telemetry metrics & device state broadcast, 100% enum coverage)
└── main.cpp                  (Bootstrapping, dependency injection & task launches)
```

#### 0.3.1 Binding Architectural Invariants (Non-Negotiable)

1. **Total Shell-Core Model (100% Information Hiding)**:
   - All external ingress into L3 (from L4) must target **L3 Public headers exclusively (`Protocol_Device.h`, `Protocol_Facade.h`)**.
   - All external egress from L3 (to L2) must traverse **`Protocol_Facade` 및 `Routing_Engine`**을 통해 단일 진입 경로로 캡슐화되어 전달된다.
   - L3 Private headers (`Wallpad_Engine.h`, `Wallpad_Learning.h`, `Routing_Engine.h`, `Fcu_Engine.h`) are strictly forbidden from being included by L4 Services or L2 Channels.
2. **Zero Upward Dependencies & Zero Layer Skipping (Strict Pipeline)**:
   - Upward includes ($L_M \rightarrow L_N$ where $M < N$) are strictly prohibited.
   - Vertical runtime calls must follow the strictly adjacent hierarchy: $L4 \rightarrow L3 \rightarrow L2 \rightarrow L1$. Layer skipping ($L3 \rightarrow L1$ or $L4 \rightarrow L2$) is prohibited.
3. **No Middle-Man Semantic Leakage**:
   - L3 scheduler decisions (such as "stale device polling") belong strictly inside L3 (`Wallpad_Learning::_stale_poll_cnt`). L3 must never pass scheduler semantics down to L2 channels as parameters or delegate metric increments to lower layers.
4. **No Overreaching Metric Invasions**:
   - L3 protocol convergence (`Wallpad_CheckConvergence`) must only stabilize its own cache and signal system milestone `SYS_EVT_CACHE_READY`. It is strictly forbidden for L3 to wipe or reset L1 hardware metrics (`g_pkt_stats`, `g_metrics`).
5. **L0 Base Foundation Soil & Cross-Cutting Platform DIP (Rule 17)**:
   - L0 Foundation (`System_Platform.h`, `System_Config.h`, `System_Buffer.h`) is the universal "Foundation Soil" accessible directly by all tiers ($L1 \sim L4$).
   - Cross-cutting platform concerns (Task WDT, HTTP OTA, Reboot Log in RTC/NVS, CPU/Temp metrics, Network/Transport traffic counters, Trace Sinks, Shutdown Hooks, Wi-Fi contracts) are declared as abstract C++ contracts in `include/L0_Foundation/System_Platform.h` (`System_*`), implemented in L1 HAL (`Diagnostics_Driver.cpp`, `OTA_Driver.cpp`, `Wifi_Driver.cpp`), and consumed directly by L4 Services.
   - **Zero Domain Hooks in L0**: Higher-domain flags (such as protocol convergence `g_probe_convergence_reset`) are permanently eradicated from L0. Protocol state is 100% sealed inside L3 Private (`Wallpad_Engine::s_relearn_requested`). L4 requests relearn via L3 Public facade (`ProtocolDiag_RequestRelearn()`), and L2 polls it via injected DI callback (`onTakeRelearnRequest`).
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
11. **Flash Endurance Protection & Sealed System State via RTC SRAM**:
    - Dynamic polling cache, probing matrix, and volatile runtime tracking are preserved across soft resets and WDT reboots in RTC Fast/Slow SRAM (`RTC_NOINIT_ATTR`). Flash NVS commits are strictly debounced and executed only upon cache convergence (`SYS_EVT_CACHE_READY`) or explicit shutdown.
    - System state variables (crash stage marker `s_stage_marker`, rescue flag `s_rescue_mode`, rollback flag `s_rollback_detected`) are 100% sealed as file-static variables inside L1 HAL (`Diagnostics_Driver.cpp`), exposed exclusively via L0 abstract contracts (`System_MarkStage`, `System_IsRescueMode`, `System_IsRollbackDetected`, `System_SetRollbackDetected`). All `extern` leaks for these variables are permanently eliminated.
12. **Microsecond-Level Hot-Path Determinism & Lock Churn Elimination**:
    - **Zero Heap & Direct Singleton Access**: Hot RX/TX execution paths (`Task_Ch1`, `Task_Ch2Ch3`) operate with 0% dynamic allocation. Parser factories and multi-hop delegates are permanently eliminated; core protocol engines are accessed via direct singleton instances (`Universal_GetEngine()`).
    - **Single-Scoped Critical Section (Zero Lock Churn)**: Packet verification, cache touch, and response updates must execute within a single unified critical section (`portENTER_CRITICAL` / `MutexLocker`). Repeated acquire-release cycles ("lock churn") within a single packet lifecycle are strictly forbidden, reducing WCET by 30~50 µs.
    - **Hard Real-Time Budget**: `Task_Ch1` peak WCET must not exceed 300 µs (empirical baseline: 290.7 µs / 69,776 cycles @ 240MHz). Over 90% of bus packets must be completely processed within 136.5 µs (< 32,768 cycles).
13. **Canonical 5-Tier API & Callback Topology (Vertical, Horizontal & Inversion Taxonomy)**:
    - **Category 1 (Downlink Command Dispatch)**: Subcommands and RPCs must utilize static, immutable jump tables (`kRpcDispatchTable`, `SubCmdDef`). High-throughput dispatchers must employ compile-time `constexpr` FNV-1a 32-bit hash evaluation with `switch-case` jumps to replace $O(N)$ string comparisons with $O(1)$ integer comparisons.
    - **Category 2 (Platform Lifecycle DIP Hooks)**: System-wide panic, crash, reboot, and pre-OTA notifications must route through a unified `SystemLifecycleEvent` observer registry. Hardware WDT feeding must be statically linked or inlined with zero runtime pointer indirection.
    - **Category 3 (Transport-to-Protocol Inversion SPI)**: Lower-layer drivers (`RS485_CH`, `Bridge_CH`, `TCP_CH`) must invert dependencies to upper-layer protocols strictly via zero-vtable C++ SPI structures (`RS485_PacketDispatcher`, `ReactorParticipant`). All dispatchers must enforce an **Infallible Contract** validated once at boot; repetitive `nullptr` checks inside hot loops are strictly forbidden. L2 Bridge channels must normalize multi-slot dispatching into a uniform driver array (`BridgeSlotDriver[MAX_SLOTS]`).
    - **Category 4 (Intra-Layer Coupling - True vs. False Decoupling)**: Components residing in the same architectural layer (e.g., $L3$ `Wallpad_Engine` and `Protocol_Device`) must link directly at compile-time. Utilizing runtime function pointers, callbacks, or registration hooks between modules of the same layer constitutes "Anemic Decoupling" and is strictly prohibited.
    - **Category 5 (Uplink State Propagation - Asynchronous Queue Isolation)**: Domain state changes ($L3$) destined for network/management services ($L4$) must never execute service callbacks synchronously on the real-time core. All uplink state events must be enqueued as plain telemetry structs into a dedicated FreeRTOS static queue residing at the foundation/transport layer ($L0/L2$), completely isolating Core 1 real-time stacks from Core 0 network latency.
14. **Callback Hygiene & Execution Context Contract (RLBC Principle)**:
    - **Release Lock Before Callback (RLBC)**: Lower-level modules must never invoke upper-level callbacks, listeners, or SPI hooks while holding a mutex or spinlock. The sequence must strictly follow: **Snapshot State under Lock $\rightarrow$ Release Lock $\rightarrow$ Invoke Callback/Enqueue Event**.
    - **Execution Context Contract**: Every callback signature must explicitly document and adhere to its execution context:
      - *Hot-Path Callbacks (Core 1)*: Run in `Task_Ch1` context; `noexcept`; execution time budget $< 10\,\mu\text{s}$; strictly 0 heap allocation, 0 FreeRTOS blocking, 0 network socket I/O.
      - *Cold-Path Observers (Core 0)*: Run in `Task_Net` or `Task_Telnet` context; asynchronous consumer queue processing.

---

### 0.4 Framework & Toolchain Environment Specifications
- **Target Hardware**: M5Stack AtomS3 Lite (ESP32-S3FN8, 240MHz Dual-Core, 320KB SRAM, 8MB Flash)
- **Framework**: `framework-arduinoespressif32 @ 3.1.3` (Arduino-ESP32 Core v3.1.x)
- **Underlying SDK / ESP-IDF**: **ESP-IDF v5.3.2** (`ESP_IDF_VERSION_VAL(5, 3, 2)`)
- **Toolchain**: `xtensa-esp-elf-gcc / g++ 13.2.0 (crosstool-NG esp-13.2.0_20240530)`
- **C++ Standard**: **C++23** (`-std=gnu++23`)
- **Exception Model**: **Exceptions Disabled (`-fno-exceptions`)** (Zero runtime exception overhead; `std::terminate()` on unhandled abort)

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

### 1.3 Execution Path Tiering Architecture Specification (Hot / Warm / Cold Path 3계층 실행 경로 규격)

임베디드 게이트웨이 시스템의 실시간 결정론(Real-Time Determinism), 대용량 처리량(Throughput), 그리고 데이터 무결성(Data Integrity)을 동시에 보장하기 위해 전체 실행 경로를 **핫패스(Hot Path)**, **웜패스(Warm Path)**, **콜드패스(Cold Path)**의 3계층으로 엄격히 분리하고 경로별 엔지니어링 불변식을 강제합니다.

#### 1.3.1 Hot Path: 실시간 I/O, 초저지연, 타이밍 결정론 (Timing Determinism & Zero Jitter)
- **목표 지연시간**: $\le 10\,\mu s$ (Zero Jitter)
- **적용 대상**: UART ISR, RS-485 물리 슬롯 타이밍 제어 (`Task_Ch1`, `Task_Ch2Ch3` 마스터/슬레이브 송수신 루프)
- **4대 최적화 불변식**:
  1. **동기화 및 락 (Synchronization & Lock)**:
     - **스핀락/뮤텍스 배제**: 무한 스핀락 및 멀티코어 버스 락을 배제하고, `std::atomic<uint32_t>` 기반의 Wait-free / Lock-free SPSC(Single Producer Single Consumer) 원형 링버퍼 적용. `memory_order_release` (생산자) / `memory_order_acquire` (소비자)로 오버헤드 최소화.
     - **Direct Task Notification**: `xQueueSendFromISR` 등 무거운 FreeRTOS 큐(150~300 사이클)를 배제하고, 데이터는 SPSC 링버퍼에 즉시 적재하며 웨이크업은 Direct Task Notification (`vTaskNotifyGiveFromISR`, `ulTaskNotifyTake`, 15~30 사이클)으로 처리.
     - **임계 영역 최소화**: `taskENTER_CRITICAL` 체류 시간은 수 $\mu s$ 이내로 제한하며, 임계 영역 내부에서는 원자적 인덱스 교환 및 플래그 갱신만 수행하고 패킷 복사나 파싱은 임계 영역 외부에서 실행.
  2. **메모리 계층 및 캐시 (Memory Hierarchy & Cache)**:
     - **Zero Dynamic Allocation**: 런타임 `malloc`, `free`, `new`, `delete` 및 동적 문자열 할당 영구 금지. 컴파일 타임 고정 크기 정적 메모리(`StaticPacket`, `std::array`)만 참조.
     - **내부 SRAM 완전 상주 (`IRAM_ATTR` / `DRAM_ATTR`)**: 플래시(XIP) 캐시 미스 스톨(Stall) 및 콜드패스 플래시 Erase/Write 중 Cache Invalidation으로 인한 CPU 패닉을 방지하기 위해, 핫패스 함수 및 ISR은 `IRAM_ATTR`(.iram0.text), LUT(Lookup Table) 및 CRC 테이블은 `DRAM_ATTR`로 내부 SRAM에 고정 배치.
     - **거짓 공유(False Sharing) 차단**: 링버퍼의 `write_head`와 `read_tail` 등 멀티코어 경합 변수는 `alignas(64)` 캐시 라인 정렬 패딩을 부여하여 물리 캐시 라인 간섭 차단.
  3. **제어 흐름 및 CPU 파이프라인 (Execution Flow & Pipeline)**:
     - **루프 불변식 레지스터 호이스팅(Register Hoisting)**: 구조체 포인터 멤버 간접 역참조(`pRing->head != pRing->tail`)를 배제하고 로컬 스택 변수로 레지스터 호이스팅하여 CPU 레지스터(Xtensa a2~a7)에서 초고속 처리 후 루프 종료 시점에 단 1회 메모리 반영.
     - **분기 예측 최적화 & Branchless**: 분기 예측 실패(15~20 사이클 플러시) 방지를 위해 지배적인 패킷 수신 경로에 C++20 `[[likely]]` / `[[unlikely]]`를 명시하고, 체크섬/비트 연산은 Branchless 비트 마스킹으로 치환.
     - **하드웨어 FIFO & DMA 연동**: 상태 레지스터 비지 폴링을 배제하고 UART 하드웨어 FIFO 워터마크 인터럽트 및 DMA 컨트롤러 활용.
  4. **런타임 및 I/O (Runtime, FPU & I/O)**:
     - **I/O 포맷팅 절대 금지**: `printf`, `sprintf`, `ESP_LOG` 등 C 표준 라이브러리 포맷팅 출력은 스택 과소비 및 블로킹을 유발하므로 핫패스 내 완전 금지. 원시 바이너리 텔레메트리 큐 기록 후 백그라운드로 오프로드.
     - **부동소수점(FPU) 배제**: 컨텍스트 스위칭 시 FPU Lazy Stacking 오버헤드를 막기 위해 float/double 연산을 금지하고 정수 비트 시프트 또는 고정소수점(Fixed-point) 산술로 통일.
     - **가상 함수(vtable) 제거**: 런타임 간접 함수 포인터 역참조 및 인라이닝 차단을 방지하기 위해 CRTP(Curiously Recurring Template Pattern) 또는 C++20 Concepts 기반 정적 다형성 적용.

#### 1.3.2 Warm Path: FSM 상태 갱신, 패킷 디코딩, 처리량 및 배압 제어 (Throughput, Flow & Backpressure)
- **목표 지연시간**: 수 $ms \sim$ 수십 $ms$
- **적용 대상**: `Task_Broker`, `Task_Protocol`, `TcpReactor::runTask` (패킷 파싱, FSM 상태 머신 전이, TCP 스트리밍 I/O)
- **5대 핵심 설계 원칙**:
  1. **배압 제어 및 버퍼 관리 (Backpressure Strategy)**:
     - **상태 압축 (State Coalescing / Deduplication)**: 주기적 상태 폴링 패킷 인입 시 동일 기기 ID의 이전 슬롯을 최신 상태로 덮어써 큐 폭발 방지.
     - **오래된 데이터 드롭 (Drop-oldest)**: 버퍼 만충 시 최신 패킷 우선 반영 및 가장 오래된 비제어 데이터 드롭. 단, 제어 명령(Command Queue)은 별도 분리하여 무손실 보장.
     - **큐 수신 타임아웃 명시**: `portMAX_DELAY` 무한 블로킹을 지양하고 명시적 타임아웃(예: `pdMS_TO_TICKS(100)`)을 설정하여 통신 두절 감지 및 Task Watchdog(TWDT) 정기 피딩.
  2. **동시성 및 스케줄링 (Concurrency & Priority)**:
     - **우선순위 역전(Priority Inversion) 방지**: 공유 자원 동기화 시 일반 바이너리 세마포어가 아닌 **우선순위 상속(Priority Inheritance) 메커니즘이 내장된 FreeRTOS Mutex(`xSemaphoreCreateMutex`)** 필수 사용.
     - **우선순위 티어링(Priority Tiering)**: $\text{ISR (최고)} > \text{Hot Path (P5\sim P6)} > \text{Warm Path (P4)} > \text{Cold Path (P1\sim P2)}$.
     - **CPU 독점 방지 및 협력적 양보**: 대량 패킷 파싱 루프 중간에 이벤트 기반 블로킹 또는 명시적 `taskYIELD()`, `vTaskDelay(1)`를 배치하여 하위 태스크 기아 방지.
  3. **메모리 수명 및 자원 관리 (Memory Lifecycle)**:
     - **동적 메모리 단편화 차단**: 빈번한 `malloc`/`free`나 `std::string` 조합을 금지하고, 컴파일 타임 고정 크기 메모리 풀(Object Pool) 또는 정적 순환 버퍼 재사용.
     - **Zero-Copy 핸드오버 및 소유권 명확화**: 핫패스 링버퍼 $\rightarrow$ 웜패스 FSM $\rightarrow$ TCP 송신 간 복사를 최소화하고, 버퍼 슬롯의 인덱스 소유권 및 해제(Release) 생명주기를 엄격히 추적.
  4. **이벤트 필터링 및 디바운싱 (Event Throttling)**:
     - **상태 중복 전송 억제 (Deduplication)**: 내부 그림자 상태 캐시(Shadow State Table)를 유지하여 실제 물리 상태 변경(Delta)이 발생한 경우에만 상위 플랫폼(SmartThings / TCP)으로 이벤트 방출.
     - **상위 플랫폼 플러딩 방지**: SmartThings 허브 메시지 큐 오버플로우 방지를 위한 디바운스/쓰로틀 타이머(300~500$ms$ Window) 적용. Edge Driver(Lua `cosock`)에서도 비차단 타이머로 이벤트 발행 속도 제어.
  5. **오류 격리 및 관측 가능성 (Fault Isolation & Observability)**:
     - **오류 국소화 (Drop & Resync)**: 패킷 CRC 오류나 프레임 깨짐 발생 시 태스크 패닉 없이 해당 프레임만 안전 폐기하고 다음 동기화 헤더(`0xF7` 등)로 복구.
     - **레이트 리밋(Rate-limited) 로깅**: 반복 오류 발생 시 로그 스팸으로 인한 UART/플래시 고갈을 방지하기 위해 에러 로깅 주기(Rate Limit) 제어.
     - **헬스 메트릭 수집**: Throughput, Queue High-Watermark, Drop 패킷 카운터를 원자적으로 누적하여 콜드패스 진단 서비스에 비간섭 스냅샷 제공.

#### 1.3.3 Cold Path: 부팅/초기화, 설정 영속화, 네트워크 페어링, 무결성 & 불간섭 (Integrity, Safe Fallback & Non-Interference)
- **목표 지연시간**: 수백 $ms \sim$ 수 초 허용
- **적용 대상**: `Task_Telnet` (CLI 진단 콘솔), `Task_Mgmt` (NVS 설정 영속화, Wi-Fi/OTA, 시스템 헬스 모니터)
- **5대 핵심 설계 원칙**:
  1. **핫/웜패스 불간섭 원칙 (Isolation & Non-Interference)**:
     - **SPI 플래시/NVS 쓰기 시 I-Cache Stall 차단**: NVS 섹터 Erase/Write 중 하드웨어 I-Cache 비활성화에 대비하여 Hot Path가 SRAM(`IRAM_ATTR`)에 상주함을 보증하고, 런타임 쓰기 작업은 통신 트래픽이 비는 유휴 시점(Idle Window)으로 지연(Deferred Commit) 처리.
     - **스케줄링 우선순위 최하위 격리**: 시스템 최하위 우선순위(Priority 1~2)로 배치하여 실시간 버스 제어 태스크를 절대 선점하지 못하도록 격리.
  2. **방어적 설계 및 데이터 무결성 (Integrity & Fault Tolerance)**:
     - **원자적 저장 및 전원 차단 내성(Power-cut Tolerance)**: 설정 데이터는 A/B 뱅크 교대 기록 및 CRC-16/32 체크섬 검증을 적용하여 기록 도중 정전 발생 시에도 데이터 무결성 보존.
     - **안전한 공장 초기화 폴백 (Safe Fallback)**: NVS 손상 또는 CRC 불일치 시 패닉 없이 ROM/플래시 기본 설정(Factory Default)으로 자동 복구.
     - **입력값 전수 유효성 검증 (Defensive Validation)**: CLI 명령어, Web/App 설정값, OTA 메타데이터는 허용 범위(Range check)와 스키마를 엄격히 전수 검증.
  3. **워치독 관리 및 점진적 처리 (WDT & Cooperative Yielding)**:
     - **태스크 워치독(TWDT) 만료 방지**: 대용량 NVS 플러시, Flash Erase, TLS 핸드셰이크 등 수백 $ms$ 이상 소요 작업은 블록 단위 분할(Chunking) 및 매 스텝마다 명시적 `vTaskDelay(pdMS_TO_TICKS(1))` 삽입으로 WDT 리셋 및 타 태스크 기아 방지.
     - **재시도 지수 백오프 (Exponential Backoff)**: Wi-Fi 및 업스트림 소켓 재연결 실패 시 $1s \rightarrow 2s \rightarrow 4s \dots \max 60s$ 지연 적용으로 자원 낭비 차단.
  4. **메모리 관리 및 누수 차단 (Resource Teardown)**:
     - **임시 자원의 완전한 해제 (Zero-leak via RAII)**: cJSON, 문자열 포맷팅 등 동적 힙 사용 후 예외 분기를 포함한 모든 경로에서 C++ RAII 기반 100% 자원 및 디스크립터 해제.
     - **힙 단편화(Fragmentation) 예방**: 수 분 단위 주기 실행 작업은 동적 할당을 지양하고 부팅 시 1회 고정 할당 또는 정적 재사용 버퍼 채택.
  5. **풍부한 관측 가능성 (Rich Diagnostics & Logging)**:
     - 상세 텍스트 포맷팅 로깅(`printf`, `ESP_LOGI`)을 적극 허용하여 부팅 시퀀스, 연결 상태, 스택 여유량(`uxTaskGetStackHighWaterMark`), 시스템 최소 힙 잔여량(`esp_get_minimum_free_heap_size`)의 상세 추적성 확보.

#### 1.3.4 Hot - Warm - Cold 통합 토폴로지 및 비교 매트릭스

```mermaid
flowchart TD
    subgraph HotPath["[핫패스 (Hot Path)] 지연시간: <= 10us (Zero Jitter)"]
        HW["RS-485 / UART HW FIFO"] -->|ISR + GDMA| ISR["UART ISR (IRAM_ATTR)"]
        ISR -->|Wait-free SPSC / Direct Notify| HotTask["Task_Ch1 / Task_Ch2Ch3 (Priority 5~6)"]
        HotTask -.->|Static DRAM / No Heap / Branchless| HotTask
    end

    subgraph WarmPath["[웜패스 (Warm Path)] 지연시간: 수 ms ~ 수십 ms"]
        HotTask -->|Zero-Copy Slot Handover| WarmWorker["Task_Broker / Task_Protocol (Priority 4)"]
        WarmWorker -->|FSM Update / State Deduplication| StateTable["Shadow State Cache"]
        WarmWorker -->|Rate Limiting / Backpressure| TCPBridge["TCP Transport Bridge"]
    end

    subgraph ColdPath["[콜드패스 (Cold Path)] 지연시간: 수백 ms ~ 수 초"]
        WarmWorker -.->|Deferred Event / Metrics| ColdMgmt["Task_Mgmt / Task_CLI (Priority 1~2)"]
        ColdMgmt -->|Atomic Write + CRC| NVS["NVS / Flash Storage"]
        ColdMgmt -->|Cooperative Yield + WDT Feed| CLI["CLI Console / Web OTA"]
    end

    ColdMgmt -.->|Config Snapshot (Read-Only)| HotTask
```

| 검토 영역 | 핫패스 (Hot Path) | 웜패스 (Warm Path) | 콜드패스 (Cold Path) |
|---|---|---|---|
| **설계 목표** | 극저지연, 타이밍 결정론 (Zero Jitter) | 높은 처리량, 배압 제어, 공존 (Flow) | 데이터 무결성, 고장 복구력, 불간섭 (Safety) |
| **지연 허용치** | $\le 10\,\mu s$ | 수 $ms \sim$ 수십 $ms$ | 수백 $ms \sim$ 수 초 |
| **스케줄링 티어** | ISR / Priority 5~6 (High) | Priority 4 (Core Worker) | Priority 1~2 (Background) |
| **동기화 기법** | Wait-free SPSC, Direct Task Notify | Mutex (우선순위 상속), FreeRTOS Queue | EventGroup, Bounded Delay, 블로킹 I/O |
| **메모리/캐시** | `IRAM_ATTR`, SRAM 고정, `alignas(64)` | 고정 크기 풀, Zero-Copy 슬롯 | RAII 힙 할당 허용, 단편화 방지 정적 버퍼 |
| **제어 흐름** | Branchless, 레지스터 호이스팅, DMA | 상태 압축(Coalescing), Drop-oldest, FSM | 루프 Chunking, 지수 백오프, 안전 폴백 |
| **런타임/IO** | 정수/고정소수점, CRTP 인라인, I/O 절대 금지 | Rate-limited 로깅, 통계 카운터 원자적 갱신 | 상세 텍스트 로깅(`printf`), NVS/SPI 플래시 기록 |
| **워치독(WDT)** | 인터럽트 마스크 최소화 | 대기 타임아웃 명시, `taskYIELD()` 협력 양보 | 블록 분할 처리, 루프 내 명시적 WDT 피딩 |
| **최우선 가치** | **결정론적 타이밍 (Timing Determinism)** | **처리량 및 시스템 안정성 (Throughput & Flow)** | **데이터 무결성 및 복구력 (Integrity & Safety)** |

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
| **L4 Services (CLI & Mgmt)** | `CLI_Service.cpp`, `Cli_CmdConfig.cpp`, `Cli_CmdDevice.cpp`, `Cli_CmdSystem.cpp`, `Cli_CmdTrace.cpp`, `Mgmt_Service.cpp`, `Mgmt_Rpc.cpp`, `Mgmt_Telemetry.cpp` | ANSI Telnet FSM; 8-slot ring history; Port 8900 JSON-RPC reactor; zero direct L1/L2 dependencies | **Zero-Heap CLI**, 0 deadlock, direct L0 platform DIP integration |
| **L3 Protocol Engine** | `Protocol_Router.cpp`, `Protocol_Device.cpp`, `Protocol_Facade.cpp`, `Wallpad_Engine.cpp`, `Wallpad_Learning.cpp`, `Fcu_Engine.cpp` | Public Shell / Private Core segregation; `span` zero-copy codecs; dynamic 48-slot polling matrix; 100% pure protocol logic (**0% L1 HAL pollution**) | 0 CRC error, 0 dropped frame, auto protocol matrix solver |
| **L2 Transport & Data Link** | `RS485_CH.cpp`, `TCP_CH.cpp`, `Bridge_CH.cpp` | Ch1~Ch4 FreeRTOS dedicated timeslot loops; Core 0 non-blocking TCP select reactor; embedded IP whitelist; zero-copy bridge proxy | Strict L2 transport leaves, 0 L3/L4 semantic awareness |
| **L1 Physical HAL Drivers** | `Uart_Driver.cpp`, `Diagnostics_Driver.cpp`, `OTA_Driver.cpp`, `Wifi_Driver.cpp` | Unified HW UART0~2 + SoftwareSerial; NVS `LogManager`; Task WDT monitor; dual-partition rollback engine; Wi-Fi FSM | Complete HW information hiding, atomic driver metrics |
| **L0 Foundation Soil** | `System_Buffer.cpp`, `System_Config.cpp`, `System_Platform.cpp` | Universal static leaf; `FixedBuf`/`AppendBuf` zero-heap builders; `System_*` abstract platform contracts | Accessible by all layers ($L1 \sim L4$), zero upward dependencies |

---

## 6. Architecture Anti-Patterns vs. Modern Standards (C++23 Standards)

### 6.1 Anti-Patterns & Modern Equivalents Matrix

| Domain | Legacy Anti-Pattern (STRICTLY FORBIDDEN) | Modern Standard (C++23 / GW Canonical Standard) | Rationale & Prevention |
|---|---|---|---|
| **Architecture** | **Upward Include ($L_M \rightarrow L_N, M < N$)**: Lower layers referencing higher-layer headers | **Strict Downlink ($L4 \rightarrow L3 \rightarrow L2 \rightarrow L1$)**: L2 possesses 0% knowledge of L3/L4 | Prevents rebuild cascading when higher layers change; completely eliminates cyclic dependencies. |
| **Architecture** | **Middle-Man Pass-Through**: L2/L3 forwarding or wrapping L0 Foundation Soil APIs | **Direct Leaf Access**: All layers ($L1 \sim L4$) directly reference L0 Foundation Soil | Eliminates pointless wrapper boilerplate; maximizes compiler inlining and optimization. |
| **Architecture** | **Extern State Leak**: Sockets, channel mutexes, or queues exposed in headers via `extern` | **100% Information Hiding**: Sealed `static` inside `.cpp`; exposed exclusively via thread-safe read-only Snapshot APIs | Eliminates race conditions; guarantees thread safety and immutability. |
| **Memory / Hot Path** | **Heap in Hot Path**: Dynamic allocation (`new`, `malloc`, dynamic `String`) during packet RX/TX | **100% Zero-Heap**: `std::array`, `std::span`, static ring buffers, `AppendBuf` | Permanently prevents heap fragmentation, panics, and crashes during 24/7/365 continuous operation. |
| **Memory / Hot Path** | **Silent Command Drop**: Silently dropping critical control commands when queues saturate | **Deterministic Drop Semantics**: State/Polling=Drop-Head, VIP/Control=Drop-Tail with synchronous error code | Prevents state desynchronization with SmartThings app; prompts immediate upstream retransmission. |
| **Memory / Hot Path** | **Frequent Flash Write**: Writing directly to NVS Flash upon every runtime state transition | **RTC SRAM Retention + 30s Debounce**: Volatile cache in `RTC_NOINIT_ATTR`, debounced commit on convergence | Preserves SPI Flash endurance and prevents wear-out. |
| **Memory / Hot Path** | **Runtime Table Init**: Calculating/initializing lookup tables (CRC, etc.) at runtime startup | **`consteval` Flash Lookup Tables (C++20/23)**: Compile-time evaluation; placed directly in Flash `.rodata` | Zero boot latency; zero SRAM consumption. |
| **Control Flow** | **Chained `if-else` / `strcasecmp`**: Linear sequential string comparisons | **One-shot Lowercase + `constexpr` Table Dispatch**: Binary search (`std::lower_bound`) over sorted `constexpr` table | Eliminates $O(N)$ string comparison overhead; enables $O(\log N)$ dispatch. |
| **Control Flow** | **Raw Enum Cast**: Excessive `static_cast<uint8_t>(e)` | **`std::to_underlying(e)` (C++23)** | Improves readability and type-safe value conversions. |
| **Control Flow** | **Manual Byte Shift**: `((b[0]<<8)\|b[1])` or non-standard macros | **`std::byteswap()` (C++23) & `if consteval`** | Hardware-accelerated bit manipulation and compile-time evaluation. |
| **Control Flow** | **Dummy Return in Unreachable**: Pointless dummy returns in default labels | **`std::unreachable()` (C++23)** | Suppresses dead code generation and optimizes jump tables by hinting unreachable paths. |
| **Control Flow** | **Ad-hoc Flags & `millis()`**: Scattered `bool` flags and ad-hoc timestamp checks | **Explicit FSM**: Unified explicit state machine transition loops | Eliminates non-deterministic race conditions and state leaks. |
| **Control Flow** | **Out-Param / `esp_err_t` Error Handling**: Raw error codes and mutable pointer arguments | **`std::expected<T, E>` Monadic (C++23)**: 1-byte enum class error type with monadic chaining | Type-safe value/error propagation without C++ exceptions (`-fno-exceptions`). |
| **Control Flow** | **Nested `if` Slot Searches**: Deeply nested `if` / null checks during slot lookups | **`std::optional` Monadic (C++23)**: `.and_then()`, `.transform()`, `.value_or()` | Streamlines control flow and dramatically improves readability. |
| **Concurrency** | **Reentrant Lock Trap**: Invoking socket transmission or external callbacks while holding locks | **Pending Buffer Queue**: Enqueue to pending buffer, release lock, process sequentially in worker loop | Completely eliminates self-deadlocks. |
| **Concurrency** | **Single Mutex Monopoly**: Exclusive mutex for frequent multi-task reads | **`std::shared_mutex` (C++17/23)**: Frequent reads=`shared_lock`, rare writes=`unique_lock` | Eliminates thread contention and maximizes multi-core throughput. |
| **Concurrency** | **Infinite Mutex Wait (`portMAX_DELAY`)**: Indefinite lock waits | **Defensive Timeout**: Enforce `MAX_LOCK_HOLD_MS` (max 50ms) lock acquisition timeout | Prevents system-wide hangs during bus traffic congestion. |
| **Concurrency** | **FreeRTOS Callback Wrapper Struct**: Boilerplate structs/wrappers to pass lambdas | **`static` Lambda / `static operator()` (C++23)**: Zero-overhead stateless lambda conversion to raw function pointers | Direct binding to `TaskFunction_t` without object pointer overhead. |
| **API & Callbacks** | **Intra-Layer Anemic Decoupling**: Modules in the same layer ($L_N \leftrightarrow L_N$) using runtime function pointer hooks | **Direct Compile-Time Binding**: Direct C++ calls to public headers within the same layer | Eliminates indirect branch mispredictions, null-check overhead, and memory bloat. |
| **API & Callbacks** | **Misplaced Event Queue**: Telemetry FreeRTOS queue privatized inside L4 Service causing upward callback traps | **Transport/Foundation Event Bus**: Queue defined in L0/L2; L3 enqueues directly via downward call | Eliminates reverse listeners (`DeviceStateListener`) and `main.cpp` coupling glue. |
| **Concurrency / Hot Path** | **Hot-Path Lock Churn**: Repeated acquire/release cycles of spinlocks/mutexes per packet (`updateResponse` then `markVerified`) | **Single Consolidated Critical Section**: Single unified RAII scoped lock per packet lifecycle | Shaves 30~50 µs off Core 1 peak WCET and prevents cache coherency thrashing. |
| **Control Flow** | **Repetitive Hot-Loop Null Checks**: Repeatedly checking `if (s_dispatcher.foo)` on every iteration in Core 1 hot loop | **Infallible Boot-Time Contract**: Assert dispatcher validity once at startup; invoke branchless in hot path | Eliminates pipeline branch stalls and reduces WCET in hard real-time tasks. |

---

### 6.2 Decision Matrix: Table-Driven vs. `switch-case` Priority Guidelines

The gateway firmware strictly categorizes branching structures into Table-Driven dispatch vs. `switch-case` based on the nature of the data and control flow.

```
                  [Branching Architecture Decision]
                                  │
              ┌───────────────────┴───────────────────┐
              ▼                                       ▼
    "Is it data mapping or                  "Is it a sequential FSM
 external input dispatch (string/pkt)?"      transition or exhaustive enum check?"
              │                                       │
              ▼                                       ▼
  ★ Priority 1: Table-Driven               ★ Priority 1: switch-case
```

#### 🥇 When to Choose Table-Driven Dispatch (Priority 1)
1. **String-based Command Matching (CLI, JSON-RPC)**:
   - Because C++ does not support `switch` on strings, use **sorted `constexpr` tables + `std::string_view` binary search (`std::lower_bound`)** as the standard.
   - Example: `ConsoleCommandEntry g_cmd_table[] = { {"clear", ...}, {"info", ...} };`
2. **Packet Header / Command Byte Routing (Packet Dispatcher)**:
   - When dispatching protocol command bytes (`0x31`, `0x41`, `0x42`, etc.) to dedicated handler functions:
   - Replace massive `switch` blocks with **function pointer tables (`using PacketHandler = void(*)(span<const uint8_t>);`)**. Adding new commands requires only adding entries to the table without modifying existing dispatch logic (Open-Closed Principle).
3. **Hardware Pin / Multi-Dimensional Configuration Mapping (Configuration Matrix)**:
   - Per-channel GPIOs, UART baud rates, timer intervals, etc. must be defined as data matrices (`constexpr ChannelConfig g_channel_table[NUM_CH]`) rather than branching code.

#### 🥇 When to Choose `switch-case` (Priority 1)
1. **Finite State Machines (FSM State Transitions)**:
   - Communication protocol frame reception stages (`WAIT_HEADER` $\rightarrow$ `READ_LEN` $\rightarrow$ `READ_PAYLOAD` $\rightarrow$ `VERIFY_CRC`).
   - Telnet ANSI escape sequence parser.
   - **Rationale**: Table dispatch introduces stack context passing overhead between lambdas/function pointers, whereas `switch-case` operates directly on local variables, allowing the compiler to generate an optimal single-cycle jump table (`O(1)`).
2. **Exhaustive Compile-Time Enum Validation (Exhaustive Enum Check)**:
   - Enforced via `-Wswitch -Werror=switch` compiler flags to instantly detect unhandled enum cases at **compile time** when new enumerators are added.
3. **Pure Enum-to-String Conversion (Enum to `std::string_view`)**:
   - `[[nodiscard]] constexpr std::string_view getDeviceTypeName(DeviceType type) noexcept`
   - Inlining `switch-case` + `std::unreachable()` is optimal for cache locality and binary size compared to storing string pointer arrays in Flash memory.

> [!NOTE] **[Golden Rule]**:
> - **When binding operations to external input or structuring static data, prioritize Table-Driven dispatch.**
> - **When managing sequential FSM flows or enforcing enum exhaustiveness at compile time, prioritize `switch-case`.**
> - **Inlining complex business logic (tens of lines) inside `case` labels ("God Switch") is strictly forbidden**; always delegate to individual modular handler functions.

---

### 6.3 Modern C++23 Language Features & Implementation Guide (GCC 13.2.0 / ESP-IDF 5.3)

The gateway firmware standardizes on `-std=gnu++23`, `-fno-exceptions`, and `toolchain-xtensa-esp-elf@13.2.0+20240530`.
The following patterns and constraints are validated for safe operation in real-time embedded communication environments.

#### 6.3.1 `std::expected<T, E>` Monadic Error Handling (Standard Replacement for Protocol Parsers)
- **Objective**: Replaces ambiguous `bool` return + out-param pointers or non-standard `esp_err_t`, providing type-safe value/error propagation without C++ exceptions (`-fno-exceptions`).
- **Mandatory Invariants**:
  1. **Error Type Size Constraint**: The error type `E` must strictly be defined as a 1-byte enum class (e.g. `enum class ParseErr : uint8_t`). Using strings or large structs introduces copy overhead and degrades register allocation.
  2. **`-fno-exceptions` Safety**: Calling `ev.value()` aborts immediately via `std::terminate()` / `abort()` because `std::bad_expected_access` cannot be thrown under `-fno-exceptions`.
  3. **Permitted Accessors**: Strictly use `has_value()`, `operator*`, `.error()`, `.value_or()`, and monadic chaining (`.and_then()`, `.transform()`, `.or_else()`).

```cpp
#include <expected>
#include <span>
#include <cstdint>
#include <cstring>
#include <bit>
#include <utility>

enum class ParseErr : uint8_t { TooShort, BadHeader, BadLen, BadChecksum };

struct Frame {
    uint8_t cmd;
    std::span<const uint8_t> payload;
};

constexpr std::expected<Frame, ParseErr> parseFrame(std::span<const uint8_t> raw) noexcept {
    if (raw.size() < 5) return std::unexpected(ParseErr::TooShort);
    if (raw[0] != 0xF7) return std::unexpected(ParseErr::BadHeader);

    uint16_t len_be;
    std::memcpy(&len_be, raw.data() + 2, sizeof(len_be));
    const uint16_t len = (std::endian::native == std::endian::little)
                       ? std::byteswap(len_be) : len_be;

    if (raw.size() < 4u + len) return std::unexpected(ParseErr::BadLen);
    return Frame{ raw[1], raw.subspan(4, len) };
}

// Monadic chaining pipeline (C++23)
auto ev = parseFrame(buf)
            .and_then(validateChecksum) // expected<Frame, ParseErr> -> expected<Frame, ParseErr>
            .transform(toBusEvent);     // Frame -> BusEvent transformation
if (!ev) {
    handleParseError(ev.error());
    return;
}
dispatchBusEvent(*ev);
```

#### 6.3.2 `consteval` Compile-Time Lookup Tables (Enforced Flash `.rodata` Placement)
- **Objective**: Evaluates CRC tables (CRC8, Modbus CRC16) and protocol dispatch tables entirely at compile time instead of iterating in loops during boot, placing them directly into Flash `.rodata`.
- **Benefits**: Zero boot latency; zero runtime SRAM consumption.

```cpp
#include <array>
#include <span>

consteval auto make_crc8_table(uint8_t poly = 0x07) {
    std::array<uint8_t, 256> t{};
    for (unsigned i = 0; i < 256; ++i) {
        uint8_t c = static_cast<uint8_t>(i);
        for (int b = 0; b < 8; ++b) {
            c = (c & 0x80) ? static_cast<uint8_t>((c << 1) ^ poly) : static_cast<uint8_t>(c << 1);
        }
        t[i] = c;
    }
    return t;
}
inline constexpr auto kCrc8Table = make_crc8_table();

constexpr uint8_t calcCrc8(std::span<const uint8_t> data) noexcept {
    uint8_t c = 0;
    for (auto b : data) c = kCrc8Table[c ^ b];
    return c;
}
static_assert(calcCrc8(std::array<uint8_t, 3>{0x01, 0x02, 0x03}) != 0); // Compile-time validation
```

#### 6.3.3 `std::to_underlying` & `std::unreachable` (Enum Optimization)
- **`std::to_underlying(e)`**: Provides clean, type-safe integer conversion when copying enum values into wire buffers without ugly casting macros (`#include <utility>`).
- **`std::unreachable()`**: In exhaustive internal FSM `switch-case` blocks where boundary and header validation is already completed, hints unreachable branches to the compiler (`__builtin_unreachable()`) to generate optimal jump tables.
  - ⚠️ **Critical Caution (UB Warning)**: Never invoke in raw packet parsing stages where input is unverified. Input parsing failures must strictly return safe error results such as `ParseErr`.

```cpp
enum class Cmd : uint8_t { Poll = 0x01, Ack = 0x02, Query = 0x10 };

buf[1] = std::to_underlying(Cmd::Poll);

// Exhaustive internal state handling after external validation
switch (static_cast<Cmd>(buf[1])) {
    case Cmd::Poll:  handlePoll();  break;
    case Cmd::Ack:   handleAck();   break;
    case Cmd::Query: handleQuery(); break;
    default: std::unreachable();    // Compiler jump table optimization hint
}
```

#### 6.3.4 `static` Lambdas & `static operator()` (Zero-Overhead FreeRTOS & Callbacks)
- **Objective**: Leverages C++23 P1169R4 to declare stateless lambdas with `static`, completely eliminating the hidden `this` pointer argument and directly binding to FreeRTOS task entry signatures (`TaskFunction_t` / `void(*)(void*)`) or hardware interrupt callbacks without intermediate wrapper structs.

```cpp
// Direct binding to FreeRTOS task creation without wrapper structs
xTaskCreate([](void* arg) static {
    for (;;) {
        // ...
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}, "Ch1_Master", 4096, nullptr, 19, nullptr);
```

#### 6.3.5 `if consteval` (Dual Compile-Time vs. Hardware-Accelerated Branching)
- **Objective**: Enables unified functions that operate during compile-time constant evaluation (e.g. table generation) while utilizing hardware-accelerated intrinsics (`std::byteswap`, `std::memcpy`) in runtime hot paths.

```cpp
constexpr uint16_t load_be16(const uint8_t* p) noexcept {
    if consteval {
        return static_cast<uint16_t>((p[0] << 8) | p[1]); // Compile-time constant expression path
    } else {
        uint16_t v;
        std::memcpy(&v, p, sizeof(v));                    // Runtime path (alignment-safe HW accelerated)
        return std::byteswap(v);
    }
}
```

#### 6.3.6 `std::optional` Monadic Chaining (Streamlined Slot Queries)
- **Objective**: Composes repository slot lookups, snapshot queries, and state extraction into a clean linear pipeline without deeply nested null checks.

```cpp
std::optional<uint8_t> find_device_slot(uint8_t device_id) noexcept;
std::optional<DeviceSnapshot> get_slot_snapshot(uint8_t slot_idx) noexcept;

const bool is_online = find_device_slot(target_id)
                         .and_then(get_slot_snapshot)
                         .transform([](const auto& snap) { return snap.online; })
                         .value_or(false);
```

#### 6.3.7 `[[assume]]` Attribute (Hot Path Optimization Hints)
- **Objective**: Uses the C++23 `[[assume(expr)]]` attribute (GCC 13) to communicate 4-byte buffer alignment or length constraints to the compiler, unlocking `-ftree-vectorize` SIMD optimization.
- ⚠️ **Caution**: Violating the condition results in undefined behavior (UB); apply strictly to 100% physically guaranteed invariants.

```cpp
void copyAlignedDmaBuffer(uint8_t* dst, const uint8_t* src, size_t len) noexcept {
    [[assume(len % 4 == 0)]];
    [[assume(reinterpret_cast<uintptr_t>(dst) % 4 == 0)]];
    // 32-bit word transfer loop optimized via verified 4-byte alignment
}
```

#### 6.3.8 Toolchain Constraints & Strictly Forbidden Patterns (Compiler & Embedded Invariants)

1. **GCC 14+ Exclusives Strictly Forbidden (Current Toolchain: GCC 13.2.0)**:
   - ❌ `Deducing this` (`this auto&& self` syntax)
   - ❌ `<print>`, `std::print`, `std::println`
   - ❌ `std::flat_map`, `std::flat_set`
   - ❌ `std::mdspan`, `std::generator`, `import std`
2. **Binary / Flash Memory Bloat Prohibitions**:
   - ❌ `<iostream>`: Bloats binary by tens of kilobytes. Strictly prohibited.
   - ❌ `<format>` / `std::format`: Excessive template instantiation bloats Flash. Maintain L0 `AppendBuf` and `snprintf`.
3. **Hot Path Memory Safety (Pillar 3 Zero-Heap Alignment)**:
   - ❌ `std::string`: Forbidden in RX/TX hot paths (`Task_Ch1`, `Task_Ch2Ch3`, `RS485_CH`). Strictly use **`std::string_view`** (including `contains`, `starts_with`).
   - ❌ `std::move_only_function` / `std::function`: May invoke dynamic heap allocation (`new`) when capturing state beyond Small Buffer Optimization (SBO). Strictly use **raw C function pointers (`void(*)(span<const uint8_t>)`)** in hot paths.
4. **IRAM / ISR Safety**:
   - When calling `constexpr` functions from inside `IRAM_ATTR` ISR contexts, enforce inline expansion using `[[gnu::always_inline]]` to avoid Flash cache miss panics.



