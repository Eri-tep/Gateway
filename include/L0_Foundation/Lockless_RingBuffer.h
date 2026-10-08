#pragma once

// ============================================================================
// Lockless_RingBuffer.h — L0 Foundation Layer (Pure Base Leaf Soil)
// Single-Producer Single-Consumer (SPSC) Lock-Free Ring Buffer
// ============================================================================
// Canonical 4+1 Layer: L0 Foundation Leaf — Zero OS/FreeRTOS dependencies.
// Designed for Hot Path (Producer) <-> Warm Path (Consumer) Zero-Copy Handover.
//
// Guarantees:
//   - Zero Heap: Static array storage allocated within template class.
//   - Zero Lock: Pure atomic acquire-release synchronization (0ns interrupt blocking).
//   - Cache Isolation: alignas(64) on head_ and tail_ to eliminate False Sharing.
//   - O(1) Masking: Power-of-two capacity compile-time static_assert.
// ============================================================================

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>
#include "L0_Foundation/System_Platform.h"

namespace Foundation {

template <typename T, size_t Capacity>
class LocklessSpscRingBuffer {
  static_assert((Capacity > 0) && ((Capacity & (Capacity - 1)) == 0),
                "Capacity must be a non-zero power of 2");
  static constexpr size_t MASK = Capacity - 1;

public:
  constexpr LocklessSpscRingBuffer() noexcept = default;
  ~LocklessSpscRingBuffer() noexcept = default;

  // Non-copyable, non-movable (strict single-ownership)
  LocklessSpscRingBuffer(const LocklessSpscRingBuffer &) = delete;
  LocklessSpscRingBuffer &operator=(const LocklessSpscRingBuffer &) = delete;
  LocklessSpscRingBuffer(LocklessSpscRingBuffer &&) = delete;
  LocklessSpscRingBuffer &operator=(LocklessSpscRingBuffer &&) = delete;

  /// Push item into ring buffer (Producer only, e.g. Hot Path).
  /// Returns true on success, false if full (Drop-Tail).
  [[nodiscard]] bool push(const T &item) noexcept {
    const uint32_t head = head_.load(std::memory_order_relaxed);
    const uint32_t tail = tail_.load(std::memory_order_acquire);

    if (head - tail >= Capacity) {
      return false; // Buffer full (Drop-Tail)
    }

    storage_[head & MASK] = item;
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  /// Push item with move semantics.
  [[nodiscard]] bool push(T &&item) noexcept {
    const uint32_t head = head_.load(std::memory_order_relaxed);
    const uint32_t tail = tail_.load(std::memory_order_acquire);

    if (head - tail >= Capacity) {
      return false; // Buffer full (Drop-Tail)
    }

    storage_[head & MASK] = std::move(item);
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  /// Pop item from ring buffer (Consumer only, e.g. Warm Path).
  /// Returns true on success, false if empty.
  [[nodiscard]] bool pop(T &out_item) noexcept {
    const uint32_t tail = tail_.load(std::memory_order_relaxed);
    const uint32_t head = head_.load(std::memory_order_acquire);

    if (head == tail) {
      return false; // Buffer empty
    }

    out_item = std::move(storage_[tail & MASK]);
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  /// Pop item returning std::optional.
  [[nodiscard]] std::optional<T> pop() noexcept {
    const uint32_t tail = tail_.load(std::memory_order_relaxed);
    const uint32_t head = head_.load(std::memory_order_acquire);

    if (head == tail) {
      return std::nullopt;
    }

    T item = std::move(storage_[tail & MASK]);
    tail_.store(tail + 1, std::memory_order_release);
    return item;
  }

  /// Check if buffer is empty (safe from both Producer and Consumer).
  [[nodiscard]] bool empty() const noexcept {
    return head_.load(std::memory_order_relaxed) == tail_.load(std::memory_order_relaxed);
  }

  /// Check if buffer is full (safe from Producer).
  [[nodiscard]] bool full() const noexcept {
    return (head_.load(std::memory_order_relaxed) - tail_.load(std::memory_order_relaxed)) >= Capacity;
  }

  /// Approximate count of items in buffer (Unsigned modular arithmetic handles 2^32 wrap-around).
  [[nodiscard]] size_t size() const noexcept {
    const uint32_t head = head_.load(std::memory_order_relaxed);
    const uint32_t tail = tail_.load(std::memory_order_relaxed);
    return static_cast<size_t>(head - tail);
  }

  /// Buffer capacity (compile-time constant).
  [[nodiscard]] static constexpr size_t capacity() noexcept {
    return Capacity;
  }

  /// Reset buffer to empty state (Must be called only when neither producer nor consumer is active).
  void reset() noexcept {
    tail_.store(0, std::memory_order_relaxed);
    head_.store(0, std::memory_order_relaxed);
  }

private:
  // Head index: Modified ONLY by Producer (Cache-line isolated)
  alignas(64) std::atomic<uint32_t> head_{0};

  // Tail index: Modified ONLY by Consumer (Cache-line isolated)
  alignas(64) std::atomic<uint32_t> tail_{0};

  // Contiguous slot storage: Cache-line aligned
  alignas(64) T storage_[Capacity]{};
};

/// Multi-Producer Single-Consumer (MPSC) Hybrid Ring Buffer
/// - Producers: Serialized via lightweight SMP hardware spinlock (~20 cyc).
/// - Consumer: 100% Lock-Free pop via underlying SPSC ring buffer (~27 cyc).
template <typename T, size_t Capacity>
class SpinlockMpscRingBuffer {
public:
  constexpr SpinlockMpscRingBuffer() noexcept = default;
  ~SpinlockMpscRingBuffer() noexcept = default;

  SpinlockMpscRingBuffer(const SpinlockMpscRingBuffer &) = delete;
  SpinlockMpscRingBuffer &operator=(const SpinlockMpscRingBuffer &) = delete;
  SpinlockMpscRingBuffer(SpinlockMpscRingBuffer &&) = delete;
  SpinlockMpscRingBuffer &operator=(SpinlockMpscRingBuffer &&) = delete;

  /// Push item with producer-side spinlock serialization (Multi-Producer safe).
  [[nodiscard]] bool push(const T &item) noexcept {
    CriticalSectionLocker lock(mux_);
    return ring_.push(item);
  }

  /// Push item with move semantics and producer-side spinlock serialization.
  [[nodiscard]] bool push(T &&item) noexcept {
    CriticalSectionLocker lock(mux_);
    return ring_.push(std::move(item));
  }

  /// Pop item (Single-Consumer lock-free).
  [[nodiscard]] bool pop(T &out_item) noexcept {
    return ring_.pop(out_item);
  }

  /// Pop item returning std::optional (Single-Consumer lock-free).
  [[nodiscard]] std::optional<T> pop() noexcept {
    return ring_.pop();
  }

  [[nodiscard]] bool empty() const noexcept { return ring_.empty(); }
  [[nodiscard]] bool full() const noexcept { return ring_.full(); }
  [[nodiscard]] size_t size() const noexcept { return ring_.size(); }
  [[nodiscard]] static constexpr size_t capacity() noexcept { return Capacity; }

  void reset() noexcept {
    CriticalSectionLocker lock(mux_);
    ring_.reset();
  }

private:
  LocklessSpscRingBuffer<T, Capacity> ring_;
  portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
};

} // namespace Foundation
