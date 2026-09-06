#include "TelnetCli.h"
#include "CliCommands.h"
#include "WallpadParser.h"
#include "esp_ota_ops.h"
#include "esp_task_wdt.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>

TelnetManager g_telnet_manager(Config::TCP::TELNET_PORT);
TelnetTracer g_telnet_tracer;

std::atomic<bool> g_restart_pending{false};
const char *g_restart_reason = nullptr;
TelnetManager::WifiScanReq g_wifi_scan_req;

// ============================================================================
// SECTION 1: TELNET TRACER MODULE (TelnetTracer)
// ============================================================================

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

void TelnetTracer::flushToClient() {
  int c_fd = _client_fd.load(std::memory_order_acquire);
  if (c_fd < 0)
    return;

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

  static struct {
    uint8_t dev_id;
    struct timeval t_bus_tx;
    bool active;
  } s_ch1_tracker = {0, {0, 0}, false};

  struct SessionTracker {
    uint8_t dev_id, sub1, sub2;
    struct timeval t_req_rx, t_bus_tx, t_bus_rx;
    bool is_query, is_control, active;
  };

  static SessionTracker s_wp_tracker[3] = {};
  static struct {
    struct timeval t_rx;
    bool active;
  } s_door_tracker = {{0, 0}, false};
  static struct timeval s_last_pkt_tv = {0, 0};

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
    // ★ 고정 오프셋(data[3]/data[5]/data[6]) 대신 parser 동적 추출 사용
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
        processTx(s_wp_tracker[wp_idx]);
      }
    } else if (entry.channel == 4 || entry.channel == 5) {
      if (!entry.is_tx) {
        is_new_req = true;
        s_door_tracker.t_rx = entry.tv;
        s_door_tracker.active = true;
      } else if (s_door_tracker.active) {
        delay_ms = calc_delay_ms(entry.tv, s_door_tracker.t_rx);
        delay_tag = "PASS-THRU";
        s_door_tracker.active = false;
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
    sendTelnetMsgLen(c_fd, line_buf, idx);
  }
}

// ============================================================================
// SECTION 2: TELNET OUTPUT HELPERS & UTILITIES
// ============================================================================

void sendTelnetMsg(int sock, const char *str) {
  if (sock >= 0 && str) {
    sendTelnetMsgLen(sock, str, strlen(str));
  }
}

void sendTelnetMsgLen(int sock, const char *str, size_t len) {
  if (sock < 0 || !str || len == 0)
    return;
  size_t sent = 0;
  int retries = 0;

  while (sent < len && retries < 10) {
    int r = send(sock, str + sent, len - sent, MSG_DONTWAIT);
    if (r > 0) {
      sent += r;
      retries = 0;
    } else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      fd_set wfds;
      FD_ZERO(&wfds);
      FD_SET(sock, &wfds);
      struct timeval tv = {0, 5000}; // 최대 5ms 소켓 가용 대기 (Event-Driven)
      int sel = select(sock + 1, nullptr, &wfds, nullptr, &tv);
      if (sel <= 0) {
        retries++;
      }
    } else {
      break;
    }
  }
  g_wdt_monitor.feed(5);
}

void sendTelnetMsgf(int sock, const char *fmt, ...) {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  sendTelnetMsg(sock, buf);
}

TelnetManager::TelnetManager(uint16_t port) : _port(port) {}

void TelnetManager::writeCharToClient(EmbeddedCli *cli, char c) {
  if (!cli)
    return;
  auto *session = static_cast<TelnetSession *>(cli->appContext);
  if (session && session->sock >= 0) {
    if (session->txLen < sizeof(session->txBuf)) {
      session->txBuf[session->txLen++] = c;
    }
    if (session->txLen >= sizeof(session->txBuf) - 1 || c == '\n') {
      sendTelnetMsgLen(session->sock, session->txBuf, session->txLen);
      session->txLen = 0;
      session->needsSend = false;
    }
  }
}

void TelnetManager::bindCommands(TelnetSession *session) {
  if (!session->cli)
    return;

  struct CmdDef {
    const char *n;
    const char *h;
    void (*b)(EmbeddedCli *, char *, void *);
  };
  static const CmdDef cmds[] = {
      {"stats", "Show real-time HW metrics & traffic stats [clear]", SystemCli::cmdStats},
      {"devs", "Show device registry & cache [1|2|clear]", WallpadCli::cmdDevs},
      {"wifi", "Manage WiFi STA connection [status|scan|connect|disconnect]", WifiCli::cmdWifi},
      {"trace", "Packet monitoring [on|off|ctl|ack|pol|rmt|drp|ch|devid]", WallpadCli::cmdTrace},
      {"wallpad", "Wallpad protocol & auto-probing [status|list|set|save|delete|auto|reset]", WallpadCli::cmdWallpad},
      {"ctl", "Device control blueprints & active learning [view|learn|status|q|reset]", WallpadCli::cmdCtl},
      {"config", "View or modify runtime configuration [set|reset]", ConfigCli::cmdConfig},
      {"save", "Save current runtime configuration to NVS flash", ConfigCli::cmdSave},
      {"logview", "Persistent reboot history & crash logs [list|<1-20>|last|clear]", SystemCli::cmdLogView},
      {"coredump", "Show crash core dump summary or erase partition [clear]", SystemCli::cmdCoreDump},
      {"ota", "Dual-partition OTA & auto-rollback management [status|rollback|validate]", SystemCli::cmdOta},
      {"reboot", "Perform hardware system reboot with safe shutdown", SystemCli::cmdReboot},
      {"q", "Immediately stop active packet tracing (shortcut for 'trace off')", WallpadCli::cmdStop},
      {"exit", "Disconnect current Telnet CLI session", cmdExit},
      {"help", "Display comprehensive command reference and usage examples", SystemCli::cmdHelp},
      {"?", "Display comprehensive command reference (alias for 'help')", SystemCli::cmdHelp}};

  for (const auto &c : cmds) {
    CliCommandBinding b;
    b.name = c.n;
    b.help = c.h;
    b.tokenizeArgs = true;
    b.context = session;
    b.binding = c.b;
    embeddedCliAddBinding(session->cli.get(), b);
  }
}

void TelnetManager::onClientData(TelnetSession *session, const char *data,
                                 size_t len) {
  if (!session || session->sock < 0 || !data || len == 0)
    return;
  session->last_activity_ms = millis();
  bool should_close = false;

  for (size_t i = 0; i < len; i++) {
    uint8_t c = static_cast<uint8_t>(data[i]);
    if (session->iacState == IacState::GOT_IAC) {
      session->iacState = (c == TelnetCmd::WILL || c == TelnetCmd::WONT ||
                           c == TelnetCmd::DO || c == TelnetCmd::DONT)
                              ? IacState::GOT_OPTION
                          : (c == TelnetCmd::SB) ? IacState::IN_SUBNEG
                                                 : IacState::NORMAL;
      continue;
    } else if (session->iacState == IacState::GOT_OPTION) {
      session->iacState = IacState::NORMAL;
      continue;
    } else if (session->iacState == IacState::IN_SUBNEG) {
      if (c == TelnetCmd::SE)
        session->iacState = IacState::NORMAL;
      continue;
    } else if (c == TelnetCmd::IAC) {
      session->iacState = IacState::GOT_IAC;
      continue;
    }

    if (session->sessionState == AWAITING_PASSWORD) {
      if (c == '\r' || c == '\n') {
        if (session->pwLen > 0) {
          session->pwBuffer[session->pwLen] = '\0';
          if (!handlePassword(session, session->pwBuffer)) {
            should_close = true;
            break;
          }
          session->pwLen = 0;
        }
      } else if (c == '\b' || c == 0x7F) {
        if (session->pwLen > 0)
          session->pwLen--;
      } else if (isprint(c) && session->pwLen < sizeof(session->pwBuffer) - 1) {
        session->pwBuffer[session->pwLen++] = c;
      }
    } else if (session->cli) {
      embeddedCliReceiveChar(session->cli.get(), (char)c);
    }
  }

  if (should_close) {
    handleClientDisconnect(session);
    return;
  }

  if (session->sessionState == AUTHENTICATED && session->cli) {
    embeddedCliProcess(session->cli.get());
  }
}

// ============================================================================
// SECTION 3: TELNET MANAGER SERVER & SESSION LIFECYCLE
// ============================================================================

static bool Tcp_ConstantTimeStrcmp(const char *a, const char *b) {
  if (!a || !b)
    return false;
  size_t len_a = strlen(a), len_b = strlen(b);
  if (len_a != len_b)
    return false;
  volatile int result = 0;
  for (size_t i = 0; i < len_a; ++i)
    result |= a[i] ^ b[i];
  return result == 0;
}

TelnetManager::AuthResult TelnetManager::evaluateAuth(
    const char *clean_pw, const char *stored_hash,
    AuthBlockEntry *blk, uint32_t now_ms) {
  if (blk && blk->failedCount >= 3 &&
      !TimeUtils::isElapsed(blk->lastFailedMs, Config::TCP::AUTH_FAIL_PENALTY_MS)) {
    return AuthResult::LOCKED_OUT;
  }

  char input_hash[68];
  System_Sha256ToHex(clean_pw, input_hash);

  bool auth_ok = false;
  if (strlen(stored_hash) > 0 &&
      Tcp_ConstantTimeStrcmp(input_hash, stored_hash)) {
    auth_ok = true;
  }

#ifdef DEFAULT_TELNET_PASS
  if (!auth_ok && strcasecmp(clean_pw, DEFAULT_TELNET_PASS) == 0) {
    auth_ok = true;
    {
      CriticalSectionLocker lock(&g_config_mux);
      strncpy(g_config.telnet_pass_hash, input_hash,
              sizeof(g_config.telnet_pass_hash) - 1);
      g_config.telnet_pass_hash[sizeof(g_config.telnet_pass_hash) - 1] = '\0';
    }
    Config_Save();
  }
#endif

  if (auth_ok) {
    if (blk) {
      blk->failedCount = 0;
      blk->lastFailedMs = 0;
    }
    return AuthResult::OK;
  }

  if (blk) {
    blk->failedCount++;
    blk->lastFailedMs = now_ms;
  }
  return AuthResult::WRONG_PASSWORD;
}

bool TelnetManager::handlePassword(TelnetSession *session,
                                   const char *password) {
  uint32_t now = millis();
  IPAddress clientIp = session->clientIp;

  AuthBlockEntry *blk = nullptr;
  for (int i = 0; i < 4; ++i) {
    if (_authBlocks[i].ip == clientIp) {
      blk = &_authBlocks[i];
      break;
    }
  }

  if (!blk) {
    for (int i = 0; i < 4; ++i) {
      if (_authBlocks[i].failedCount == 0 ||
          (now - _authBlocks[i].lastFailedMs > 60000)) {
        blk = &_authBlocks[i];
        blk->ip = clientIp;
        blk->failedCount = 0;
        blk->lastFailedMs = 0;
        break;
      }
    }
  }

  char clean_pw[64] = {0};
  size_t len = 0;
  for (size_t i = 0; password[i] != '\0' && len < sizeof(clean_pw) - 1; i++) {
    if (isprint((unsigned char)password[i]) && password[i] != ' ') {
      clean_pw[len++] = password[i];
    }
  }

  AuthResult res = evaluateAuth(clean_pw, g_config.telnet_pass_hash, blk, now);

  if (res == AuthResult::LOCKED_OUT) {
    sendTelnetMsg(
        session->sock,
        "\r\n[SECURITY] Too many failed attempts. Try again in 5 seconds.\r\n");
    return false;
  }

  if (res == AuthResult::OK) {
    session->sessionState = AUTHENTICATED;
    sendTelnetMsg(session->sock, "\r\nAuthentication successful.\r\n"
                                 "Welcome to Gateway Bridge Diagnostics!\r\n"
                                 "Type 'help' or '?' for available commands, "
                                 "or press [TAB] to auto-complete.\r\n\r\n");

    if (g_rollback_detected) {
      char warn_msg[384];
      const esp_partition_t *cur = esp_ota_get_running_partition();
      const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
      snprintf(warn_msg, sizeof(warn_msg),
               "================================================================================\r\n"
               " ⚠️  [SYSTEM AUTO-ROLLBACK NOTICE]\r\n"
               " ⚠️  Firmware automatically rolled back to stable partition '%s'!\r\n"
               " ⚠️  Failed partition '%s' crashed during boot and was invalidated.\r\n"
               "================================================================================\r\n\r\n",
               cur ? cur->label : "app0", other ? other->label : "app1");
      sendTelnetMsg(session->sock, warn_msg);
    }
    if (g_rescue_mode.load(std::memory_order_relaxed)) {
      sendTelnetMsg(session->sock,
               "================================================================================\r\n"
               " 🚨  [RESCUE SAFE MODE ACTIVE]\r\n"
               " 🚨  Connected via Emergency SoftAP (Sweet_Home_Rescue). RS-485 tasks bypassed.\r\n"
               " 🚨  Use 'ota status' or 'coredump' to diagnose and upload new firmware.\r\n"
               "================================================================================\r\n\r\n");
    }

    EmbeddedCliConfig *config = embeddedCliDefaultConfig();
    config->cliBuffer = nullptr;
    config->cliBufferSize = 0;
    config->rxBufferSize = 128;
    config->cmdBufferSize = 128;
    config->historyBufferSize = 256;
    config->maxBindingCount = 32;
    config->enableAutoComplete = true;

    session->cli.reset(embeddedCliNew(config));
    if (session->cli) {
      session->cli->appContext = session;
      session->cli->writeChar = writeCharToClient;
      bindCommands(session);
      embeddedCliProcess(session->cli.get());
      session->needsSend = true;
      return true;
    } else {
      session->sessionState = AWAITING_PASSWORD;
      sendTelnetMsg(session->sock,
                    "\r\n[ERROR] Out of memory to allocate CLI instance.\r\n");
      return false;
    }
  }

  sendTelnetMsg(session->sock, "\r\nInvalid password.\r\n");
  return false;
}

void TelnetManager::sendScanResult(const WifiScanReq &req,
                                   const char *result_str) {
  MutexLocker cliLock(_cli_mutex);
  for (int i = 0; i < Config::TCP::MAX_TELNET_CLIENTS; ++i) {
    TelnetSession &s = _sessions[i];
    if (s.sock >= 0 && s.sessionId == req.sessionId) {
      sendTelnetMsg(s.sock, result_str);
      break;
    }
  }
}

void TelnetManager::cmdExit(EmbeddedCli *cli, char *args, void *context) {
  auto *session = static_cast<TelnetSession *>(context);
  if (session && session->sock >= 0) {
    sendTelnetMsg(session->sock, "Goodbye!\r\n");
    g_telnet_manager.handleClientDisconnect(session);
  }
}

void TelnetManager::onClientConnect(int new_sock,
                                    const struct sockaddr_in &client_addr,
                                    uint32_t now) {
  if (new_sock < 0)
    return;

  const uint8_t *ip_bytes = reinterpret_cast<const uint8_t *>(&client_addr.sin_addr.s_addr);
  IPAddress remote_ip(ip_bytes[0], ip_bytes[1], ip_bytes[2], ip_bytes[3]);
  Serial.printf("[TELNET] Incoming connection from %s (sock: %d)\r\n",
                remote_ip.toString().c_str(), new_sock);

  if (!Tcp_IsAllowedIP(remote_ip)) {
    Serial.printf("[TELNET] Connection rejected: IP %s not allowed!\r\n",
                  remote_ip.toString().c_str());
    close(new_sock);
    return;
  }

  int flags = fcntl(new_sock, F_GETFL, 0);
  fcntl(new_sock, F_SETFL, flags | O_NONBLOCK);
  int nodelay = 1;
  setsockopt(new_sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

  int emptySlot = -1;
  {
    for (int i = 0; i < Config::TCP::MAX_TELNET_CLIENTS; ++i) {
      if (_sessions[i].sock < 0) {
        emptySlot = i;
        break;
      }
    }

    if (emptySlot == -1) {
      uint32_t oldest_time = 0xFFFFFFFF;
      int oldest_idx = 0;
      for (int i = 0; i < Config::TCP::MAX_TELNET_CLIENTS; ++i) {
        if (_sessions[i].connected_at_ms < oldest_time) {
          oldest_time = _sessions[i].connected_at_ms;
          oldest_idx = i;
        }
      }
      _sessions[oldest_idx].reset();
      emptySlot = oldest_idx;
    }

    _sessions[emptySlot].reset();
    _sessions[emptySlot].sock = new_sock;
    _sessions[emptySlot].clientIp = remote_ip;
    _sessions[emptySlot].sessionState = AWAITING_PASSWORD;
    _sessions[emptySlot].connected_at_ms = now;
    _sessions[emptySlot].last_activity_ms = now;
    _sessions[emptySlot].wasConnected = true;
    _sessions[emptySlot].sessionId = _nextSessionId++;
  }

  const uint8_t telnet_init_opts[] = {
      TelnetCmd::IAC, TelnetCmd::WILL, TelnetCmd::OPT_ECHO,
      TelnetCmd::IAC, TelnetCmd::WILL, TelnetCmd::OPT_SUPPRESS_GA};
  sendTelnetMsgLen(new_sock, reinterpret_cast<const char *>(telnet_init_opts),
                   sizeof(telnet_init_opts));
  sendTelnetMsg(new_sock, "\r\nPassword: ");
}

void TelnetManager::handleClientDisconnect(TelnetSession *session) {
  if (!session || session->sock < 0)
    return;

  if (g_telnet_tracer.isClient(session->sock)) {
    g_telnet_tracer.setTrace(false);
    g_telnet_tracer.setClient(-1);
  }
  session->reset();
}

void TelnetManager::shutdownForReboot() {
  g_telnet_tracer.setTrace(false);
  g_telnet_tracer.setClient(-1);
  MutexLocker cliLock(_cli_mutex);
  for (int i = 0; i < Config::TCP::MAX_TELNET_CLIENTS; ++i) {
    _sessions[i].reset();
  }
  if (_server_fd >= 0) {
    close(_server_fd);
    _server_fd = -1;
  }
}

void TelnetManager::startServer() {
  if (!_cli_mutex)
    _cli_mutex = xSemaphoreCreateMutex();

  if (_server_fd < 0) {
    _server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (_server_fd >= 0) {
      int opt = 1;
      setsockopt(_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
      int flags = fcntl(_server_fd, F_GETFL, 0);
      fcntl(_server_fd, F_SETFL, flags | O_NONBLOCK);

      struct sockaddr_in server_addr;
      memset(&server_addr, 0, sizeof(server_addr));
      server_addr.sin_family = AF_INET;
      server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
      server_addr.sin_port = htons(_port);

      int b = bind(_server_fd, reinterpret_cast<struct sockaddr *>(&server_addr), sizeof(server_addr));
      int l = listen(_server_fd, Config::TCP::MAX_TELNET_CLIENTS);
      Serial.printf("[TELNET] Server initialized on port %u (bind=%d, listen=%d, fd=%d, errno=%d)\r\n",
                    _port, b, l, _server_fd, errno);
    } else {
      Serial.printf("[TELNET] ERROR: Failed to create server socket! errno=%d\r\n", errno);
    }
  }
}

void TelnetManager::tick() {
  fd_set readfds, writefds, errorfds;
  FD_ZERO(&readfds);
  FD_ZERO(&writefds);
  FD_ZERO(&errorfds);

  int max_fd = -1;
  if (_server_fd >= 0) {
    FD_SET(_server_fd, &readfds);
    max_fd = std::max(max_fd, _server_fd);
  }

  int active_clients = 0;
  {
    MutexLocker cliLock(_cli_mutex);
    for (int i = 0; i < Config::TCP::MAX_TELNET_CLIENTS; ++i) {
      int s = _sessions[i].sock;
      if (s >= 0) {
        active_clients++;
        FD_SET(s, &readfds);
        FD_SET(s, &errorfds);
        if (_sessions[i].needsSend || _sessions[i].txLen > 0) {
          FD_SET(s, &writefds);
        }
        max_fd = std::max(max_fd, s);
      }
    }
  }

  // 동적 select 타임아웃: 미접속 시 1초 커널 수면, 접속 중일 때 10ms 입력 반응성
  struct timeval tv;
  if (active_clients == 0) {
    tv.tv_sec = 1;
    tv.tv_usec = 0;
  } else {
    tv.tv_sec = 0;
    tv.tv_usec = 10000;
  }

  int activity = select(max_fd + 1, &readfds, &writefds, &errorfds, &tv);

  if (activity < 0) {
    if (errno == EINTR)
      return; // 시그널 인터럽트는 슬립 없이 즉시 복귀
    vTaskDelay(pdMS_TO_TICKS(10));
    return;
  }

  MutexLocker cliLock(_cli_mutex);
  uint32_t now = millis();

  if (_server_fd >= 0 && FD_ISSET(_server_fd, &readfds)) {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int new_sock =
        accept(_server_fd, (struct sockaddr *)&client_addr, &client_len);
    if (new_sock >= 0) {
      onClientConnect(new_sock, client_addr, now);
    }
  }

  for (int i = 0; i < Config::TCP::MAX_TELNET_CLIENTS; ++i) {
    TelnetSession &s = _sessions[i];
    if (s.sock < 0)
      continue;

    if (TimeUtils::isElapsed(s.last_activity_ms, Config::TCP::TELNET_SESSION_TIMEOUT_MS)) {
      sendTelnetMsg(s.sock, "\r\n[SYSTEM] Disconnected due to inactivity.\r\n");
      handleClientDisconnect(&s);
      continue;
    }

    if (FD_ISSET(s.sock, &errorfds)) {
      handleClientDisconnect(&s);
      continue;
    }

    if (FD_ISSET(s.sock, &readfds)) {
      char rx_buf[128];
      int len = recv(s.sock, rx_buf, sizeof(rx_buf) - 1, 0);
      if (len > 0) {
        rx_buf[len] = '\0';
        onClientData(&s, rx_buf, len);
      } else if (len == 0 ||
                 (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        handleClientDisconnect(&s);
        continue;
      }
    }

    if (FD_ISSET(s.sock, &writefds) && s.txLen > 0) {
      int sent = send(s.sock, s.txBuf, s.txLen, MSG_DONTWAIT);
      if (sent > 0) {
        if (static_cast<size_t>(sent) < s.txLen) {
          memmove(s.txBuf, s.txBuf + sent, s.txLen - sent);
          s.txLen -= sent;
        } else {
          s.txLen = 0;
          s.needsSend = false;
        }
      } else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        handleClientDisconnect(&s);
      }
    }
  }
}

void Task_Telnet(void *pvParameters) {
  esp_task_wdt_add(nullptr);
  if (!g_tracer_sem)
    g_tracer_sem = xSemaphoreCreateBinary();
  g_telnet_manager.startServer();

  for (;;) {
    g_wdt_monitor.feed(5);

    // [지연 재부팅] _cli_mutex가 완전히 해제된 루프 최상단에서 확인.
    // cmdReboot이 이 플래그를 세팅하면 데드락 없이 안전하게 재부팅 실행.
    if (g_restart_pending.load(std::memory_order_acquire)) {
      g_restart_pending.store(false, std::memory_order_relaxed);
      System_Restart(g_restart_reason ? g_restart_reason : "Telnet Command");
    }

    g_telnet_manager.tick();

    // 활성 클라이언트가 있을 때만 트레이스 플러시 및 5ms 세마포어 대기
    // 미접속 시에는 즉시 다음 루프로 진입하여 select(1s) 커널 수면
    if (g_telnet_manager.hasActiveClients()) {
      g_telnet_tracer.flushToClient();
      xSemaphoreTake(g_tracer_sem, pdMS_TO_TICKS(5));
    } else {
      xSemaphoreTake(g_tracer_sem, 0);
    }
  }
}