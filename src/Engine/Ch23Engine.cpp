#include "EngineInternal.h"
#include "esp_task_wdt.h"
#include <algorithm>
#include <cstring>

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
    xEventGroupWaitBits(g_system_event_group, SYS_EVT_SYSTEM_RUNNING, pdFALSE, pdFALSE, portMAX_DELAY);
  }

  for (;;) {
    g_wdt_monitor.feed(task_idx);
    if (UNLIKELY(g_ota_in_progress.load(std::memory_order_relaxed))) {
      if (g_system_event_group) {
        xEventGroupWaitBits(g_system_event_group, SYS_EVT_OTA_IDLE, pdFALSE, pdFALSE, pdMS_TO_TICKS(1000));
      } else {
        vTaskDelay(pdMS_TO_TICKS(100));
      }
      continue;
    }

    struct TaskAckPollContext {
      TimestampedPacketQueue<8> *ack_q;
      const WallpadChannelConfig *cfg;
      SingleChannelStats *stats;
    };
    TaskAckPollContext poll_ctx{&ack_queue, cfg, stats};

    auto poll_ack = [](void *arg) {
      auto *ctx = static_cast<TaskAckPollContext *>(arg);
      if (!ctx || !ctx->ack_q || !ctx->cfg || !ctx->stats)
        return;

      StaticPacket next_ack;
      uint32_t next_due = 0;
      uint32_t now = millis();

      while (ctx->ack_q->peek(next_ack, next_due)) {
        if (now >= next_due) {
          if (ctx->ack_q->dequeue(next_ack, next_due)) {
            SemaphoreHandle_t u_mux =
                (ctx->cfg->uart_num == UART_NUM_1)   ? g_uart1_mutex
                : (ctx->cfg->uart_num == UART_NUM_2) ? g_uart2_mutex
                                                     : nullptr;
            if (u_mux) {
              MutexLocker lock(u_mux, pdMS_TO_TICKS(100));
              if (lock.isLocked()) {
                uart_write_bytes(ctx->cfg->uart_num, next_ack.data.data(),
                                 next_ack.length);
              } else {
                ctx->stats->timeouts.fetch_add(1, std::memory_order_relaxed);
                g_telnet_tracer.trace(
                    "[WARN] UART mutex timeout on virtual ACK\r\n");
              }
            } else {
              uart_write_bytes(ctx->cfg->uart_num, next_ack.data.data(),
                               next_ack.length);
            }
            g_telnet_tracer.trace(ctx->cfg->channel_id, true, TraceType::ACK,
                               next_ack);
            ctx->stats->tx_pkts.fetch_add(1, std::memory_order_relaxed);
          }
        } else {
          break;
        }
      }
    };

    poll_ack(&poll_ctx);

    StaticPacket req;
    if (Uart_RecvPacket(cfg->uart_num, req, 100, poll_ack, &poll_ctx) ==
        UartRxStatus::SUCCESS) {
      stats->rx_pkts.fetch_add(1, std::memory_order_relaxed);
      req.channel_id = cfg->channel_id;

      auto *parser = WallpadParserFactory::getActiveParser();
      span<const uint8_t> frame(req.data.data(), req.length);

      bool is_query = parser->isQueryPacket(frame);
      g_telnet_tracer.trace(cfg->channel_id, false,
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
            g_telnet_tracer.trace("[WARN] Wallpad virtual ACK queue overflow, "
                               "packet dropped.\r\n");
          }
        } else {
          stats->uncached_pkts.fetch_add(1, std::memory_order_relaxed);
        }
      } else {
        // CH2 및 CH3에서 쿼리가 아닌 제어 패킷이 수신되면 Auto Probing Engine에 학습 전달
        g_auto_probing_engine.feedControlFrame(frame);
        if (parser->isControlPacket(frame)) {
          StaticPacket dummy_ack;
          g_control_dispatcher.dispatch(req, dummy_ack);
        }
      }
    }
  }
}

void Task_Ch4(void *pvParameters) {
  esp_task_wdt_add(nullptr);
  StaticPacket packet_to_tx;

  static uint8_t buf[128] = {0};
  static size_t buf_len = 0;
  static uint32_t last_byte_ms = 0;  // 마지막 수신 바이트 타임스탬프
  static StaticPacket last_tx_pkt{};
  static uint32_t last_tx_ms = 0;
  static StaticPacket last_pkt{};
  static uint32_t last_pkt_ms = 0;

  if (g_system_event_group) {
    xEventGroupWaitBits(g_system_event_group, SYS_EVT_SYSTEM_RUNNING, pdFALSE, pdFALSE, portMAX_DELAY);
  }

  if (!g_initial_caching_complete.load(std::memory_order_acquire)) {
    if (g_system_event_group) {
      xEventGroupWaitBits(g_system_event_group, SYS_EVT_CACHE_READY, pdFALSE, pdFALSE,
                          pdMS_TO_TICKS(Config::Timing::INITIAL_CACHING_GRACE_PERIOD_MS));
    } else {
      vTaskDelay(pdMS_TO_TICKS(Config::Timing::INITIAL_CACHING_GRACE_PERIOD_MS));
    }
    g_wdt_monitor.feed(3);
  }

  for (;;) {
    g_wdt_monitor.feed(3);
    if (UNLIKELY(g_ota_in_progress.load(std::memory_order_relaxed))) {
      if (g_system_event_group) {
        xEventGroupWaitBits(g_system_event_group, SYS_EVT_OTA_IDLE, pdFALSE, pdFALSE, pdMS_TO_TICKS(1000));
      } else {
        vTaskDelay(pdMS_TO_TICKS(100));
      }
      continue;
    }

    if (xQueueReceive(g_ch4_passthrough_queue, &packet_to_tx, 0) == pdTRUE) {
      if (packet_to_tx.length >= 3) {
        g_doorphone_tracker.processFrame(packet_to_tx.data[0], packet_to_tx.data[packet_to_tx.length - 1], packet_to_tx.length);
      }
      g_telnet_tracer.trace(4, true, TraceType::RMT, packet_to_tx);
      last_tx_pkt = packet_to_tx;
      g_doorphone_serial.write(packet_to_tx.data.data(), packet_to_tx.length);
      last_tx_ms = millis();
      g_pkt_stats.ch4.tx_pkts.fetch_add(1, std::memory_order_relaxed);
    }

    const uint32_t ib_timeout = Config::Timing::getDoorphoneInterByteTimeoutMs(g_config.doorphone_baud_rate);
    uint32_t burst_spin_total = 0;
    while (g_doorphone_serial.available() > 0) {
      uint8_t byte = static_cast<uint8_t>(g_doorphone_serial.read());
      uint32_t now = millis();

      if (buf_len > 0 && last_byte_ms > 0 && TimeUtils::isElapsed(last_byte_ms, ib_timeout)) {
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
        while (g_doorphone_serial.available() == 0 && (millis() - drain_start < 6)) {
          esp_rom_delay_us(100);
        }
        burst_spin_total += (millis() - drain_start);
      }
    }

    Config::Doorphone::FramingStatus cur_status = g_doorphone_tracker.status.load(std::memory_order_relaxed);
    if (cur_status == Config::Doorphone::FramingStatus::LOCKED && buf_len >= 3) {
      uint8_t target_stx = g_doorphone_tracker.candidate_stx.load(std::memory_order_relaxed);
      uint8_t target_etx = g_doorphone_tracker.candidate_etx.load(std::memory_order_relaxed);
      uint8_t target_len = g_doorphone_tracker.candidate_len.load(std::memory_order_relaxed);

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
              StaticPacket drp_pkt{4, static_cast<uint8_t>(std::min<size_t>(buf_len - p, 16))};
              memcpy(drp_pkt.data.data(), &buf[p], drp_pkt.length);
              g_telnet_tracer.trace(4, false, TraceType::DRP, drp_pkt);
              g_pkt_stats.ch4.invalid_frames.fetch_add(1, std::memory_order_relaxed);
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

          uint32_t now = millis();
          bool is_debounce = (packet.length == last_pkt.length &&
                              memcmp(packet.data.data(), last_pkt.data.data(), packet.length) == 0 &&
                              now - last_pkt_ms < Config::Timing::DOORPHONE_DEBOUNCE_MS);

          if (!is_debounce) {
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
              uint8_t end_front  = dp_prof ? dp_prof->end_front  : 0xB8;
              uint8_t end_lobby  = dp_prof ? dp_prof->end_lobby  : 0x60;

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
                Mgmt_BroadcastDoorphoneEvent(f, l);
              }
            }

            g_telnet_tracer.trace(4, false, TraceType::RMT, packet);
            g_pkt_stats.ch4.rx_pkts.fetch_add(1, std::memory_order_relaxed);
          }

          p += found_len;
          last_byte_ms = 0;
        } else {
          if (buf_len - p >= 64) {
            StaticPacket drp_pkt{4, static_cast<uint8_t>(std::min<size_t>(buf_len - p, 16))};
            memcpy(drp_pkt.data.data(), &buf[p], drp_pkt.length);
            g_telnet_tracer.trace(4, false, TraceType::DRP, drp_pkt);
            g_pkt_stats.ch4.invalid_frames.fetch_add(1, std::memory_order_relaxed);
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

          Config::Doorphone::FramingStatus prev_status = g_doorphone_tracker.status.load(std::memory_order_relaxed);
          
          // 프로파일 카탈로그에 일치하는 도어폰 규격이 있으면 즉시 영구 잠금(LOCKED)
          const DoorphoneSpec *dp_spec = ProfileMatcher::matchDoorphone(pkt_stx, pkt_etx, 5);
          char cur_dp_ns[16];
          Config::Doorphone::FramingTracker::getNvsNamespace(g_config.wallpad_profile, cur_dp_ns, sizeof(cur_dp_ns));

          if (dp_spec && pkt_stx == dp_spec->stx && pkt_etx == dp_spec->etx && packet.length >= 5) {
            g_doorphone_tracker.setFixedLock(dp_spec->stx, dp_spec->etx, dp_spec->len);
            g_doorphone_tracker.saveToNvs(cur_dp_ns);
          } else {
            g_doorphone_tracker.processFrame(pkt_stx, pkt_etx, packet.length, cur_dp_ns);
            Config::Doorphone::FramingStatus status = g_doorphone_tracker.status.load(std::memory_order_relaxed);
            if (prev_status != Config::Doorphone::FramingStatus::LOCKED &&
                status == Config::Doorphone::FramingStatus::LOCKED) {
              g_doorphone_tracker.saveToNvs(cur_dp_ns);
            }
          }

          uint32_t now = millis();
          bool is_debounce = (packet.length == last_pkt.length &&
                              memcmp(packet.data.data(), last_pkt.data.data(), packet.length) == 0 &&
                              now - last_pkt_ms < Config::Timing::DOORPHONE_DEBOUNCE_MS);

          if (!is_debounce) {
            last_pkt = packet;
            last_pkt_ms = now;
            g_telnet_tracer.trace(4, false, TraceType::RMT, packet);
            g_pkt_stats.ch4.rx_pkts.fetch_add(1, std::memory_order_relaxed);
          }
        }
        buf_len = 0;
      }
      last_byte_ms = 0;
    }

    uint32_t wait_ms = (buf_len > 0) ? 1 : 5;
    if (last_byte_ms > 0) {
      uint32_t elapsed = millis() - last_byte_ms;
      if (elapsed < Config::Timing::DOORPHONE_IPG_MS) {
        wait_ms = (buf_len > 0) ? 1 : (Config::Timing::DOORPHONE_IPG_MS - elapsed);
      } else {
        wait_ms = 1;
      }
    }
    wait_ms = std::max<uint32_t>(wait_ms, 1);

    if (xQueueReceive(g_ch4_passthrough_queue, &packet_to_tx, pdMS_TO_TICKS(wait_ms)) == pdTRUE) {
      if (packet_to_tx.length >= 3) {
        g_doorphone_tracker.processFrame(packet_to_tx.data[0], packet_to_tx.data[packet_to_tx.length - 1], packet_to_tx.length);
      }
      g_telnet_tracer.trace(4, true, TraceType::RMT, packet_to_tx);
      g_doorphone_serial.write(packet_to_tx.data.data(), packet_to_tx.length);
      last_tx_ms = millis();
      g_pkt_stats.ch4.tx_pkts.fetch_add(1, std::memory_order_relaxed);
    }
  }
}