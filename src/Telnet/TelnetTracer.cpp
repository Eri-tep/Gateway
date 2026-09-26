#include "TelnetCli.h"
#include "WallpadParser.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <sys/socket.h>

TelnetTracer g_telnet_tracer;


bool TelnetTracer::passesFilter(uint8_t channel, TraceType type,
                                const StaticPacket &pkt) const {
  uint8_t ch_mask = getChannelMask();
  if (ch_mask != 0 && channel >= 1 && channel <= 6 && !((1 << channel) & ch_mask))
    return false;

  TraceType mode = getFilterMode();
  if (mode == TraceType::ALL)
    return true;

  uint8_t target = getFilterTargetVal();
  if (mode == TraceType::CH)
    return (channel == target);

  if (mode == TraceType::DEVID) {
    uint8_t pkt_dev_id = 0, dummy_s1 = 0, dummy_s2 = 0;
    if (pkt.length >= 5 && pkt.data[0] == PKT_STX) {
      auto *parser = WallpadParserFactory::getActiveParser();
      if (parser) {
        span<const uint8_t> frame(pkt.data.data(), pkt.length);
        parser->extractDeviceKey(frame, pkt_dev_id, dummy_s1, dummy_s2);
      }
    } else if (pkt.length == 5 && pkt.data[0] == 0x7F) {
      pkt_dev_id = pkt.data[1];  // 도어폰 패킷 (별도 프로토콜)
    }
    return (pkt_dev_id == target);
  }

  return (type == mode);
}

void TelnetTracer::trace(uint8_t channel, bool is_tx, TraceType type,
                         const StaticPacket &pkt) {
  if (!isTraceEnabled() || !passesFilter(channel, type, pkt))
    return;

  uint32_t ticket = _head.fetch_add(1, std::memory_order_relaxed);
  size_t idx = ticket & RING_MASK;

  gettimeofday(&_traceRing[idx].entry.tv, nullptr);
  _traceRing[idx].entry.channel = channel;
  _traceRing[idx].entry.is_tx = is_tx;
  _traceRing[idx].entry.type = type;
  _traceRing[idx].entry.len =
      (static_cast<size_t>(pkt.length) > sizeof(_traceRing[idx].entry.data))
          ? sizeof(_traceRing[idx].entry.data)
          : static_cast<uint8_t>(pkt.length);
  if (_traceRing[idx].entry.len > 0) {
    memcpy(_traceRing[idx].entry.data.data(), pkt.data.data(),
           _traceRing[idx].entry.len);
  }

  _traceRing[idx].seq.store(ticket + 1, std::memory_order_release);

  if (g_tracer_sem)
    xSemaphoreGive(g_tracer_sem);
}

void TelnetTracer::trace(const char *fmt, ...) {
  if (!isTraceEnabled())
    return;

  char buf[128];
  va_list args;
  va_start(args, fmt);
  int len = vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  if (len > 0) {
    uint32_t ticket = _head.fetch_add(1, std::memory_order_relaxed);
    size_t idx = ticket & RING_MASK;

    gettimeofday(&_traceRing[idx].entry.tv, nullptr);
    _traceRing[idx].entry.channel = 0;
    _traceRing[idx].entry.is_tx = false;
    _traceRing[idx].entry.type = TraceType::MSG;
    _traceRing[idx].entry.len =
        (static_cast<size_t>(len) > sizeof(_traceRing[idx].entry.data))
            ? sizeof(_traceRing[idx].entry.data)
            : static_cast<uint8_t>(len);
    memcpy(_traceRing[idx].entry.data.data(), buf, _traceRing[idx].entry.len);

    _traceRing[idx].seq.store(ticket + 1, std::memory_order_release);
  }

  if (g_tracer_sem)
    xSemaphoreGive(g_tracer_sem);
}

struct Ch1Tracker {
  uint8_t dev_id;
  struct timeval t_bus_tx;
  bool active;
};

struct SessionTracker {
  uint8_t dev_id, sub1, sub2;
  struct timeval t_req_rx, t_bus_tx, t_bus_rx;
  bool is_query, is_control, active;
};

struct DoorTracker {
  struct timeval t_rx;
  uint8_t rx_channel;
  bool active;
};

struct Ew11Tracker {
  struct timeval t_rx;
  uint8_t dev_id;
  bool active;
};

static Ch1Tracker s_ch1_tracker = {0, {0, 0}, false};
static SessionTracker s_wp_tracker[3] = {};
static DoorTracker s_door_tracker = {{0, 0}, 0, false};
static Ew11Tracker s_ew11_tracker = {{0, 0}, 0, false};
static struct timeval s_last_pkt_tv = {0, 0};

void TelnetTracer::resetTrackers() noexcept {
  memset(&s_ch1_tracker, 0, sizeof(s_ch1_tracker));
  memset(s_wp_tracker, 0, sizeof(s_wp_tracker));
  memset(&s_door_tracker, 0, sizeof(s_door_tracker));
  memset(&s_ew11_tracker, 0, sizeof(s_ew11_tracker));
  memset(&s_last_pkt_tv, 0, sizeof(s_last_pkt_tv));
}

void TelnetTracer::setClient(int sock) noexcept {
  resetTrackers();
  _client_fd.store(sock, std::memory_order_release);
}

void TelnetTracer::flushToClient() {
  if (isPaused())
    return;

  int c_fd = _client_fd.load(std::memory_order_acquire);
  if (c_fd < 0)
    return;

  if (g_telnet_tx_sem && xSemaphoreTake(g_telnet_tx_sem, 0) != pdTRUE) {
    return; // CLI 명령 등이 소켓에 출력 중일 때는 트레이서가 즉시 양보하여 끼어들기 및 타임아웃 assertion 방지
  }
  struct TxSemGuard {
    SemaphoreHandle_t sem;
    ~TxSemGuard() {
      if (sem) xSemaphoreGive(sem);
    }
  } sem_guard{g_telnet_tx_sem};

  constexpr size_t BATCH_SIZE = 8;
  TracePacketEntry local_batch[BATCH_SIZE];
  size_t batch_count = 0;

  uint32_t head = _head.load(std::memory_order_acquire);
  if (head - _tail > RING_CAP) {
    _tail = head - RING_CAP;
  }

  while (batch_count < BATCH_SIZE && _tail != head) {
    size_t idx = _tail & RING_MASK;
    uint32_t expected_seq = _tail + 1;
    if (_traceRing[idx].seq.load(std::memory_order_acquire) != expected_seq) {
      break;
    }
    local_batch[batch_count++] = _traceRing[idx].entry;
    _tail++;
  }

  if (batch_count == 0)
    return;

  auto calc_delay_ms = [](const struct timeval &now,
                          const struct timeval &prev) -> long {
    if (prev.tv_sec == 0)
      return -1;
    long total_ms =
        (now.tv_sec - prev.tv_sec) * 1000 + (now.tv_usec - prev.tv_usec) / 1000;
    return (total_ms >= 0 && total_ms < 60000) ? total_ms : -1;
  };

  for (size_t i = 0; i < batch_count; ++i) {
    TracePacketEntry &entry = local_batch[i];
    uint8_t dev_id = 0, sub1 = 0, sub2 = 0;
    if (entry.len >= 5 && entry.data[0] == PKT_STX) {
      auto *parser = WallpadParserFactory::getActiveParser();
      if (parser) {
        span<const uint8_t> frame(entry.data.data(), entry.len);
        parser->extractDeviceKey(frame, dev_id, sub1, sub2);
      }
    }

    long delay_ms = -1;
    bool is_new_req = false;
    const char *delay_tag = nullptr;

    auto init_tracker = [&](SessionTracker &tr, TraceType type) {
      tr.dev_id = dev_id;
      tr.sub1 = sub1;
      tr.sub2 = sub2;
      tr.t_req_rx = entry.tv;
      tr.t_bus_tx = {0, 0};
      tr.t_bus_rx = {0, 0};
      tr.is_query = (type == TraceType::QRY);
      tr.is_control = (type == TraceType::CTL);
      tr.active = true;
    };

    auto processTx = [&](SessionTracker &tr) {
      if (tr.active) {
        delay_ms = calc_delay_ms(entry.tv, tr.t_req_rx);
        delay_tag = tr.is_query ? "CACHE  " : "FWD ACK";
        tr.active = false;
      }
    };

    if (entry.channel == 2 || entry.channel == 3 || entry.channel == 6) {
      int wp_idx = (entry.channel == 2) ? 0 : (entry.channel == 3) ? 1 : 2;
      if (!entry.is_tx) { // RX from Wallpad / App
        is_new_req = true;
        init_tracker(s_wp_tracker[wp_idx], entry.type);
        if (entry.type == TraceType::CTL) {
          const char *tags[] = {"CMD_CH2", "CMD_CH3", "CMD_CH6"};
          delay_tag = tags[wp_idx];
          delay_ms = -2;
        }
      } else if (entry.type == TraceType::ACK) { // TX to Wallpad / App
        if (entry.channel == 6 && s_ew11_tracker.active && s_ew11_tracker.dev_id == dev_id) {
          delay_ms = calc_delay_ms(entry.tv, s_ew11_tracker.t_rx);
          delay_tag = "PASSTHRU";
          s_ew11_tracker.active = false;
        } else {
          processTx(s_wp_tracker[wp_idx]);
        }
      }
    } else if (entry.channel == 4) {
      if (!entry.is_tx) {
        is_new_req = true;
        s_door_tracker.t_rx = entry.tv;
        s_door_tracker.rx_channel = 4;
        s_door_tracker.active = true;
        delay_tag = "PASSTHRU";
        delay_ms = -2;
      } else {
        if (s_door_tracker.active) {
          delay_ms = calc_delay_ms(entry.tv, s_door_tracker.t_rx);
          delay_tag = "INJECT ";
          s_door_tracker.active = false;
        } else {
          delay_tag = "INJECT ";
          delay_ms = -2;
        }
      }
    } else if (entry.channel == 5) {
      if (!entry.is_tx) {
        is_new_req = true;
        s_ew11_tracker.t_rx = entry.tv;
        s_ew11_tracker.dev_id = dev_id;
        s_ew11_tracker.active = true;
      } else {
        for (auto &tr : s_wp_tracker) {
          if (tr.active && tr.dev_id == dev_id) {
            delay_ms = calc_delay_ms(entry.tv, tr.t_req_rx);
            delay_tag = "INJECT ";
            tr.active = false;
            break;
          }
        }
      }
    } else if (entry.channel == 1) {
      if (entry.is_tx) {
        is_new_req = true;
        s_ch1_tracker.dev_id = dev_id;
        s_ch1_tracker.t_bus_tx = entry.tv;
        s_ch1_tracker.active = true;
        if (entry.type == TraceType::CTL) {
          for (auto &tr : s_wp_tracker) {
            if (tr.active && tr.dev_id == dev_id) {
              tr.t_bus_tx = entry.tv;
              delay_ms = calc_delay_ms(entry.tv, tr.t_req_rx);
              delay_tag = "GW FWD ";
              break;
            }
          }
        }
      } else if (entry.type == TraceType::ACK) {
        if (s_ch1_tracker.active && s_ch1_tracker.dev_id == dev_id) {
          delay_ms = calc_delay_ms(entry.tv, s_ch1_tracker.t_bus_tx);
          delay_tag = "DEV ACK";
          s_ch1_tracker.active = false;
        }
      }
    }

    if (is_new_req && s_last_pkt_tv.tv_sec > 0) {
      long gap = calc_delay_ms(entry.tv, s_last_pkt_tv);
      if (gap > 50 || gap < 0) {
        sendTelnetMsgLen(c_fd, "\r\n", 2);
      }
    }
    s_last_pkt_tv = entry.tv;

    char line_buf[320];
    struct tm timeinfo;
    time_t sec = static_cast<time_t>(entry.tv.tv_sec);
    localtime_r(&sec, &timeinfo);

    size_t idx =
        snprintf(line_buf, sizeof(line_buf), "%02d:%02d:%02d.%03ld   ",
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec,
                 entry.tv.tv_usec / 1000);

    if (entry.type == TraceType::MSG) {
      idx += snprintf(line_buf + idx, sizeof(line_buf) - idx, "[SYSTEM MSG]  ");
      if (idx < sizeof(line_buf) - entry.len) {
        memcpy(line_buf + idx, entry.data.data(), entry.len);
        idx += entry.len;
      }
    } else {
      const char *type_str = (entry.type == TraceType::QRY) ? "QRY" :
                             (entry.type == TraceType::CTL) ? "CTL" :
                             (entry.type == TraceType::ACK) ? "ACK" :
                             (entry.type == TraceType::DRP) ? "DRP" :
                             (entry.channel == 5)           ? "TCP" : "RMT";
      idx += snprintf(line_buf + idx, sizeof(line_buf) - idx,
                      "[CH#%u]  %s %s   ",
                      entry.channel,
                      entry.is_tx ? "==>" : "<==",
                      type_str);

      for (size_t j = 0; j < entry.len && idx < sizeof(line_buf) - 25; j++) {
        uint8_t b = entry.data[j];
        line_buf[idx++] = HexLUT::LUT[b][0];
        line_buf[idx++] = HexLUT::LUT[b][1];
        line_buf[idx++] = ' ';
      }
    }

    if (delay_tag) {
      size_t display_cols = idx;
      constexpr size_t ALIGN_COLUMN = 104;

      while (display_cols++ < ALIGN_COLUMN && idx < sizeof(line_buf) - 30) {
        line_buf[idx++] = ' ';
      }
      if (display_cols >= ALIGN_COLUMN) {
        for (int k = 0; k < 4 && idx < sizeof(line_buf) - 30; k++)
          line_buf[idx++] = ' ';
      }

      if (delay_ms >= 0) {
        idx += snprintf(line_buf + idx, sizeof(line_buf) - idx,
                        "[%s : +%3ldms]", delay_tag, delay_ms);
      } else if (delay_ms == -2) {
        idx +=
            snprintf(line_buf + idx, sizeof(line_buf) - idx, "[%s]", delay_tag);
      }
    }

    idx += snprintf(line_buf + idx, sizeof(line_buf) - idx, "\r\n");
    send(c_fd, line_buf, idx, MSG_DONTWAIT);
  }

  if (g_telnet_tx_sem) {
    xSemaphoreGive(g_telnet_tx_sem);
  }
}

