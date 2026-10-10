#pragma once

// ============================================================================
// SequenceLock: Level 0 Pure Base Lockless Synchronization Primitive (C++23)
// ============================================================================

#include "L0_Foundation/System_Platform.h"
#include <atomic>
#include <cstdint>
#if defined(ESP_PLATFORM)
#include <esp_rom_sys.h>
#endif

namespace Gateway::Foundation {

/**
 * @brief High-performance lockless Sequence Lock (Seqlock).
 *
 * Provides ultra-low latency lockless reads for single-writer or spinlock-protected
 * multi-writer architectures on ESP32-S3 SMP dual-core.
 * - Readers never acquire spinlocks or disable interrupts (~15 cycles vs ~191 cycles).
 * - Writers serialize via external spinlock (e.g. portMUX_TYPE) and increment sequence:
 *   even -> odd (write in progress) -> even (write complete).
 */
class SequenceLock {
private:
  alignas(4) std::atomic<uint32_t> _sequence{0};

public:
  constexpr SequenceLock() noexcept = default;

  // Non-copyable, non-movable
  SequenceLock(const SequenceLock &) = delete;
  SequenceLock &operator=(const SequenceLock &) = delete;
  SequenceLock(SequenceLock &&) = delete;
  SequenceLock &operator=(SequenceLock &&) = delete;

  /**
   * @brief Begin a lockless read transaction.
   * Spins only if a writer is actively holding write_begin().
   * @return Current even sequence counter.
   */
  [[nodiscard]] inline uint32_t read_begin() const noexcept {
    while (true) {
      uint32_t seq = _sequence.load(std::memory_order_acquire);
      if ((seq & 1U) == 0U) [[likely]] {
        return seq;
      }
      #if defined(ESP_PLATFORM)
      esp_rom_delay_us(0);
      #endif
    }
  }

  /**
   * @brief Verify if the read transaction completed without concurrent writer modification.
   * @param initial_seq The sequence value returned by read_begin().
   * @return true if data was modified or torn (caller must retry); false if consistent.
   */
  [[nodiscard]] inline bool read_retry(uint32_t initial_seq) const noexcept {
    std::atomic_thread_fence(std::memory_order_acquire);
    return _sequence.load(std::memory_order_relaxed) != initial_seq;
  }

  /**
   * @brief Transition sequence to odd, indicating a write is in progress.
   * Must be called while holding external writer serialization lock.
   */
  inline void write_begin() noexcept {
    _sequence.fetch_add(1U, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_release);
  }

  /**
   * @brief Transition sequence back to even, publishing completed writes.
   * Must be called before releasing external writer serialization lock.
   */
  inline void write_end() noexcept {
    _sequence.fetch_add(1U, std::memory_order_release);
  }

  /**
   * @brief Query current sequence snapshot.
   */
  [[nodiscard]] inline uint32_t sequence() const noexcept {
    return _sequence.load(std::memory_order_relaxed);
  }
};

/**
 * @brief RAII Critical Section + Sequence Lock Writer Guard.
 * Atomically acquires portMUX_TYPE spinlock and transitions Seqlock to odd on entry,
 * then transitions Seqlock to even and exits spinlock on exit.
 */
class [[nodiscard]] CriticalSeqWriterGuard {
private:
  portMUX_TYPE *_mux{nullptr};
  SequenceLock &_seq;

public:
  explicit CriticalSeqWriterGuard(portMUX_TYPE *mux, SequenceLock &seq) noexcept
      : _mux(mux), _seq(seq) {
    if (_mux) {
      portENTER_CRITICAL(_mux);
    }
    _seq.write_begin();
  }

  explicit CriticalSeqWriterGuard(portMUX_TYPE &mux, SequenceLock &seq) noexcept
      : _mux(&mux), _seq(seq) {
    portENTER_CRITICAL(_mux);
    _seq.write_begin();
  }

  ~CriticalSeqWriterGuard() noexcept {
    _seq.write_end();
    if (_mux) {
      portEXIT_CRITICAL(_mux);
    }
  }

  CriticalSeqWriterGuard(const CriticalSeqWriterGuard &) = delete;
  CriticalSeqWriterGuard &operator=(const CriticalSeqWriterGuard &) = delete;
  CriticalSeqWriterGuard(CriticalSeqWriterGuard &&) = delete;
  CriticalSeqWriterGuard &operator=(CriticalSeqWriterGuard &&) = delete;
};

} // namespace Gateway::Foundation
