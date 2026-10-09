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
  size_t k = 0;
  const size_t data_len = data.size();

  // SWAR 4-Byte Chunk Processing: 4 bytes in -> 12 characters out
  while (k + 4 <= data_len && idx + 13 <= out.size()) {
    alignas(4) char chunk[12];
    const auto &h0 = HexLUT::LUT[data[k]];
    const auto &h1 = HexLUT::LUT[data[k + 1]];
    const auto &h2 = HexLUT::LUT[data[k + 2]];
    const auto &h3 = HexLUT::LUT[data[k + 3]];

    chunk[0] = h0[0]; chunk[1] = h0[1]; chunk[2] = ' ';
    chunk[3] = h1[0]; chunk[4] = h1[1]; chunk[5] = ' ';
    chunk[6] = h2[0]; chunk[7] = h2[1]; chunk[8] = ' ';
    chunk[9] = h3[0]; chunk[10] = h3[1]; chunk[11] = ' ';

    memcpy(&out[idx], chunk, 12);
    idx += 12;
    k += 4;
  }

  // Remainder tail processing (1~3 bytes)
  while (k < data_len && idx + 3 < out.size()) {
    const auto &hex_chars = HexLUT::LUT[data[k]];
    out[idx++] = hex_chars[0];
    out[idx++] = hex_chars[1];
    out[idx++] = ' ';
    k++;
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
