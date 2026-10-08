// ============================================================================
// RS485_CH.cpp — L2 Transport / Data Link Channels
// Physical RS-485 Channel Implementation (CH1~CH4)
// ============================================================================
//
// STEP 4 MIGRATION: This file absorbs the full EngineTask.cpp God File contents.
// All queue storage, Task_Ch1/Ch2Ch3/Ch4 bodies, and channel helpers now reside
// here as the canonical L2 RS-485 channel implementation.
//
// STEP 5 CLEANUP (TODO): Split ControlDispatcher and L4 listener registration
// into separate EngineCore.cpp under Service/ layer.
//
// INVARIANTS (AGENTS.md):
//   - FreeRTOS queue handles are static (file-local). Zero extern leak (Rule 17).
//   - No dynamic allocation in Task RX/TX paths (Rule 4).
//   - RS485_EnqueueCh1Ctrl/Vip/Ch4Pass are the ONLY external TX entry points.
// ============================================================================
// ============================================================================
// EngineTask: Level 4 RTOS Task Scheduling, Queues & RS-485 Engine
// Implementation
// ============================================================================

#include "L2_Transport/RS485_CH.h"
#include "L1_HAL/Diagnostics_Driver.h"
#include "L1_HAL/Uart_Driver.h"

#include "esp_task_wdt.h"
#include <Arduino.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <span>

// ── Core Repositories & Metrics Trackers ──
static RS485_PacketDispatcher s_dispatcher{};

void RS485_RegisterDispatcher(const RS485_PacketDispatcher &dispatcher) noexcept {
  assert(dispatcher.onBuildPoll != nullptr);
  assert(dispatcher.onBusPacket != nullptr);
  assert(dispatcher.onTimeout != nullptr);
  assert(dispatcher.onDispatchControl != nullptr);
  assert(dispatcher.onGetPollIntervalMs != nullptr);
  assert(dispatcher.onGetStx != nullptr);
  assert(dispatcher.onExtractLength != nullptr);
  assert(dispatcher.onValidatePacket != nullptr);
  assert(dispatcher.onIsQueryPacket != nullptr);
  s_dispatcher = dispatcher;
}

// ── Static FreeRTOS Queues & Storage Pools (File-local) ──
static StaticQueue_t s_ch1_ctrl_queue_buf, s_ch4_pass_queue_buf, s_ch1_vip_queue_buf;
static uint8_t
    s_ch1_ctrl_storage[Config::Queue::POOL_SIZE_CONTROL * sizeof(StaticPacket)];
static uint8_t s_ch4_pass_storage[Config::Queue::POOL_SIZE_CH4_PASS *
                                  sizeof(StaticPacket)];
static uint8_t s_ch1_vip_storage[Config::Queue::POOL_SIZE_VIP * sizeof(StaticPacket)];

static QueueHandle_t s_ch1_control_queue = nullptr, s_ch1_vip_queue = nullptr;
static QueueSetHandle_t s_ch1_queue_set = nullptr;
static QueueHandle_t s_uart0_event_queue = nullptr, s_uart1_event_queue = nullptr,
                     s_uart2_event_queue = nullptr;
static QueueHandle_t s_ch4_passthrough_queue = nullptr;
static SemaphoreHandle_t s_uart0_mutex = nullptr, s_uart1_mutex = nullptr,
                         s_uart2_mutex = nullptr;

QueueHandle_t *Engine_GetUartEventQueuePtr(uint8_t uart_num) noexcept {
  if (uart_num == 0) return &s_uart0_event_queue;
  if (uart_num == 1) return &s_uart1_event_queue;
  if (uart_num == 2) return &s_uart2_event_queue;
  return nullptr;
}

void Engine_InitQueues() {
  s_uart0_mutex = xSemaphoreCreateMutex();
  s_uart1_mutex = xSemaphoreCreateMutex();
  s_uart2_mutex = xSemaphoreCreateMutex();
  assert(s_uart0_mutex != nullptr && s_uart1_mutex != nullptr &&
         s_uart2_mutex != nullptr);

  s_ch1_control_queue = xQueueCreateStatic(Config::Queue::POOL_SIZE_CONTROL, sizeof(StaticPacket),
                                           s_ch1_ctrl_storage, &s_ch1_ctrl_queue_buf);
  s_ch1_vip_queue = xQueueCreateStatic(Config::Queue::POOL_SIZE_VIP, sizeof(StaticPacket),
                                       s_ch1_vip_storage, &s_ch1_vip_queue_buf);
  s_ch4_passthrough_queue = xQueueCreateStatic(Config::Queue::POOL_SIZE_CH4_PASS, sizeof(StaticPacket),
                                               s_ch4_pass_storage, &s_ch4_pass_queue_buf);

  s_ch1_queue_set = xQueueCreateSet(Config::Queue::POOL_SIZE_CONTROL + Config::Queue::POOL_SIZE_VIP);
  if (s_ch1_queue_set) {
    xQueueAddToSet(s_ch1_vip_queue, s_ch1_queue_set);
    xQueueAddToSet(s_ch1_control_queue, s_ch1_queue_set);
  }
}

CoreDumpInfo g_coredump_info;

bool Queue_EnqueueDropTail(QueueHandle_t queue,
                           const StaticPacket &packet) noexcept {
  if (UNLIKELY(!queue))
    return false;
  if (xQueueSend(queue, &packet, 0) == pdTRUE) {
    return true;
  }
  Diag_RecordChannelQueueFull(1);
  return false;
}

// ── RS-485 Channel TX Enqueue (canonical L2 → internal queue bridge) ─────────

bool Engine_EnqueueCh1(const StaticPacket &pkt, bool vip) noexcept {
  QueueHandle_t q = vip ? s_ch1_vip_queue : s_ch1_control_queue;
  return Queue_EnqueueDropTail(q, pkt);
}

bool Engine_EnqueueCh4Pass(const StaticPacket &pkt) noexcept {
  if (UNLIKELY(!s_ch4_passthrough_queue))
    return false;
  return (xQueueSend(s_ch4_passthrough_queue, &pkt, 0) == pdTRUE);
}

void RS485_EnqueueCh4Passthrough(const StaticPacket &pkt) noexcept {
  Engine_EnqueueCh4Pass(pkt);
}

// ============================================================================
// Internal Types & Forward Declarations
// ============================================================================

enum class UartRxStatus { SUCCESS, TIMEOUT };
using UartPollCallback = uint32_t (*)(void *ctx);

QueueHandle_t Uart_GetEventQueue(uart_port_t u_num);
UartRxStatus Uart_RecvPacket(uart_port_t u_num, StaticPacket &out,
                             uint32_t tout_ms,
                             UartPollCallback on_poll = nullptr,
                             void *poll_ctx = nullptr,
                             const StaticPacket *echo_match = nullptr);

void Ch1_WaitBusIdle(uint32_t silence_ms);
void Ch1_HandleCtrl(const StaticPacket &ctrlPacket);
void Ch1_PollNext(size_t &current_dev_idx);
void Ch1_SetState(Ch1State &cur_state, Ch1State new_state);

// ============================================================================
// 2. UART RX Stream Demux & Packet Validation (formerly UartRx.cpp)
// ============================================================================

static inline bool Uart_DrainToStreamBuffer(uart_port_t u_num, uint8_t *stream,
                                            size_t &stream_len,
                                            size_t max_stream_buf,
                                            uint32_t &last_rx_ms) {
  size_t avail = 0;
  uart_get_buffered_data_len(u_num, &avail);
  if (avail == 0)
    return false;

  uint8_t temp[Config::Packet::UART_READ_CHUNK];
  size_t read_limit = std::min(avail, sizeof(temp));
  int rx = uart_read_bytes(u_num, temp, read_limit, 0);
  if (rx <= 0)
    return false;

  if (stream_len + rx > max_stream_buf) {
    size_t overflow = (stream_len + rx) - max_stream_buf;
    if (overflow < stream_len) {
      memmove(stream, stream + overflow, stream_len - overflow);
      stream_len -= overflow;
    } else {
      stream_len = 0;
    }
  }
  size_t copy_len =
      std::min(static_cast<size_t>(rx), max_stream_buf - stream_len);
  memcpy(stream + stream_len, temp, copy_len);
  stream_len += copy_len;
  last_rx_ms = millis();
  return true;
}

QueueHandle_t Uart_GetEventQueue(uart_port_t u_num) {
  switch (u_num) {
  case UART_NUM_0:
    return s_uart0_event_queue;
  case UART_NUM_1:
    return s_uart1_event_queue;
  case UART_NUM_2:
    return s_uart2_event_queue;
  default:
    return nullptr;
  }
}

// ── Persistent Per-Channel Stream Context (Prevents Packet Truncation on Return) ──
struct ChannelRxStreamContext {
  uint8_t stream[Config::Packet::MAX_STREAM_BUF]{};
  size_t stream_len{0};
  uint32_t last_rx_ms{0};
};

static ChannelRxStreamContext s_uart_rx_streams[3];

static inline void Uart_FlushChannelInput(uart_port_t u_num) {
  uart_flush_input(u_num);
  size_t u_idx = static_cast<size_t>(u_num);
  if (u_idx < 3) {
    s_uart_rx_streams[u_idx].stream_len = 0;
    s_uart_rx_streams[u_idx].last_rx_ms = 0;
  }
}

UartRxStatus Uart_RecvPacket(uart_port_t u_num, StaticPacket &out,
                             uint32_t tout_ms, UartPollCallback on_poll,
                             void *poll_ctx, const StaticPacket *echo_match) {
  size_t u_idx = static_cast<size_t>(u_num);
  if (u_idx >= 3)
    return UartRxStatus::TIMEOUT;

  auto &rx_ctx = s_uart_rx_streams[u_idx];
  uint8_t *stream = rx_ctx.stream;
  size_t &stream_len = rx_ctx.stream_len;
  uint32_t &last_rx_ms = rx_ctx.last_rx_ms;
  constexpr size_t max_stream_buf = sizeof(rx_ctx.stream);

  uint32_t start_ms = millis();
  const bool is_auto_unlocked =
      s_dispatcher.onIsAutoUnlocked ? s_dispatcher.onIsAutoUnlocked() : false;
  const uint8_t stx =
      s_dispatcher.onGetStx ? s_dispatcher.onGetStx() : PKT_STX;
  QueueHandle_t evt_q = Uart_GetEventQueue(u_num);

  while (millis() - start_ms < tout_ms) {
    esp_task_wdt_reset();
    uint32_t poll_max_wait = 25;
    if (on_poll)
      poll_max_wait = on_poll(poll_ctx);

    if (stream_len >= 3) {
      if (is_auto_unlocked) {
        if (last_rx_ms > 0 &&
            TimeUtils::isElapsed(last_rx_ms,
                                 Config::Timing::WALLPAD_AUTO_IPG_MS)) {
          if (s_dispatcher.onFeedAutoFrame) {
            s_dispatcher.onFeedAutoFrame(
                std::span<const uint8_t>(stream, stream_len));
          }

          if (echo_match && echo_match->length == stream_len &&
              memcmp(echo_match->data.data(), stream, stream_len) == 0) {
            stream_len = 0;
            last_rx_ms = 0;
            continue;
          }

          size_t copy_len = std::min(stream_len, out.data.size());
          out.length = static_cast<uint8_t>(copy_len);
          memcpy(out.data.data(), stream, copy_len);
          stream_len = 0;
          last_rx_ms = 0;
          return UartRxStatus::SUCCESS;
        }
      } else {
        size_t idx = 0;
        while (idx < stream_len) {
          if (stream[idx] != stx) {
            const void *stx_ptr = memchr(&stream[idx], stx, stream_len - idx);
            if (!stx_ptr) {
              break;
            }
            idx = static_cast<const uint8_t *>(stx_ptr) - stream;
          }

          int len_res =
              s_dispatcher.onExtractLength
                  ? s_dispatcher.onExtractLength(stream, stream_len, idx)
                  : -1;
          if (len_res == 0) [[unlikely]] {
            break;
          }
          if (len_res < 0) [[unlikely]] {
            idx++;
            continue;
          }

          uint8_t pkt_len = static_cast<uint8_t>(len_res);
          uint8_t *pkt = &stream[idx];
          std::span<const uint8_t> pkt_span(pkt, pkt_len);
          if (s_dispatcher.onValidatePacket &&
              !s_dispatcher.onValidatePacket(pkt_span)) [[unlikely]] {
            uint8_t ch = (u_num == UART_NUM_0)   ? 1
                         : (u_num == UART_NUM_1) ? 2
                                                 : 3;
            StaticPacket drp_pkt{ch, pkt_len};
            memcpy(drp_pkt.data.data(), pkt, pkt_len);
            System_TracePacket(ch, false, TraceType::DRP, drp_pkt);
            idx++;
            continue;
          }

          if (echo_match && echo_match->length == pkt_len &&
              memcmp(echo_match->data.data(), pkt, pkt_len) == 0) {
            idx += pkt_len;
            continue;
          }

          out.length = pkt_len;
          memcpy(out.data.data(), pkt, pkt_len);
          size_t consumed = idx + pkt_len;
          if (consumed < stream_len)
            memmove(stream, stream + consumed, stream_len - consumed);
          stream_len = (consumed < stream_len) ? (stream_len - consumed) : 0;
          return UartRxStatus::SUCCESS;
        }

        if (idx > 0) {
          if (idx < stream_len)
            memmove(stream, stream + idx, stream_len - idx);
          stream_len = (idx < stream_len) ? (stream_len - idx) : 0;
        }
      }
    }

    uint32_t elapsed = millis() - start_ms;
    if (elapsed >= tout_ms)
      break;
    uint32_t rem_ms = tout_ms - elapsed;
    uint32_t wait_ms = std::min<uint32_t>(rem_ms, std::max<uint32_t>(1, poll_max_wait));

    bool received_new_bytes = false;
    if (evt_q) {
      uart_event_t evt;
      if (xQueueReceive(evt_q, &evt, pdMS_TO_TICKS(wait_ms)) == pdTRUE) {
        if (evt.type == UART_DATA) {
          received_new_bytes = Uart_DrainToStreamBuffer(
              u_num, stream, stream_len, max_stream_buf, last_rx_ms);
        } else if (evt.type == UART_FIFO_OVF || evt.type == UART_BUFFER_FULL) {
          Uart_FlushChannelInput(u_num);
          xQueueReset(evt_q);
        }
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(std::min<uint32_t>(wait_ms, 2)));
    }

    if (!received_new_bytes) {
      Uart_DrainToStreamBuffer(u_num, stream, stream_len, max_stream_buf,
                               last_rx_ms);
    }
  }

  if (stream_len >= 3 && is_auto_unlocked) {
    if (s_dispatcher.onFeedAutoFrame) {
      s_dispatcher.onFeedAutoFrame(std::span<const uint8_t>(stream, stream_len));
    }
    if (!(echo_match && echo_match->length == stream_len &&
          memcmp(echo_match->data.data(), stream, stream_len) == 0)) {
      size_t copy_len = std::min(stream_len, out.data.size());
      out.length = static_cast<uint8_t>(copy_len);
      memcpy(out.data.data(), stream, copy_len);
      return UartRxStatus::SUCCESS;
    }
  }

  return UartRxStatus::TIMEOUT;
}

// ============================================================================
// 3. Control Enqueue Pipeline
// ============================================================================

bool RS485_EnqueueControl(const StaticPacket &pkt, bool vip) noexcept {
  QueueHandle_t q = vip ? s_ch1_vip_queue : s_ch1_control_queue;
  if (Queue_EnqueueDropTail(q, pkt)) {
    System_TracePacket(1, true, TraceType::CTL, pkt);
    return true;
  }
  return false;
}

static std::atomic<uint32_t> s_last_ch1_tx_ms{0};

void Ch1_RecordTxFinish() {
  s_last_ch1_tx_ms.store(millis(), std::memory_order_release);
}

void Ch1_WaitBusIdle(uint32_t silence_ms) {
  // 1. 연속 제어 명령 간 35ms 최소 Guard Interval 보장 (월패드 RX 버퍼 처리 여유 확보)
  uint32_t last_tx = s_last_ch1_tx_ms.load(std::memory_order_acquire);
  if (last_tx > 0) {
    uint32_t now_tx = millis();
    constexpr uint32_t kGuardIntervalMs = 35;
    if (now_tx - last_tx < kGuardIntervalMs) {
      uint32_t rem_tx = kGuardIntervalMs - (now_tx - last_tx);
      if (rem_tx > 0) {
        vTaskDelay(pdMS_TO_TICKS(rem_tx));
      }
    }
  }

  // 2. 물리 버스 무음(Bus Silence) 확인
  uint32_t last_act = Diag_GetChannelLastActivityMs(1);
  uint32_t now_ms = millis();

  if (now_ms - last_act < silence_ms) {
    uint32_t rem_ms = silence_ms - (now_ms - last_act);
    if (rem_ms > 0) {
      vTaskDelay(pdMS_TO_TICKS(rem_ms));
    }
  }
}

void Ch1_HandleCtrl(const StaticPacket &ctrlPacket) {
  Ch1_WaitBusIdle(Config::Timing::CH1_INTER_PACKET_DELAY_MS);

  StaticPacket ack;
  bool got_ack = false;

  {
    MutexLocker lock(s_uart0_mutex, pdMS_TO_TICKS(100));
    if (!lock.isLocked()) {
      Diag_RecordChannelLockTimeout(1);
      System_TraceMessage(
          "[WARN] Dropped CH1 ctrl packet, mutex timed out.\r\n");
      return;
    }

    Uart_FlushChannelInput(UART_NUM_0);
    uart_write_bytes(UART_NUM_0, ctrlPacket.data.data(), ctrlPacket.length);
    uart_wait_tx_done(UART_NUM_0,
                      pdMS_TO_TICKS(Config::Timing::UART_TX_DONE_TIMEOUT_MS));
    Diag_RecordChannelActivity(1, millis());
    Ch1_RecordTxFinish();
    Diag_RecordChannelTx(1);

    got_ack = (Uart_RecvPacket(UART_NUM_0, ack, Config::Timing::CH1_POLL_TIMEOUT_MS,
                               nullptr, nullptr, &ctrlPacket) == UartRxStatus::SUCCESS);
  }

  if (got_ack) {
    Diag_RecordChannelActivity(1, millis());
    System_TracePacket(1, false, TraceType::ACK, ack);
    Diag_RecordChannelRx(1);
    ack.channel_id = 1;
    if (s_dispatcher.onBusPacket) {
      Diag_ScopedCh1Latency sc;
      s_dispatcher.onBusPacket(1, ack, &ctrlPacket);
    }
    ack.channel_id = ctrlPacket.channel_id;

    struct WallpadForwardConfig {
      uart_port_t uart_num;
      SemaphoreHandle_t &mutex;
      uint8_t channel_id;
    };
    const WallpadForwardConfig wp_cfg[] = {
        {UART_NUM_1, s_uart1_mutex, 2}, // CH2
        {UART_NUM_2, s_uart2_mutex, 3}, // CH3
    };

    int wp_idx =
        static_cast<int>(ctrlPacket.channel_id) - 2; // CH2 → 0, CH3 → 1
    if (wp_idx >= 0 && wp_idx <= 1) {
      const WallpadForwardConfig &cfg = wp_cfg[wp_idx];
      bool fwd_success = false;
      {
        MutexLocker lock(cfg.mutex, pdMS_TO_TICKS(100));
        if (lock.isLocked()) {
          uart_write_bytes(cfg.uart_num, ack.data.data(), ack.length);
          fwd_success = true;
        } else {
          Diag_RecordChannelLockTimeout(cfg.channel_id);
          System_TraceMessage("[WARN] UART mutex timeout forwarding ACK\r\n");
        }
      }
      if (fwd_success) {
        System_TracePacket(ctrlPacket.channel_id, true, TraceType::ACK, ack);
        Diag_RecordChannelTx(cfg.channel_id);
      }
    }
  } else {
    Diag_RecordChannelTimeout(1);
    System_TraceMessage(
        "[WARN] Device did not ACK control packet in time.\r\n");
  }
}

void Ch1_SetState(Ch1State &cur_state, Ch1State new_state) noexcept {
  if (cur_state != new_state) {
    const Ch1State old = cur_state;
    cur_state = new_state;
    Diag_RecordCh1StateTransition(static_cast<uint8_t>(old),
                                  static_cast<uint8_t>(new_state),
                                  millis());
  }
}

// ============================================================================
// 4. CH1 Polling Master Loop & Task (formerly Ch1Polling.cpp)
// ============================================================================

void Ch1_PollNext(size_t &current_dev_idx) {
  (void)current_dev_idx;
  if (!s_dispatcher.onBuildPoll)
    return;

  StaticPacket q_pkt;
  uint8_t poll_dev_id = 0, poll_sub1 = 0, poll_sub2 = 0;
  if (!s_dispatcher.onBuildPoll(q_pkt, poll_dev_id, poll_sub1, poll_sub2)) {
    return;
  }

  constexpr uint8_t kMaxRetries = 3;
  constexpr uint32_t kDelayMs = Config::Timing::CH1_INTER_PACKET_DELAY_MS;
  constexpr TickType_t kUartLockTimeout = pdMS_TO_TICKS(50);
  StaticPacket ack;
  bool got_ack = false;
  bool tx_executed = false;

  for (uint8_t retry = 0; retry < kMaxRetries; ++retry) {
    Ch1_WaitBusIdle(kDelayMs);

    {
      MutexLocker lock(s_uart0_mutex, kUartLockTimeout);
      if (!lock.isLocked()) {
        Diag_RecordChannelLockTimeout(1);
        System_TraceMessage("[WARN] UART0 mutex timeout on poll\r\n");
        return;
      }

      uint32_t last_act = Diag_GetChannelLastActivityMs(1);
      uint32_t elapsed = millis() - last_act;
      if (elapsed < kDelayMs) {
        continue;
      }

      Uart_FlushChannelInput(UART_NUM_0);
      uart_write_bytes(UART_NUM_0, q_pkt.data.data(), q_pkt.length);
      uart_wait_tx_done(UART_NUM_0,
                        pdMS_TO_TICKS(Config::Timing::UART_TX_DONE_TIMEOUT_MS));
      Diag_RecordChannelActivity(1, millis());
      Diag_RecordChannelTx(1);
      tx_executed = true;

      got_ack = (Uart_RecvPacket(UART_NUM_0, ack, Config::Timing::CH1_POLL_TIMEOUT_MS,
                                 nullptr, nullptr, &q_pkt) == UartRxStatus::SUCCESS);
    }

    if (tx_executed) {
      System_TracePacket(1, true, TraceType::QRY, q_pkt);
      break;
    }
  }

  if (!tx_executed) {
    return;
  }

  if (got_ack) {
    Diag_RecordChannelActivity(1, millis());
    System_TracePacket(1, false, TraceType::ACK, ack);
    Diag_RecordChannelRx(1);
    ack.channel_id = 1;
    if (s_dispatcher.onBusPacket) {
      Diag_ScopedCh1Latency sc;
      s_dispatcher.onBusPacket(1, ack, &q_pkt);
    }
  } else {
    Diag_RecordChannelTimeout(1);
    if (s_dispatcher.onTimeout) {
      s_dispatcher.onTimeout(poll_dev_id, poll_sub1, poll_sub2);
    }
  }
}

void Task_Ch1(void *pvParameters) {
  esp_task_wdt_add(nullptr);
  if (g_system_event_group) {
    xEventGroupWaitBits(g_system_event_group, SYS_EVT_SYSTEM_RUNNING, pdFALSE,
                        pdFALSE, portMAX_DELAY);
  }
  StaticPacket ctrlPacket;
  size_t current_dev_idx = 0;
  Ch1State current_state = Ch1State::IDLE;
  uint32_t next_poll_due_ms = millis();

  for (;;) {
    System_FeedWdt(Config::Task::WDT_ID_CH1);
    if (UNLIKELY(g_ota_in_progress.load(std::memory_order_relaxed))) {
      Ch1_SetState(current_state, Ch1State::IDLE);
      if (g_system_event_group) {
        xEventGroupWaitBits(g_system_event_group, SYS_EVT_OTA_IDLE, pdFALSE,
                            pdFALSE, pdMS_TO_TICKS(1000));
      } else {
        vTaskDelay(pdMS_TO_TICKS(100));
      }
      next_poll_due_ms = millis();
      continue;
    }

    uart_event_t u_evt;
    while (xQueueReceive(s_uart0_event_queue, (void *)&u_evt, 0) == pdTRUE) {
      if (u_evt.type == UART_FIFO_OVF || u_evt.type == UART_BUFFER_FULL) {
        Diag_RecordChannelInvalidFrame(1);
        Uart_FlushChannelInput(UART_NUM_0);
      } else if (u_evt.type == UART_PARITY_ERR ||
                 u_evt.type == UART_FRAME_ERR) {
        Diag_RecordChannelCrcError(1);
      }
    }

    const uint32_t poll_interval = s_dispatcher.onGetPollIntervalMs();

    uint32_t now = millis();
    uint32_t rem_ms = (now < next_poll_due_ms) ? (next_poll_due_ms - now) : 0;
    TickType_t wait_ticks = (rem_ms > 0) ? pdMS_TO_TICKS(rem_ms) : 1;

    QueueSetMemberHandle_t activated = nullptr;
    if (s_ch1_queue_set) {
      activated = xQueueSelectFromSet(s_ch1_queue_set, wait_ticks);
    } else {
      vTaskDelay(wait_ticks);
    }

    if (s_ch1_vip_queue &&
        xQueueReceive(s_ch1_vip_queue, &ctrlPacket, 0) == pdTRUE) {
      Ch1_SetState(current_state, Ch1State::VIP_CONTROL);
      Ch1_HandleCtrl(ctrlPacket);
      Ch1_SetState(current_state, Ch1State::IDLE);
      continue; // VIP 처리 완료 후 다음 루프로 즉시 재평가
    }

    if (activated == s_ch1_control_queue && s_ch1_control_queue &&
        xQueueReceive(s_ch1_control_queue, &ctrlPacket, 0) == pdTRUE) {
      std::span<const uint8_t> frame(ctrlPacket.data.data(), ctrlPacket.length);
      bool is_query = s_dispatcher.onIsQueryPacket(frame);

      Ch1_SetState(current_state,
                   is_query ? Ch1State::POLL_DEVICE : Ch1State::NORMAL_CONTROL);
      Ch1_HandleCtrl(ctrlPacket);
      Ch1_SetState(current_state, Ch1State::IDLE);
      continue; // 일반 제어 처리 완료 후 다음 루프로 즉시 재평가
    }

    if (activated == nullptr || now >= next_poll_due_ms) {
      Ch1_SetState(current_state, Ch1State::POLL_DEVICE);
      Ch1_PollNext(current_dev_idx);
      Ch1_SetState(current_state, Ch1State::IDLE);
      next_poll_due_ms = millis() + poll_interval;
    }
  }
}

// ============================================================================
// 5. CH2/CH3 HW Wallpad Slaves & CH4 SW Doorphone (formerly Ch23Engine.cpp)
// ============================================================================

struct TaskAckPollContext {
  TimestampedPacketQueue<8> *ack_q;
  const WallpadChannelConfig *cfg;
  SingleChannelStats *stats;
  SemaphoreHandle_t uart_mutex;
};

static uint32_t Ch2Ch3_DrainVirtualAckQueue(void *arg) {
  auto *ctx = static_cast<TaskAckPollContext *>(arg);
  if (UNLIKELY(!ctx || !ctx->ack_q || !ctx->cfg || !ctx->stats))
    return 25;

  StaticPacket next_ack;
  uint32_t now = millis();

  while (ctx->ack_q->dequeueIfDue(now, next_ack)) {
    MutexLocker lock(ctx->uart_mutex, pdMS_TO_TICKS(100));
    if (LIKELY(lock.isLocked())) {
      uart_write_bytes(ctx->cfg->uart_num, next_ack.data.data(),
                       next_ack.length);
    } else {
      ctx->stats->lock_timeouts.fetch_add(1, std::memory_order_relaxed);
      System_TraceMessage("[WARN] UART mutex timeout on virtual ACK\r\n");
    }
    System_TracePacket(ctx->cfg->channel_id, true, TraceType::ACK,
                       next_ack);
    ctx->stats->tx_pkts.fetch_add(1, std::memory_order_relaxed);
  }

  const auto next_due = ctx->ack_q->getNextDueMs();
  if (next_due.has_value()) {
    now = millis();
    return (*next_due > now) ? std::max<uint32_t>(1, *next_due - now) : 1;
  }
  return 25;
}

static void RunSlaveChannelLoop(WallpadChannelConfig *cfg, size_t task_idx) {
  if (!cfg)
    return;

  esp_task_wdt_add(nullptr);
  SingleChannelStats *stats = Diag_GetChannelStats(cfg->channel_id);
  if (!stats) {
    esp_task_wdt_delete(nullptr);
    vTaskDelete(nullptr);
    return;
  }

  Uart_FlushChannelInput(cfg->uart_num);
  TimestampedPacketQueue<8> ack_queue;

  if (g_system_event_group) {
    xEventGroupWaitBits(g_system_event_group, SYS_EVT_SYSTEM_RUNNING, pdFALSE,
                        pdFALSE, portMAX_DELAY);
  }

  SemaphoreHandle_t u_mux = (cfg->uart_num == UART_NUM_1) ? s_uart1_mutex : s_uart2_mutex;
  TaskAckPollContext poll_ctx{&ack_queue, cfg, stats, u_mux};

  for (;;) {
    System_FeedWdt(task_idx);
    if (UNLIKELY(g_ota_in_progress.load(std::memory_order_relaxed))) {
      if (g_system_event_group) {
        xEventGroupWaitBits(g_system_event_group, SYS_EVT_OTA_IDLE, pdFALSE,
                            pdFALSE, pdMS_TO_TICKS(1000));
      } else {
        vTaskDelay(pdMS_TO_TICKS(100));
      }
      continue;
    }

    Ch2Ch3_DrainVirtualAckQueue(&poll_ctx);

    StaticPacket req;
    if (Uart_RecvPacket(cfg->uart_num, req, 100, Ch2Ch3_DrainVirtualAckQueue,
                        &poll_ctx) == UartRxStatus::SUCCESS) {
      stats->rx_pkts.fetch_add(1, std::memory_order_relaxed);
      req.channel_id = cfg->channel_id;
      std::span<const uint8_t> frame(req.data.data(), req.length);
      StaticPacket virtual_ack;
      if (s_dispatcher.onHandleSubBusQuery &&
          s_dispatcher.onHandleSubBusQuery(cfg->channel_id, req, virtual_ack)) [[likely]] {
        System_TracePacket(cfg->channel_id, false, TraceType::QRY, req);
        const auto &timing = TimingConfig_Get();
        uint32_t delay_ms = (cfg->channel_id == 2)
                                ? timing.ch2_cache_delay_ms
                                : timing.ch3_cache_delay_ms;
        uint32_t target_due = millis() + delay_ms;
        if (!ack_queue.enqueue(virtual_ack, target_due)) [[unlikely]] {
          stats->uncached_pkts.fetch_add(1, std::memory_order_relaxed);
          System_TraceMessage("[WARN] Wallpad virtual ACK queue overflow, "
                              "packet dropped.\r\n");
        }
      } else {
        if (s_dispatcher.onFeedControlFrame) [[unlikely]] {
          s_dispatcher.onFeedControlFrame(frame);
        }
        StaticPacket dummy_ack;
        if (s_dispatcher.onDispatchControl) [[unlikely]] {
          s_dispatcher.onDispatchControl(req, dummy_ack);
        }
      }
    }
  }
}

void Task_Ch2(void *pvParameters) {
  auto *cfg = static_cast<WallpadChannelConfig *>(pvParameters);
  RunSlaveChannelLoop(cfg, 1 /* CH2 WDT Slot */);
}

void Task_Ch3(void *pvParameters) {
  auto *cfg = static_cast<WallpadChannelConfig *>(pvParameters);
  RunSlaveChannelLoop(cfg, 2 /* CH3 WDT Slot */);
}

// ============================================================================
// 6. Channel 4 Sub-Wallpad Passthrough & Doorphone Bridge Engine
// ============================================================================

static inline void Ch4_SendPassthrough(const StaticPacket &pkt,
                                       StaticPacket &last_tx_pkt,
                                       uint32_t &last_tx_ms) {
  if (pkt.length >= 3 && s_dispatcher.onDoorphoneFrameDetected) {
    s_dispatcher.onDoorphoneFrameDetected(pkt.data[0], pkt.data[pkt.length - 1], pkt.length);
  }
  System_TracePacket(4, true, TraceType::RMT, pkt);
  last_tx_pkt = pkt; // Correctly recorded in all code paths to avoid echo
                     // reflection misinterpretation
  Uart_WriteSwSerial(pkt.data.data(), pkt.length);
  last_tx_ms = millis();
  Diag_RecordChannelTx(4);
}

static inline void Ch4_HandleDoorphoneEvent(const StaticPacket &packet,
                                            StaticPacket &last_pkt,
                                            uint32_t &last_pkt_ms,
                                            uint32_t now) {
  if (packet.length != last_pkt.length) [[likely]] {
    last_pkt = packet;
    last_pkt_ms = now;
    if (s_dispatcher.onDoorphonePacket) {
      s_dispatcher.onDoorphonePacket(packet);
    }
    System_TracePacket(4, false, TraceType::RMT, packet);
    Diag_RecordChannelRx(4);
    return;
  }

  bool is_debounce =
      (memcmp(packet.data.data(), last_pkt.data.data(), packet.length) == 0 &&
       !TimeUtils::isElapsed(last_pkt_ms,
                             Config::Timing::DOORPHONE_DEBOUNCE_MS));
  if (is_debounce) [[unlikely]]
    return;

  last_pkt = packet;
  last_pkt_ms = now;

  if (s_dispatcher.onDoorphonePacket) {
    s_dispatcher.onDoorphonePacket(packet);
  }

  System_TracePacket(4, false, TraceType::RMT, packet);
  Diag_RecordChannelRx(4);
}

static inline void Ch4_DropInvalidFrame(const uint8_t *data, size_t len) {
  StaticPacket drp_pkt{4, static_cast<uint8_t>(std::min<size_t>(len, 16))};
  memcpy(drp_pkt.data.data(), data, drp_pkt.length);
  System_TracePacket(4, false, TraceType::DRP, drp_pkt);
  Diag_RecordChannelInvalidFrame(4);
}

void Task_Ch4(void *pvParameters) {
  esp_task_wdt_add(nullptr);
  StaticPacket packet_to_tx;

  uint8_t buf[128] = {0};
  size_t buf_len = 0;
  uint32_t last_byte_ms = 0; // 마지막 수신 바이트 타임스탬프
  StaticPacket last_tx_pkt{};
  uint32_t last_tx_ms = 0;
  StaticPacket last_pkt{};
  uint32_t last_pkt_ms = 0;

  if (g_system_event_group) {
    xEventGroupWaitBits(g_system_event_group, SYS_EVT_SYSTEM_RUNNING, pdFALSE,
                        pdFALSE, portMAX_DELAY);
  }

  if (g_system_event_group) {
    const uint32_t wait_start = millis();
    while (millis() - wait_start <
           Config::Timing::INITIAL_CACHING_GRACE_PERIOD_MS) {
      System_FeedWdt(Config::Task::WDT_ID_CH4);
      EventBits_t bits = xEventGroupWaitBits(
          g_system_event_group, SYS_EVT_CACHE_READY, pdFALSE, pdFALSE,
          pdMS_TO_TICKS(200));
      if (bits & SYS_EVT_CACHE_READY) {
        break;
      }
    }
    System_FeedWdt(Config::Task::WDT_ID_CH4);
  }

  for (;;) {
    System_FeedWdt(Config::Task::WDT_ID_CH4);
    if (UNLIKELY(g_ota_in_progress.load(std::memory_order_relaxed))) {
      if (g_system_event_group) {
        xEventGroupWaitBits(g_system_event_group, SYS_EVT_OTA_IDLE, pdFALSE,
                            pdFALSE, pdMS_TO_TICKS(1000));
      } else {
        vTaskDelay(pdMS_TO_TICKS(100));
      }
      continue;
    }

    if (xQueueReceive(s_ch4_passthrough_queue, &packet_to_tx, 0) == pdTRUE) {
      Ch4_SendPassthrough(packet_to_tx, last_tx_pkt, last_tx_ms);
    }

    const uint32_t ib_timeout = Config::Timing::getDoorphoneInterByteTimeoutMs(
        Config_Get().doorphone_baud_rate);
    const int avail = Uart_AvailableSwSerial();
    if (avail > 0) {
      const uint32_t now = millis();
      if (buf_len > 0 && last_byte_ms > 0 &&
          TimeUtils::isElapsed(last_byte_ms, ib_timeout)) {
        buf_len = 0;
      }

      uint8_t temp[32];
      const int to_read = std::min(avail, static_cast<int>(sizeof(temp)));
      const int read_bytes = Uart_ReadSwSerial(temp, to_read);
      for (int i = 0; i < read_bytes; ++i) {
        if (buf_len < sizeof(buf)) {
          buf[buf_len++] = temp[i];
        } else {
          memmove(buf, buf + 1, buf_len - 1);
          buf[buf_len - 1] = temp[i];
        }
      }
      if (read_bytes > 0) {
        last_byte_ms = now;
      }
    }

    uint8_t target_stx = 0;
    uint8_t target_etx = 0;
    uint8_t target_len = 0;
    bool is_locked = s_dispatcher.onDoorphoneGetLockedFraming
        ? s_dispatcher.onDoorphoneGetLockedFraming(target_stx, target_etx, target_len)
        : false;

    if (is_locked && buf_len >= 3) {
      size_t p = 0;
      while (p < buf_len) {
        if (buf[p] != target_stx) {
          const void *hit = memchr(&buf[p], target_stx, buf_len - p);
          if (!hit) {
            break;
          }
          p = static_cast<const uint8_t *>(hit) - buf;
        }

        bool frame_found = false;
        size_t found_len = 0;

        if (target_len >= 3 && target_len <= 64) {
          if (buf_len - p >= target_len) {
            if (buf[p + target_len - 1] == target_etx) {
              frame_found = true;
              found_len = target_len;
            } else {
              Ch4_DropInvalidFrame(&buf[p], buf_len - p);
              p++;
              continue;
            }
          } else {
            break;
          }
        } else {
          const size_t search_limit = std::min(buf_len, p + 64);
          if (search_limit > p + 2) {
            const void *etx_hit = memchr(&buf[p + 2], target_etx, search_limit - (p + 2));
            if (etx_hit) {
              frame_found = true;
              found_len = (static_cast<const uint8_t *>(etx_hit) - &buf[p]) + 1;
            }
          }
        }

        if (frame_found) {
          if (last_tx_pkt.length == found_len &&
              memcmp(last_tx_pkt.data.data(), &buf[p], found_len) == 0 &&
              last_tx_ms > 0 && !TimeUtils::isElapsed(last_tx_ms, 250)) {
            p += found_len;
            last_byte_ms = 0;
            continue;
          }

          StaticPacket packet{4, static_cast<uint8_t>(found_len)};
          memcpy(packet.data.data(), &buf[p], found_len);

          Ch4_HandleDoorphoneEvent(packet, last_pkt, last_pkt_ms, millis());

          p += found_len;
          last_byte_ms = 0;
        } else {
          if (buf_len - p >= 64) {
            Ch4_DropInvalidFrame(&buf[p], buf_len - p);
            p++;
            continue;
          }
          break;
        }
      }

      if (p > 0) {
        if (p < buf_len) {
          memmove(buf, buf + p, buf_len - p);
          buf_len -= p;
        } else {
          buf_len = 0;
        }
      }
    }

    if (last_byte_ms > 0 &&
        TimeUtils::isElapsed(last_byte_ms, Config::Timing::DOORPHONE_IPG_MS)) {

      if (is_locked) {
        buf_len = 0;
      } else {
        if (buf_len >= 3) {
          StaticPacket packet{4, static_cast<uint8_t>(buf_len)};
          memcpy(packet.data.data(), buf, buf_len);

          uint8_t pkt_stx = packet.data[0];
          uint8_t pkt_etx = packet.data[packet.length - 1];

          if (s_dispatcher.onDoorphoneFrameDetected) {
            s_dispatcher.onDoorphoneFrameDetected(pkt_stx, pkt_etx, packet.length);
          }

          Ch4_HandleDoorphoneEvent(packet, last_pkt, last_pkt_ms, millis());
        }
        buf_len = 0;
      }
      last_byte_ms = 0;
    }

    uint32_t wait_ms = (buf_len > 0) ? 1 : 5;
    if (last_byte_ms > 0) {
      uint32_t elapsed = millis() - last_byte_ms;
      if (elapsed < Config::Timing::DOORPHONE_IPG_MS) {
        wait_ms =
            (buf_len > 0) ? 1 : (Config::Timing::DOORPHONE_IPG_MS - elapsed);
      } else {
        wait_ms = 1;
      }
    }
    wait_ms = std::max<uint32_t>(wait_ms, 1);

    if (xQueueReceive(s_ch4_passthrough_queue, &packet_to_tx,
                      pdMS_TO_TICKS(wait_ms)) == pdTRUE) {
      Ch4_SendPassthrough(packet_to_tx, last_tx_pkt, last_tx_ms);
    }
  }
}

// ============================================================================
// RS485_CH Public API Implementation (Canonical L2 TX Entry Points)
// ============================================================================
// These are the ONLY externally-visible paths into the CH1/CH4 TX queues.
// All other code must use these functions — never access queue handles directly.

void RS485_InitQueues() {
  Engine_InitQueues();
}

QueueHandle_t *RS485_GetUartEventQueuePtr(uint8_t uart_num) noexcept {
  return Engine_GetUartEventQueuePtr(uart_num);
}

[[nodiscard]] bool RS485_EnqueueCh1Ctrl(const StaticPacket &pkt) noexcept {
  return Engine_EnqueueCh1(pkt, false);
}

[[nodiscard]] bool RS485_EnqueueCh1Vip(const StaticPacket &pkt) noexcept {
  return Engine_EnqueueCh1(pkt, true);
}

[[nodiscard]] bool RS485_EnqueueCh4Pass(const StaticPacket &pkt) noexcept {
  return Engine_EnqueueCh4Pass(pkt);
}
