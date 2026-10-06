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

// ── Virtual Hub Device Classification (0: Wallpad, 1: Air Conditioner) ──
enum class HubDeviceType : uint8_t {
  WALLPAD_COMPATIBLE = 0,
  AIR_CONDITIONER = 1
};

// ── EW11 Client Socket Slot Snapshot (Information Hiding POD) ──
struct HubClientSlotSnapshot {
  bool enabled{false};
  bool is_connected{false};
  char name[16]{""};
  char target_ip[16]{""};
  uint16_t target_port{0};
  HubDeviceType dev_type{HubDeviceType::WALLPAD_COMPATIBLE};
  uint32_t last_rx_ms{0};
  uint32_t rx_pkts{0};
  uint32_t tx_pkts{0};
  uint32_t dropped_pkts{0};
};

// ── Port 8900 Management TCP Session Snapshot ──
struct MgmtSessionSnapshot {
  bool is_active{false};
  char peer_ip[16]{""};
  uint32_t connected_at_ms{0};
  uint32_t last_activity_ms{0};
};

// ── HTTP OTA Snapshot POD ──
struct HttpOtaSnapshot {
  bool in_progress{false};
  char status[64]{"Idle"};
  uint8_t progress_pct{0};
  char last_error[64]{""};
};

struct SysSnapshot {
  uint32_t free_heap{0};
  uint32_t min_free_heap{0};
  uint32_t total_heap{0};
  uint32_t sketch_size_kb{0};
  uint32_t flash_total_kb{0};
  uint32_t uptime_ms{0};
  bool wifi_connected{false};
  int8_t wifi_rssi{0};
  char wifi_ip[16]{""};
};

struct HwSnapshot {
  uint8_t cpu0_cur{0}, cpu0_15m_avg{0}, cpu0_15m_peak{0}, cpu0_24h_avg{0}, cpu0_24h_peak{0};
  uint8_t cpu1_cur{0}, cpu1_15m_avg{0}, cpu1_15m_peak{0}, cpu1_24h_avg{0}, cpu1_24h_peak{0};
  uint16_t ram_cur{0}, ram_15m_avg{0}, ram_15m_peak{0}, ram_24h_avg{0}, ram_24h_peak{0};
  int8_t temp_cur{0}, temp_15m_avg{0}, temp_15m_peak{0}, temp_24h_avg{0}, temp_24h_peak{0};
};

struct StackSnapshot {
  uint16_t ch1_stack{0}, ch2_stack{0}, ch3_stack{0}, ch4_stack{0}, net_stack{0}, telnet_stack{0};
};

struct ChanStats {
  uint32_t rx_pkts{0};
  uint32_t tx_pkts{0};
  uint32_t crc_errors{0};
  uint32_t invalid_frames{0};
  uint32_t timeouts{0};
  uint32_t uncached_pkts{0};
  uint32_t last_activity_ms{0};
};

struct TcpChanStats {
  uint32_t rx_pkts{0};
  uint32_t tx_pkts{0};
  uint32_t dropped_pkts{0};
  uint32_t uncached_pkts{0};
  uint16_t connection_count{0};
  bool is_connected{false};
};

struct PktSnapshot {
  ChanStats ch1;
  ChanStats ch2;
  ChanStats ch3;
  ChanStats ch4;
  TcpChanStats ch5;
  TcpChanStats ch6;
};

struct LogEntry {
  uint32_t timestamp{0};
  char reason[32]{""};
  SysSnapshot stats_snapshot;
  HwSnapshot hw_snapshot;
  StackSnapshot stack_snapshot;
  PktSnapshot packet_stats_snapshot;
};

struct AppendBuf;

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
