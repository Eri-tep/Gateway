# GW Home Modern C++ Style & Safety Guidelines

This document defines official guidelines for memory safety, type safety, real-time performance, and modern C++ (C++17) coding standards in the `GW Home` firmware (v2.0.6+).

---

## 1. String & Protocol Key Comparison Standard

- Keys and profile identifiers that may be case-insensitive or originate from CLI/NVS inputs must be compared using **`strcasecmp`**.
- Protocol engine checks (e.g. `"auto"`) must be encapsulated inside an inline helper function (`isAutoProfile(desc)`) as a single source of truth.

```cpp
// ❌ BAD: Mixed strcmp and strcasecmp across different call sites
if (strcmp(desc.key, "auto") == 0) { ... }
if (strcasecmp(desc.key, "auto") == 0) { ... }

// ✅ GOOD: Unified via dedicated helper
inline bool isAutoProfile(const VendorProfileDescriptor &desc) const {
  return strcasecmp(desc.key, "auto") == 0;
}
```

---

## 2. DRY Formatting Standards (AppendBuf & Utility Buffers)

- Buffer structures offering variadic formatting (`appendFormat`, `operator()`) must delegate to a **single private `va_list` helper (`appendFormatV`)** instead of duplicating `vsnprintf` logic.
- Prevent buffer overflows by clamping `offset + n < cap`.

```cpp
// ✅ GOOD: Variadic delegation to va_list helper
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

## 3. No Duplicate Enumerations or Redundant Aliases

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
  CUSTOM3 = 3,
  COUNT = 4
};
```

---

## 4. Safe Span Polyfill & Buffer Views

- When wrapping C-style fixed-size arrays (`T (&arr)[N]`) into a `span`, deduce size automatically via template C-array constructors.
- Keep explicit pointer + length constructors `span<T>(ptr, len)` for dynamically sized sub-buffers.

```cpp
// ✅ GOOD: Template C-array constructor support
template <typename T> class span {
  ...
  template <size_t N>
  constexpr span(T (&arr)[N]) noexcept : ptr_(arr), len_(N) {}
};
```

---

## 5. C++17 Nested Namespaces

- Under C++17, use compact nested namespace syntax `namespace A::B { ... }` instead of deeply nested indentation blocks.

```cpp
// ❌ BAD: Legacy C++98/03 nested indentation
namespace Config {
  namespace Task {
    constexpr size_t STACK_SIZE = 8192;
  }
}

// ✅ GOOD: C++17 compact nested namespace
namespace Config::Task {
constexpr size_t STACK_SIZE = 8192;
}
```

---

## 6. RAII Lock Safety: Never Use Anonymous Temporaries

- RAII synchronization primitives (`CriticalSectionLocker`, `MutexLocker`) must **always be instantiated with an explicit named local variable**.
- An unnamed temporary object is destroyed (unlocked) immediately at the end of that statement line, causing severe concurrency violations.

```cpp
// 🚨 CRITICAL BUG: Lock is released immediately on this line!
CriticalSectionLocker(&_mux); 
shared_variable++; // Completely unprotected!

// ✅ GOOD: Lock is held until the end of the enclosing scope
CriticalSectionLocker lock(&_mux);
shared_variable++;
```

---

## 7. RAII Smart Pointers for C Library Resources

- Wrap dynamically allocated resources from C libraries (e.g. `EmbeddedCli*`) in **`std::unique_ptr` with custom stateless deleters** instead of raw pointers.
- Stateless deleters incur zero additional RAM overhead due to Empty Base Optimization (EBO).

```cpp
// ✅ GOOD: Custom Deleter + std::unique_ptr
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

## 8. Hot-Path Metadata Access Encapsulation

- In high-frequency hot paths (communication loops, packet parsing), encapsulate access to global repositories within inline helpers (`activeProfile()`).
- Provides a clean single insertion point for future profile-epoch caching.

```cpp
class UniversalProtocolEngine : public IWallpadParser {
private:
  inline VendorProfileDescriptor activeProfile() const {
    VendorProfileDescriptor d;
    ProfileRepository::getActiveProfile(d);
    return d;
  }
};
```

---

## 9. Table-Driven & Anti-Blocking Standards

- **No String Chains**: Never use `strcmp`/`strcasecmp` chains in telemetry or state serialization. Dispatch via compile-time `enum class` and `switch`.
- **No Blocking in Sequences**: Never spawn tasks using `vTaskDelay` for sequential control. Use state machines or asynchronous timers.
- **Hot-Path Parser Modularization**: Decompose monolithic packet parsers into inline single-purpose helper functions per device class.

