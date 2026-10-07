#include "L0_Foundation/System_Buffer.h"

namespace TimeUtils {
bool isElapsed(uint32_t start_ms, uint32_t duration_ms) noexcept {
  return (millis() - start_ms) >= duration_ms;
}

long elapsedMs(const struct timeval &now, const struct timeval &prev) noexcept {
  if (prev.tv_sec == 0)
    return -1;
  const long total_ms =
      (now.tv_sec - prev.tv_sec) * 1000 + (now.tv_usec - prev.tv_usec) / 1000;
  return (total_ms >= 0 && total_ms < 60000) ? total_ms : -1;
}
} // namespace TimeUtils

namespace Fmt {
void FormatHex(std::span<const uint8_t> data, std::span<char> out) noexcept {
  if (out.empty())
    return;
  size_t idx = 0;
  for (size_t k = 0; k < data.size() && idx + 3 < out.size(); ++k) {
    const auto &hex_chars = HexLUT::LUT[data[k]];
    out[idx++] = hex_chars[0];
    out[idx++] = hex_chars[1];
    out[idx++] = ' ';
  }
  out[idx] = '\0';
}

void FormatHex(const uint8_t *data, size_t len, char *out,
               size_t out_len) noexcept {
  if (!out || out_len == 0)
    return;
  FormatHex(std::span<const uint8_t>(data, data ? len : 0),
            std::span<char>(out, out_len));
}

void FormatElapsed(uint32_t now, uint32_t timestamp, char *out,
                   size_t out_len) noexcept {
  if (!out || out_len == 0)
    return;
  if (timestamp == 0) {
    snprintf(out, out_len, "Never");
    return;
  }
  const uint32_t el_ms = (now >= timestamp) ? (now - timestamp) : 0;
  const float el = el_ms / 1000.0f;
  if (el < 60.0f) {
    snprintf(out, out_len, "%.1fs", el);
  } else {
    snprintf(out, out_len, "%lum", static_cast<unsigned long>(el / 60));
  }
}
} // namespace Fmt

void AppendBuf::appendFormatV(const char *fmt, va_list a) {
  if (!buf || offset >= cap)
    return;
  const int n = vsnprintf(buf + offset, cap - offset, fmt, a);
  if (n > 0) {
    offset = std::min(offset + static_cast<size_t>(n), cap - 1);
  } else if (n < 0) {
    buf[offset] = '\0';
  }
}

void AppendBuf::appendFormat(const char *fmt, ...) {
  va_list a;
  va_start(a, fmt);
  appendFormatV(fmt, a);
  va_end(a);
}

void AppendBuf::append(std::string_view sv) noexcept {
  if (sv.empty() || !buf || offset >= cap)
    return;
  const size_t copy_len = std::min(sv.size(), cap - 1 - offset);
  if (copy_len > 0) {
    memcpy(buf + offset, sv.data(), copy_len);
    offset += copy_len;
  }
  buf[offset] = '\0';
}

void AppendBuf::append(const char *str) noexcept {
  if (!str || !buf || offset >= cap)
    return;
  const size_t len = strlen(str);
  const size_t copy_len = std::min(len, cap - 1 - offset);
  if (copy_len > 0) {
    memcpy(buf + offset, str, copy_len);
    offset += copy_len;
  }
  buf[offset] = '\0';
}
