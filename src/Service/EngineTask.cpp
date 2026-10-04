// ============================================================================
// EngineTask: Level 4 RTOS Task Scheduling, Queues & RS-485 Engine
// Implementation
// ============================================================================

#include "Service/EngineTask.h"

namespace {
DeviceStateListener s_dev_listener = nullptr;
DoorphoneEventListener s_doorphone_listener = nullptr;
Ch5ForwardHandler s_ch5_forwarder = nullptr;
} // namespace

void Engine_RegisterDeviceStateListener(DeviceStateListener listener) noexcept {
  s_dev_listener = listener;
}

void Engine_RegisterDoorphoneListener(DoorphoneEventListener listener) noexcept {
  s_doorphone_listener = listener;
}

void Engine_RegisterCh5ForwardHandler(Ch5ForwardHandler handler) noexcept {
  s_ch5_forwarder = handler;
}

#include "esp_task_wdt.h"
#include <WiFi.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

// ── Core Repositories & Metrics Trackers ──
ControlDispatcher g_control_dispatcher;

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
static SemaphoreHandle_t s_ctrl_queue_mutex = nullptr;
static SemaphoreHandle_t s_uart0_mutex = nullptr, s_uart1_mutex = nullptr,
                         s_uart2_mutex = nullptr;
static std::atomic<bool> s_initial_caching_complete{false};

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
  s_ctrl_queue_mutex = xSemaphoreCreateMutex();

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

  Transport::Doorphone_RegisterTxHandler([](const StaticPacket &pkt) noexcept {
    if (s_ch4_passthrough_queue) {
      xQueueSend(s_ch4_passthrough_queue, &pkt, 0);
    }
  });
}

CoreDumpInfo g_coredump_info;

bool Queue_EnqueueDropHead(QueueHandle_t queue,
                           const StaticPacket &packet) noexcept {
  if (UNLIKELY(!queue))
    return false;
  MutexLocker lock(s_ctrl_queue_mutex);
  if (xQueueSend(queue, &packet, 0) == pdTRUE)
    return true;
  StaticPacket dummy;
  xQueueReceive(queue, &dummy, 0);
  return (xQueueSend(queue, &packet, 0) == pdTRUE);
}

// ============================================================================
// Internal Types & Forward Declarations
// ============================================================================

enum class UartRxStatus { SUCCESS, TIMEOUT };
using UartPollCallback = void (*)(void *ctx);

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

namespace PacketBuilder {
void Ch1_BuildQueryPacket(StaticPacket &out, uint8_t dev_id, uint8_t sub1,
                          uint8_t sub2);
}

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

UartRxStatus Uart_RecvPacket(uart_port_t u_num, StaticPacket &out,
                             uint32_t tout_ms, UartPollCallback on_poll,
                             void *poll_ctx, const StaticPacket *echo_match) {
  uint8_t stream[Config::Packet::MAX_STREAM_BUF];
  size_t stream_len = 0;
  uint32_t start_ms = millis();
  uint32_t last_rx_ms = 0;
  auto *const parser = WallpadParserFactory::getActiveParser();
  const bool is_auto_unlocked =
      (parser && parser->isAutoMode() && !parser->isLocked());
  const uint8_t stx = parser ? parser->getStx() : PKT_STX;
  QueueHandle_t evt_q = Uart_GetEventQueue(u_num);

  while (millis() - start_ms < tout_ms) {
    esp_task_wdt_reset();
    if (on_poll)
      on_poll(poll_ctx);

    if (stream_len >= 3) {
      if (is_auto_unlocked) {
        if (last_rx_ms > 0 &&
            TimeUtils::isElapsed(last_rx_ms,
                                 Config::Timing::WALLPAD_AUTO_IPG_MS)) {
          g_auto_probing_engine.feedFrame(
              span<const uint8_t>(stream, stream_len));

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
            idx++;
            continue;
          }

          int len_res =
              parser ? parser->extractPacketLength(stream, stream_len, idx)
                     : -1;
          if (len_res == 0) {
            break;
          }
          if (len_res < 0) {
            idx++;
            continue;
          }

          uint8_t pkt_len = static_cast<uint8_t>(len_res);
          uint8_t *pkt = &stream[idx];
          span<const uint8_t> pkt_span(pkt, pkt_len);
          if (!parser->validatePacket(pkt_span)) {
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
    uint32_t wait_ms = std::min<uint32_t>(rem_ms, 5);

    bool received_new_bytes = false;
    if (evt_q) {
      uart_event_t evt;
      if (xQueueReceive(evt_q, &evt, pdMS_TO_TICKS(wait_ms)) == pdTRUE) {
        if (evt.type == UART_DATA) {
          received_new_bytes = Uart_DrainToStreamBuffer(
              u_num, stream, stream_len, sizeof(stream), last_rx_ms);
        } else if (evt.type == UART_FIFO_OVF || evt.type == UART_BUFFER_FULL) {
          uart_flush_input(u_num);
          xQueueReset(evt_q);
        }
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(std::min<uint32_t>(wait_ms, 2)));
    }

    if (!received_new_bytes) {
      Uart_DrainToStreamBuffer(u_num, stream, stream_len, sizeof(stream),
                               last_rx_ms);
    }
  }

  if (stream_len >= 3 && is_auto_unlocked) {
    g_auto_probing_engine.feedFrame(span<const uint8_t>(stream, stream_len));
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
// 3. Control Dispatcher Pipeline
// ============================================================================

bool ControlDispatcher::dispatch(StaticPacket &req,
                                 StaticPacket &virtual_ack_out) {
  if (UNLIKELY(req.length < 5))
    return false;
  auto *parser = WallpadParserFactory::getActiveParser();
  if (!parser)
    return false;
  span<const uint8_t> frame(req.data.data(), req.length);

  uint8_t dev_id = 0, sub1 = 0, sub2 = 0;
  const bool has_key = parser->extractDeviceKey(frame, dev_id, sub1, sub2);

  if (parser->isQueryPacket(frame)) {
    virtual_ack_out.channel_id = req.channel_id;
    return has_key &&
           g_device_repo.copyVirtualAck(dev_id, sub1, sub2, virtual_ack_out);
  }

  GroupControlTemplate grp{};
  const bool has_grp =
      (has_key && dev_id != 0) && g_control_registry.findGroup(dev_id, grp);

  bool is_ctl = parser->isControlPacket(frame);
  if (!is_ctl && has_grp && grp.frame_len > 4 &&
      frame.size() >= grp.frame_len) {
    VendorProfileDescriptor desc;
    ProfileRepository::getActiveProfile(desc);
    const uint8_t op_off =
        (desc.opcode_offset < frame.size()) ? desc.opcode_offset : 4;
    is_ctl = (frame[op_off] == grp.raw_template[op_off]);
  }
  if (!is_ctl)
    return false;

  auto drop = [&]() {
    System_TracePacket(req.channel_id, false, TraceType::DRP, req);
    return false;
  };

  if (has_grp) {
    // 가스: 원격 '열림' 차단 (닫힘 값만 허용)
    if (grp.coverage.dev_class == DeviceClass::GAS &&
        grp.close_slot.discovered &&
        grp.close_slot.action_offset < req.length &&
        req.data[grp.close_slot.action_offset] != grp.close_slot.off_val)
      return drop();

    // 난방: 온도 설정 범위 검증
    const auto &ts = grp.temp_slot;
    if (grp.coverage.dev_class == DeviceClass::THERMOSTAT && ts.discovered &&
        ts.action_offset < req.length && ts.category_offset != 0xFF &&
        ts.category_offset < req.length &&
        req.data[ts.category_offset] == ts.category_val) {
      const uint8_t t = req.data[ts.action_offset];
      if (t < 5 || t > 35)
        return drop();
    }
  }

  RouteEndpoint ep{1, -1, 0};
  const bool route_known =
      has_key && g_route_registry.lookupRoute(dev_id, sub1, sub2, ep);

  if (route_known && ep.channel_id == 5 && ep.slot_idx >= 0 &&
      ep.slot_idx < Config::TCP::MAX_EW11_SLOTS) {
    const bool unidir = (has_grp && grp.isUnidirectional()) || dev_id == 0x34;
    if (s_ch5_forwarder) {
      s_ch5_forwarder(static_cast<uint8_t>(ep.slot_idx), req, unidir);
    }
    return false;
  }

  QueueHandle_t q =
      (req.channel_id == 6) ? s_ch1_vip_queue : s_ch1_control_queue;
  if (Queue_EnqueueDropHead(q, req))
    System_TracePacket(1, true, TraceType::CTL, req);
  return false;
}

namespace PacketCodec {
uint8_t calculateChecksum(std::span<const uint8_t> data) noexcept {
  auto *parser = WallpadParserFactory::getActiveParser();
  return parser ? parser->calculateChecksum(data) : 0;
}
} // namespace PacketCodec

namespace PacketBuilder {
void Ch1_BuildQueryPacket(StaticPacket &out, uint8_t dev_id, uint8_t sub1,
                          uint8_t sub2) {
  auto *parser = WallpadParserFactory::getActiveParser();
  if (parser) {
    parser->buildQueryPacket(dev_id, sub1, sub2, out);
  }
}
} // namespace PacketBuilder

static std::atomic<uint32_t> s_last_ch1_tx_ms{0};

void Ch1_RecordTxFinish() {
  s_last_ch1_tx_ms.store(millis(), std::memory_order_release);
}

void Ch1_WaitBusIdle(uint32_t silence_ms) {
  // 1. 연속 제어 명령 간 120ms Guard Interval 보장
  uint32_t last_tx = s_last_ch1_tx_ms.load(std::memory_order_acquire);
  if (last_tx > 0) {
    uint32_t now_tx = millis();
    constexpr uint32_t kGuardIntervalMs = 120;
    if (now_tx - last_tx < kGuardIntervalMs) {
      uint32_t rem_tx = kGuardIntervalMs - (now_tx - last_tx);
      if (rem_tx > 0) {
        vTaskDelay(pdMS_TO_TICKS(rem_tx) > 0 ? pdMS_TO_TICKS(rem_tx) : 1);
      }
    }
  }

  uint32_t last_act = g_pkt_stats.ch1.last_activity_ms.load(std::memory_order_acquire);
  uint32_t now_ms = millis();

  if (now_ms - last_act < silence_ms) {
    uint32_t rem_ms = silence_ms - (now_ms - last_act);
    if (rem_ms > 0) {
      TickType_t delay_ticks = pdMS_TO_TICKS(rem_ms);
      vTaskDelay(delay_ticks > 0 ? delay_ticks : 1);
    }
  }
}

void Ch1_HandleCtrl(const StaticPacket &ctrlPacket) {
  uint8_t dev_id = 0, sub1 = 0, sub2 = 0;
  auto *const parser = WallpadParserFactory::getActiveParser();
  if (parser) {
    span<const uint8_t> ctl_span(ctrlPacket.data.data(), ctrlPacket.length);
    parser->extractDeviceKey(ctl_span, dev_id, sub1, sub2);
  }

  Ch1_WaitBusIdle(Config::Timing::CH1_INTER_PACKET_DELAY_MS);

  {
    MutexLocker lock(s_uart0_mutex, pdMS_TO_TICKS(100));
    if (!lock.isLocked()) {
      g_pkt_stats.ch1.timeouts.fetch_add(1, std::memory_order_relaxed);
      System_TraceMessage(
          "[WARN] Dropped CH1 ctrl packet, mutex timed out.\r\n");
      return;
    }

    uart_flush_input(UART_NUM_0);
    uart_write_bytes(UART_NUM_0, ctrlPacket.data.data(), ctrlPacket.length);
    uart_wait_tx_done(UART_NUM_0,
                      pdMS_TO_TICKS(Config::Timing::UART_TX_DONE_TIMEOUT_MS));
    g_pkt_stats.ch1.last_activity_ms.store(millis(), std::memory_order_release);
    Ch1_RecordTxFinish();
    g_pkt_stats.ch1.tx_pkts.fetch_add(1, std::memory_order_relaxed);
  }

  StaticPacket ack;
  if (Uart_RecvPacket(UART_NUM_0, ack, Config::Timing::CH1_POLL_TIMEOUT_MS,
                      nullptr, nullptr, &ctrlPacket) == UartRxStatus::SUCCESS) {
    g_pkt_stats.ch1.last_activity_ms.store(millis(), std::memory_order_release);
    System_TracePacket(1, false, TraceType::ACK, ack);
    g_pkt_stats.ch1.rx_pkts.fetch_add(1, std::memory_order_relaxed);
    ack.channel_id = 1;
    if (s_dev_listener) {
      s_dev_listener(g_device_repo.updateFromBus(ack));
    }
    if (dev_id != 0) {
      g_route_registry.recordRoute(1, -1, dev_id, sub1, sub2);
    }
    ack.channel_id = ctrlPacket.channel_id;

    struct WallpadForwardConfig {
      uart_port_t uart_num;
      SemaphoreHandle_t &mutex;
      SingleChannelStats &stats;
    };
    const WallpadForwardConfig wp_cfg[] = {
        {UART_NUM_1, s_uart1_mutex, g_pkt_stats.ch2}, // CH2
        {UART_NUM_2, s_uart2_mutex, g_pkt_stats.ch3}, // CH3
    };

    int wp_idx =
        static_cast<int>(ctrlPacket.channel_id) - 2; // CH2 → 0, CH3 → 1
    if (wp_idx >= 0 && wp_idx <= 1) {
      const WallpadForwardConfig &cfg = wp_cfg[wp_idx];
      {
        MutexLocker lock(cfg.mutex, pdMS_TO_TICKS(100));
        if (lock.isLocked()) {
          uart_write_bytes(cfg.uart_num, ack.data.data(), ack.length);
          System_TracePacket(ctrlPacket.channel_id, true, TraceType::ACK,
                                ack);
          cfg.stats.tx_pkts.fetch_add(1, std::memory_order_relaxed);
        } else {
          System_TraceMessage("[WARN] UART mutex timeout forwarding ACK\r\n");
        }
      }
    }
  } else {
    g_pkt_stats.ch1.timeouts.fetch_add(1, std::memory_order_relaxed);
    System_TraceMessage(
        "[WARN] Device did not ACK control packet in time.\r\n");
  }
}

void Ch1_SetState(Ch1State &cur_state, Ch1State new_state) {
  if (cur_state != new_state) {
    Ch1State old = cur_state;
    cur_state = new_state;
    if (new_state == Ch1State::POLL_DEVICE) {
      g_ch1_state_metrics.poll_cnt.fetch_add(1, std::memory_order_relaxed);
    } else if (new_state == Ch1State::VIP_CONTROL) {
      g_ch1_state_metrics.vip_cnt.fetch_add(1, std::memory_order_relaxed);
    } else if (new_state == Ch1State::NORMAL_CONTROL) {
      g_ch1_state_metrics.normal_cnt.fetch_add(1, std::memory_order_relaxed);
    }

    g_ch1_state_metrics.last_from_state.store(static_cast<uint8_t>(old),
                                              std::memory_order_relaxed);
    g_ch1_state_metrics.last_to_state.store(static_cast<uint8_t>(new_state),
                                            std::memory_order_relaxed);
    g_ch1_state_metrics.last_transition_ms.store(millis(),
                                                 std::memory_order_relaxed);
  }
}

// ============================================================================
// 4. CH1 Polling Master Loop & Task (formerly Ch1Polling.cpp)
// ============================================================================

namespace {
constexpr uint32_t CACHE_CONVERGENCE_STABLE_MS = 1500;
} // namespace

namespace {
static inline int
Ch1_ScoreCandidate(const PollingTargetRegistry::PollingCandidate &tgt,
                   const DeviceStateEntry *cached_dev) {
  constexpr uint8_t CH23_MASK = (1 << 2) | (1 << 3);
  if (tgt.source_channels != 0 && (tgt.source_channels & CH23_MASK) == 0) {
    return 999;
  }

  RouteEndpoint ep;
  if (g_route_registry.lookupRoute(tgt.dev_id, tgt.sub1, tgt.sub2, ep) &&
      ep.channel_id == 5) {
    return 999;
  }

  if (tgt.raw_ack_len == 0 || !cached_dev || cached_dev->last_updated_ms == 0) {
    return 1;
  }
  if (cached_dev->is_online) {
    return 2;
  }
  if (TimeUtils::isElapsed(cached_dev->last_stale_poll_ms,
                           Config::Timing::CH1_STALE_POLL_INTERVAL_MS)) {
    return 3;
  }
  return 999;
}
} // namespace

void Ch1_PollNext(size_t &current_dev_idx) {
  g_polling_targets.sweepExpired(Config::Timing::STALE_DEVICE_THRESHOLD_MS);

  // 불필요한 9.7KB BSS 버퍼를 제거하고 224B 경량 메타데이터 스택 배열 활용
  PollingTargetRegistry::PollingCandidate
      candidates[PollingTargetRegistry::MAX_TARGETS];
  size_t active_cnt = g_polling_targets.getActiveCandidates(
      candidates, PollingTargetRegistry::MAX_TARGETS);

  uint8_t poll_dev_id = 0, poll_sub1 = 0, poll_sub2 = 0;
  const uint8_t *poll_raw_ptr = nullptr;
  uint8_t poll_raw_len = 0;
  bool target_selected = false;
  uint32_t now = millis();

  if (!target_selected && active_cnt > 0) {
    size_t chosen_idx = active_cnt;
    int chosen_score = 999;

    for (size_t i = 0; i < active_cnt; i++) {
      size_t idx = (current_dev_idx + i) % active_cnt;
      const auto &tgt = candidates[idx];
      const auto *cached_dev =
          g_device_repo.find(tgt.dev_id, tgt.sub1, tgt.sub2);
      int score = Ch1_ScoreCandidate(tgt, cached_dev);

      // 1순위(미검증/신규), 2순위(온라인 정상 주기), 3순위(10초 경과 오프라인 재탐색)
      // 라운드로빈 순서에서 처음 만나는 자격 충족 후보를 즉시 선택하여
      // 온라인 기기 독점에 의한 오프라인 기기 기아(Starvation) 방지
      if (score <= 3) {
        chosen_idx = idx;
        chosen_score = score;
        break;
      }
    }

    if (chosen_idx < active_cnt) {
      const auto &tgt = candidates[chosen_idx];
      poll_dev_id = tgt.dev_id;
      poll_sub1 = tgt.sub1;
      poll_sub2 = tgt.sub2;
      poll_raw_len = tgt.raw_query_len;
      if (poll_raw_len > 0) {
        g_polling_targets.getQueryData(tgt.entry_idx, poll_raw_ptr,
                                        poll_raw_len);
      }

      if (chosen_score == 3) {
        g_device_repo.setLastStalePollMs(tgt.dev_id, tgt.sub1, tgt.sub2, now);
        g_ch1_state_metrics.stale_poll_cnt.fetch_add(1,
                                                     std::memory_order_relaxed);
      }

      current_dev_idx = (chosen_idx + 1) % active_cnt;
      target_selected = true;
    }
  }

  if (!target_selected) {
    size_t dev_cnt = g_device_repo.count();
    if (dev_cnt > 0) {
      size_t idx = current_dev_idx % dev_cnt;
      auto *dev = g_device_repo.getAt(idx);
      current_dev_idx = (idx + 1) % dev_cnt;
      if (dev &&
          (dev->is_online || dev->last_updated_ms == 0 ||
           TimeUtils::isElapsed(dev->last_stale_poll_ms,
                                Config::Timing::CH1_STALE_POLL_INTERVAL_MS))) {
        RouteEndpoint ep;
        if (!g_route_registry.lookupRoute(dev->dev_id, dev->sub1, dev->sub2,
                                          ep) ||
            ep.channel_id != 5) {
          poll_dev_id = dev->dev_id;
          poll_sub1 = dev->sub1;
          poll_sub2 = dev->sub2;
          if (!dev->is_online)
            g_device_repo.setLastStalePollMsByIndex(idx, now);
          target_selected = true;
        }
      }
    }
  }

  if (target_selected) {
    constexpr uint8_t kMaxRetries = 3;
    constexpr uint32_t kDelayMs = Config::Timing::CH1_INTER_PACKET_DELAY_MS;
    constexpr TickType_t kUartLockTimeout = pdMS_TO_TICKS(5);
    bool sent = false;

    for (uint8_t retry = 0; retry < kMaxRetries; ++retry) {
      Ch1_WaitBusIdle(kDelayMs);

      MutexLocker lock(s_uart0_mutex, kUartLockTimeout);
      if (!lock.isLocked()) {
        // High-priority control transaction in progress on UART0 (Policy A:
        // abort poll cycle)
        return;
      }

      uint32_t last_act = g_pkt_stats.ch1.last_activity_ms.load(std::memory_order_acquire);
      uint32_t elapsed = millis() - last_act;
      if (elapsed < kDelayMs) {
        // TOCTOU: bus became active right before lock acquisition
        continue;
      }

      StaticPacket q_pkt;
      if (poll_raw_len > 0 && poll_raw_ptr) {
        q_pkt.channel_id = 1;
        q_pkt.length = poll_raw_len;
        memcpy(q_pkt.data.data(), poll_raw_ptr, poll_raw_len);
      } else {
        PacketBuilder::Ch1_BuildQueryPacket(q_pkt, poll_dev_id, poll_sub1,
                                            poll_sub2);
      }
      System_TracePacket(1, true, TraceType::QRY, q_pkt);

      uart_flush_input(UART_NUM_0);
      uart_write_bytes(UART_NUM_0, q_pkt.data.data(), q_pkt.length);
      uart_wait_tx_done(UART_NUM_0,
                        pdMS_TO_TICKS(Config::Timing::UART_TX_DONE_TIMEOUT_MS));
      g_pkt_stats.ch1.last_activity_ms.store(millis(), std::memory_order_release);
      g_pkt_stats.ch1.tx_pkts.fetch_add(1, std::memory_order_relaxed);

      StaticPacket ack;
      if (Uart_RecvPacket(UART_NUM_0, ack, Config::Timing::CH1_POLL_TIMEOUT_MS,
                          nullptr, nullptr, &q_pkt) == UartRxStatus::SUCCESS) {
        g_pkt_stats.ch1.last_activity_ms.store(millis(), std::memory_order_release);
        System_TracePacket(1, false, TraceType::ACK, ack);
        g_pkt_stats.ch1.rx_pkts.fetch_add(1, std::memory_order_relaxed);
        ack.channel_id = 1;
        g_polling_targets.updateResponse(q_pkt.data.data(), q_pkt.length,
                                         ack.data.data(), ack.length);
        if (s_dev_listener) {
          s_dev_listener(g_device_repo.updateFromBus(ack));
        }
        g_polling_targets.markVerified(poll_dev_id, poll_sub1, poll_sub2);
        g_route_registry.recordRoute(1, -1, poll_dev_id, poll_sub1, poll_sub2);
        g_auto_probing_engine.feedOpcodePair(
            span<const uint8_t>(q_pkt.data.data(), q_pkt.length),
            span<const uint8_t>(ack.data.data(), ack.length));
      } else {
        g_pkt_stats.ch1.timeouts.fetch_add(1, std::memory_order_relaxed);
        g_device_repo.handlePollingTimeout(poll_dev_id, poll_sub1, poll_sub2);
      }

      sent = true;
      break;
    }

    if (!sent) {
      // Abort poll cycle due to repeated TOCTOU bus activity
      return;
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

  static uint32_t s_stable_start_ms = 0;
  static size_t s_last_active_tgts = 0;
  static bool s_convergence_done = false;
  uint32_t next_poll_due_ms = millis();

  for (;;) {
    g_wdt_monitor.feed(0);
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
        g_pkt_stats.ch1.invalid_frames.fetch_add(1, std::memory_order_relaxed);
        uart_flush_input(UART_NUM_0);
      } else if (u_evt.type == UART_PARITY_ERR ||
                 u_evt.type == UART_FRAME_ERR) {
        g_pkt_stats.ch1.crc_errors.fetch_add(1, std::memory_order_relaxed);
      }
    }

    if (g_probe_convergence_reset.load(std::memory_order_acquire)) {
      g_probe_convergence_reset.store(false, std::memory_order_release);
      s_convergence_done = false;
      s_stable_start_ms = 0;
      s_last_active_tgts = 0;
      s_initial_caching_complete.store(false, std::memory_order_release);
      if (g_system_event_group) {
        xEventGroupClearBits(g_system_event_group, SYS_EVT_CACHE_READY);
      }
      System_TraceMessage("[AUTO PROBE] Convergence state reset. Re-learning "
                            "bus offsets...\r\n");
    }

    if (!s_convergence_done) {
      size_t active_tgts = g_polling_targets.activeCount();
      size_t online_devs = g_device_repo.getOnlineCount();

      if (active_tgts != s_last_active_tgts) {
        s_last_active_tgts = active_tgts;
        s_stable_start_ms = millis();
      }

      bool is_all_online = (online_devs >= active_tgts);
      auto *parser = WallpadParserFactory::getActiveParser();
      if (parser && parser->isAutoMode() &&
          !g_auto_probing_engine.isOffsetsLocked()) {
        is_all_online = (g_polling_targets.verifiedCount() >= active_tgts);
      }

      if (active_tgts > 0 && is_all_online) {
        if (s_stable_start_ms == 0) {
          s_stable_start_ms = millis();
        } else if (TimeUtils::isElapsed(
                       s_stable_start_ms,
                       CACHE_CONVERGENCE_STABLE_MS)) { // 1.5초간 신규 기기 증가
                                                       // 멈춤 & 전원 온라인
                                                       // 확인 시 최종 수렴!
          s_convergence_done = true;
          s_initial_caching_complete.store(true, std::memory_order_release);
          if (g_system_event_group) {
            xEventGroupSetBits(g_system_event_group, SYS_EVT_CACHE_READY);
          }
          if (parser && parser->isAutoMode() &&
              !g_auto_probing_engine.isOffsetsLocked()) {
            g_auto_probing_engine.analyzeCacheMatrix();
          }
          g_pkt_stats.resetAll();
          g_polling_targets.resetHits();
          g_metrics.reset();
          g_ch1_state_metrics.normal_cnt.store(0, std::memory_order_relaxed);
          g_ch1_state_metrics.vip_cnt.store(0, std::memory_order_relaxed);
          System_TraceMessage(
              "[SYSTEM MSG]  ★ 2nd-Tier Cache Converged (Zero Offline). "
              "Runtime metrics synchronized.\r\n");
          g_control_registry.synthesizeFromConvergedCache();
          System_TraceMessage(
              "[CTL] Control template synthesis triggered.\r\n");
        }
      } else {
        s_stable_start_ms = 0;
      }
    }

    size_t active_tgts = g_polling_targets.activeCount();
    const uint32_t poll_interval = (s_convergence_done || active_tgts == 0)
                                       ? g_timing_config.ch1_poll_interval_ms
                                       : 20;

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
      auto *parser = WallpadParserFactory::getActiveParser();
      span<const uint8_t> frame(ctrlPacket.data.data(), ctrlPacket.length);
      bool is_query = parser && parser->isQueryPacket(frame);

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
};

static void Ch2Ch3_DrainVirtualAckQueue(void *arg) {
  auto *ctx = static_cast<TaskAckPollContext *>(arg);
  if (!ctx || !ctx->ack_q || !ctx->cfg || !ctx->stats)
    return;

  StaticPacket next_ack;
  uint32_t next_due = 0;
  uint32_t now = millis();

  while (ctx->ack_q->peek(next_ack, next_due)) {
    if (now < next_due)
      break;
    if (ctx->ack_q->dequeue(next_ack, next_due)) {
      SemaphoreHandle_t u_mux =
          (ctx->cfg->uart_num == UART_NUM_1) ? s_uart1_mutex : s_uart2_mutex;
      if (u_mux) {
        MutexLocker lock(u_mux, pdMS_TO_TICKS(100));
        if (lock.isLocked()) {
          uart_write_bytes(ctx->cfg->uart_num, next_ack.data.data(),
                           next_ack.length);
        } else {
          ctx->stats->timeouts.fetch_add(1, std::memory_order_relaxed);
          System_TraceMessage("[WARN] UART mutex timeout on virtual ACK\r\n");
        }
      } else {
        uart_write_bytes(ctx->cfg->uart_num, next_ack.data.data(),
                         next_ack.length);
      }
      System_TracePacket(ctx->cfg->channel_id, true, TraceType::ACK,
                            next_ack);
      ctx->stats->tx_pkts.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

void Task_Ch2Ch3(void *pvParameters) {
  auto *cfg = static_cast<WallpadChannelConfig *>(pvParameters);
  if (!cfg)
    return;

  esp_task_wdt_add(nullptr);
  SingleChannelStats *stats = (cfg->channel_id == 2)   ? &g_pkt_stats.ch2
                              : (cfg->channel_id == 3) ? &g_pkt_stats.ch3
                                                       : nullptr;
  if (!stats) {
    esp_task_wdt_delete(nullptr);
    vTaskDelete(nullptr);
    return;
  }

  size_t task_idx = (cfg && cfg->channel_id == 3) ? 2 : 1;
  uart_flush_input(cfg->uart_num);
  TimestampedPacketQueue<8> ack_queue;

  if (g_system_event_group) {
    xEventGroupWaitBits(g_system_event_group, SYS_EVT_SYSTEM_RUNNING, pdFALSE,
                        pdFALSE, portMAX_DELAY);
  }

  TaskAckPollContext poll_ctx{&ack_queue, cfg, stats};

  for (;;) {
    g_wdt_monitor.feed(task_idx);
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

      auto *parser = WallpadParserFactory::getActiveParser();
      span<const uint8_t> frame(req.data.data(), req.length);

      bool is_query = parser->isQueryPacket(frame);
      System_TracePacket(cfg->channel_id, false,
                            is_query ? TraceType::QRY : TraceType::CTL, req);

      if (is_query) {
        uint8_t dev_id = 0, sub1 = 0, sub2 = 0;
        parser->extractDeviceKey(frame, dev_id, sub1, sub2);
        g_polling_targets.registerOrTouch(cfg->channel_id, dev_id, sub1, sub2,
                                          req.data.data(), req.length);
        StaticPacket virtual_ack;
        if (g_control_dispatcher.dispatch(req, virtual_ack)) {
          uint32_t delay_ms = (cfg->channel_id == 2)
                                  ? g_timing_config.ch2_cache_delay_ms
                                  : g_timing_config.ch3_cache_delay_ms;
          uint32_t target_due = millis() + delay_ms;
          if (!ack_queue.enqueue(virtual_ack, target_due)) {
            stats->uncached_pkts.fetch_add(1, std::memory_order_relaxed);
            System_TraceMessage("[WARN] Wallpad virtual ACK queue overflow, "
                                  "packet dropped.\r\n");
          }
        } else {
          stats->uncached_pkts.fetch_add(1, std::memory_order_relaxed);
        }
      } else {
        // CH2 및 CH3에서 쿼리가 아닌 제어 패킷이 수신되면 Auto Probing Engine에
        // 학습 전달
        g_auto_probing_engine.feedControlFrame(frame);
        if (parser->isControlPacket(frame)) {
          StaticPacket dummy_ack;
          g_control_dispatcher.dispatch(req, dummy_ack);
        }
      }
    }
  }
}

// ============================================================================
// 6. Channel 4 Sub-Wallpad Passthrough & Doorphone Bridge Engine
// ============================================================================

static inline void Ch4_SendPassthrough(const StaticPacket &pkt,
                                       StaticPacket &last_tx_pkt,
                                       uint32_t &last_tx_ms, char *cur_dp_ns) {
  if (pkt.length >= 3) {
    Config::Doorphone::FramingTracker::getNvsNamespace(g_config.wallpad_profile,
                                                       cur_dp_ns, 16);
    g_doorphone_tracker.processFrame(pkt.data[0], pkt.data[pkt.length - 1],
                                     pkt.length, cur_dp_ns);
  }
  System_TracePacket(4, true, TraceType::RMT, pkt);
  last_tx_pkt = pkt; // Correctly recorded in all code paths to avoid echo
                     // reflection misinterpretation
  g_doorphone_serial.write(pkt.data.data(), pkt.length);
  last_tx_ms = millis();
  g_pkt_stats.ch4.tx_pkts.fetch_add(1, std::memory_order_relaxed);
}

static inline void Ch4_HandleDoorphoneEvent(const StaticPacket &packet,
                                            StaticPacket &last_pkt,
                                            uint32_t &last_pkt_ms,
                                            uint32_t now) {
  bool is_debounce =
      (packet.length == last_pkt.length &&
       memcmp(packet.data.data(), last_pkt.data.data(), packet.length) == 0 &&
       !TimeUtils::isElapsed(last_pkt_ms,
                             Config::Timing::DOORPHONE_DEBOUNCE_MS));
  if (is_debounce)
    return;

  last_pkt = packet;
  last_pkt_ms = now;

  if (packet.length >= 2) {
    uint8_t opcode = packet.data[1];
    bool state_changed = false;
    uint8_t pkt_stx = packet.data[0];
    uint8_t pkt_etx = packet.data[packet.length - 1];
    const DoorphoneSpec *dp_prof =
        ProfileMatcher::matchDoorphone(pkt_stx, pkt_etx, packet.length);

    uint8_t bell_front = dp_prof ? dp_prof->bell_front : 0xB5;
    uint8_t bell_lobby = dp_prof ? dp_prof->bell_lobby : 0x5A;
    uint8_t end_front = dp_prof ? dp_prof->end_front : 0xB8;
    uint8_t end_lobby = dp_prof ? dp_prof->end_lobby : 0x60;

    if (opcode == bell_front) { // 현관 벨 호출
      g_doorphone_state.front_bell.store(true, std::memory_order_release);
      g_doorphone_state.last_bell_ms.store(now, std::memory_order_release);
      state_changed = true;
    } else if (opcode == end_front || opcode == 0xB6) { // 현관 무응답/통화 종료
      g_doorphone_state.front_bell.store(false, std::memory_order_release);
      state_changed = true;
    } else if (opcode == bell_lobby || opcode == 0x5F) { // 로비 벨/호출
      g_doorphone_state.lobby_bell.store(true, std::memory_order_release);
      g_doorphone_state.last_bell_ms.store(now, std::memory_order_release);
      state_changed = true;
    } else if (opcode == end_lobby) { // 로비 통화 종료
      g_doorphone_state.lobby_bell.store(false, std::memory_order_release);
      state_changed = true;
    }

    if (state_changed) {
      bool f = g_doorphone_state.front_bell.load(std::memory_order_relaxed);
      bool l = g_doorphone_state.lobby_bell.load(std::memory_order_relaxed);
      if (s_doorphone_listener) {
        s_doorphone_listener(f, l);
      }
    }
  }

  System_TracePacket(4, false, TraceType::RMT, packet);
  g_pkt_stats.ch4.rx_pkts.fetch_add(1, std::memory_order_relaxed);
}

static inline void Ch4_DropInvalidFrame(const uint8_t *data, size_t len) {
  StaticPacket drp_pkt{4, static_cast<uint8_t>(std::min<size_t>(len, 16))};
  memcpy(drp_pkt.data.data(), data, drp_pkt.length);
  System_TracePacket(4, false, TraceType::DRP, drp_pkt);
  g_pkt_stats.ch4.invalid_frames.fetch_add(1, std::memory_order_relaxed);
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
  char cur_dp_ns[16];
  Config::Doorphone::FramingTracker::getNvsNamespace(
      g_config.wallpad_profile, cur_dp_ns, sizeof(cur_dp_ns));

  if (g_system_event_group) {
    xEventGroupWaitBits(g_system_event_group, SYS_EVT_SYSTEM_RUNNING, pdFALSE,
                        pdFALSE, portMAX_DELAY);
  }

  if (!s_initial_caching_complete.load(std::memory_order_acquire)) {
    const uint32_t wait_start = millis();
    while (!s_initial_caching_complete.load(std::memory_order_acquire) &&
           (millis() - wait_start <
            Config::Timing::INITIAL_CACHING_GRACE_PERIOD_MS)) {
      g_wdt_monitor.feed(3);
      if (g_system_event_group) {
        EventBits_t bits = xEventGroupWaitBits(
            g_system_event_group, SYS_EVT_CACHE_READY, pdFALSE, pdFALSE,
            pdMS_TO_TICKS(200));
        if (bits & SYS_EVT_CACHE_READY) {
          break;
        }
      } else {
        vTaskDelay(pdMS_TO_TICKS(200));
      }
    }
    g_wdt_monitor.feed(3);
  }

  for (;;) {
    g_wdt_monitor.feed(3);
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
      Ch4_SendPassthrough(packet_to_tx, last_tx_pkt, last_tx_ms, cur_dp_ns);
    }

    const uint32_t last_bell =
        g_doorphone_state.last_bell_ms.load(std::memory_order_relaxed);
    if (last_bell > 0 &&
        TimeUtils::isElapsed(last_bell,
                             Config::Timing::DOORPHONE_BELL_TIMEOUT_MS)) {
      bool changed = false;
      if (g_doorphone_state.front_bell.load(std::memory_order_relaxed)) {
        g_doorphone_state.front_bell.store(false, std::memory_order_release);
        changed = true;
      }
      if (g_doorphone_state.lobby_bell.load(std::memory_order_relaxed)) {
        g_doorphone_state.lobby_bell.store(false, std::memory_order_release);
        changed = true;
      }
      if (changed) {
        g_doorphone_state.last_bell_ms.store(0, std::memory_order_release);
        if (s_doorphone_listener) {
          s_doorphone_listener(false, false);
        }
      }
    }

    const uint32_t ib_timeout = Config::Timing::getDoorphoneInterByteTimeoutMs(
        g_config.doorphone_baud_rate);
    uint32_t burst_spin_total = 0;
    while (g_doorphone_serial.available() > 0) {
      uint8_t byte = static_cast<uint8_t>(g_doorphone_serial.read());
      uint32_t now = millis();

      if (buf_len > 0 && last_byte_ms > 0 &&
          TimeUtils::isElapsed(last_byte_ms, ib_timeout)) {
        buf_len = 0;
      }

      if (buf_len < sizeof(buf)) {
        buf[buf_len++] = byte;
      } else {
        memmove(buf, buf + 1, buf_len - 1);
        buf_len--;
        buf[buf_len++] = byte;
      }
      last_byte_ms = now;

      if (burst_spin_total < 20) {
        uint32_t drain_start = millis();
        while (g_doorphone_serial.available() == 0 &&
               (millis() - drain_start < 6)) {
          esp_rom_delay_us(100);
        }
        burst_spin_total += (millis() - drain_start);
      }
    }

    Config::Doorphone::FramingStatus cur_status =
        g_doorphone_tracker.status.load(std::memory_order_relaxed);
    if (cur_status == Config::Doorphone::FramingStatus::LOCKED &&
        buf_len >= 3) {
      uint8_t target_stx =
          g_doorphone_tracker.candidate_stx.load(std::memory_order_relaxed);
      uint8_t target_etx =
          g_doorphone_tracker.candidate_etx.load(std::memory_order_relaxed);
      uint8_t target_len =
          g_doorphone_tracker.candidate_len.load(std::memory_order_relaxed);

      size_t p = 0;
      while (p < buf_len) {
        if (buf[p] != target_stx) {
          p++;
          continue;
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
          for (size_t i = p + 2; i < buf_len && (i - p + 1) <= 64; ++i) {
            if (buf[i] == target_etx) {
              frame_found = true;
              found_len = (i - p) + 1;
              break;
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

      if (cur_status == Config::Doorphone::FramingStatus::LOCKED) {
        buf_len = 0;
      } else {
        if (buf_len >= 3) {
          StaticPacket packet{4, static_cast<uint8_t>(buf_len)};
          memcpy(packet.data.data(), buf, buf_len);

          uint8_t pkt_stx = packet.data[0];
          uint8_t pkt_etx = packet.data[packet.length - 1];

          Config::Doorphone::FramingStatus prev_status =
              g_doorphone_tracker.status.load(std::memory_order_relaxed);

          // 프로파일 카탈로그에 일치하는 도어폰 규격이 있으면 즉시 영구
          // 잠금(LOCKED)
          const DoorphoneSpec *dp_spec =
              ProfileMatcher::matchDoorphone(pkt_stx, pkt_etx, 5);
          char cur_dp_ns_local[16];
          Config::Doorphone::FramingTracker::getNvsNamespace(
              g_config.wallpad_profile, cur_dp_ns_local,
              sizeof(cur_dp_ns_local));

          if (dp_spec && pkt_stx == dp_spec->stx && pkt_etx == dp_spec->etx &&
              packet.length >= 5) {
            if (prev_status != Config::Doorphone::FramingStatus::LOCKED) {
              g_doorphone_tracker.setFixedLock(dp_spec->stx, dp_spec->etx,
                                               dp_spec->len);
              g_doorphone_tracker.saveToNvs(cur_dp_ns_local);
            }
          } else {
            g_doorphone_tracker.processFrame(pkt_stx, pkt_etx, packet.length,
                                             cur_dp_ns_local);
            Config::Doorphone::FramingStatus status =
                g_doorphone_tracker.status.load(std::memory_order_relaxed);
            if (prev_status != Config::Doorphone::FramingStatus::LOCKED &&
                status == Config::Doorphone::FramingStatus::LOCKED) {
              g_doorphone_tracker.saveToNvs(cur_dp_ns_local);
            }
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
      Ch4_SendPassthrough(packet_to_tx, last_tx_pkt, last_tx_ms, cur_dp_ns);
    }
  }
}
