// ============================================================================
// DoorphoneTracker: Level 2 Doorphone Protocol Framing & State Machine Implementation
// ============================================================================

#include "Transport/DoorphoneTracker.h"
#include <esp_timer.h>

extern QueueHandle_t g_ch4_passthrough_queue;

namespace Transport {

DoorphoneController g_doorphone_controller;

static void sendDpPacket(uint8_t stx, uint8_t op, uint8_t etx) {
  StaticPacket pkt{4, 5};
  pkt.data[0] = stx;
  pkt.data[1] = op;
  pkt.data[2] = 0x00;
  pkt.data[3] = 0x00;
  pkt.data[4] = etx;
  if (g_ch4_passthrough_queue) {
    xQueueSend(g_ch4_passthrough_queue, &pkt, 0);
  }
}

void DoorphoneController::init() {
  if (!_timer) {
    esp_timer_create_args_t timer_args{};
    timer_args.callback = onTimerCallback;
    timer_args.name = "dp_fsm_timer";
    esp_timer_create(&timer_args, &_timer);
  }
}

bool DoorphoneController::startSequence(uint8_t stx, uint8_t etx, uint8_t op_call,
                                        uint8_t op_open, uint8_t op_end) {
  Step expected = Step::IDLE;
  if (!_step.compare_exchange_strong(expected, Step::CALL_SENT)) {
    return false;
  }

  _c_stx.store(stx, std::memory_order_release);
  _c_etx.store(etx, std::memory_order_release);
  _op_open.store(op_open, std::memory_order_release);
  _op_end.store(op_end, std::memory_order_release);

  sendDpPacket(stx, op_call, etx);

  if (_timer) {
    esp_timer_start_once(_timer, 1000000); // 1초 후 문열림 패킷 전송
  }
  return true;
}

void DoorphoneController::cancel() {
  if (_timer) {
    esp_timer_stop(_timer);
  }
  _step.store(Step::IDLE, std::memory_order_release);
}

void DoorphoneController::onTimerCallback(void * /*arg*/) {
  Step cur = g_doorphone_controller._step.load(std::memory_order_acquire);
  if (cur == Step::CALL_SENT) {
    sendDpPacket(g_doorphone_controller._c_stx.load(std::memory_order_acquire),
                 g_doorphone_controller._op_open.load(std::memory_order_acquire),
                 g_doorphone_controller._c_etx.load(std::memory_order_acquire));
    g_doorphone_controller._step.store(Step::OPEN_SENT,
                                       std::memory_order_release);
    if (g_doorphone_controller._timer) {
      esp_timer_start_once(g_doorphone_controller._timer, 750000); // 750ms 후 종료 패킷
    }
  } else if (cur == Step::OPEN_SENT) {
    sendDpPacket(g_doorphone_controller._c_stx.load(std::memory_order_acquire),
                 g_doorphone_controller._op_end.load(std::memory_order_acquire),
                 g_doorphone_controller._c_etx.load(std::memory_order_acquire));
    g_doorphone_state.front_bell.store(false, std::memory_order_release);
    g_doorphone_state.lobby_bell.store(false, std::memory_order_release);
    g_doorphone_controller._step.store(Step::IDLE, std::memory_order_release);
  }
}

} // namespace Transport
