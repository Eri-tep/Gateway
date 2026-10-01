#pragma once

// ============================================================================
// DoorphoneTracker: Level 2 Doorphone Protocol Framing & State Machine (FSM)
// ============================================================================

#include "Base/SystemConfig.h"
#include "Base/SystemPlatform.h"
#include "System/LockUtils.h"

#include <atomic>
#include <esp_timer.h>

namespace Transport {

class DoorphoneController {
public:
  enum class Step : uint8_t { IDLE = 0, CALL_SENT, OPEN_SENT };

private:
  std::atomic<Step> _step{Step::IDLE};
  std::atomic<uint8_t> _c_stx{0x7F};
  std::atomic<uint8_t> _c_etx{0xEE};
  std::atomic<uint8_t> _op_open{0};
  std::atomic<uint8_t> _op_end{0};
  esp_timer_handle_t _timer{nullptr};

public:
  void init();
  bool startSequence(uint8_t stx, uint8_t etx, uint8_t op_call, uint8_t op_open,
                     uint8_t op_end);
  void cancel();
  [[nodiscard]] bool isBusy() const noexcept {
    return _step.load(std::memory_order_acquire) != Step::IDLE;
  }
  [[nodiscard]] Step getStep() const noexcept {
    return _step.load(std::memory_order_acquire);
  }

  static void onTimerCallback(void *arg);
};

extern DoorphoneController g_doorphone_controller;

} // namespace Transport
