#pragma once

// ============================================================================
// BufferUtils: Level 0 Zero-Heap String Builders & Fast Formatters
// ============================================================================

#include "L0_Base/System_Platform.h"
#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>

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
[[nodiscard]] bool isElapsed(uint32_t start_ms, uint32_t duration_ms) noexcept;
[[nodiscard]] long elapsedMs(const struct timeval &now,
                             const struct timeval &prev) noexcept;
} // namespace TimeUtils

struct AppendBuf;
struct HwSnapshot;
struct PktSnapshot;
struct StackSnapshot;
class TaskWdtMonitor;

namespace Fmt {
constexpr char DIV80[] = "------------------------------------------------"
                         "--------------------------------\r\n";
constexpr size_t DIV80_LEN = sizeof(DIV80) - 1;

constexpr char DIV80EQ[] = "=============================================="
                           "==================================\r\n";
constexpr size_t DIV80EQ_LEN = sizeof(DIV80EQ) - 1;

void FormatHex(std::span<const uint8_t> data, std::span<char> out) noexcept;
void FormatHex(const uint8_t *data, size_t len, char *out,
               size_t out_len) noexcept;

void FormatElapsed(uint32_t now, uint32_t timestamp, char *out,
                   size_t out_len) noexcept;

void FormatHwMetrics(AppendBuf &out, const HwSnapshot &hw);
void FormatNetworkStats(AppendBuf &out, const PktSnapshot &pkt);
void FormatRs485Stats(AppendBuf &out, const PktSnapshot &pkt);
void FormatTaskStacks(AppendBuf &out, const StackSnapshot &st,
                      const TaskWdtMonitor &wdt);
} // namespace Fmt

struct AppendBuf {
  char *buf;
  size_t cap;
  size_t offset = 0;

public:
  void appendFormatV(const char *fmt, va_list a);
  void appendFormat(const char *fmt, ...) __attribute__((format(printf, 2, 3)));

  template <size_t N> void append(const char (&str)[N]) noexcept {
    if (!buf || offset >= cap)
      return;
    const size_t copy_len = std::min(N - 1, cap - 1 - offset);
    if (copy_len > 0) {
      memcpy(buf + offset, str, copy_len);
      offset += copy_len;
      buf[offset] = '\0';
    }
  }

  void append(std::string_view sv) noexcept;
  void append(const char *str) noexcept;
};

template <size_t N> struct FixedBuf : public AppendBuf {
  char storage[N]{0};

  constexpr FixedBuf() noexcept : AppendBuf{storage, N, 0} {}

  const char *c_str() const noexcept { return storage; }
  operator const char *() const noexcept { return storage; }

  void reset() noexcept {
    offset = 0;
    if constexpr (N > 0) {
      storage[0] = '\0';
    }
  }
};
