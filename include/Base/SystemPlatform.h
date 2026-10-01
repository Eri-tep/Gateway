#pragma once

// ============================================================================
// SystemPlatform: Level 0 Pure Base Infrastructure Definitions
// ============================================================================

#include "driver/uart.h"
#include "esp_core_dump.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <Arduino.h>
#include <IPAddress.h>
#include <Preferences.h>
#include <SoftwareSerial.h>
#include <array>
#include <atomic>
#include <cctype>
#include <string_view>
#include <sys/time.h>
#include <time.h>
#include <type_traits>

#ifndef LIKELY
#define LIKELY(x) __builtin_expect(!!(x), 1)
#endif
#ifndef UNLIKELY
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif

#if __has_include(<span>)
#include <span>
using std::span;
#else
namespace Gateway {
template <typename T> class span {
  T *ptr_{nullptr};
  size_t len_{0};

public:
  constexpr span() noexcept = default;
  constexpr span(T *ptr, size_t len) noexcept : ptr_(ptr), len_(len) {}
  constexpr span(T *first, T *last) noexcept
      : ptr_(first), len_(last - first) {}
  template <size_t N>
  constexpr span(
      std::array<typename std::remove_const<T>::type, N> &arr) noexcept
      : ptr_(arr.data()), len_(N) {}
  template <size_t N>
  constexpr span(
      const std::array<typename std::remove_const<T>::type, N> &arr) noexcept
      : ptr_(arr.data()), len_(N) {}
  template <size_t N>
  constexpr span(T (&arr)[N]) noexcept : ptr_(arr), len_(N) {}
  constexpr T *data() const noexcept { return ptr_; }
  constexpr size_t size() const noexcept { return len_; }
  constexpr bool empty() const noexcept { return len_ == 0; }
  constexpr T &operator[](size_t idx) const noexcept { return ptr_[idx]; }
  constexpr T *begin() const noexcept { return ptr_; }
  constexpr T *end() const noexcept { return ptr_ + len_; }
  constexpr span<T>
  subspan(size_t offset,
          size_t count = static_cast<size_t>(-1)) const noexcept {
    if (offset >= len_)
      return span<T>();
    size_t actual_count =
        (count == static_cast<size_t>(-1) || offset + count > len_)
            ? (len_ - offset)
            : count;
    return span<T>(ptr_ + offset, actual_count);
  }
};
} // namespace Gateway
using Gateway::span;
#endif

using std::string_view;

inline uint32_t FastCrc32(const uint8_t *data, size_t len) noexcept {
  return ~esp_rom_crc32_le(~0U, data, len);
}

template <typename T> struct NvsEnvelope {
  uint32_t magic{0x4757484D}; // "GWHM"
  uint16_t version{1};
  uint16_t data_len{sizeof(T)};
  uint32_t crc32{0};
  T payload{};

  void seal() noexcept {
    crc32 = FastCrc32(reinterpret_cast<const uint8_t *>(&payload), sizeof(T));
  }

  bool verify() const noexcept {
    if (magic != 0x4757484D || data_len != sizeof(T))
      return false;
    uint32_t computed =
        FastCrc32(reinterpret_cast<const uint8_t *>(&payload), sizeof(T));
    return (computed == crc32);
  }
};

template <class T>
inline bool nvsGetEnv(Preferences &p, const char *key, T &out) {
  NvsEnvelope<T> env{};
  if (p.getBytesLength(key) != sizeof(env))
    return false;
  if (p.getBytes(key, &env, sizeof(env)) != sizeof(env) || !env.verify())
    return false;
  out = env.payload;
  return true;
}

template <class T>
inline bool nvsPutEnv(Preferences &p, const char *key, const T &v) {
  NvsEnvelope<T> env{};
  env.payload = v;
  env.seal();
  return (p.putBytes(key, &env, sizeof(env)) == sizeof(env));
}

template <class T>
inline bool nvsPutEnvNs(const char *ns, const char *key, const T &v) {
  Preferences p;
  if (!p.begin(ns, false))
    return false;
  bool ok = nvsPutEnv(p, key, v);
  p.end();
  return ok;
}

template <size_t N> inline void setStr(char (&dst)[N], const char *src) {
  snprintf(dst, N, "%s", src ? src : "");
}

struct CoreDumpInfo {
  bool valid{false};
  char task_name[16]{};
  uint32_t exc_pc{0};
  uint32_t exc_cause{0};
  uint32_t bt[16]{};
  uint8_t bt_depth{0};
  bool bt_corrupted{false};
};

extern CoreDumpInfo g_coredump_info;

struct DoorphoneSpec {
  uint32_t baud_rate;
  uint8_t stx;
  uint8_t etx;
  uint8_t len;
  const char *desc;
  uint8_t bell_front;
  uint8_t bell_lobby;
  uint8_t call_front;
  uint8_t call_lobby;
  uint8_t open_front;
  uint8_t open_lobby;
  uint8_t end_front;
  uint8_t end_lobby;
};

SoftwareSerialConfig Door_SerialConfig(uint8_t data_bits, uint8_t parity,
                                       uint8_t stop_bits);

void Tcp_EnableKeepalive(int sock, int idle, int intvl, int cnt);

// ── Fixed-Size Packet Structures ──
constexpr uint8_t PKT_STX = 0xF7;
constexpr uint8_t PKT_ETX = 0xEE;

struct StaticPacket {
  uint8_t channel_id;
  uint8_t length;
  std::array<uint8_t, 64> data;
};
