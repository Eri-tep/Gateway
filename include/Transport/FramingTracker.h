#pragma once

// ============================================================================
// FramingTracker: Level 2 Generic Serial Packet Framing Learning & Lock
// ============================================================================

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>

enum class FramingStatus : uint8_t {
  WAITING = 0,
  LEARNING = 1,
  LOCKED = 2,
  NOISY = 3
};

struct FramingTracker {
  std::atomic<FramingStatus> status{FramingStatus::WAITING};
  std::atomic<uint8_t> candidate_stx{0};
  std::atomic<uint8_t> candidate_etx{0};
  std::atomic<uint8_t> candidate_len{0};
  std::atomic<uint8_t> consecutive_matches{0};
  std::atomic<uint8_t> consecutive_mismatches{0};
  std::atomic<bool> is_custom_fixed{false};

  void setFixedLock(uint8_t stx, uint8_t etx, uint8_t len) noexcept;
  void reset() noexcept;
  void clearNvs(const char *nvs_ns, const char *tag = "FRAMING") noexcept;
  void processFrame(uint8_t stx, uint8_t etx, uint8_t len, const char *nvs_ns,
                    const char *tag = "FRAMING") noexcept;

  static void getNvsNamespace(uint8_t prof_idx, char *out_ns,
                              size_t max_len) noexcept {
    snprintf(out_ns, max_len, "dp_frame_p%u",
             static_cast<unsigned int>(prof_idx & 0x03));
  }

  void restoreFromNvs(const char *nvs_ns = "dp_frame_p0",
                      const char *tag = "FRAMING") noexcept;
  void saveToNvs(const char *nvs_ns = "dp_frame_p0",
                 const char *tag = "FRAMING") noexcept;

  [[nodiscard]] bool isConsistent(uint8_t stx, uint8_t etx) const noexcept;
};
