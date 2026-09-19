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
      } else if (parser->isControlPacket(frame)) {
        StaticPacket dummy_ack;
        g_control_dispatcher.dispatch(req, dummy_ack);
      }
    }
  }
}

void Task_Ch4(void *pvParameters) {
  esp_task_wdt_add(nullptr);
  StaticPacket packet_to_tx;

  // 범용 인터패킷 갭(IPG) 기반 패킷화 엔진
  // STX/ETX에 무관하게 어떤 제조사 도어폰이든 25ms 침묵을 1프레임 종료로 판정 (대형 30~64B 패킷 수용을 위해 128B 버퍼)
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

    // TX: 앱/허브에서 도어폰으로 보내는 제어 패킷 (PassThrough)
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

    // RX: 도어폰 하드웨어에서 들어오는 바이트를 스트림 버퍼에 누적
    // 범용 버스트 수신: 패킷 전송 중 바이트 간 지연(최대 16ms)을 안전하게 버퍼링하기 위해
    // 데이터 유입 시작 시 짧은 폴링으로 1프레임을 온전히 긁어모음
    const uint32_t ib_timeout = Config::Timing::getDoorphoneInterByteTimeoutMs(g_config.doorphone_baud_rate);
    // 3860 bps 기준 다음 바이트 도착 대기 (단일 바이트 최대 6ms, 연속 패킷 누적 최대 20ms 스핀으로 WDT 및 Core1 멀티태스킹 보호)
    uint32_t burst_spin_total = 0;
    while (g_doorphone_serial.available() > 0) {
      uint8_t byte = static_cast<uint8_t>(g_doorphone_serial.read());
      uint32_t now = millis();

      // 연속성 검증: 새 버스트 유입 시 직전 미완성 조각이 16ms 이상 끊긴 과거 쓰레기라면 초기화
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

    // [월패드급 슬라이딩 윈도우 스트림 파서]
    Config::Doorphone::FramingStatus cur_status = g_doorphone_tracker.status.load(std::memory_order_relaxed);
    if (cur_status == Config::Doorphone::FramingStatus::LOCKED && buf_len >= 3) {
      uint8_t target_stx = g_doorphone_tracker.candidate_stx.load(std::memory_order_relaxed);
      uint8_t target_etx = g_doorphone_tracker.candidate_etx.load(std::memory_order_relaxed);
      uint8_t target_len = g_doorphone_tracker.candidate_len.load(std::memory_order_relaxed);

      size_t p = 0;
      while (p < buf_len) {
        // 1. STX 탐색 (앞선 노이즈 바이트 자동 스킵)
        if (buf[p] != target_stx) {
          p++;
          continue;
        }

        // 2. 가변/고정 패킷 길이 슬라이싱 탐색
        bool frame_found = false;
        size_t found_len = 0;

        if (target_len >= 3 && target_len <= 64) {
          // 고정 길이 우선 검사
          if (buf_len - p >= target_len) {
            if (buf[p + target_len - 1] == target_etx) {
              frame_found = true;
              found_len = target_len;
            } else {
              // STX는 일치하나 고정 길이 위치의 바이트가 ETX와 불일치 (노이즈/변칙 프레임)
              // 현재 STX를 노이즈로 간주하고 즉시 스킵하여 버퍼에서 폐기(Drop)
              StaticPacket drp_pkt{4, static_cast<uint8_t>(std::min<size_t>(buf_len - p, 16))};
              memcpy(drp_pkt.data.data(), &buf[p], drp_pkt.length);
              g_telnet_tracer.trace(4, false, TraceType::DRP, drp_pkt);
              g_pkt_stats.ch4.invalid_frames.fetch_add(1, std::memory_order_relaxed);
              p++;
              continue;
            }
          } else {
            // 패킷 바이트 추가 수신 대기
            break;
          }
        } else {
          // 고정 길이가 아니거나 미설정된 경우, 가변 ETX 탐색 (3B ~ 64B)
          for (size_t i = p + 2; i < buf_len && (i - p + 1) <= 64; ++i) {
            if (buf[i] == target_etx) {
              frame_found = true;
              found_len = (i - p) + 1;
              break;
            }
          }
        }

        if (frame_found) {
          // [순수 범용 에코 필터링] 직전 250ms 이내 송신 패킷과 100% 바이트 단위 일치 시 에코로 폐기 (대형 30B 패킷 전송 시간 수용)
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

            // 도어폰 초인종(벨) 및 호출 종료 상태 실시간 감지 & CH6 브로드캐스트
            if (packet.length >= 2) {
              uint8_t opcode = packet.data[1];
              bool state_changed = false;
              uint8_t pkt_stx = packet.data[0];
              uint8_t pkt_etx = packet.data[packet.length - 1];
              const Config::Doorphone::DoorphoneProfile *dp_prof =
                  Config::Doorphone::matchDoorphoneCatalog(pkt_stx, pkt_etx, packet.length);

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
            // 가변 탐색에서 64바이트 이내에 ETX를 못 찾으면 STX 폐기
            StaticPacket drp_pkt{4, static_cast<uint8_t>(std::min<size_t>(buf_len - p, 16))};
            memcpy(drp_pkt.data.data(), &buf[p], drp_pkt.length);
            g_telnet_tracer.trace(4, false, TraceType::DRP, drp_pkt);
            g_pkt_stats.ch4.invalid_frames.fetch_add(1, std::memory_order_relaxed);
            p++;
            continue;
          }
          // 불완전 패킷 (추가 데이터 수신 대기)
          break;
        }
      }

      // 파싱 완료된 바이트 슬라이딩 시프트
      if (p > 0) {
        if (p < buf_len) {
          memmove(buf, buf + p, buf_len - p);
          buf_len -= p;
        } else {
          buf_len = 0;
        }
      }
    }

    // 미학습/학습 초기 상태 인터패킷 갭(IPG) 감지 및 피딩 (침묵 25ms 도달 시)
    if (last_byte_ms > 0 &&
        TimeUtils::isElapsed(last_byte_ms, Config::Timing::DOORPHONE_IPG_MS)) {

      if (cur_status == Config::Doorphone::FramingStatus::LOCKED) {
        // ★ LOCKED 상태: 고정 규격(STX+길이+ETX)에 부합하지 못하고 남은 잔여 데이터는 온전한 패킷이 아닌 불완전 노이즈 조각이므로 완전 폐기
        buf_len = 0;
      } else {
        // ★ 미학습(WAITING/LEARNING) 상태: IPG로 패킷 프레임 수집 & 동적 학습
        if (buf_len >= 3) {
          StaticPacket packet{4, static_cast<uint8_t>(buf_len)};
          memcpy(packet.data.data(), buf, buf_len);

          uint8_t pkt_stx = packet.data[0];
          uint8_t pkt_etx = packet.data[packet.length - 1];

          Config::Doorphone::FramingStatus prev_status = g_doorphone_tracker.status.load(std::memory_order_relaxed);
          g_doorphone_tracker.processFrame(pkt_stx, pkt_etx, packet.length);
          Config::Doorphone::FramingStatus status = g_doorphone_tracker.status.load(std::memory_order_relaxed);

          if (prev_status != Config::Doorphone::FramingStatus::LOCKED &&
              status == Config::Doorphone::FramingStatus::LOCKED) {
            g_doorphone_tracker.saveToNvs();
          }

          // 미학습 상태에서도 모니터링/테스트를 위해 전달
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

    // Event-Driven 블로킹: 
    // 수신 중(buf_len > 0)일 때는 다음 바이트를 놓치지 않도록 1ms 초단기 대기
    // 평상시(아이들)에는 CPU 점유율 0% 유지를 위해 5ms 대기
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

    // TX 큐 블로킹 수신: wait_ms 동안 커널 레벨 Blocked 대기하므로 CPU 점유율 0% 유지
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