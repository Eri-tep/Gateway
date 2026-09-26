# GW Home Gateway Architecture & Specifications

This document defines the system architecture, directory layout, execution pathways (Hot/Warm/Cold paths), concurrency/locking rules, and immutable design constraints for `GW Home` (M5Stack AtomS3 Lite-based RS-485 / TCP Smart Home Gateway).

---

## 1. System Architecture & Immutable Constraints

### 1.1 Runtime Reliability Principles
- **24/7 Uninterrupted Operation**: Maintain zero ESP32 Watchdog Timer (WDT) resets under all operational conditions.
- **Zero Real-time Loop Dynamic Allocation**: Heap allocation (`new`, `malloc`, dynamic `String`) is strictly forbidden in packet hot paths (`Task_Ch1`, `Task_Ch2Ch3`). Use stack-allocated or fixed-size buffers (`StaticPacket`, `AppendBuf`).
- **LOCKED Device Push Isolation**: To prevent noise packet propagation, real-time push to SmartThings (CH6 / TCP 8900) must strictly transmit verified state changes from `LOCKED` devices only.

### 1.2 FreeRTOS Task Topology (Immutable)

| Task Name | Core Affinity | Priority | Entry Function | Path Classification | Responsibility |
| :--- | :---: | :---: | :--- | :--- | :--- |
| `CH#1_IoT` | Core 1 | 19 (High) | `Task_Ch1()` | **Hot Path** | RS-485 physical master polling & device state sync |
| `CH#2_WP#1` | Core 1 | 18 (High) | `Task_Ch2Ch3()` | Warm Path | Wallpad #1 RS-485 slave virtual ACK response |
| `CH#3_WP#2` | Core 1 | 18 (High) | `Task_Ch2Ch3()` | Warm Path | Wallpad #2 RS-485 slave virtual ACK response |
| `CH#4_WP#3` | Core 1 | 10 (Med) | `Task_Ch4()` | Warm Path | Doorphone SoftwareSerial communication |
| `Network` | Core 0 | 5 (Med) | `Task_Network()` | Warm Path | Wi-Fi connection, CH5 EW11 hubs, CH6 Mgmt RPC |
| `Telnet_CLI` | Core 0 | 2 (Low) | `Task_Telnet()` | Cold Path | Telnet CLI diagnostics, learning wizard, packet tracer |

---

## 2. Source Code & Header Directory Structure

All source files are encapsulated across 8 domain subdirectories following the Single Responsibility Principle (SRP).

```
/
├── include/
│   ├── Common.h                  # Backward-compatibility umbrella facade
│   ├── core/                     # System core domain headers
│   │   ├── Platform.h            # Hardware/OS types, CRC, NvsEnvelope, HexLUT
│   │   ├── Config.h              # Port, timing, buffer, and runtime config
│   │   ├── Buffers.h             # Fmt, AppendBuf, IP network utilities
│   │   ├── Metrics.h             # MutexLocker, CriticalSectionLocker, system metrics
│   │   └── Devices.h             # StaticPacket, DeviceStateEntry, DeviceRepository
│   ├── ControlTemplate.h         # Control blueprints & slot coverage engine interface
│   ├── WallpadParser.h           # Universal packet parser & auto-probing interface
│   ├── TelnetCli.h               # Telnet session, CLI engine, packet tracer interface
│   ├── MgmtRpc.h                 # SmartThings JSON-RPC management server interface
│   └── CliCommands.h             # Telnet CLI command registration interface
│
├── src/
│   ├── main.cpp                  # System bootstrap (setup / loop)
│   ├── CLI/                      # Telnet CLI (router, status, control table, config, etc.)
│   ├── Control/                  # Control blueprints (Template, Learning, Nvs, Registry)
│   ├── Core/                     # System core (Globals, Health, Config, Log, WarmCache)
│   ├── Engine/                   # Communication engines (CH1, CH2/3, serial RX)
│   ├── Management/               # Management servers (MgmtRpc, HttpOta, Telemetry)
│   ├── Network/                  # Network (HubManager, NetworkManager)
│   ├── Parser/                   # Protocol parsers (AutoProbing, ProfileRepository, etc.)
│   └── Telnet/                   # Telnet server (Auth, Server, Tracer, Wizard)
```

---

## 3. Channel & Port Mapping

| Channel | Physical/Logical Interface | Port / Pins | Role | Description |
| :--- | :--- | :--- | :--- | :--- |
| **CH1** | UART0 (RS-485) | Hardware Default | Home IoT sub-device master bus | Real-time physical bus control |
| **CH2** | UART1 (RS-485) | RX: 5, TX: 6 (Configurable) | Main wallpad bridge (Virtual slave) | Immediate Virtual ACK response |
| **CH3** | UART2 (RS-485) | RX: 7, TX: 8 (Configurable) | Sub wallpad bridge (Virtual slave) | Immediate Virtual ACK response |
| **CH4** | SoftwareSerial | RX: 38, TX: 39 | Doorphone (videophone) bus | Call & door unlock bridge |
| **CH5** | TCP Client | 8898 / 8891~8894 | EW11 multi-hub bridge client | External extended serial buses |
| **CH6** | TCP Server | 8900 | SmartThings unified JSON-RPC server | Device control & telemetry push |
| **CLI** | TCP Server | 23 | Telnet diagnostic & admin CLI | Packet trace & learning wizard |

---

## 4. Concurrency & Locking Guidelines

1. **Spinlocks & Critical Sections (`portMUX_TYPE`)**:
   - Limit `taskENTER_CRITICAL(&mux)` / `CriticalSectionLocker` hold time to **under tens of microseconds (µs)**.
   - Strictly prohibit I/O operations (`Serial.print`), memory allocation, NVS operations, and blocking calls inside critical sections.
2. **Mutexes (`SemaphoreHandle_t`)**:
   - Use `MutexLocker` for TCP socket transmission and shared buffer synchronization.
   - Lock acquisition timeout must not exceed `Config::Timing::MAX_LOCK_HOLD_MS`.
3. **NVS Persistence Policy**:
   - Debounce runtime template updates and configuration writes (`Config::Timing::WARM_CACHE_NVS_DEBOUNCE_MS`) to protect flash memory endurance.
