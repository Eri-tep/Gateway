#pragma once

// ============================================================================
// PLATFORM & COMPLIANCE DEFINITIONS (Extracted from Common.h SECTION 1)
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
#include <Preferences.h>
#include <SoftwareSerial.h>
#include <WiFi.h>
#include <array>
#include <atomic>
#include <cctype>
#include <string_view>
#include <sys/time.h>
#include <time.h>
#include <type_traits>

struct CoreDumpInfo {
  bool valid{false};
  char task_name[16]{};
  uint32_t exc_pc{0};
  uint32_t exc_cause{0};
  uint32_t bt[16]{};
  uint8_t bt_depth{0};
  bool bt_corrupted{false};
};

struct DoorphoneSpec {
  uint32_t baud_rate;     // 통신 속도
  uint8_t  stx;           // 시작 바이트
  uint8_t  etx;           // 종료 바이트
  uint8_t  len;           // 프레임 길이
  const char *desc;       // 규격 명칭
  uint8_t bell_front;     // 현관 벨 호출 수신
  uint8_t bell_lobby;     // 로비 벨 호출 수신
  uint8_t call_front;     // 현관 통화 시작 송신
  uint8_t call_lobby;     // 로비 통화 시작 송신
  uint8_t open_front;     // 현관 문열림 송신
  uint8_t open_lobby;     // 로비 문열림 송신
  uint8_t end_front;      // 현관 통화 종료 송신
  uint8_t end_lobby;      // 로비 통화 종료 송신
};

extern CoreDumpInfo g_coredump_info;
void System_CheckCoreDump();

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

inline SoftwareSerialConfig Door_SerialConfig(uint8_t data_bits, uint8_t parity,
                                              uint8_t stop_bits) {
  if (data_bits == 7 && stop_bits == 1) {
    if (parity == 1)
      return SWSERIAL_7E1;
    if (parity == 2)
      return SWSERIAL_7O1;
  } else if (data_bits == 8) {
    if (stop_bits == 1) {
      if (parity == 1)
        return SWSERIAL_8E1;
      if (parity == 2)
        return SWSERIAL_8O1;
    } else if (stop_bits == 2 && parity == 0) {
      return SWSERIAL_8N2;
    }
  }
  return SWSERIAL_8N1;
}

namespace HexLUT {
constexpr auto generateLUT() {
  std::array<std::array<char, 2>, 256> lut{};
  constexpr char hexDigits[] = "0123456789ABCDEF";
  for (size_t i = 0; i < 256; ++i) {
    lut[i][0] = hexDigits[(i >> 4) & 0x0F];
    lut[i][1] = hexDigits[i & 0x0F];
  }
  return lut;
}
alignas(16) inline constexpr auto LUT = generateLUT();
} // namespace HexLUT

namespace TimeUtils {
[[nodiscard]] inline bool isElapsed(uint32_t start_ms,
                                    uint32_t duration_ms) noexcept {
  return (millis() - start_ms) >= duration_ms;
}
} // namespace TimeUtils
