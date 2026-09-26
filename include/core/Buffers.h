#pragma once

// ============================================================================
// OUTPUT FORMATTERS & BUFFER UTILITIES (Extracted from Common.h SECTION 3)
// ============================================================================

#include "Platform.h"
#include <WiFi.h>

namespace Fmt {
constexpr char DIV80[] = "------------------------------------------------"
                         "--------------------------------\r\n";
constexpr size_t DIV80_LEN = sizeof(DIV80) - 1;

constexpr char DIV80EQ[] = "=============================================="
                           "==================================\r\n";
constexpr size_t DIV80EQ_LEN = sizeof(DIV80EQ) - 1;

inline void FormatHex(const uint8_t *data, size_t len, char *out,
                      size_t out_len) noexcept {
  size_t idx = 0;
  for (size_t k = 0; k < len && idx + 3 < out_len; ++k) {
    const auto &hex_chars = HexLUT::LUT[data[k]];
    out[idx++] = hex_chars[0];
    out[idx++] = hex_chars[1];
    out[idx++] = ' ';
  }
  if (out_len > 0) {
    out[idx] = '\0';
  }
}

inline void FormatElapsed(uint32_t now, uint32_t timestamp, char *out,
                          size_t out_len) noexcept {
  if (timestamp == 0) {
    snprintf(out, out_len, "Never");
  } else {
    float el = (now - timestamp) / 1000.0f;
    if (el < 60.0f)
      snprintf(out, out_len, "%.1fs", el);
    else
      snprintf(out, out_len, "%lum", static_cast<unsigned long>(el / 60));
  }
}
} // namespace Fmt

struct AppendBuf {
  char *buf;
  size_t cap;
  size_t offset = 0;

private:
  void appendFormatV(const char *fmt, va_list a) {
    if (offset >= cap)
      return;
    int n = vsnprintf(buf + offset, cap - offset, fmt, a);
    if (n > 0)
      offset = std::min(offset + static_cast<size_t>(n), cap - 1);
  }

public:
  void appendFormat(const char *fmt, ...)
      __attribute__((format(printf, 2, 3))) {
    va_list a;
    va_start(a, fmt);
    appendFormatV(fmt, a);
    va_end(a);
  }

  template <size_t N> void append(const char (&str)[N]) noexcept {
    if (offset >= cap)
      return;
    size_t copy_len = std::min(N - 1, cap - 1 - offset);
    if (copy_len > 0) {
      memcpy(buf + offset, str, copy_len);
      offset += copy_len;
      buf[offset] = '\0';
    }
  }

  void append(std::string_view sv) noexcept {
    if (sv.empty() || offset >= cap)
      return;
    size_t copy_len = std::min(sv.size(), cap - 1 - offset);
    if (copy_len > 0) {
      memcpy(buf + offset, sv.data(), copy_len);
      offset += copy_len;
      buf[offset] = '\0';
    }
  }

  void append(const char *str) noexcept {
    if (!str || offset >= cap)
      return;
    size_t len = strlen(str);
    size_t copy_len = std::min(len, cap - 1 - offset);
    if (copy_len > 0) {
      memcpy(buf + offset, str, copy_len);
      offset += copy_len;
      buf[offset] = '\0';
    }
  }
};

[[nodiscard]] inline bool Tcp_IsAllowedIP(IPAddress ip) {
  // 1. Local Loopback
  if (ip == IPAddress(127, 0, 0, 1))
    return true;

  // 2. RFC 1918 Private IPv4 Networks (부팅 직후 DHCP 마스크 미완료 상태에서도 즉시 허용)
  if (ip[0] == 10)
    return true; // 10.0.0.0/8
  if (ip[0] == 172 && (ip[1] >= 16 && ip[1] <= 31))
    return true; // 172.16.0.0/12 (includes 172.30.1.x, 172.30.2.x)
  if (ip[0] == 192 && ip[1] == 168)
    return true; // 192.168.0.0/16

  // 3. Dynamic STA Subnet Match
  if (WiFi.isConnected()) {
    IPAddress sta_ip = WiFi.localIP();
    IPAddress sta_mask = WiFi.subnetMask();
    if ((ip & sta_mask) == (sta_ip & sta_mask))
      return true;
  }

  // 4. Dynamic SoftAP Subnet Match
  if (WiFi.getMode() == WIFI_MODE_AP || WiFi.getMode() == WIFI_MODE_APSTA) {
    IPAddress ap_ip = WiFi.softAPIP();
    IPAddress ap_mask = WiFi.softAPSubnetMask();
    if ((ip & ap_mask) == (ap_ip & ap_mask))
      return true;
  }

  return false;
}
