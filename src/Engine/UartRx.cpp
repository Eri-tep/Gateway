#include "EngineInternal.h"
#include "esp_task_wdt.h"
#include <algorithm>
#include <cstring>

QueueHandle_t Uart_GetEventQueue(uart_port_t u_num) {
  switch (u_num) {
  case UART_NUM_0: return g_uart0_event_queue;
  case UART_NUM_1: return g_uart1_event_queue;
  case UART_NUM_2: return g_uart2_event_queue;
  default: return nullptr;
  }
}

UartRxStatus Uart_RecvPacket(uart_port_t u_num, StaticPacket &out,
                            uint32_t tout_ms,
                            UartPollCallback on_poll,
                            void *poll_ctx,
                            const StaticPacket *echo_match) {
  uint8_t temp[Config::Packet::UART_READ_CHUNK];
  uint8_t stream[Config::Packet::MAX_STREAM_BUF];
  size_t stream_len = 0;
  uint32_t start_ms = millis();
  uint32_t last_rx_ms = 0;
  auto *const parser = WallpadParserFactory::getActiveParser();
  const bool is_auto_unlocked = (parser && parser->isAutoMode() && !parser->isLocked());
  const uint8_t stx = parser ? parser->getStx() : PKT_STX;
  QueueHandle_t evt_q = Uart_GetEventQueue(u_num);

  while (millis() - start_ms < tout_ms) {
    esp_task_wdt_reset();
    if (on_poll)
      on_poll(poll_ctx);

    if (stream_len >= 3) {
      if (is_auto_unlocked) {
        if (last_rx_ms > 0 && TimeUtils::isElapsed(last_rx_ms, Config::Timing::WALLPAD_AUTO_IPG_MS)) {
          g_auto_probing_engine.feedFrame(span<const uint8_t>(stream, stream_len));

          if (echo_match && echo_match->length == stream_len &&
              memcmp(echo_match->data.data(), stream, stream_len) == 0) {
            stream_len = 0;
            last_rx_ms = 0;
            continue;
          }

          out.length = static_cast<uint8_t>(stream_len);
          memcpy(out.data.data(), stream, stream_len);
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

          int len_res = parser ? parser->extractPacketLength(stream, stream_len, idx) : -1;
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
            uint8_t ch = (u_num == UART_NUM_0) ? 1 : (u_num == UART_NUM_1) ? 2 : 3;
            StaticPacket drp_pkt{ch, pkt_len};
            memcpy(drp_pkt.data.data(), pkt, pkt_len);
            g_telnet_tracer.trace(ch, false, TraceType::DRP, drp_pkt);
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
          size_t avail = 0;
          uart_get_buffered_data_len(u_num, &avail);
          if (avail > 0) {
            size_t read_limit = std::min(avail, sizeof(temp));
            int rx = uart_read_bytes(u_num, temp, read_limit, 0);
            if (rx > 0) {
              if (stream_len + rx > sizeof(stream)) {
                size_t overflow = (stream_len + rx) - sizeof(stream);
                if (overflow < stream_len) {
                  memmove(stream, stream + overflow, stream_len - overflow);
                  stream_len -= overflow;
                } else {
                  stream_len = 0;
                }
              }
              size_t copy_len = std::min(static_cast<size_t>(rx), sizeof(stream) - stream_len);
              memcpy(stream + stream_len, temp, copy_len);
              stream_len += copy_len;
              received_new_bytes = true;
              last_rx_ms = millis();
            }
          }
        } else if (evt.type == UART_FIFO_OVF || evt.type == UART_BUFFER_FULL) {
          uart_flush_input(u_num);
          xQueueReset(evt_q);
        }
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(std::min<uint32_t>(wait_ms, 2)));
    }

    if (!received_new_bytes) {
      size_t avail = 0;
      uart_get_buffered_data_len(u_num, &avail);
      if (avail > 0) {
        size_t read_limit = std::min(avail, sizeof(temp));
        int rx = uart_read_bytes(u_num, temp, read_limit, 0);
        if (rx > 0) {
          if (stream_len + rx > sizeof(stream)) {
            size_t overflow = (stream_len + rx) - sizeof(stream);
            if (overflow < stream_len) {
              memmove(stream, stream + overflow, stream_len - overflow);
              stream_len -= overflow;
            } else {
              stream_len = 0;
            }
          }
          size_t copy_len = std::min(static_cast<size_t>(rx), sizeof(stream) - stream_len);
          memcpy(stream + stream_len, temp, copy_len);
          stream_len += copy_len;
          last_rx_ms = millis();
        }
      }
    }
  }

  if (stream_len >= 3 && is_auto_unlocked) {
    g_auto_probing_engine.feedFrame(span<const uint8_t>(stream, stream_len));
    if (!(echo_match && echo_match->length == stream_len &&
          memcmp(echo_match->data.data(), stream, stream_len) == 0)) {
      out.length = static_cast<uint8_t>(stream_len);
      memcpy(out.data.data(), stream, stream_len);
      return UartRxStatus::SUCCESS;
    }
  }

  return UartRxStatus::TIMEOUT;
}
