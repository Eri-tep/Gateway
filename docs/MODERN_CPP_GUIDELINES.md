# GW Home Modern C++ Style, Safety & Naming Standards

This document defines official guidelines for codebase naming conventions, memory safety, type safety, modern C++ (C++17) idioms, and real-time embedded coding standards for the `GW Home` firmware.

---

## 1. Codebase Naming Conventions

### 1.1 Global & Free Function Naming Rules
Global and free functions must strictly follow the **`Domain_VerbNoun`** format (PascalCase with underscore domain delimiter) to explicitly communicate domain ownership and intent.

| Domain Prefix | Target Area | Examples |
| :--- | :--- | :--- |
| `System_` | Hardware telemetry, reboot, core dumps, system snapshots | `System_ReadTempC()`, `System_Restart()`, `System_TakeSnapshot()` |
| `Config_` | NVS configuration load, save, reset | `Config_Load()`, `Config_Save()`, `Config_ResetDefaults()` |
| `Cache_` | RTC memory & NVS warm cache synchronization | `Cache_SaveToRtc()`, `Cache_SaveToNvs()`, `Cache_RestoreOnBoot()` |
| `Hub_` | CH5 multi-slot TCP hub client management & I/O | `Hub_LoadConfig()`, `Hub_SaveConfig()`, `Hub_SetSlot()`, `Hub_SendPacket()` |
| `Tcp_` | TCP socket utilities, client IP filtering, keepalive | `Tcp_IsAllowedIP()`, `Tcp_EnableKeepalive()` |
| `Door_` | Doorphone (CH4) serial protocol and opcode validation | `Door_SerialConfig()`, `Door_IsValidOpcode()` |
| `Ch1_` ~ `Ch6_` | Dedicated RS-485 and TCP channel packet handlers | `Ch1_BuildQueryPacket()`, `Ch6_SendAck()`, `Ch6_Data()` |
| `Boot_` | `main.cpp` boot sequence (`static void` scope only) | `Boot_InitHardwareAndDevices()`, `Boot_StartTasks()` |

> **Rules & Prohibitions**:
> - Formatting utilities belong inside `namespace Fmt { ... }`, not in the global function namespace.
> - The `Boot_` prefix is strictly reserved for `static` startup functions inside `main.cpp`.
> - Prefix-less camelCase global functions (e.g. `readChipTempC()`) are strictly forbidden.
> - Standalone PascalCase global functions without domain prefixes (e.g. `CheckAndLogLastResetReason()`) are forbidden.

### 1.2 FreeRTOS Task Function Naming Rules
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

### 1.3 Global Variables & Aliasing Rules
Global variables must carry the **`g_`** prefix and possess **exactly one canonical, authoritative symbol name**.

| Canonical Global Name | Type | Description |
| :--- | :--- | :--- |
| `g_config` | `RuntimeConfig` | System runtime configuration instance |
| `g_metrics` | `SystemMetricsTracker` | Hardware ringbuffer metrics collector |
| `g_ch1_bus_ms` | `std::atomic<uint32_t>` | CH1 bus last activity timestamp (ms) |
| `g_pkt_stats` | `PacketStatistics` | Real-time packet and error statistics counters |
| `g_polling_targets` | `PollingTargetRegistry` | Dynamic polling target registry (1st-Tier Cache) |
| `g_device_repo` | `DeviceRepository` | Device real-time state repository (2nd-Tier Cache) |

> **Information Hiding & Scope Reduction Mandate**:
> - Hardware communication slots, sockets, and channel mutexes (e.g. EW11 slots `s_hub_slots`, `s_ch5_mutex`) must NEVER be exposed as `extern` global variables.
> - They must be strictly encapsulated inside `.cpp` translation units as `static`, exposed only via read-only snapshot APIs (e.g., `Bridge_GetSlotSnapshot()`).

> **Prohibitions**:
> - Never declare reference aliases to an existing instance (`inline auto &g_metrics = g_metrics_tracker;`).
> - Never declare duplicate type aliases (`using DeviceRepo = DeviceRepository;`).
> - Never declare redundant alias namespaces (`namespace Kernel = SystemOrchestrator;`).

### 1.4 Constants & Namespaces
- All configuration constants must be declared inside `namespace Config::<Group>` using `UPPER_SNAKE_CASE`.
- Do not mirror namespace constants into the global scope using `using` or `inline constexpr`. Reference them explicitly at call sites as `Config::<Group>::<CONST>`.

### 1.5 Plain Data Structures (Snapshots & Stats)
Plain data structures designed for lock-free atomic snapshotting/memcpy operations without internal atomics must use the **`Snapshot`** or **`Stats`** suffix (e.g. `SysSnapshot`, `Snapshot`, `SlotRuntime`, `ChanStats`).

---

## 2. String & Protocol Key Comparison Standard

- Keys and profile identifiers originating from CLI or NVS inputs must be compared case-insensitively.
- **Single Identifier Check**: Encapsulate inside an inline helper function (`isAutoProfile(desc)`) using `strcasecmp`.
- **Multi-Branch Dispatch (No strcasecmp Chains)**: When dispatching across multiple string commands or device classes, **normalize input to lowercase in a stack buffer once**, then dispatch via a `constexpr` lookup table with `std::string_view` exact matching. Do NOT chain `strcasecmp` calls.

```cpp
// ❌ BAD: Chained strcasecmp calls (O(N) string comparisons with high branch penalty)
if (strcasecmp(cls, "light") == 0) { ... }
else if (strcasecmp(cls, "thermo") == 0) { ... }

// ✅ GOOD: Lowercase normalization + constexpr table dispatch
struct ClassEntry { std::string_view key; DeviceClass cls; const char *def_name; };
static constexpr ClassEntry kTable[] = {
    {"light",  DeviceClass::SWITCH,     "Light"},
    {"thermo", DeviceClass::THERMOSTAT, "Thermo"},
};
char lower[32];
toLowerCopy(cls, lower, sizeof(lower));
std::string_view sv{lower};
for (const auto &entry : kTable) {
  if (entry.key == sv) { ...; break; }
}
```

---

## 3. DRY Formatting Standards (AppendBuf & Utility Buffers)

- Buffer structures offering variadic formatting (`appendFormat`, `operator()`) must delegate to a **single private `va_list` helper (`appendFormatV`)** instead of duplicating `vsnprintf` logic.
- Prevent buffer overflows by clamping `offset + n < cap`.

```cpp
// ✅ GOOD: Variadic delegation to a single va_list helper
struct AppendBuf {
  char *buf;
  size_t cap;
  size_t offset = 0;

private:
  void appendFormatV(const char *fmt, va_list a) {
    if (offset >= cap) return;
    int n = vsnprintf(buf + offset, cap - offset, fmt, a);
    if (n > 0) offset = std::min(offset + (size_t)n, cap - 1);
  }

public:
  void appendFormat(const char *fmt, ...) __attribute__((format(printf, 2, 3))) {
    va_list a; va_start(a, fmt); appendFormatV(fmt, a); va_end(a);
  }
  void operator()(const char *fmt, ...) __attribute__((format(printf, 2, 3))) {
    va_list a; va_start(a, fmt); appendFormatV(fmt, a); va_end(a);
  }
};
```

---

## 4. No Duplicate Enumerations or Redundant Aliases

- Never define redundant aliases mapping to identical integer values within an `enum class`.
- Enforce exactly one authoritative identifier across the codebase.

```cpp
// ❌ BAD: Defining both AUTO and ADAPTIVE for slot 0
enum class WallpadProfileIndex : uint8_t {
  AUTO = 0,
  ADAPTIVE = 0,
  CUSTOM1 = 1
};

// ✅ GOOD: Single unambiguous identifier
enum class WallpadProfileIndex : uint8_t {
  ADAPTIVE = 0,
  CUSTOM1 = 1,
  CUSTOM2 = 2,
  COUNT = 3
};
```

---

## 5. Buffer Views: Transitioning to `std::span`

- **C++17 Baseline**: Use the zero-allocation `span<T>` polyfill (`include/Base/BufferUtils.h`) supporting template deduction for fixed C-arrays (`T (&arr)[N]`) and explicit pointer-length slices (`span<T>(ptr, len)`).
- **C++20/C++23 Target**: Migrate seamlessly to standard `#include <span>` and `std::span<const uint8_t>`. Both share identical 8-byte register footprints (`a2`, `a3` on Xtensa) and zero runtime overhead.
- Under both standards, never pass raw pointer-length pairs across layer boundaries without a span wrapper.

---

## 6. Language Standards: C++17 Baseline to C++23 Target

- **Namespace Syntax**: Use compact nested namespace syntax `namespace A::B { ... }` instead of deeply nested indentation blocks.
- **Compile-Time Computation**: Leverage `constexpr` for compile-time evaluations and lookup tables. Under C++20/23, expand to `consteval` and `constexpr` algorithms where applicable.
- **Standard Alignment**: Ensure all C++17 code paths remain 100% forward-compatible with C++23 toolchains (zero deprecated syntax).

---

## 7. RAII Safety Standards (Lock & Resource Management)

### 7.1 RAII Lock Safety: Never Use Anonymous Temporaries
- RAII synchronization primitives (`CriticalSectionLocker`, `MutexLocker`) must **always be instantiated with an explicit named local variable**.
- An unnamed temporary object is destroyed (unlocked) immediately at the end of that statement line, causing critical concurrency violations.

```cpp
// 🚨 CRITICAL BUG: Lock is released immediately on this line!
CriticalSectionLocker(&_mux); 
shared_variable++; // Completely unprotected!

// ✅ GOOD: Lock is held until the end of the enclosing scope
CriticalSectionLocker lock(&_mux);
shared_variable++;
```

### 7.2 RAII Smart Pointers for C Library Resources
- Wrap dynamically allocated resources from C libraries (e.g. `EmbeddedCli*`) in **`std::unique_ptr` with custom stateless deleters** instead of raw pointers.
- Stateless deleters incur zero additional RAM overhead due to Empty Base Optimization (EBO).

```cpp
// ✅ GOOD: Custom Stateless Deleter + std::unique_ptr
struct CliDeleter {
  void operator()(EmbeddedCli *p) const noexcept {
    if (p) embeddedCliFree(p);
  }
};

struct TelnetSession {
  std::unique_ptr<EmbeddedCli, CliDeleter> cli;

  void reset() {
    ...
    cli.reset(); // Safely freed automatically
  }
};
```

---

## 8. Multi-Thread Synchronization: Read-Heavy Shared State

- In ESP-IDF / Xtensa toolchains (GCC 8.4+, `__GTHREADS=1`), `std::shared_mutex` is fully supported.
- For globally shared state with frequent multi-task reads and rare writes (such as `g_config`), prefer **`std::shared_mutex`**:
  - **Read path**: Use `std::shared_lock<std::shared_mutex>` allowing concurrent reads across tasks without contention.
  - **Write path**: Use `std::unique_lock<std::shared_mutex>` for exclusive modification.
- Critical sections (`portMUX_TYPE` / `taskENTER_CRITICAL`) are reserved exclusively for ISR-safety or microsecond-level atomic operations and MUST NOT enclose blocking I/O or NVS writes.

---

## 9. Modern C++ Evolution: C++20 & C++23 Adoption Standards

When targeting modern toolchains (GCC 13.2+ with ESP-IDF v5.3+ / Arduino ESP32 v3.x), the codebase elevates from C++17 to C++23. The following idioms are canonized for high-reliability embedded systems:

### 9.1 `std::expected<T, E>`: Zero-Heap Monadic Error Handling (C++23)
- Permanently eliminates the anti-pattern of paired `bool` return flags and mutable output references (`bool parse(..., Packet &out)`).
- Enforces strict no-heap (`-fno-exceptions`) error propagation with Return Value Optimization (RVO) in register space.
- Enables clean monadic chaining (`.and_then()`, `.or_else()`).

```cpp
#include <expected>
#include <span>

enum class ParseError : uint8_t { FrameTooShort, InvalidFraming, ChecksumMismatch };

std::expected<DeviceKey, ParseError> extractDeviceKey(std::span<const uint8_t> frame) noexcept {
  if (frame.size() < 4) return std::unexpected(ParseError::FrameTooShort);
  if (frame.front() != 0xF7 || frame.back() != 0xEE) return std::unexpected(ParseError::InvalidFraming);
  return DeviceKey{ frame[3], frame[5], frame[6] };
}
```

### 9.2 `std::span<const uint8_t>`: Zero-Copy Safe Buffer Views (C++20)
- Replaces raw C pointer-length pairs (`const uint8_t *data, size_t len`) with an 8-byte view (pointer + size) passed entirely in hardware registers (`a2`, `a3`).
- Incurs zero dynamic allocation, zero stack overhead, and prevents buffer overruns through bounds-aware `.subspan()`.

### 9.3 Concepts & Constraints: Compile-Time Bus Packet Contracts (C++20)
- Eliminates brittle `std::enable_if_t` / SFINAE boilerplate.
- Enforces payload serializability and memory constraints at compile time with **0 bytes of runtime metadata overhead**.

```cpp
#include <concepts>
#include <type_traits>

template <typename T>
concept ValidBusPacket = std::is_trivially_copyable_v<T> && (sizeof(T) <= 64);

template <ValidBusPacket Pkt>
bool enqueueTxPacket(RingbufHandle_t ringbuf, const Pkt &packet) noexcept {
  return xRingbufferSend(ringbuf, &packet, sizeof(packet), 0) == pdTRUE;
}
```

### 9.4 `std::to_underlying` (C++23) & `map.contains` (C++20)
- `std::to_underlying(e)`: Replaces verbose `static_cast<std::underlying_type_t<Enum>>(e)` for type-safe enum-to-integer conversion.
- `map.contains(key)`: Provides clear, readable existence checks without iterator boilerplate.

### 9.5 Forbidden Modern Features (Embedded Guardrails)
1. **`std::print` & `std::format` (PROHIBITED)**: Adds 100KB~200KB of runtime format parsing tables to Flash. Maintain zero-allocation `AppendBuf` and ESP-IDF logging macros (`ESP_LOGI`).
2. **C++20 Coroutines / `std::generator` (PROHIBITED)**: Secretly allocates heap frames (`malloc`/`operator new`). Retain deterministic static FreeRTOS task loops and explicit FSMs.
3. **`-fno-exceptions` & `-fno-rtti` (MANDATORY)**: Must remain strictly enforced under C++23 to guarantee deterministic execution time and minimal flash footprint.
