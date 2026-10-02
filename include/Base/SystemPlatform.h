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

#include <span>
#include <utility>

using std::span;
using std::string_view;
using std::to_underlying;

[[nodiscard]] inline uint32_t FastCrc32(std::span<const uint8_t> data) noexcept {
  return ~esp_rom_crc32_le(~0U, data.data(), data.size());
}

[[nodiscard]] inline uint32_t FastCrc32(const uint8_t *data, size_t len) noexcept {
  return FastCrc32(std::span<const uint8_t>(data, len));
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

void Tcp_EnableKeepalive(int sock, int idle, int intvl, int cnt);

// ── Fixed-Size Packet Structures ──
constexpr uint8_t PKT_STX = 0xF7;
constexpr uint8_t PKT_ETX = 0xEE;

struct StaticPacket {
  uint8_t channel_id;
  uint8_t length;
  std::array<uint8_t, 64> data;
};

// ── System Lifecycle & Synchronization Primitives ──
extern EventGroupHandle_t g_system_event_group;
extern std::atomic<bool> g_ota_in_progress;
extern std::atomic<bool> g_probe_convergence_reset;
constexpr EventBits_t SYS_EVT_OTA_IDLE = (1 << 0);
constexpr EventBits_t SYS_EVT_CACHE_READY = (1 << 1);
constexpr EventBits_t SYS_EVT_SYSTEM_RUNNING = (1 << 2);
