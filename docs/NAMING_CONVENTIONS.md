# GW Home Architecture & Codebase Naming Conventions

This document defines the official naming conventions to ensure code quality, readability, consistency, and long-term maintainability for the `GW Home` project. All contributors and agents must strictly adhere to these rules.

---

## 1. Global & Free Function Naming Rules

Global and free functions must follow the **`Domain_VerbNoun`** format (PascalCase with underscore domain delimiter) to clearly signal domain ownership and behavior.

| Domain Prefix | Target Area | Examples |
| :--- | :--- | :--- |
| `System_` | Hardware telemetry, restart, core dumps, snapshot capture | `System_ReadTempC()`, `System_Restart()`, `System_TakeSnapshot()` |
| `Config_` | NVS configuration load/save/reset | `Config_Load()`, `Config_Save()`, `Config_ResetDefaults()` |
| `Cache_` | RTC memory & NVS warm cache sync across reboots | `Cache_SaveToRtc()`, `Cache_SaveToNvs()`, `Cache_RestoreOnBoot()` |
| `Hub_` | CH5 multi-slot TCP hub client management & I/O | `Hub_LoadConfig()`, `Hub_SaveConfig()`, `Hub_SetSlot()`, `Hub_SendPacket()` |
| `Queue_` | RTOS queue specialized behaviors | `Queue_EnqueueDropHead()` |
| `Tcp_` | TCP socket utilities, client IP filtering | `Tcp_IsAllowedIP()`, `Tcp_EnableKeepalive()` |
| `Door_` | Doorphone (CH4/CH5) protocol and serial communication | `Door_SerialConfig()`, `Door_IsValidOpcode()` |
| `Uart_` | UART physical layer I/O | `Uart_RecvPacket()` |
| `Ch1_` ~ `Ch6_` | Dedicated RS-485 and TCP channel handlers | `Ch1_BuildQueryPacket()`, `Ch6_SendAck()`, `Ch6_Data()` |
| `Boot_` | `main.cpp` boot sequence (`static void` scope only) | `Boot_InitHardwareAndDevices()`, `Boot_StartTasks()` |

> **Rules & Prohibitions**:
> - Formatting utilities (e.g. metric formatters) belong inside `namespace Fmt { ... }`, not in the global function namespace.
> - The `Boot_` prefix is strictly reserved for `static` startup functions inside `main.cpp`.
> - Prefix-less camelCase global functions (e.g. `readChipTempC()`, `isAllowedClientIP()`) are prohibited.
> - Standalone PascalCase global functions without domain prefixes (e.g. `CheckAndLogLastResetReason()`) are prohibited.

---

## 2. FreeRTOS Task Function Naming Rules

Task entry functions must be declared and exported directly as **`Task_<Domain>`** without redundant indirection or wrapper layers.

| Task Name | Assigned Core | Responsibility |
| :--- | :---: | :--- |
| `Task_Ch1` | Core 1 | RS-485 CH1 IoT device master polling & control |
| `Task_Ch2Ch3` | Core 1 | RS-485 CH2/CH3 Wallpad slave virtual ACK responses |
| `Task_Ch4` | Core 1 | CH4 Doorphone bidirectional communication |
| `Task_Network` | Core 0 | Wi-Fi, OTA, TCP socket servers (CH5, CH6) |
| `Task_Telnet` | Core 0 | Telnet CLI diagnostics and management console |

> **Prohibitions**:
> - Redundant wrappers such as `Core1_Ch1Task` are prohibited.
> - Aliased function pointers/symbols such as `Task_IoTChannel1 = Core1_Ch1Task` are prohibited.

---

## 3. Global Variables & Aliasing Rules

Global variables must carry the `g_` prefix and possess **exactly one canonical, authoritative symbol name**.

| Canonical Global Name | Type | Description |
| :--- | :--- | :--- |
| `g_config` | `RuntimeConfig` | System runtime configuration instance |
| `g_metrics` | `SystemMetricsTracker` | Hardware ringbuffer metrics collector |
| `g_ch1_bus_ms` | `std::atomic<uint32_t>` | CH1 bus last activity timestamp (ms) |
| `g_pkt_stats` | `PacketStatistics` | Real-time packet and error counters |
| `g_polling_targets` | `PollingTargetRegistry` | Dynamic polling target registry |
| `g_auto_probing_engine` | `AutoProbingEngine` | Universal auto-probing engine |
| `g_wdt_monitor` | `TaskWdtMonitor` | Task watchdog health monitor |
| `g_hub_slots` | `HubClientSlot[]` | CH5 multi-client slot pool |

> **Prohibitions**:
> - Never declare reference aliases to an existing instance (`inline auto &g_metrics = g_metrics_tracker;`).
> - Never declare duplicate type aliases (`using DeviceRepo = DeviceRepository;`).
> - Never declare redundant alias namespaces (`namespace Kernel = SystemOrchestrator;`).

---

## 4. Constants & Namespaces

- All configuration constants must be declared inside `namespace Config::<Group>` using `UPPER_SNAKE_CASE`.
- Do not mirror namespace constants into the global scope using `using` or `inline constexpr`. Reference them explicitly at call sites as `Config::<Group>::<CONST>`.

---

## 5. Snapshot Data Structures

Plain data structures designed for lock-free atomic snapshotting/memcpy operations without internal atomics must use the **`Snapshot`** or **`Stats`** suffix:

- `SysSnapshot`
- `HwSnapshot`
- `StackSnapshot`
- `ChanStats`
- `TcpChanStats`
- `PktSnapshot`
