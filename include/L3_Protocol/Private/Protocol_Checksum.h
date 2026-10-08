#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

// ============================================================================
// Protocol_Checksum: Pure Stateless Checksum Engine (L3 Private Leaf)
// ============================================================================
// Provides stateless constexpr checksum calculation primitives, direct inlined
// switch-case dispatch, and a mathematical fail-closed contract via uint16_t.
// ============================================================================

enum class ChecksumAlgo : uint8_t {
  UNKNOWN = 0,         // Uninitialized, unverified, or unrecognized (Fail-Closed)
  XOR_ALL = 1,         // XOR 0 .. N-3 (Hyundai HT, EzVille, etc.)
  XOR_NO_STX = 2,      // XOR 1 .. N-3 (Kocom)
  SUM_ALL = 3,         // Sum 0 .. N-3 modulo 256 (Commax Legacy)
  SUM_NO_STX = 4,      // Sum 1 .. N-3 modulo 256 (Commax Modern)
  TWOS_COMPLEMENT = 5, // (0x100 - Sum[1..N-3]) % 256 (Samsung SDS / EZON)
  ONES_COMPLEMENT = 6, // (~Sum[0..N-3]) % 256
  CRC8_MAXIM = 7,      // CRC-8 (poly 0x31, init 0x00)
  NONE = 8             // Pure framing without checksum byte (Doorphone 0x02..0x03)
};

// Declared outside enum class to preserve -Wswitch warning enforcement
inline constexpr size_t kChecksumAlgoCount = static_cast<size_t>(ChecksumAlgo::NONE) + 1;

#ifndef K_CHECKSUM_INVALID_DEFINED
#define K_CHECKSUM_INVALID_DEFINED
// Structural Fail-Closed Sentinel: 0x0100 cannot equal any uint8_t byte (0..255)
inline constexpr uint16_t kChecksumInvalid = 0x0100;
#endif

// ── Pure Stateless Checksum Primitives (constexpr implies inline) ───────────

constexpr uint8_t calcXorAll(const uint8_t *p, size_t end) noexcept {
  uint8_t r = 0;
  for (size_t i = 0; i < end; ++i) r ^= p[i];
  return r;
}

constexpr uint8_t calcXorNoStx(const uint8_t *p, size_t end) noexcept {
  uint8_t r = 0;
  for (size_t i = 1; i < end; ++i) r ^= p[i];
  return r;
}

constexpr uint8_t calcSumAll(const uint8_t *p, size_t end) noexcept {
  uint8_t r = 0;
  for (size_t i = 0; i < end; ++i) r += p[i];
  return r;
}

constexpr uint8_t calcSumNoStx(const uint8_t *p, size_t end) noexcept {
  uint8_t r = 0;
  for (size_t i = 1; i < end; ++i) r += p[i];
  return r;
}

constexpr uint8_t calcTwosComp(const uint8_t *p, size_t end) noexcept {
  return static_cast<uint8_t>(-calcSumNoStx(p, end));
}

constexpr uint8_t calcOnesComp(const uint8_t *p, size_t end) noexcept {
  return static_cast<uint8_t>(~calcSumAll(p, end));
}

constexpr uint8_t calcCrc8Maxim(const uint8_t *d, size_t n) noexcept {
  uint8_t crc = 0;
  for (size_t i = 0; i < n; ++i) {
    crc ^= d[i];
    for (int b = 0; b < 8; ++b) {
      crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x31)
                         : static_cast<uint8_t>(crc << 1);
    }
  }
  return crc;
}

// ── Direct Inlined Dispatcher (No default: label -> -Wswitch enforced) ──────

[[nodiscard]] constexpr uint16_t calculateChecksumDirect(ChecksumAlgo algo,
                                                        const uint8_t *data,
                                                        size_t len) noexcept {
  if (data == nullptr || len < 3) [[unlikely]]
    return kChecksumInvalid;

  const size_t payload_end = len - 2;
  switch (algo) {
    case ChecksumAlgo::XOR_ALL:         return calcXorAll(data, payload_end);
    case ChecksumAlgo::XOR_NO_STX:      return calcXorNoStx(data, payload_end);
    case ChecksumAlgo::SUM_ALL:         return calcSumAll(data, payload_end);
    case ChecksumAlgo::SUM_NO_STX:      return calcSumNoStx(data, payload_end);
    case ChecksumAlgo::TWOS_COMPLEMENT: return calcTwosComp(data, payload_end);
    case ChecksumAlgo::ONES_COMPLEMENT: return calcOnesComp(data, payload_end);
    case ChecksumAlgo::CRC8_MAXIM:      return calcCrc8Maxim(data, payload_end);
    case ChecksumAlgo::UNKNOWN:         return kChecksumInvalid;
    case ChecksumAlgo::NONE:            return kChecksumInvalid;
  }
  // Safe fallback for illegal out-of-range integer casts
  return kChecksumInvalid;
}

// ── Compile-Time Static Assert Golden Test Vectors ──────────────────────────

namespace ChecksumCompileTimeVerification {
  // Hyundai Golden Query (11 Bytes): F7 0B 01 18 01 01 00 00 00 12 EE (Payload XOR cs = 0x12)
  constexpr uint8_t kGoldenQuery[11] = {
      0xF7, 0x0B, 0x01, 0x18, 0x01, 0x01, 0x00, 0x00, 0x00, 0x12, 0xEE
  };

  static_assert(calculateChecksumDirect(ChecksumAlgo::XOR_NO_STX, kGoldenQuery, sizeof(kGoldenQuery)) == 0x12,
                "Golden Vector Check Failed: Hyundai XOR_NO_STX must return 0x12");

  static_assert(calculateChecksumDirect(ChecksumAlgo::UNKNOWN, kGoldenQuery, sizeof(kGoldenQuery)) == kChecksumInvalid,
                "Fail-Closed Check Failed: UNKNOWN must return 0x0100");

  static_assert(calculateChecksumDirect(ChecksumAlgo::NONE, kGoldenQuery, sizeof(kGoldenQuery)) == kChecksumInvalid,
                "Fail-Closed Check Failed: NONE must return 0x0100");

  static_assert(calculateChecksumDirect(ChecksumAlgo::XOR_ALL, nullptr, 11) == kChecksumInvalid,
                "Defensive Check Failed: nullptr must return 0x0100");

  static_assert(calculateChecksumDirect(ChecksumAlgo::XOR_ALL, kGoldenQuery, 2) == kChecksumInvalid,
                "Defensive Check Failed: len < 3 must return 0x0100");
}
