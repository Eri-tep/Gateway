# GW Home Gateway Architecture, Specifications & Design Philosophy

This document defines the system specifications, runtime topology, channel mappings, deadlock-free concurrency/locking hierarchies, and the **7 Core Architectural & Refactoring Pillars** across the 4-Tier Clean Architecture modules.

---

### 0. Canonical Clean Architecture Topology (4-Tier + 1 Foundation Soil)

> **Mandatory Architectural Standard (v4.0.0 Canonical & Modularized)**:
> 1. **Foundation Soil (L0 Base 전역 순수 기반 Leaf)**: 수직 파이프라인의 층이 아니며, 전 계층($L1 \sim L4$)이 딛고 서 있는 불변의 전역 Leaf(Universal Soil). 컴파일 타임 상수, 핀맵, 고정 버퍼 규격 제공.
> 2. **4-Tier Strict Vertical Pipeline ($L4 \rightarrow L3 \rightarrow L2 \rightarrow L1$)**: 
>    - **L4 Service**: 비즈니스 애플리케이션 (`EngineTask`, `BridgeService`, `ConsoleCli`, `Console/`, `Remote/`)
>    - **L3 Protocol**: 패킷 코덱, 웜스타트 캐시 & 자동 프로빙 (`WallpadParser`, `PollingRegistry`, `AutoProbingEngine`, `ModbusProtocol`, `DeviceRegistry`, `ControlTemplate`, Leaf: `ProtocolTypes.h`)
>    - **L2 Transport**: 물리 채널, 소켓 엔진, 프레이밍 추적 (`NetworkRouter`, `TcpReactor`, `DoorphoneTracker`, `FramingTracker`, Leaf: `TransportTypes.h`)
>    - **L1 System**: OS 프리미티브 & 인프라 (`LockUtils`, `SystemStorage`, `SystemDiagnostics`, `SystemOta`)
> 3. **Zero Upward Includes**: 하위 계층이 상위 계층을 include하는 행위 수학적으로 0건.
> 4. **No Middle-Man Pass-Through**: L0 Foundation Soil에 접근하기 위해 중간 계층이 불필요한 패스스루 래퍼를 두는 안티패턴 배제.
> 5. **Complete Information Hiding (0-extern)**: 모든 런타임 전역 통신 배열 및 락 노출을 전면 폐기하고, `.cpp` 내부 `static` 번역 단위 변수로 완전 은닉. 외부는 읽기 전용 Snapshot API로만 소비.
> 6. **Single Responsibility Submodule Balance (400~800 Lines Sweet Spot)**: 단일 파일 1,000줄 이상의 God File을 엄격히 금지하며, 도메인별 응집 모듈로 분할.

```
┌─────────────────────────────────────────────────────────────────────────┐
│ L4 Service: 도메인 비즈니스 로직 (EngineTask, BridgeService, Console, Remote)│
│  ├─ Console: CmdSystem.cpp, CmdConfig.cpp, CmdDevice.cpp, CmdTrace.cpp │
│  └─ Remote:  MgmtRpc.cpp, RemoteTelemetry.cpp, WifiManager.cpp         │
└────────────────────────────────────┬────────────────────────────────────┘
                                     │ (수직 런타임: 오직 L3만 호출)
                                     ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ L3 Protocol: 패킷 프레이밍, 코덱, 캐시, 자동학습 (Wallpad, Modbus, Registry) │
│  ├─ WallpadParser.cpp (STX/ETX/CS 코덱, ProfileRepository)              │
│  ├─ PollingRegistry.cpp (48슬롯 스케줄러, RTC SRAM/NVS 웜스타트 캐시)   │
│  └─ AutoProbingEngine.cpp (엔트로피 분석, 통계 프로빙 FSM, 매트릭스)    │
│ └─▶ [L3.0 Leaf: ProtocolTypes.h] (StaticPacket, DecodedDeviceState 등) │
└────────────────────────────────────┬────────────────────────────────────┘
                                     │ (수직 런타임: 오직 L2만 호출)
                                     ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ L2 Transport: 물리 버스, 소켓 I/O, Reactor, 프레이밍 학습               │
│  ├─ TcpReactor.cpp, NetworkRouter.cpp, DoorphoneTracker.cpp            │
│  └─ FramingTracker.cpp (L0에서 L2로 승격된 STX/ETX/LEN 동적 학습 FSM)  │
│ └─▶ [L2.0 Leaf: TransportTypes.h] (HubClientSlotSnapshot, RouteEndpoint)│
└────────────────────────────────────┬────────────────────────────────────┘
                                     │ (수직 런타임: 오직 L1만 호출)
                                     ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ L1 System: OS 프리미티브, NVS, WDT, OTA (LockUtils, Storage, Diagnostics)│
└─────────────────────────────────────────────────────────────────────────┘
  ▲                         ▲                         ▲                ▲
  │ (컴파일 타임 Leaf 참조)   │ (컴파일 타임 Leaf 참조)   │                │
  └─────────────────────────┴─────────────┬───────────┴────────────────┘
                                          │
┌─────────────────────────────────────────┴───────────────────────────────┐
│              L0 Base: Foundation Soil (전역 순수 기반 Leaf)              │
│  - SystemConfig.h: 네트워크 포트, 버퍼 크기, 불변 타이밍 파라미터         │
│  - SystemPlatform.h: ESP32-S3 GPIO 핀 매핑, StaticPacket 구조체         │
│  - BufferUtils.h: Zero-Allocation 스크래치 버퍼 AppendBuf<N>            │
└─────────────────────────────────────────────────────────────────────────┘
```

### 0.1 Framework & Toolchain Environment Specifications
- **Target Hardware**: M5Stack AtomS3 Lite (ESP32-S3FN8, 240MHz Dual-Core, 320KB SRAM, 8MB Flash)
- **Framework**: `framework-arduinoespressif32 @ 4.20017.260907+sha.dcc1105b`
- **Underlying SDK / ESP-IDF**: **ESP-IDF v4.4.7** (`ESP_IDF_VERSION_VAL(4, 4, 7)`)
- **Toolchain**: `xtensa-esp32s3-elf-gcc / g++ 8.4.0 (2021r2-patch5)`
- **C++ Standard**: **C++17** (`-std=gnu++17`)

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
