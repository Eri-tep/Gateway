#include "EngineInternal.h"
#include "esp_task_wdt.h"
#include <algorithm>
#include <cstring>

namespace {
constexpr uint32_t CACHE_CONVERGENCE_STABLE_MS = 1500;
} // namespace

void Ch1_PollNext(size_t &current_dev_idx) {
  g_polling_targets.sweepExpired(Config::Timing::STALE_DEVICE_THRESHOLD_MS);

  static PollingTargetEntry s_active_targets[PollingTargetRegistry::MAX_TARGETS];
  size_t active_cnt = g_polling_targets.getActiveTargets(s_active_targets, PollingTargetRegistry::MAX_TARGETS);

  uint8_t poll_dev_id = 0, poll_sub1 = 0, poll_sub2 = 0;
  const uint8_t *poll_raw_ptr = nullptr;
  uint8_t poll_raw_len = 0;
  bool target_selected = false;
  uint32_t now = millis();

  if (!target_selected && active_cnt > 0) {
    int best_prio = 999;
    size_t best_idx = 0;

    for (size_t i = 0; i < active_cnt; i++) {
      size_t idx = (current_dev_idx + i) % active_cnt;
      const auto &tgt = s_active_targets[idx];

      constexpr uint8_t CH23_MASK = (1 << 2) | (1 << 3);
      if (tgt.source_channels != 0 && (tgt.source_channels & CH23_MASK) == 0) {
        continue;
      }

      RouteEndpoint ep;
      if (g_route_registry.lookupRoute(tgt.dev_id, tgt.sub1, tgt.sub2, ep) && ep.channel_id == 5) {
        continue;
      }

      const auto *cached_dev = g_device_repo.find(tgt.dev_id, tgt.sub1, tgt.sub2);

      if (tgt.raw_ack_len == 0 || !cached_dev || cached_dev->last_updated_ms == 0) {
        best_prio = 1;
        best_idx = idx;
        break;
      } else if (cached_dev->is_online) {
        if (best_prio > 2) {
          best_prio = 2;
          best_idx = idx;
        }
      } else if (TimeUtils::isElapsed(cached_dev->last_stale_poll_ms, Config::Timing::CH1_STALE_POLL_INTERVAL_MS)) {
        if (best_prio > 3) {
          best_prio = 3;
          best_idx = idx;
        }
      }
    }

    if (best_prio <= 3) {
      const auto &tgt = s_active_targets[best_idx];
      poll_dev_id = tgt.dev_id;
      poll_sub1 = tgt.sub1;
      poll_sub2 = tgt.sub2;
      poll_raw_len = tgt.raw_query_len;
      if (poll_raw_len > 0)
        poll_raw_ptr = tgt.raw_query_data.data();

      if (best_prio == 3) {
        g_device_repo.setLastStalePollMs(tgt.dev_id, tgt.sub1, tgt.sub2, now);
        g_ch1_state_metrics.stale_poll_cnt.fetch_add(1, std::memory_order_relaxed);
      }

      current_dev_idx = (best_idx + 1) % active_cnt;
      target_selected = true;
    }
  }

  if (!target_selected) {
    size_t dev_cnt = g_device_repo.count();
    if (dev_cnt > 0) {
      size_t idx = current_dev_idx % dev_cnt;
      auto *dev = g_device_repo.getAt(idx);
      current_dev_idx = (idx + 1) % dev_cnt;
      if (dev && (dev->is_online || dev->last_updated_ms == 0 ||
                  TimeUtils::isElapsed(dev->last_stale_poll_ms, Config::Timing::CH1_STALE_POLL_INTERVAL_MS))) {
        RouteEndpoint ep;
        if (!g_route_registry.lookupRoute(dev->dev_id, dev->sub1, dev->sub2, ep) || ep.channel_id != 5) {
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
    MutexLocker lock(g_uart0_mutex, pdMS_TO_TICKS(100));
    if (lock.isLocked()) {
      StaticPacket q_pkt;
      if (poll_raw_len > 0 && poll_raw_ptr) {
        q_pkt.channel_id = 1;
        q_pkt.length = poll_raw_len;
        memcpy(q_pkt.data.data(), poll_raw_ptr, poll_raw_len);
      } else {
        PacketBuilder::Ch1_BuildQueryPacket(q_pkt, poll_dev_id, poll_sub1,
                                             poll_sub2);
      }
      g_telnet_tracer.trace(1, true, TraceType::QRY, q_pkt);
      Ch1_WaitBusIdle(Config::Timing::CH1_INTER_PACKET_DELAY_MS);

      uart_flush_input(UART_NUM_0);
      uart_write_bytes(UART_NUM_0, q_pkt.data.data(), q_pkt.length);
      uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(Config::Timing::UART_TX_DONE_TIMEOUT_MS));
      g_ch1_bus_ms.store(millis(), std::memory_order_release);
      g_pkt_stats.ch1.tx_pkts.fetch_add(1, std::memory_order_relaxed);

      StaticPacket ack;
      if (Uart_RecvPacket(UART_NUM_0, ack,
                          Config::Timing::CH1_POLL_TIMEOUT_MS,
                          nullptr, nullptr,
                          &q_pkt) ==
          UartRxStatus::SUCCESS) {
        g_ch1_bus_ms.store(millis(), std::memory_order_release);
        g_telnet_tracer.trace(1, false, TraceType::ACK, ack);
        g_pkt_stats.ch1.rx_pkts.fetch_add(1, std::memory_order_relaxed);
        ack.channel_id = 1;
        g_polling_targets.updateResponse(q_pkt.data.data(), q_pkt.length,
                                         ack.data.data(), ack.length);
        g_device_repo.updateFromBus(ack);
        g_polling_targets.markVerified(poll_dev_id, poll_sub1, poll_sub2);
        g_route_registry.recordRoute(1, -1, poll_dev_id, poll_sub1, poll_sub2);
        g_auto_probing_engine.feedOpcodePair(
            span<const uint8_t>(q_pkt.data.data(), q_pkt.length),
            span<const uint8_t>(ack.data.data(), ack.length));
      } else {
        g_pkt_stats.ch1.timeouts.fetch_add(1, std::memory_order_relaxed);
        g_device_repo.handlePollingTimeout(poll_dev_id, poll_sub1, poll_sub2);
      }
    }
  }
}
void Task_Ch1(void *pvParameters) {
  esp_task_wdt_add(nullptr);
  if (g_system_event_group) {
    xEventGroupWaitBits(g_system_event_group, SYS_EVT_SYSTEM_RUNNING, pdFALSE, pdFALSE, portMAX_DELAY);
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
        xEventGroupWaitBits(g_system_event_group, SYS_EVT_OTA_IDLE, pdFALSE, pdFALSE, pdMS_TO_TICKS(1000));
      } else {
        vTaskDelay(pdMS_TO_TICKS(100));
      }
      next_poll_due_ms = millis();
      continue;
    }

    uart_event_t u_evt;
    while (xQueueReceive(g_uart0_event_queue, (void *)&u_evt, 0) == pdTRUE) {
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
      g_initial_caching_complete.store(false, std::memory_order_release);
      if (g_system_event_group) {
        xEventGroupClearBits(g_system_event_group, SYS_EVT_CACHE_READY);
      }
      g_telnet_tracer.trace("[AUTO PROBE] Convergence state reset. Re-learning bus offsets...\r\n");
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
      if (parser && parser->isAutoMode() && !g_auto_probing_engine.isOffsetsLocked()) {
        is_all_online = (g_polling_targets.verifiedCount() >= active_tgts);
      }

      if (active_tgts > 0 && is_all_online) {
        if (s_stable_start_ms == 0) {
          s_stable_start_ms = millis();
        } else if (TimeUtils::isElapsed(s_stable_start_ms, CACHE_CONVERGENCE_STABLE_MS)) { // 1.5초간 신규 기기 증가 멈춤 & 전원 온라인 확인 시 최종 수렴!
          s_convergence_done = true;
          g_initial_caching_complete.store(true, std::memory_order_release);
          if (g_system_event_group) {
            xEventGroupSetBits(g_system_event_group, SYS_EVT_CACHE_READY);
          }
          if (parser && parser->isAutoMode() && !g_auto_probing_engine.isOffsetsLocked()) {
            MutexLocker u0_lock(g_uart0_mutex, pdMS_TO_TICKS(100));
            Ch1_WaitBusIdle(Config::Timing::CH1_INTER_PACKET_DELAY_MS);
            g_auto_probing_engine.analyzeCacheMatrix();
          }
          g_pkt_stats.resetAll();
          g_polling_targets.resetHits();
          g_metrics.reset();
          g_ch1_state_metrics.normal_cnt.store(0, std::memory_order_relaxed);
          g_ch1_state_metrics.vip_cnt.store(0, std::memory_order_relaxed);
          g_telnet_tracer.trace("[SYSTEM MSG]  ★ 2nd-Tier Cache Converged (Zero Offline). Runtime metrics synchronized.\r\n");
          g_control_registry.synthesizeFromConvergedCache();
          g_telnet_tracer.trace("[CTL] Control template synthesis triggered.\r\n");
        }
      } else {
        s_stable_start_ms = 0;
      }
    }

    size_t active_tgts = g_polling_targets.activeCount();
    const uint32_t poll_interval =
        (s_convergence_done || active_tgts == 0)
            ? g_timing_config.ch1_poll_interval_ms
            : 20;

    uint32_t now = millis();
    uint32_t rem_ms = (now < next_poll_due_ms) ? (next_poll_due_ms - now) : 0;
    TickType_t wait_ticks = (rem_ms > 0) ? pdMS_TO_TICKS(rem_ms) : 1;

    QueueSetMemberHandle_t activated = nullptr;
    if (g_ch1_queue_set) {
      activated = xQueueSelectFromSet(g_ch1_queue_set, wait_ticks);
    } else {
      vTaskDelay(wait_ticks);
    }

    if (g_ch1_vip_queue && xQueueReceive(g_ch1_vip_queue, &ctrlPacket, 0) == pdTRUE) {
      Ch1_SetState(current_state, Ch1State::VIP_CONTROL);
      Ch1_HandleCtrl(ctrlPacket);
      Ch1_SetState(current_state, Ch1State::IDLE);
      continue; // VIP 처리 완료 후 다음 루프로 즉시 재평가
    }

    if (activated == g_ch1_control_queue && g_ch1_control_queue &&
        xQueueReceive(g_ch1_control_queue, &ctrlPacket, 0) == pdTRUE) {
      auto *parser = WallpadParserFactory::getActiveParser();
      span<const uint8_t> frame(ctrlPacket.data.data(), ctrlPacket.length);
      bool is_query = parser && parser->isQueryPacket(frame);

      Ch1_SetState(current_state, is_query ? Ch1State::POLL_DEVICE : Ch1State::NORMAL_CONTROL);
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
