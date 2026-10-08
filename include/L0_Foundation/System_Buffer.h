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
#include <optional>
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

[[gnu::always_inline]] constexpr void storeBe16(std::span<uint8_t, 2> p, uint16_t v) noexcept {
  p[0] = static_cast<uint8_t>(v >> 8);
  p[1] = static_cast<uint8_t>(v & 0xFF);
}
[[gnu::always_inline]] constexpr void storeBe16(uint8_t *p, uint16_t v) noexcept {
  p[0] = static_cast<uint8_t>(v >> 8);
  p[1] = static_cast<uint8_t>(v & 0xFF);
}

[[gnu::always_inline]] constexpr void storeLe16(std::span<uint8_t, 2> p, uint16_t v) noexcept {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>(v >> 8);
}
[[gnu::always_inline]] constexpr void storeLe16(uint8_t *p, uint16_t v) noexcept {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>(v >> 8);
}

[[nodiscard]] constexpr std::optional<uint16_t> loadBe16At(std::span<const uint8_t> b, size_t off) noexcept {
  if (b.size() < 2 || off > b.size() - 2) return std::nullopt;
  return static_cast<uint16_t>((static_cast<uint16_t>(b[off]) << 8) | b[off + 1]);
}
[[nodiscard]] constexpr std::optional<uint16_t> loadLe16At(std::span<const uint8_t> b, size_t off) noexcept {
  if (b.size() < 2 || off > b.size() - 2) return std::nullopt;
  return static_cast<uint16_t>((static_cast<uint16_t>(b[off + 1]) << 8) | b[off]);
}
} // namespace Endian

static_assert(HexLUT::kHexLut[0x00][0] == '0' && HexLUT::kHexLut[0x00][1] == '0', "HexLUT 0x00 check failed");
static_assert(HexLUT::kHexLut[0xFF][0] == 'F' && HexLUT::kHexLut[0xFF][1] == 'F', "HexLUT 0xFF check failed");
static_assert(Endian::loadBe16(std::array<uint8_t, 2>{0x12, 0x34}) == 0x1234, "loadBe16 test failed");
static_assert(Endian::loadLe16(std::array<uint8_t, 2>{0x34, 0x12}) == 0x1234, "loadLe16 test failed");
static_assert(Endian::loadBe32(std::array<uint8_t, 4>{0x12, 0x34, 0x56, 0x78}) == 0x12345678, "loadBe32 test failed");

// 1. 물리 바이트 레이아웃 직접 검증 (storeBe16 / storeLe16 엔디안 정합성)
static_assert([] {
  std::array<uint8_t, 4> buf{};
  std::span s{buf};
  Endian::storeBe16(s.subspan<0, 2>(), 0xFEDC);
  Endian::storeLe16(s.subspan<2, 2>(), 0xFEDC);
  return buf[0] == 0xFE && buf[1] == 0xDC &&
         buf[2] == 0xDC && buf[3] == 0xFE;
}(), "Endian store byte layout mismatch");

// 2. store <-> load 왕복 및 최상위 비트(MSB>=0x8000) 부호 확장 회귀 검증
static_assert([] {
  std::array<uint8_t, 4> buf{};
  std::span s{buf};
  Endian::storeBe16(s.subspan<0, 2>(), 0xFEDC);
  Endian::storeLe16(s.subspan<2, 2>(), 0x1234);
  return Endian::loadBe16(s.subspan<0, 2>()) == 0xFEDC &&
         Endian::loadLe16(s.subspan<2, 2>()) == 0x1234 &&
         Endian::loadBe16At(buf, 0) == 0xFEDC &&
         Endian::loadLe16At(buf, 2) == 0x1234;
}(), "Endian load/store roundtrip or sign-extension failed");

// 3. 경계 초과, off > size 언더플로 방어, 극단값(size_t max) 검증
static_assert([] {
  const std::array<uint8_t, 4> buf{0x12, 0x34, 0x56, 0x78};
  return Endian::loadBe16At(buf, 2) == 0x5678 &&
         !Endian::loadBe16At(buf, 3).has_value() &&
         !Endian::loadLe16At(buf, 3).has_value() &&
         !Endian::loadBe16At(buf, 4).has_value() &&
         !Endian::loadBe16At(buf, 5).has_value() &&
         !Endian::loadBe16At(buf, static_cast<size_t>(-1)).has_value();
}(), "Endian load*At bounds check or underflow defense failed");

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
  char *buf{nullptr};
  size_t cap{0};
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

namespace Hash {
// 32-bit FNV-1a case-insensitive compile-time hash
[[nodiscard]] constexpr uint32_t fnv1a32_ci(std::string_view s) noexcept {
  uint32_t hash = 0x811c9dc5u;
  for (char c : s) {
    uint8_t lc = (c >= 'A' && c <= 'Z') ? static_cast<uint8_t>(c + ('a' - 'A'))
                                        : static_cast<uint8_t>(c);
    hash ^= lc;
    hash *= 0x01000193u;
  }
  return hash;
}

// 32-bit FNV-1a case-insensitive runtime hash
[[nodiscard]] inline uint32_t fnv1a32_ci_rt(const char *s) noexcept {
  uint32_t hash = 0x811c9dc5u;
  if (!s)
    return hash;
  while (*s) {
    char c = *s++;
    uint8_t lc = (c >= 'A' && c <= 'Z') ? static_cast<uint8_t>(c + ('a' - 'A'))
                                        : static_cast<uint8_t>(c);
    hash ^= lc;
    hash *= 0x01000193u;
  }
  return hash;
}
// 8-bit Device Key Hash for O(1) repository & router slot lookup
[[nodiscard]] constexpr uint8_t deviceKey8(uint8_t dev_id, uint8_t sub1, uint8_t sub2) noexcept {
  return static_cast<uint8_t>(dev_id + sub1 * 3 + sub2 * 7);
}
} // namespace Hash

