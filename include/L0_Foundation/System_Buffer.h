#pragma once

// ============================================================================
// BufferUtils: Level 0 Zero-Heap String Builders & Fast Formatters
// ============================================================================

#include "L0_Foundation/System_Platform.h"
#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>

namespace HexLUT {
consteval auto generateLUT() noexcept {
  std::array<std::array<char, 2>, 256> lut{};
  constexpr char hexDigits[] = "0123456789ABCDEF";
  for (size_t i = 0; i < 256; ++i) {
    lut[i][0] = hexDigits[(i >> 4) & 0x0F];
    lut[i][1] = hexDigits[i & 0x0F];
  }
  return lut;
}
alignas(16) inline constexpr auto kHexLut = generateLUT();
inline constexpr auto &LUT = kHexLut;
} // namespace HexLUT

namespace Endian {
// Note: Pass fixed-extent spans (e.g. raw.subspan<2, 2>() / raw.first<2>()) or pointers/arrays.
// Dynamic-extent spans do not implicitly convert to fixed-extent span<const uint8_t, N>.
[[nodiscard]] [[gnu::always_inline]] constexpr uint16_t loadBe16(std::span<const uint8_t, 2> p) noexcept {
  return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}
[[nodiscard]] [[gnu::always_inline]] constexpr uint16_t loadBe16(const uint8_t *p) noexcept {
  return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}
[[nodiscard]] [[gnu::always_inline]] constexpr uint32_t loadBe32(std::span<const uint8_t, 4> p) noexcept {
  return (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8)  |
         static_cast<uint32_t>(p[3]);
}
[[nodiscard]] [[gnu::always_inline]] constexpr uint32_t loadBe32(const uint8_t *p) noexcept {
  return (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8)  |
         static_cast<uint32_t>(p[3]);
}
[[nodiscard]] [[gnu::always_inline]] constexpr uint16_t loadLe16(std::span<const uint8_t, 2> p) noexcept {
  return static_cast<uint16_t>((static_cast<uint16_t>(p[1]) << 8) | p[0]);
}
[[nodiscard]] [[gnu::always_inline]] constexpr uint16_t loadLe16(const uint8_t *p) noexcept {
  return static_cast<uint16_t>((static_cast<uint16_t>(p[1]) << 8) | p[0]);
}
} // namespace Endian

static_assert(HexLUT::kHexLut[0x00][0] == '0' && HexLUT::kHexLut[0x00][1] == '0', "HexLUT 0x00 check failed");
static_assert(HexLUT::kHexLut[0xFF][0] == 'F' && HexLUT::kHexLut[0xFF][1] == 'F', "HexLUT 0xFF check failed");
static_assert(Endian::loadBe16(std::array<uint8_t, 2>{0x12, 0x34}) == 0x1234, "loadBe16 test failed");
static_assert(Endian::loadLe16(std::array<uint8_t, 2>{0x34, 0x12}) == 0x1234, "loadLe16 test failed");
static_assert(Endian::loadBe32(std::array<uint8_t, 4>{0x12, 0x34, 0x56, 0x78}) == 0x12345678, "loadBe32 test failed");

namespace TimeUtils {
[[nodiscard]] bool isElapsed(uint32_t start_ms, uint32_t duration_ms) noexcept;
[[nodiscard]] long elapsedMs(const struct timeval &now,
                             const struct timeval &prev) noexcept;
} // namespace TimeUtils

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
