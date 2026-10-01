#include "Console.h"
#include "Core.h"
#include "Engine.h"
#include "Bridge.h"
#include "Protocol.h"
#include "Service.h"
#include "embedded_cli.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#include <lwip/sockets.h>
#include <memory>
#include <WiFi.h>

// ============================================================================
// From src/Telnet/Telnet.cpp
// ============================================================================

// ============================================================================
// GLOBAL INSTANCES & SYNCHRONIZATION OBJECTS
// ============================================================================

TelnetManager g_telnet_manager(Config::TCP::TELNET_PORT);
TelnetTracer g_telnet_tracer;
SemaphoreHandle_t g_telnet_tx_sem = nullptr;
std::atomic<bool> g_restart_pending{false};
const char *g_restart_reason = nullptr;
TelnetManager::WifiScanReq g_wifi_scan_req;

// ============================================================================
// TELNET LOW-LEVEL OUTPUT HELPERS
// ============================================================================

void sendTelnetMsg(int sock, const char *str) {
  if (sock >= 0 && str) {
    sendTelnetMsgLen(sock, str, strlen(str));
  }
}

void sendTelnetMsgLen(int sock, const char *str, size_t len) {
  if (sock < 0 || !str || len == 0)
    return;

  if (!g_telnet_tx_sem) {
    g_telnet_tx_sem = xSemaphoreCreateMutex();
  }

  if (g_telnet_tx_sem && xSemaphoreTake(g_telnet_tx_sem, pdMS_TO_TICKS(100)) == pdTRUE) {
    size_t sent = 0;
    int retries = 0;

    while (sent < len && retries < 10) {
      int r = send(sock, str + sent, len - sent, MSG_DONTWAIT);
      if (r > 0) {
        sent += r;
        retries = 0;
      } else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        retries++;
        vTaskDelay(pdMS_TO_TICKS(1));
      } else {
        break;
      }
    }
    xSemaphoreGive(g_telnet_tx_sem);
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

// ============================================================================
// TELNET AUTHENTICATION LOGIC
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

namespace {
constexpr uint32_t AUTH_FAIL_PENALTY_MS = 5000;
} // namespace

TelnetManager::AuthResult TelnetManager::evaluateAuth(
    const char *clean_pw, const char *stored_hash,
    AuthBlockEntry *blk, uint32_t now_ms) {
  if (blk && blk->failedCount >= 3 &&
      !TimeUtils::isElapsed(blk->lastFailedMs, AUTH_FAIL_PENALTY_MS)) {
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
  // 대소문자 구분 비교로 보안 엔트로피 강화 [C-4]
  if (!auth_ok && strcmp(clean_pw, DEFAULT_TELNET_PASS) == 0) {
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
  // 기존 IP 검색
  for (int i = 0; i < 4; ++i) {
    if (_authBlocks[i].ip == clientIp) {
      blk = &_authBlocks[i];
      break;
    }
  }

  // 슬롯 없으면 LRU(가장 오래된 슬롯) 재사용 — null blk로 인한 락아웃 bypass 차단 [C-4]
  if (!blk) {
    AuthBlockEntry *oldest = &_authBlocks[0];
    for (int i = 1; i < 4; ++i) {
      if (_authBlocks[i].lastFailedMs < oldest->lastFailedMs) {
        oldest = &_authBlocks[i];
      }
    }
    oldest->ip = clientIp;
    oldest->failedCount = 0;
    oldest->lastFailedMs = 0;
    blk = oldest;
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
  }

  if (res == AuthResult::OK) {
    session->sessionState = AUTHENTICATED;
    sendTelnetMsg(session->sock, "\r\nAuthentication successful.\r\n"
                                 "Welcome to Gateway Bridge Diagnostics!\r\n"
                                 "Type 'help' for available commands, "
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

// ============================================================================
// TELNET CLI TERMINAL DRIVER & COMMAND BINDINGS
// ============================================================================

TelnetManager::TelnetManager(uint16_t port) : _port(port) {}

void TelnetManager::writeCharToClient(EmbeddedCli *cli, char c) {
  if (!cli)
    return;
  auto *session = static_cast<TelnetSession *>(cli->appContext);
  if (!session || session->sock < 0)
    return;

  // Strip ANSI/VT100 escape sequences (ESC [ ... letter)
  using EscState = TelnetSession::EscState;
  if (c == '\x1B') {
    session->esc_state = EscState::GOT_ESC;
    return;
  }
  if (session->esc_state == EscState::GOT_ESC) {
    session->esc_state = (c == '[') ? EscState::IN_CSI : EscState::NORMAL;
    return;
  }
  if (session->esc_state == EscState::IN_CSI) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
      session->esc_state = EscState::NORMAL;
    }
    return;
  }

  uint8_t uc = static_cast<uint8_t>(c);
  if ((uc < 0x20 && c != '\r' && c != '\n' && c != '\t') || uc == 0x7F) {
    return;
  }

  if (c == '\t') {
    for (int i = 0; i < 4; ++i) {
      if (session->txLen < sizeof(session->txBuf)) {
        session->txBuf[session->txLen++] = ' ';
      }
    }
    return;
  }

  if (session->txLen < sizeof(session->txBuf)) {
    session->txBuf[session->txLen++] = c;
  }
  if (session->txLen >= sizeof(session->txBuf) - 1 || c == '\n' || c == '\r') {
    sendTelnetMsgLen(session->sock, session->txBuf, session->txLen);
    session->txLen = 0;
    session->needsSend = false;
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
      {"ctl", "Device control specs [view|reset|name|class]", WallpadCli::cmdCtl},
      {"config", "View or modify runtime configuration [set|reset]", ConfigCli::cmdConfig},
      {"save", "Save current runtime configuration to NVS flash", ConfigCli::cmdSave},
      {"ew11", "CH5 EW11 multi-client hub config [list|set|enable|disable]", ConfigCli::cmdEw11},
      {"routes", "Show dynamic device ingress routing table [clear]", ConfigCli::cmdRoutes},
      {"logview", "Persistent reboot history & crash logs [list|<1-20>|last|clear]", SystemCli::cmdLogView},
      {"coredump", "Show crash core dump summary or erase partition [clear]", SystemCli::cmdCoreDump},
      {"ota", "Dual-partition OTA & auto-rollback management [status|rollback|validate]", SystemCli::cmdOta},
      {"reboot", "Perform hardware system reboot with safe shutdown", SystemCli::cmdReboot},
      {"q", "Immediately stop active packet tracing (shortcut for 'trace off')", WallpadCli::cmdStop},
      {"exit", "Disconnect current Telnet CLI session", cmdExit},
      {"help", "Display comprehensive command reference and usage examples", SystemCli::cmdHelp}};

  for (const auto &c : cmds) {
    if (strcmp(c.n, "help") == 0) {
      struct InternalCliImpl {
        void *rxBuf;
        void *cmdBuf;
        uint16_t cmdSize;
        uint16_t cmdMaxSize;
        CliCommandBinding *bindings;
      };
      auto *impl = static_cast<InternalCliImpl *>(session->cli->_impl);
      if (impl && impl->bindings) {
        impl->bindings[0].name = c.n;
        impl->bindings[0].help = c.h;
        impl->bindings[0].tokenizeArgs = true;
        impl->bindings[0].context = session;
        impl->bindings[0].binding = c.b;
      }
      continue;
    }
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
      if (c == TelnetCmd::WILL || c == TelnetCmd::WONT ||
          c == TelnetCmd::DO   || c == TelnetCmd::DONT) {
        session->iacState = IacState::GOT_OPTION;
      } else if (c == TelnetCmd::SB) {
        session->iacState = IacState::IN_SUBNEG;
      } else if (c == TelnetCmd::SE) {
        session->iacState = IacState::NORMAL;
      } else {
        session->iacState = IacState::NORMAL;
      }
      continue;
    } else if (session->iacState == IacState::GOT_OPTION) {
      session->iacState = IacState::NORMAL;
      continue;
    } else if (session->iacState == IacState::IN_SUBNEG) {
      if (c == TelnetCmd::IAC) {
        session->iacState = IacState::GOT_IAC;
      }
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
    g_telnet_tracer.pause();
    embeddedCliProcess(session->cli.get());
    if (session->txLen > 0) {
      sendTelnetMsgLen(session->sock, session->txBuf, session->txLen);
      session->txLen = 0;
      session->needsSend = false;
    }
    g_telnet_tracer.resume();
  }
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

// ============================================================================
// TELNET SERVER LIFE-CYCLE & CONNECTION MANAGEMENT
// ============================================================================

void TelnetManager::onClientConnect(int new_sock,
                                    const struct sockaddr_in &client_addr,
                                    uint32_t now) {
  if (new_sock < 0)
    return;

  const uint8_t *ip_bytes = reinterpret_cast<const uint8_t *>(&client_addr.sin_addr.s_addr);
  IPAddress remote_ip(ip_bytes[0], ip_bytes[1], ip_bytes[2], ip_bytes[3]);
  Serial.printf("[TELNET] Incoming connection from %s (sock: %d)\r\n",
                remote_ip.toString().c_str(), new_sock);

  if (!Telnet_IsAllowedIP(remote_ip)) {
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
      int victim_idx = -1;
      uint32_t oldest_unauth_time = 0xFFFFFFFF;
      for (int i = 0; i < Config::TCP::MAX_TELNET_CLIENTS; ++i) {
        if (_sessions[i].sessionState == AWAITING_PASSWORD) {
          if (_sessions[i].connected_at_ms < oldest_unauth_time) {
            oldest_unauth_time = _sessions[i].connected_at_ms;
            victim_idx = i;
          }
        }
      }

      if (victim_idx >= 0) {
        _sessions[victim_idx].reset();
        emptySlot = victim_idx;
      } else {
        const char *busy_msg = "\r\n[SYSTEM] Server busy: Maximum authenticated sessions reached.\r\n";
        send(new_sock, busy_msg, strlen(busy_msg), 0);
        close(new_sock);
        return;
      }
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

  if (!g_telnet_tx_sem)
    g_telnet_tx_sem = xSemaphoreCreateMutex();

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

  struct timeval tv;
  if (active_clients == 0) {
    tv.tv_sec = 1;
    tv.tv_usec = 0;
  } else {
    tv.tv_sec = 0;
    tv.tv_usec = 10000;
  }

  TSTAGE(6);
  int activity = select(max_fd + 1, &readfds, &writefds, &errorfds, &tv);

  if (activity < 0) {
    if (errno == EINTR)
      return;
    vTaskDelay(pdMS_TO_TICKS(10));
    return;
  }

  TSTAGE(7);
  MutexLocker cliLock(_cli_mutex);
  uint32_t now = millis();

  TSTAGE(8);
  if (_server_fd >= 0 && FD_ISSET(_server_fd, &readfds)) {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int new_sock = accept(_server_fd, (struct sockaddr *)&client_addr, &client_len);
    if (new_sock >= 0) {
      onClientConnect(new_sock, client_addr, now);
    }
  }

  for (int i = 0; i < Config::TCP::MAX_TELNET_CLIENTS; ++i) {
    TelnetSession &s = _sessions[i];
    if (s.sock < 0)
      continue;

    uint32_t session_timeout = (s.sessionState == AWAITING_PASSWORD)
                                   ? 30000
                                   : Config::TCP::TELNET_SESSION_TIMEOUT_MS;
    if (TimeUtils::isElapsed(s.last_activity_ms, session_timeout)) {
      sendTelnetMsg(s.sock, "\r\n[SYSTEM] Disconnected due to inactivity.\r\n");
      handleClientDisconnect(&s);
      continue;
    }

    if (FD_ISSET(s.sock, &errorfds)) {
      handleClientDisconnect(&s);
      continue;
    }

    bool did_read = false;
    if (FD_ISSET(s.sock, &readfds)) {
      char rx_buf[128];
      TSTAGE(9);
      int len = recv(s.sock, rx_buf, sizeof(rx_buf) - 1, 0);
      if (len > 0) {
        rx_buf[len] = '\0';
        onClientData(&s, rx_buf, len);
        did_read = true;
      } else if (len == 0 ||
                 (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        handleClientDisconnect(&s);
        continue;
      }
    }

    if (!did_read && FD_ISSET(s.sock, &writefds) && s.txLen > 0) {
      TSTAGE(10);
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

bool TelnetManager::broadcastNoticeNonBlocking(const char *msg) {
  if (!msg)
    return true;
  MutexLocker cliLock(_cli_mutex, 0);
  if (!cliLock.isLocked()) {
    return false;
  }
  size_t len = strlen(msg);
  for (int i = 0; i < Config::TCP::MAX_TELNET_CLIENTS; ++i) {
    int sock = _sessions[i].sock;
    if (sock >= 0) {
      send(sock, msg, len, MSG_DONTWAIT);
    }
  }
  return true;
}

void Task_Telnet(void *pvParameters) {
  TSTAGE(1);
  const esp_err_t wdt_ret = esp_task_wdt_add(nullptr);
  const bool twdt_registered = (wdt_ret == ESP_OK);
  TSTAGE(2);
  if (!g_tracer_sem)
    g_tracer_sem = xSemaphoreCreateBinary();
  g_telnet_manager.startServer();
  TSTAGE(3);

  static bool first_feed = true;
  static uint32_t s_last_twdt_feed_ms = 0;
  static uint32_t s_max_twdt_interval_ms = 0;

  bool ota_notice_sent = false;

  for (;;) {
    if (g_ota_in_progress.load(std::memory_order_acquire)) {
      TSTAGE(4);
      if (!ota_notice_sent) {
        if (g_telnet_manager.broadcastNoticeNonBlocking(
                "\r\n[OTA] Firmware update in progress. Telnet CLI paused...\r\n")) {
          ota_notice_sent = true;
        }
      }
      if (twdt_registered) {
        uint32_t now_ms = millis();
        if (first_feed) {
          s_last_twdt_feed_ms = now_ms;
          first_feed = false;
        } else {
          uint32_t interval = now_ms - s_last_twdt_feed_ms;
          if (interval > s_max_twdt_interval_ms) {
            s_max_twdt_interval_ms = interval;
          }
          s_last_twdt_feed_ms = now_ms;
        }
        esp_task_wdt_reset();
      }
      g_wdt_monitor.feed(Config::Task::WDT_ID_TELNET);
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    ota_notice_sent = false;

    TSTAGE(5);
    if (twdt_registered) {
      uint32_t now_ms = millis();
      if (first_feed) {
        s_last_twdt_feed_ms = now_ms;
        first_feed = false;
      } else {
        uint32_t interval = now_ms - s_last_twdt_feed_ms;
        if (interval > s_max_twdt_interval_ms) {
          s_max_twdt_interval_ms = interval;
        }
        s_last_twdt_feed_ms = now_ms;
      }
      esp_task_wdt_reset();
    }
    g_wdt_monitor.feed(Config::Task::WDT_ID_TELNET);

    if (g_restart_pending.load(std::memory_order_acquire)) {
      g_restart_pending.store(false, std::memory_order_relaxed);
      System_Restart(g_restart_reason ? g_restart_reason : "Telnet Command");
    }

    g_telnet_manager.tick();

    TSTAGE(11);
    if (g_telnet_manager.hasActiveClients()) {
      g_telnet_tracer.flushToClient();
      TSTAGE(14);
      xSemaphoreTake(g_tracer_sem, pdMS_TO_TICKS(5));
    } else {
      TSTAGE(14);
      xSemaphoreTake(g_tracer_sem, 0);
    }
  }
}

// ============================================================================
// TELNET PACKET TRACER
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
      pkt_dev_id = pkt.data[1];
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

namespace {
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
} // anonymous namespace

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

  TSTAGE(12);
  if (g_telnet_tx_sem && xSemaphoreTake(g_telnet_tx_sem, 0) != pdTRUE) {
    return;
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
      if (!entry.is_tx) {
        is_new_req = true;
        init_tracker(s_wp_tracker[wp_idx], entry.type);
        if (entry.type == TraceType::CTL) {
          const char *tags[] = {"CMD_CH2", "CMD_CH3", "CMD_CH6"};
          delay_tag = tags[wp_idx];
          delay_ms = -2;
        }
      } else if (entry.type == TraceType::ACK) {
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
        TSTAGE(13);
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
    if (idx >= sizeof(line_buf)) idx = sizeof(line_buf) - 1;

    if (entry.type == TraceType::MSG) {
      auto safe_append = [&](const char *fmt, auto... args) {
        if (idx >= sizeof(line_buf)) return;
        int n = snprintf(line_buf + idx, sizeof(line_buf) - idx, fmt, args...);
        if (n > 0) idx = std::min(idx + (size_t)n, sizeof(line_buf) - 1);
      };
      safe_append("[SYSTEM MSG]  ");
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
        idx += snprintf(line_buf + idx, sizeof(line_buf) - idx, "[%s]", delay_tag);
      }
    }

    idx += snprintf(line_buf + idx, sizeof(line_buf) - idx, "\r\n");
    TSTAGE(13);
    send(c_fd, line_buf, idx, MSG_DONTWAIT);
  }
}

// ============================================================================
// From src/CLI/Cli.cpp
// ============================================================================

static inline TelnetManager::TelnetSession *getSession(void *ctx) noexcept {
  return static_cast<TelnetManager::TelnetSession *>(ctx);
}

static inline int getSock(void *ctx) noexcept {
  auto *s = getSession(ctx);
  return s ? s->sock : -1;
}

char g_cli_scratch_buf[5120];

// ============================================================================
// From src/CLI/CliNetwork.cpp
// ============================================================================

namespace WifiCli {

static std::atomic<bool> s_wifi_scan_running{false};

static void AsyncWifiScanTask(void *pvParameters) {
  if (!pvParameters) {
    s_wifi_scan_running.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
    return;
  }
  TelnetManager::WifiScanReq req = *static_cast<TelnetManager::WifiScanReq *>(pvParameters);
  vTaskDelay(pdMS_TO_TICKS(100));

  int n = WiFi.scanNetworks(false, true);

  auto scan_buf = std::make_unique<char[]>(4096);
  scan_buf[0] = '\0';
  AppendBuf out{scan_buf.get(), 4096};

  out.append("\r\n");
  out.append(Fmt::DIV80EQ);
  out.append("                      2.4GHz WI-FI ACCESS POINT SCAN RESULTS                  \r\n");
  out.append(Fmt::DIV80EQ);
  out.append("No   SSID                             RSSI      Channel  Encryption\r\n");
  out.append(Fmt::DIV80);

  if (n == 0) {
    out.append("  (No wireless networks found)\r\n");
  } else if (n < 0) {
    out.append("  [ERROR] Wi-Fi hardware scan failed or timed out.\r\n");
  } else {
    int max_display = std::min(n, 40);
    for (int i = 0; i < max_display; ++i) {
      const char *encType = "Open";
      switch (WiFi.encryptionType(i)) {
      case WIFI_AUTH_WEP: encType = "WEP"; break;
      case WIFI_AUTH_WPA_PSK: encType = "WPA"; break;
      case WIFI_AUTH_WPA2_PSK: encType = "WPA2"; break;
      case WIFI_AUTH_WPA_WPA2_PSK: encType = "WPA/WPA2"; break;
      case WIFI_AUTH_WPA2_ENTERPRISE: encType = "Enterprise"; break;
      case WIFI_AUTH_WPA3_PSK: encType = "WPA3"; break;
      case WIFI_AUTH_WPA2_WPA3_PSK: encType = "WPA2/WPA3"; break;
      default: break;
      }
      out.appendFormat("%02d   %-32s %4d dBm   %3d      %s\r\n", i + 1,
                       WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i),
                       encType);
    }
  }
  out.append(Fmt::DIV80EQ);
  out.append("\r\n");
  WiFi.scanDelete();

  g_telnet_manager.sendScanResult(req, out.buf);
  scan_buf.reset(); // FreeRTOS vTaskDelete 전 동적 힙 메모리 명시적 해제
  s_wifi_scan_running.store(false, std::memory_order_release);
  vTaskDelete(nullptr);
}

void cmdWifi(EmbeddedCli *cli, char *args, void *context) {
  auto *sess = getSession(context);
  int sock = sess ? sess->sock : -1;
  uint8_t count = embeddedCliGetTokenCount(args);
  const char *subCmd = (count > 0) ? embeddedCliGetToken(args, 1) : "status";

  if (count == 0 || strcasecmp(subCmd, "status") == 0) {
    g_cli_scratch_buf[0] = '\0';
    AppendBuf out{g_cli_scratch_buf, sizeof(g_cli_scratch_buf)};

    out.append("\r\n");
    out.append(Fmt::DIV80EQ);
    out.append("                      WI-FI HARDWARE & NETWORK STATUS                         \r\n");
    out.append(Fmt::DIV80EQ);
    out.append("Category            Parameter       Value / Target                        Status\r\n");
    out.append(Fmt::DIV80);

    bool sta_ok = (WiFi.status() == WL_CONNECTED);
    out.appendFormat("%-20s%-16s%-32s%12s\r\n", "Station (STA)", "SSID",
                     sta_ok ? WiFi.SSID().c_str() : g_config.wifi_ssid,
                     sta_ok ? "[CONNECTED]" : "[DISCONNECTED]");
    out.appendFormat("%-20s%-16s%-32s%12s\r\n", "", "IP Address",
                     sta_ok ? WiFi.localIP().toString().c_str() : "0.0.0.0",
                     sta_ok ? "[ACTIVE]" : "[IDLE]");
    char rssi_b[16];
    snprintf(rssi_b, sizeof(rssi_b), "%d dBm", WiFi.RSSI());
    out.appendFormat("%-20s%-16s%-32s%12s\r\n", "", "Signal (RSSI)",
                     sta_ok ? rssi_b : "N/A", sta_ok ? "[STABLE]" : "[IDLE]");
    out.append(Fmt::DIV80);

    bool ap_active = (WiFi.getMode() == WIFI_MODE_AP || WiFi.getMode() == WIFI_MODE_APSTA);
    out.appendFormat("%-20s%-16s%-32s%12s\r\n", "SoftAP (AP)", "SSID",
                     g_config.ap_ssid, ap_active ? "[BROADCASTING]" : "[DISABLED]");
    out.appendFormat("%-20s%-16s%-32s%12s\r\n", "", "AP IP",
                     ap_active ? WiFi.softAPIP().toString().c_str() : "0.0.0.0",
                     ap_active ? "[ACTIVE]" : "[INACTIVE]");
    out.appendFormat("%-20s%-16s%-32s%12s\r\n", "", "Clients",
                     ap_active ? "Max 4 Clients" : "0 Clients",
                     ap_active ? "[READY]" : "[OFF]");
    out.append(Fmt::DIV80EQ);
    out.append("\r\n");

    sendTelnetMsgLen(sock, out.buf, out.offset);
    return;
  }

  if (strcasecmp(subCmd, "scan") == 0) {
    bool expected = false;
    if (!s_wifi_scan_running.compare_exchange_strong(expected, true)) {
      sendTelnetMsg(sock, "[WARN] Wi-Fi scan already in progress. Please wait...\r\n");
      return;
    }
    sendTelnetMsg(sock, "[WIFI] Scanning background 2.4GHz APs (Takes 2-3s)...\r\n");
    g_wifi_scan_req.clientIp = sess ? sess->clientIp : IPAddress(0, 0, 0, 0);
    g_wifi_scan_req.sessionId = sess ? sess->sessionId : 0;
    xTaskCreatePinnedToCore(
        AsyncWifiScanTask, "WifiScanWorker", 4096, &g_wifi_scan_req, 2,
        NULL, 0);
  } else if (strcasecmp(subCmd, "connect") == 0) {
    if (count < 2) {
      sendTelnetMsg(sock, "[ERROR] Usage: wifi connect <ssid> [password]\r\n");
      return;
    }
    const char *ssid_arg = embeddedCliGetToken(args, 2);
    const char *pass_arg = (count >= 3) ? embeddedCliGetToken(args, 3) : "";

    {
      CriticalSectionLocker lock(&g_config_mux);
      strncpy(g_config.wifi_ssid, ssid_arg, sizeof(g_config.wifi_ssid) - 1);
      g_config.wifi_ssid[sizeof(g_config.wifi_ssid) - 1] = '\0';
      strncpy(g_config.wifi_password, pass_arg, sizeof(g_config.wifi_password) - 1);
      g_config.wifi_password[sizeof(g_config.wifi_password) - 1] = '\0';
    }
    Config_Save();

    sendTelnetMsgf(sock, "[WIFI] Saved SSID '%s' to NVS. Connecting...\r\n", ssid_arg);
    WiFi.disconnect(false);
    vTaskDelay(pdMS_TO_TICKS(100));
    WiFi.begin(g_config.wifi_ssid, g_config.wifi_password);
  } else if (strcasecmp(subCmd, "disconnect") == 0) {
    WiFi.disconnect(false);
    sendTelnetMsg(sock, "[WIFI] Disconnected from Wi-Fi AP.\r\n");
  } else {
    sendTelnetMsg(sock, "Usage: wifi [status | scan | connect <ssid> [password] | disconnect]\r\n");
  }
}

} // namespace WifiCli

// ============================================================================
// From src/CLI/CliSystem.cpp
// ============================================================================

namespace SystemCli {

void printSystemOverview(AppendBuf &out) {
  uint32_t ts = millis() / 1000;
  time_t now = time(nullptr);
  struct tm timeinfo;
  char time_str[64];
  const char *time_src = "System RTC";
  if (now > 1672531200) {
    localtime_r(&now, &timeinfo);
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &timeinfo);
    time_src = "NTP Synced";
  } else {
    snprintf(time_str, sizeof(time_str), "Uptime: %ud %02uh %02um %02us",
             ts / 86400, (ts % 86400) / 3600, (ts % 3600) / 60, ts % 60);
    time_src = "Unsynchronized";
  }

  auto make_ascii_bar = [](char *b, size_t sz, uint32_t p) {
    if (sz < 14)
      return;
    b[0] = '[';
    for (int i = 1; i <= 10; ++i)
      b[i] = (p >= i * 10) ? '#' : '.';
    b[11] = ']';
    b[12] = ' ';
    b[13] = '\0';
  };

  uint32_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024;
  uint32_t min_free_heap =
      heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT) / 1024;
  uint32_t total_heap = heap_caps_get_total_size(MALLOC_CAP_8BIT) / 1024;
  uint32_t heap_free_pct =
      (total_heap > 0) ? (free_heap * 100 / total_heap) : 0;

  uint32_t sketch_size = ESP.getSketchSize() / 1024;
  uint32_t flash_size = ESP.getFlashChipSize() / 1024;
  uint32_t flash_used_pct =
      (flash_size > 0) ? (sketch_size * 100 / flash_size) : 0;

  char heap_bar[16], flash_bar[16];
  make_ascii_bar(heap_bar, sizeof(heap_bar), heap_free_pct);
  make_ascii_bar(flash_bar, sizeof(flash_bar), flash_used_pct);

  auto *active = WallpadParserFactory::getActiveParser();
  auto desc = g_auto_probing_engine.getDescriptor();
  char wp_status_buf[80];
  char vendor_name_buf[UniversalProtocolEngine::kVendorNameMaxLen] = "Unknown";
  if (active) {
    active->getVendorName(vendor_name_buf, sizeof(vendor_name_buf));
  }
  const char *catalog_vendor = vendor_name_buf;

  if (g_config.wallpad_profile == 0) {
    if (desc.is_locked) {
      snprintf(wp_status_buf, sizeof(wp_status_buf), "Auto Detect (%s)", catalog_vendor);
    } else {
      snprintf(wp_status_buf, sizeof(wp_status_buf), "Auto Detect (Learning...)");
    }
  } else {
    VendorProfileDescriptor cur_p;
    if (ProfileRepository::getActiveProfile(cur_p)) {
      snprintf(wp_status_buf, sizeof(wp_status_buf), "%s (%s)",
               cur_p.name[0] ? cur_p.name : cur_p.key, catalog_vendor);
    } else {
      snprintf(wp_status_buf, sizeof(wp_status_buf), "%s", catalog_vendor);
    }
  }

  out.appendFormat("\r\n==========================================================="
      "=====================\r\n"
      "                    GATEWAY BRIDGE SYSTEM & TRAFFIC METRICS   "
      "                \r\n"
      "==============================================================="
      "=================\r\n"
      "Firmware        : %s\r\n"
      "Wallpad Profile : %s\r\n"
      "System Time     : %s (%s)\r\n"
      "Uptime          : %ud %02uh %02um %02us\r\n"
      "WiFi Connection : %s (%d dBm, IP: %s) [STABLE]\r\n"
      "Heap Memory     : %s %3u%% Free (Free %uKB / Min %uKB)\r\n"
      "Flash Storage   : %s %3u%% Used (%uKB / %uMB)\r\n",
      Config::FIRMWARE_VERSION, wp_status_buf, time_str, time_src, ts / 86400,
      (ts % 86400) / 3600, (ts % 3600) / 60, ts % 60,
      WiFi.isConnected() ? "Connected" : "Disconnected", WiFi.RSSI(),
      WiFi.localIP().toString().c_str(), heap_bar, heap_free_pct, free_heap,
      min_free_heap, flash_bar, flash_used_pct, sketch_size, flash_size / 1024);
}

void printStats(int sock) {
  static std::atomic<bool> s_busy{false};
  if (s_busy.exchange(true, std::memory_order_acquire)) {
    sendTelnetMsg(sock, "[BUSY] Stats is being generated.\r\n");
    return;
  }

  g_cli_scratch_buf[0] = '\0';
  AppendBuf out{g_cli_scratch_buf, sizeof(g_cli_scratch_buf)};

  printSystemOverview(out);

  g_wdt_monitor.feed(5);
  SysSnapshot sys_snap;
  HwSnapshot hw_snap;
  StackSnapshot stack_snap;
  PktSnapshot pkt_snap;
  System_TakeSnapshot(sys_snap, hw_snap, stack_snap, pkt_snap);

  Fmt::FormatHwMetrics(out, hw_snap);
  Fmt::FormatNetworkStats(out, pkt_snap);
  Fmt::FormatRs485Stats(out, pkt_snap);

  out.append(Fmt::DIV80);
  out.appendFormat("%-55s %24s\r\n", "Metric / Event", "Value / Counter / Status");
  out.append(Fmt::DIV80);
  out.appendFormat("%-55s %24u\r\n"
                   "%-55s %24u\r\n"
                   "%-55s %24u\r\n"
                   "%-55s %24u\r\n",
                   "Total Device Polls",
                   static_cast<unsigned>(g_ch1_state_metrics.poll_cnt.load(std::memory_order_relaxed)),
                   "VIP Controls (SmartThings App)",
                   static_cast<unsigned>(g_ch1_state_metrics.vip_cnt.load(std::memory_order_relaxed)),
                   "Normal Controls (Wallpad)",
                   static_cast<unsigned>(g_ch1_state_metrics.normal_cnt.load(std::memory_order_relaxed)),
                   "Stale Emerg Polls",
                   static_cast<unsigned>(g_ch1_state_metrics.stale_poll_cnt.load(std::memory_order_relaxed)));

  Fmt::FormatTaskStacks(out, stack_snap, g_wdt_monitor);
  out.append("================================================================================\r\n\r\n");

  sendTelnetMsgLen(sock, out.buf, out.offset);
  s_busy.store(false, std::memory_order_release);
}

void cmdStats(EmbeddedCli *cli, char *args, void *context) {
  int client = getSock(context);
  int argc = embeddedCliGetTokenCount(args);

  if (argc > 0) {
    const char *sub = embeddedCliGetToken(args, 1);
    if (strcasecmp(sub, "clear") == 0) {
      g_pkt_stats.resetAll();
      g_polling_targets.resetHits();
      g_metrics.reset();
      sendTelnetMsg(client, "All traffic statistics, hits, and metrics history CLEARED to 0.\r\n");
      return;
    }
    sendTelnetMsg(client, "Usage: stats [clear]\r\n");
    return;
  }
  printStats(client);
}

void cmdReboot(EmbeddedCli *cli, char *args, void *context) {
  int client = getSock(context);
  sendTelnetMsg(client, "Rebooting...\r\n");
  g_restart_reason = "Telnet Command";
  g_restart_pending.store(true, std::memory_order_release);
}

void cmdLogView(EmbeddedCli *cli, char *args, void *context) {
  int client = getSock(context);
  const char *sub_cmd = (embeddedCliGetTokenCount(args) > 0)
                            ? embeddedCliGetToken(args, 1)
                            : "list";

  if (strcasecmp(sub_cmd, "clear") == 0) {
    LogManager::clearRebootLog();
    sendTelnetMsg(client, "Reboot log history CLEARED from NVS flash.\r\n");
    return;
  }

  size_t count = LogManager::getLogCount();
  if (count == 0) {
    sendTelnetMsg(client, "\r\n[LOGVIEW] No persistent reboot logs found in NVS.\r\n");
    return;
  }

  if (strcasecmp(sub_cmd, "list") == 0) {
    g_cli_scratch_buf[0] = '\0';
    AppendBuf out{g_cli_scratch_buf, sizeof(g_cli_scratch_buf)};

    out.append("\r\n");
    out.append(Fmt::DIV80EQ);
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "Persistent Reboot Log History (Total: %u / %u)",
             static_cast<unsigned>(count), static_cast<unsigned>(LogManager::MAX_LOG_ENTRIES));
    out.appendFormat("%*s%s\r\n", std::max(0, (80 - static_cast<int>(strlen(hdr))) / 2), "", hdr);
    out.append(Fmt::DIV80EQ);

    for (size_t i = 0; i < count; i++) {
      LogEntry entry;
      if (LogManager::getLogEntry(i, entry)) {
        char time_buf[32] = "N/A";
        if (entry.timestamp > 0) {
          struct tm timeinfo;
          time_t sec = static_cast<time_t>(entry.timestamp);
          localtime_r(&sec, &timeinfo);
          if (timeinfo.tm_year >= 124) {
            strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
          } else {
            snprintf(time_buf, sizeof(time_buf),
                     "%04d-%02d-%02d %02d:%02d:%02d", timeinfo.tm_year + 1900,
                     timeinfo.tm_mon + 1, timeinfo.tm_mday, timeinfo.tm_hour,
                     timeinfo.tm_min, timeinfo.tm_sec);
          }
        }
        uint32_t sec = entry.stats_snapshot.uptime_ms / 1000;
        out.appendFormat("  [#%-2u] %s | Reason: %-28s | Up: %02uh %02um\r\n",
                         static_cast<unsigned>(i + 1), time_buf, entry.reason, sec / 3600,
                         (sec % 3600) / 60);
      }
    }
    out.append(Fmt::DIV80);
    out.append("\r\n");
    sendTelnetMsgLen(client, out.buf, out.offset);
    return;
  }

  size_t target_idx = 0;
  if (strcasecmp(sub_cmd, "last") == 0) {
    target_idx = 0;
  } else {
    char *endp = nullptr;
    long val = strtol(sub_cmd, &endp, 10);
    if (endp != sub_cmd && *endp == '\0' && val >= 1 && static_cast<size_t>(val) <= count) {
      target_idx = static_cast<size_t>(val - 1);
    } else {
      sendTelnetMsg(client, "Usage: logview [list | <1-20> | last | clear]\r\n");
      return;
    }
  }

  LogManager::readRebootLog(g_cli_scratch_buf, sizeof(g_cli_scratch_buf), target_idx);
  sendTelnetMsg(client, g_cli_scratch_buf);
}

void cmdCoreDump(EmbeddedCli *cli, char *args, void *context) {
  int client = getSock(context);
  if (embeddedCliGetTokenCount(args) >= 1 &&
      strcasecmp(embeddedCliGetToken(args, 1), "clear") == 0) {
    esp_core_dump_image_erase();
    sendTelnetMsg(client, "Crash core dump partition successfully ERASED.\r\n");
    return;
  }

  esp_core_dump_summary_t summary;
  esp_err_t err = esp_core_dump_get_summary(&summary);

  if (err != ESP_OK) {
    sendTelnetMsg(client,
                  "\r\n[COREDUMP] No crash core dump summary available (Partition clean or empty).\r\n");
    return;
  }

  char *buf = g_cli_scratch_buf;
  constexpr size_t buf_size = sizeof(g_cli_scratch_buf);
  int pos = 0;

  pos += snprintf(
      buf + pos, buf_size - pos,
      "\r\n========================================================================"
      "========\r\n"
      "                    CRASH CORE DUMP BACKTRACE SUMMARY                   "
      "       \r\n"
      "========================================================================"
      "========\r\n"
      "Status          : Valid Core Dump Found\r\n"
      "Crashed Task    : %s\r\n"
      "Program Counter : 0x%08X\r\n"
      "Exception Cause : %lu\r\n"
      "Backtrace Depth : %d frames%s\r\n"
      "Backtrace PCs   :\r\n",
      summary.exc_task, static_cast<unsigned>(summary.exc_pc), static_cast<unsigned long>(summary.ex_info.exc_cause),
      summary.exc_bt_info.depth,
      summary.exc_bt_info.corrupted ? " (CORRUPTED)" : "");

  for (int i = 0; i < summary.exc_bt_info.depth && pos < static_cast<int>(buf_size) - 64;
       ++i) {
    pos += snprintf(buf + pos, buf_size - pos, "  [%2d] 0x%08X\r\n", i,
                    static_cast<unsigned>(summary.exc_bt_info.bt[i]));
  }

  pos += snprintf(
      buf + pos, buf_size - pos,
      "\r\n===================================================================="
      "============\r\n"
      "Use: xtensa-esp32s3-elf-addr2line -pfiaC -e firmware.elf <PC>\r\n"
      "========================================================================"
      "========\r\n\r\n");
  sendTelnetMsg(client, buf);
}

void otaPrintStatus(AppendBuf &out) {
  const esp_partition_t *running = esp_ota_get_running_partition();
  const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
  esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
  if (running) {
    esp_ota_get_state_partition(running, &ota_state);
  }

  const char *state_desc = "Confirmed";
  const char *state_status = "[STABLE]";
  switch (ota_state) {
  case ESP_OTA_IMG_NEW:
    state_desc = "New Image (First Boot)";
    state_status = "[NEW]";
    break;
  case ESP_OTA_IMG_PENDING_VERIFY:
    state_desc = "Evaluating (Rollback Active)";
    state_status = "[PENDING]";
    break;
  case ESP_OTA_IMG_VALID:
    state_desc = "Confirmed";
    state_status = "[STABLE]";
    break;
  case ESP_OTA_IMG_INVALID:
    state_desc = "Invalidated Image";
    state_status = "[INVALID]";
    break;
  case ESP_OTA_IMG_ABORTED:
    state_desc = "Aborted Image";
    state_status = "[ABORTED]";
    break;
  default:
    break;
  }

  char run_val[36], next_val[36], timer_val[36], crash_val[36];
  snprintf(run_val, sizeof(run_val), "%s (0x%06X, %u KB)",
           running ? running->label : "app0",
           running ? static_cast<unsigned>(running->address) : 0x10000,
           running ? static_cast<unsigned>(running->size / 1024) : 3712);

  snprintf(next_val, sizeof(next_val), "%s (0x%06X, %u KB)",
           next ? next->label : "app1",
           next ? static_cast<unsigned>(next->address) : 0x3B0000,
           next ? static_cast<unsigned>(next->size / 1024) : 3712);

  bool val_done = TimeUtils::isElapsed(g_boot_start_ms, Config::Timing::OTA_VALIDATION_PERIOD_MS);
  snprintf(timer_val, sizeof(timer_val), "%s", val_done ? "120s Passed" : "Evaluating (<120s)");
  snprintf(crash_val, sizeof(crash_val), "%u Consecutive Crashes", static_cast<unsigned>(rtc_crash_counter));

  bool is_rescue = g_rescue_mode.load(std::memory_order_relaxed);

  out.append("\r\n");
  out.append(Fmt::DIV80EQ);
  out.append("                    DUAL-PARTITION OTA & ROLLBACK MONITOR                     \r\n");
  out.append(Fmt::DIV80EQ);
  out.append("Category            Parameter       Value / Target                        Status\r\n");
  out.append(Fmt::DIV80);

  out.appendFormat("%-20s%-16s%-32s%12s\r\n", "Running App",    "Partition",     run_val,             "[ACTIVE]");
  out.appendFormat("%-20s%-16s%-32s%12s\r\n", "",               "State",         state_desc,          state_status);
  out.append(Fmt::DIV80);

  esp_ota_img_states_t next_state = ESP_OTA_IMG_UNDEFINED;
  if (next) {
    esp_ota_get_state_partition(next, &next_state);
  }
  const char *next_desc = "Hardware Dual-Slot";
  const char *next_status = "[READY]";
  if (next_state == ESP_OTA_IMG_INVALID) {
    next_desc = "Invalidated (Failed Boot)";
    next_status = "[INVALID]";
  } else if (next_state == ESP_OTA_IMG_ABORTED) {
    next_desc = "Aborted Image";
    next_status = "[ABORTED]";
  }

  out.appendFormat("%-20s%-16s%-32s%12s\r\n", "Backup Target",  "Partition",     next_val,            "[STANDBY]");
  out.appendFormat("%-20s%-16s%-32s%12s\r\n", "",               "Rollback",      next_desc,           next_status);
  out.append(Fmt::DIV80);

  out.appendFormat("%-20s%-16s%-32s%12s\r\n", "Safety Guard",   "Health Timer",  timer_val,           val_done ? "[STABLE]" : "[TESTING]");
  out.appendFormat("%-20s%-16s%-32s%12s\r\n", "",               "Crash Loop",    crash_val,           rtc_crash_counter == 0 ? "[STABLE]" : "[WARNING]");
  out.appendFormat("%-20s%-16s%-32s%12s\r\n", "",               "Rescue Mode",   is_rescue ? "Forced Safe SoftAP" : "Standard Boot", is_rescue ? "[RESCUE]" : "[STABLE]");
  out.append(Fmt::DIV80EQ);
  out.append("\r\n");
}

void otaTriggerRollback(int sock) {
  sendTelnetMsg(sock, "[OTA] Invalidating current app and triggering hardware rollback to previous firmware...\r\n");
  vTaskDelay(pdMS_TO_TICKS(100));
  esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
  if (err != ESP_OK) {
    char err_buf[64];
    snprintf(err_buf, sizeof(err_buf), "[ERROR] Rollback failed (No rollback partition available, err=0x%x)\r\n", err);
    sendTelnetMsg(sock, err_buf);
  }
}

void otaValidate(int sock) {
  esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  if (err == ESP_OK) {
    sendTelnetMsg(sock, "[OTA] Current firmware manually confirmed as VALID. Auto-rollback cancelled.\r\n");
  } else {
    char err_buf[64];
    snprintf(err_buf, sizeof(err_buf), "[ERROR] Failed to mark app valid: 0x%x\r\n", err);
    sendTelnetMsg(sock, err_buf);
  }
}

void cmdOta(EmbeddedCli *cli, char *args, void *context) {
  int sock = getSock(context);
  uint8_t count = embeddedCliGetTokenCount(args);
  const char *subCmd = (count > 0) ? embeddedCliGetToken(args, 1) : "status";

  if (strcasecmp(subCmd, "status") == 0) {
    g_cli_scratch_buf[0] = '\0';
    AppendBuf out{g_cli_scratch_buf, sizeof(g_cli_scratch_buf)};
    otaPrintStatus(out);
    sendTelnetMsgLen(sock, out.buf, out.offset);
  } else if (strcasecmp(subCmd, "rollback") == 0) {
    otaTriggerRollback(sock);
  } else if (strcasecmp(subCmd, "validate") == 0) {
    otaValidate(sock);
  } else if (strcasecmp(subCmd, "cloud") == 0) {
    const char *url = (count >= 2) ? embeddedCliGetToken(args, 2) : nullptr;
    sendTelnetMsg(sock, "[OTA] Initiating GitHub Cloud HTTP(S) OTA in background...\r\n");
    Mgmt_StartHttpOta(url);
  } else {
    sendTelnetMsg(sock, "Usage: ota [status|rollback|validate|cloud [url]]\r\n");
  }
}

void cmdHelp(EmbeddedCli *cli, char *args, void *context) {
  int client = getSock(context);

  g_cli_scratch_buf[0] = '\0';
  AppendBuf out{g_cli_scratch_buf, sizeof(g_cli_scratch_buf)};

  out.append("\r\n");
  out.append(Fmt::DIV80EQ);
  out.append("                       GATEWAY BRIDGE COMMAND REFERENCE                       \r\n");
  out.append(Fmt::DIV80EQ);
  out.append("Command / Parameter               Description & Usage Example\r\n");
  out.append(Fmt::DIV80);

  out.append(" [ SYSTEM & DIAGNOSTICS ]\r\n");
  out.append("  stats [clear]                   Show real-time HW metrics & traffic stats (or reset)\r\n");
  out.append("  devs [1|2|clear]                Show device registry & cache (1: T1 Targets, 2: T2 Cache)\r\n");
  out.append("  logview [list|<1-20>|last|clear] View persistent reboot log history from NVS\r\n");
  out.append("  coredump [clear]                View crash core dump summary & backtrace\r\n");
  out.append("  ota [status|rollback|validate]  Dual-partition OTA & auto-rollback management\r\n");
  out.append("  reboot                          Safely commit buffers and reboot gateway hardware\r\n");
  out.append(Fmt::DIV80);

  out.append(" [ PROTOCOL & PROBING ]\r\n");
  out.append("  wallpad [status]                Show wallpad profile learning status & parameters\r\n");
  out.append("  wallpad list                    List available vendor & saved NVS custom profiles\r\n");
  out.append("  wallpad set <key|id>            Manually switch active wallpad profile\r\n");
  out.append("  wallpad save <name>             Save current auto-learned profile to NVS slot\r\n");
  out.append("  wallpad delete <id>             Reset a saved custom profile slot in NVS\r\n");
  out.append("  wallpad auto                    Switch to Universal Auto-Probing mode\r\n");
  out.append("  wallpad reset                   Reset auto-probing engine and re-learn bus traffic\r\n");
  out.append("  wallpad simulate <hex...>       Inject raw hex packet into auto-probing engine\r\n");
  out.append(Fmt::DIV80);
  out.append(" [ CONTROL BLUEPRINT & LEARNING ]\r\n");
  out.append("  ctl [table|list]                Display learned control blueprint table\r\n");
  out.append("  ctl <dev_id>                    Dump detailed packet blueprint & action slots (e.g. ctl 0x18)\r\n");
  out.append("  ctl learn [id|all]              Active probing session (all groups if omitted)\r\n");
  out.append("  ctl lock [dev_id|all]           Lock blueprint(s) into immutable state\r\n");
  out.append("  ctl unlock [dev_id|all]         Unlock blueprint(s) for continuous learning\r\n");
  out.append("  ctl name <dev_id> <name>        Assign custom group name (e.g. ctl name 0x1B Gas)\r\n");
  out.append("  ctl class <dev_id> <class>      Assign device class (light|outlet|vent|thermo|gas|ev)\r\n");
  out.append("  ctl status                      Show active probing real-time progress\r\n");
  out.append("  ctl reset [id|all]              Reset blueprint(s) and wipe from NVS flash\r\n");
  out.append("  trace [on|off|ctl|ack|pol|...]  Live packet stream monitoring with filters\r\n");
  out.append("  q                               Shortcut to stop live tracing immediately\r\n");
  out.append(Fmt::DIV80);

  out.append(" [ NETWORK & CONFIG ]\r\n");
  out.append("  wifi [status]                   Show Wi-Fi STA connection status & signal strength\r\n");
  out.append("  wifi scan                       Scan surrounding 2.4GHz Wi-Fi AP networks\r\n");
  out.append("  wifi connect <ssid> [password]  Connect to target Wi-Fi AP network\r\n");
  out.append("  wifi disconnect                 Disconnect current Wi-Fi station\r\n");
  out.append("  ew11 [list|status]              Show CH5 EW11 multi-client slots & framing status\r\n");
  out.append("  ew11 set <slot> <ip> [port]     Configure EW11 slot IP & port (Saved to NVS)\r\n");
  out.append("  ew11 frame <slot> <stx> <etx>   Manually fix slot packet framing in NVS\r\n");
  out.append("  ew11 reset <slot>               Reset slot framing tracker to autonomous auto-probing\r\n");
  out.append("  ew11 enable/disable <slot>      Enable or disable target EW11 client slot\r\n");
  out.append("  routes [clear]                  Show dynamic device ingress routing table\r\n");
  out.append("  config                          View all runtime configuration parameters\r\n");
  out.append("  config set <key> <val>          Modify a configuration parameter (runtime)\r\n");
  out.append("  config reset                    Reset runtime configuration to system defaults\r\n");
  out.append("  save                            Commit and save all configuration to NVS flash\r\n");
  out.append("  exit                            Disconnect current Telnet CLI session\r\n");
  out.append(Fmt::DIV80EQ);
  out.append("\r\n");

  sendTelnetMsgLen(client, out.buf, out.offset);
}

} // namespace SystemCli

// ============================================================================
// From src/CLI/CliConfig.cpp
// ============================================================================

namespace ConfigCli {

enum ParamType {
  PARAM_UINT32,
  PARAM_UINT16,
  PARAM_UCHAR,
  PARAM_STRING,
  PARAM_PASS_HASH,
  PARAM_FRAMING_CH1,
  PARAM_FRAMING_CH2,
  PARAM_FRAMING_CH3,
  PARAM_FRAMING_CH4,
  PARAM_TIMING_CH1,
  PARAM_TIMING_CH2,
  PARAM_TIMING_CH3
};

struct ConfigParamDef {
  const char *name;
  ParamType type;
  union {
    uint32_t *u32;
    uint16_t *u16;
    uint8_t *u8;
    char *str;
  } ptr;
  uint32_t minVal;
  uint32_t maxVal;
  const char *desc;
};

static const ConfigParamDef PARAM_TABLE[] = {
    // Channel Baudrates
    {"ch1_baud", PARAM_UINT32, {.u32 = &g_config.uart_baud_rate}, 1200, 115200, "CH1 Device Master Baudrate (bps)"},
    {"ch2_baud", PARAM_UINT32, {.u32 = &g_config.ch2_baud_rate}, 1200, 115200, "CH2 Main Wallpad Baudrate (bps)"},
    {"ch3_baud", PARAM_UINT32, {.u32 = &g_config.ch3_baud_rate}, 1200, 115200, "CH3 Sub Wallpad Baudrate (bps)"},
    {"ch4_baud", PARAM_UINT32, {.u32 = &g_config.doorphone_baud_rate}, 1200, 115200, "CH4 Doorphone Baudrate (bps)"},

    // Channel Framings (8N1, 8E1, 8O1, 8N2)
    {"ch1_framing", PARAM_FRAMING_CH1, {.u8 = nullptr}, 0, 0, "CH1 Framing (8N1, 8E1, 8O1, 8N2)"},
    {"ch2_framing", PARAM_FRAMING_CH2, {.u8 = nullptr}, 0, 0, "CH2 Framing (8N1, 8E1, 8O1, 8N2)"},
    {"ch3_framing", PARAM_FRAMING_CH3, {.u8 = nullptr}, 0, 0, "CH3 Framing (8N1, 8E1, 8O1, 8N2)"},
    {"ch4_framing", PARAM_FRAMING_CH4, {.u8 = nullptr}, 0, 0, "CH4 Framing (8N1, 8E1, 8O1, 8N2)"},

    // Channel Delays
    {"ch1_poll", PARAM_TIMING_CH1, {.u16 = &g_timing_config.ch1_poll_interval_ms}, 200, 5000, "CH1 Master Polling Interval (ms)"},
    {"ch2_ack",  PARAM_TIMING_CH2, {.u16 = &g_timing_config.ch2_cache_delay_ms},   5,   300,  "CH2 Main Wallpad Virtual ACK (ms)"},
    {"ch3_ack",  PARAM_TIMING_CH3, {.u16 = &g_timing_config.ch3_cache_delay_ms},   20,  1000, "CH3 Sub Wallpad Virtual ACK (ms)"},

    // Wallpad Profile
    {"profile", PARAM_UCHAR, {.u8 = &g_config.wallpad_profile}, 0, 3, "Wallpad Profile Slot (0=Auto, 1=Custom1, etc)"},

    // Wi-Fi & Network
    {"wifi_ssid", PARAM_STRING, {.str = g_config.wifi_ssid}, 0, sizeof(g_config.wifi_ssid) - 1, "Station Wi-Fi SSID"},
    {"wifi_pass", PARAM_STRING, {.str = g_config.wifi_password}, 0, sizeof(g_config.wifi_password) - 1, "Station Wi-Fi Password"},
    {"ap_ssid",   PARAM_STRING, {.str = g_config.ap_ssid}, 0, sizeof(g_config.ap_ssid) - 1, "SoftAP SSID"},
    {"ap_pass",   PARAM_STRING, {.str = g_config.ap_password}, 0, sizeof(g_config.ap_password) - 1, "SoftAP Password"},
    {"wifi_timeout", PARAM_UINT16, {.u16 = &g_config.wifi_connect_timeout_s}, 5, 120, "Wi-Fi Connection Timeout (seconds)"},

    // Security
    {"telnet_pass", PARAM_PASS_HASH, {.str = g_config.telnet_pass_hash}, 0, 0, "Telnet Login Password"},
};
static const size_t PARAM_COUNT = sizeof(PARAM_TABLE) / sizeof(ConfigParamDef);

void printConfig(int sock) {
  g_cli_scratch_buf[0] = '\0';
  AppendBuf out{g_cli_scratch_buf, sizeof(g_cli_scratch_buf)};

  char f1[8], f2[8], f3[8], f4[8];
  snprintf(f1, sizeof(f1), "%s", formatFramingStr(g_config.uart_data_bits, g_config.uart_parity, g_config.uart_stop_bits));
  snprintf(f2, sizeof(f2), "%s", formatFramingStr(g_config.ch2_data_bits, g_config.ch2_parity, g_config.ch2_stop_bits));
  snprintf(f3, sizeof(f3), "%s", formatFramingStr(g_config.ch3_data_bits, g_config.ch3_parity, g_config.ch3_stop_bits));
  snprintf(f4, sizeof(f4), "%s", formatFramingStr(g_config.doorphone_data_bits, g_config.doorphone_parity, g_config.doorphone_stop_bits));

  const char *prof_name = "Auto Detect (Slot 0)";
  if (g_config.wallpad_profile == 1) prof_name = "Custom Slot 1";
  else if (g_config.wallpad_profile == 2) prof_name = "Custom Slot 2";
  else if (g_config.wallpad_profile == 3) prof_name = "Custom Slot 3";

  out.append("\r\n");
  out.append(Fmt::DIV80EQ);
  out.append("                         GATEWAY CONFIGURATION (NVS Flash)              \r\n");
  out.append(Fmt::DIV80EQ);
  out.append("[Network & Security]\r\n");
  out.appendFormat("  WiFi Station SSID : %s\r\n", g_config.wifi_ssid[0] ? g_config.wifi_ssid : "(Not Configured)");
  out.appendFormat("  WiFi SoftAP SSID  : %s\r\n", g_config.ap_ssid[0] ? g_config.ap_ssid : "(Disabled)");
  out.appendFormat("  WiFi Connect Tout : %u sec\r\n", g_config.wifi_connect_timeout_s);
  out.appendFormat("  Telnet Password   : %s\r\n", g_config.telnet_pass_hash[0] ? "Configured (SHA-256)" : "Default (None)");
  out.append("\r\n");
  out.append("[Wallpad & Protocol]\r\n");
  out.appendFormat("  Wallpad Profile   : %s\r\n", prof_name);
  out.append("\r\n");
  out.append("[Channels & RS-485 / Delays]\r\n");
  out.appendFormat("  CH1 (Device Master) : %-6u bps, %-3s | Query Interval : %u ms\r\n",
                   static_cast<unsigned>(g_config.uart_baud_rate), f1, g_timing_config.ch1_poll_interval_ms);
  out.appendFormat("  CH2 (Main Wallpad)  : %-6u bps, %-3s | Virtual ACK    : %u ms\r\n",
                   static_cast<unsigned>(g_config.ch2_baud_rate), f2, g_timing_config.ch2_cache_delay_ms);
  out.appendFormat("  CH3 (Sub Wallpad)   : %-6u bps, %-3s | Virtual ACK    : %u ms\r\n",
                   static_cast<unsigned>(g_config.ch3_baud_rate), f3, g_timing_config.ch3_cache_delay_ms);
  out.appendFormat("  CH4 (Doorphone)     : %-6u bps, %-3s | RX/TX Isolated\r\n",
                   static_cast<unsigned>(g_config.doorphone_baud_rate), f4);
  out.append(Fmt::DIV80EQ);
  out.append("Type 'config ?' or 'config help' to view all configurable parameter keys.\r\n\r\n");

  sendTelnetMsgLen(sock, out.buf, out.offset);
}

void printConfigHelp(int sock) {
  g_cli_scratch_buf[0] = '\0';
  AppendBuf out{g_cli_scratch_buf, sizeof(g_cli_scratch_buf)};

  out.append("\r\n");
  out.append(Fmt::DIV80EQ);
  out.append("                         CONFIGURABLE PARAMETERS GUIDE                          \r\n");
  out.append(Fmt::DIV80EQ);
  out.appendFormat("%-14s %-16s %s\r\n", "Parameter Key", "Valid Range / Type", "Description");
  out.append(Fmt::DIV80);

  for (size_t i = 0; i < PARAM_COUNT; ++i) {
    const auto &p = PARAM_TABLE[i];
    char range_buf[24];
    if (p.type == PARAM_UINT32 || p.type == PARAM_UINT16 || p.type == PARAM_UCHAR ||
        p.type == PARAM_TIMING_CH1 || p.type == PARAM_TIMING_CH2 || p.type == PARAM_TIMING_CH3) {
      snprintf(range_buf, sizeof(range_buf), "%lu ~ %lu", (unsigned long)p.minVal, (unsigned long)p.maxVal);
    } else if (p.type >= PARAM_FRAMING_CH1 && p.type <= PARAM_FRAMING_CH4) {
      snprintf(range_buf, sizeof(range_buf), "8N1,8E1,8O1,8N2");
    } else if (p.type == PARAM_PASS_HASH) {
      snprintf(range_buf, sizeof(range_buf), "string (raw)");
    } else {
      snprintf(range_buf, sizeof(range_buf), "string");
    }

    out.appendFormat("%-14s %-18s %s\r\n", p.name, range_buf, p.desc);
  }

  out.append(Fmt::DIV80);
  out.append("Usage:\r\n");
  out.append("  config set <key> <value>   : Modify parameter (RAM only)\r\n");
  out.append("  save                       : Commit modified parameters to NVS flash permanently\r\n");
  out.append("  config reset               : Restore all configuration to factory defaults\r\n");
  out.append(Fmt::DIV80EQ);
  out.append("\r\n");

  sendTelnetMsgLen(sock, out.buf, out.offset);
}

template <typename T>
static bool applyUintParam(T *dest, const char *value, unsigned long min_val, unsigned long max_val,
                           int sock, const char *key) {
  char *endp = nullptr;
  unsigned long v = strtoul(value, &endp, 10);
  if (!endp || *endp != '\0' || v < min_val || v > max_val) {
    sendTelnetMsgf(sock, "[ERROR] Invalid value '%s' for '%s' (Allowed: %lu ~ %lu)\r\n", value, key, min_val, max_val);
    return false;
  }
  *dest = static_cast<T>(v);
  g_config_dirty.store(true, std::memory_order_relaxed);
  sendTelnetMsgf(sock, "[OK] Set %s = %lu (RAM only. Use 'save' to commit to NVS)\r\n", key, v);
  return true;
}

static bool applyFraming(uint8_t &dbits, uint8_t &parity, uint8_t &sbits, const char *value, int sock, const char *key) {
  uint8_t d, p, s;
  if (!parseFramingStr(value, d, p, s)) {
    sendTelnetMsgf(sock, "[ERROR] Invalid framing '%s'. Choose from: 8N1, 8E1, 8O1, 8N2\r\n", value);
    return false;
  }
  dbits = d;
  parity = p;
  sbits = s;
  g_config_dirty.store(true, std::memory_order_relaxed);
  sendTelnetMsgf(sock, "[OK] Set %s = '%s' (RAM only. Use 'save' to commit to NVS)\r\n", key, value);
  return true;
}

void setConfig(void *session_context, const char *key, const char *value) {
  int sock = getSock(session_context);
  if (!key || !value) {
    sendTelnetMsg(sock, "[ERROR] Usage: config set <key> <value>\r\n");
    return;
  }

  for (size_t i = 0; i < PARAM_COUNT; ++i) {
    const auto &p = PARAM_TABLE[i];
    if (strcasecmp(p.name, key) == 0) {
      CriticalSectionLocker lock(&g_config_mux);
      switch (p.type) {
      case PARAM_UINT32:
        applyUintParam(p.ptr.u32, value, p.minVal, p.maxVal, sock, key);
        return;
      case PARAM_UINT16:
        applyUintParam(p.ptr.u16, value, p.minVal, p.maxVal, sock, key);
        return;
      case PARAM_UCHAR:
        applyUintParam(p.ptr.u8, value, p.minVal, p.maxVal, sock, key);
        return;
      case PARAM_TIMING_CH1:
      case PARAM_TIMING_CH2:
      case PARAM_TIMING_CH3: {
        char *endp = nullptr;
        unsigned long v = strtoul(value, &endp, 10);
        if (!endp || *endp != '\0' || v < p.minVal || v > p.maxVal) {
          sendTelnetMsgf(sock, "[ERROR] Invalid delay '%s' for '%s' (Allowed: %lu ~ %lu ms)\r\n", value, key, (unsigned long)p.minVal, (unsigned long)p.maxVal);
          return;
        }
        *p.ptr.u16 = static_cast<uint16_t>(v);
        TimingConfig_Save();
        sendTelnetMsgf(sock, "[OK] Set %s = %lu ms (Saved to timing_cfg NVS immediately)\r\n", key, v);
        return;
      }
      case PARAM_FRAMING_CH1:
        applyFraming(g_config.uart_data_bits, g_config.uart_parity, g_config.uart_stop_bits, value, sock, key);
        return;
      case PARAM_FRAMING_CH2:
        applyFraming(g_config.ch2_data_bits, g_config.ch2_parity, g_config.ch2_stop_bits, value, sock, key);
        return;
      case PARAM_FRAMING_CH3:
        applyFraming(g_config.ch3_data_bits, g_config.ch3_parity, g_config.ch3_stop_bits, value, sock, key);
        return;
      case PARAM_FRAMING_CH4:
        applyFraming(g_config.doorphone_data_bits, g_config.doorphone_parity, g_config.doorphone_stop_bits, value, sock, key);
        return;
      case PARAM_STRING: {
        if (strlen(value) <= p.maxVal) {
          strncpy(p.ptr.str, value, p.maxVal);
          p.ptr.str[p.maxVal] = '\0';
          g_config_dirty.store(true, std::memory_order_relaxed);
          sendTelnetMsgf(sock, "[OK] Set %s = '%s' (RAM only. Use 'save' to commit to NVS)\r\n", key, value);
        } else {
          sendTelnetMsgf(sock, "[ERROR] String exceeds maximum length of %lu characters.\r\n", (unsigned long)p.maxVal);
        }
        return;
      }
      case PARAM_PASS_HASH: {
        char hash_hex[68];
        System_Sha256ToHex(value, hash_hex);
        strncpy(g_config.telnet_pass_hash, hash_hex, sizeof(g_config.telnet_pass_hash) - 1);
        g_config.telnet_pass_hash[sizeof(g_config.telnet_pass_hash) - 1] = '\0';
        g_config_dirty.store(true, std::memory_order_relaxed);
        sendTelnetMsg(sock, "[OK] Telnet password updated & SHA-256 hashed. Use 'save' to commit to NVS.\r\n");
        return;
      }
      }
    }
  }

  sendTelnetMsgf(sock, "[ERROR] Unknown parameter '%s'. Type 'config ?' to list all valid parameters.\r\n", key);
}

void cmdConfig(EmbeddedCli *cli, char *args, void *context) {
  int sock = getSock(context);
  int argc = embeddedCliGetTokenCount(args);

  if (argc == 0) {
    printConfig(sock);
    return;
  }

  const char *sub = embeddedCliGetToken(args, 1);
  if (strcmp(sub, "?") == 0 || strcasecmp(sub, "help") == 0) {
    printConfigHelp(sock);
    return;
  }

  if (strcasecmp(sub, "set") == 0) {
    if (argc >= 3) {
      setConfig(context, embeddedCliGetToken(args, 2),
                embeddedCliGetToken(args, 3));
    } else {
      sendTelnetMsg(sock, "[ERROR] Usage: config set <key> <value>\r\n");
    }
    return;
  }

  if (strcasecmp(sub, "reset") == 0) {
    Config_ResetDefaults();
    sendTelnetMsg(sock, "[OK] Runtime configuration reset to system factory defaults. (RAM only. Use 'save' to commit)\r\n");
    return;
  }

  sendTelnetMsg(sock, "Usage: config [set <key> <value> | reset | ? | help]\r\n");
}

void cmdSave(EmbeddedCli *cli, char *args, void *context) {
  int sock = getSock(context);
  Config_Save();
  sendTelnetMsg(sock, "[OK] Configuration successfully committed and saved to NVS flash!\r\n");
}

void cmdEw11(EmbeddedCli *cli, char *args, void *context) {
  int sock = getSock(context);
  int argc = embeddedCliGetTokenCount(args);

  if (argc == 0 || (argc == 1 && strcasecmp(embeddedCliGetToken(args, 1), "list") == 0) ||
      (argc == 1 && strcasecmp(embeddedCliGetToken(args, 1), "status") == 0)) {
    static char s_ew11_status_buf[2048];
    AppendBuf out{s_ew11_status_buf, sizeof(s_ew11_status_buf)};
    out.append("\r\n");
    out.append(Fmt::DIV80EQ);
    out.append("                      CH5 EW11 TCP CLIENT SOCKET STATUS                         \r\n");
    out.append(Fmt::DIV80EQ);
    out.appendFormat("%-6s %-13s %-6s %-18s %-14s %s\r\n",
                     "Slot", "Name", "Port", "Client IP", "Status", "Packets(RX/TX)");
    out.append(Fmt::DIV80);

    {
      MutexLocker lock(g_ch5_mutex);
      for (int s = 0; s < Config::TCP::MAX_EW11_SLOTS; s++) {
        auto &slot = g_hub_slots[s];
        const char *status_str = !slot.enabled ? "Disabled"
                                 : !slot.is_connected ? "Listening"
                                 : (slot.rx_pkts == 0) ? "Idle" : "Connected";

        out.appendFormat("#%-5d %-13s %-6u %-18s %-14s %u / %u\r\n",
                         s, slot.name,
                         slot.target_port,
                         slot.is_connected ? (slot.target_ip[0] ? slot.target_ip : "Connected") : (slot.target_ip[0] ? slot.target_ip : "-"),
                         status_str,
                         static_cast<unsigned>(slot.rx_pkts),
                         static_cast<unsigned>(slot.tx_pkts));
      }
    }
    out.append(Fmt::DIV80);

    // ── FCU Modbus 실시간 상태 테이블 ──
    out.append("\r\n");
    out.append(Fmt::DIV80EQ);
    out.append("                    CH5 FCU MODBUS DEVICE STATUS\r\n");
    out.append(Fmt::DIV80EQ);
    out.appendFormat("%-6s %-7s %-6s %-16s %-6s %-6s %-6s %-6s %-5s %-5s %s\r\n",
                     "Slot", "Name", "Port", "Client IP",
                     "Power", "Mode", "Fan", "Swing", "Tgt", "Room", "Err");
    out.append(Fmt::DIV80);

    {
      MutexLocker lock(g_ch5_mutex);
      for (uint8_t s = 1; s < Config::TCP::MAX_EW11_SLOTS; ++s) {
        const HubClientSlot &slot = g_hub_slots[s];
        Fcu::SlotRuntime rt;
        Fcu::GetSlotRuntime(s, rt);

        const char *pwr_str = rt.snap.power ? "ON" : "OFF";
        const char *mode_str = (rt.snap.mode == Fcu::Mode::Cool) ? "Cool"
                             : (rt.snap.mode == Fcu::Mode::Heat) ? "Heat"
                             : (rt.snap.mode == Fcu::Mode::FanOnly) ? "Fan" : "-";
        const char *fan_str = (rt.snap.fan_speed == Fcu::FanSpeed::Off) ? "OFF"
                            : (rt.snap.fan_speed == Fcu::FanSpeed::Low) ? "Low"
                            : (rt.snap.fan_speed == Fcu::FanSpeed::Mid) ? "Mid"
                            : (rt.snap.fan_speed == Fcu::FanSpeed::High) ? "High"
                            : (rt.snap.fan_speed == Fcu::FanSpeed::Auto) ? "Auto" : "-";
        const char *swng_str = (rt.snap.swing == Fcu::Swing::On) ? "ON" : "OFF";
        const char *err_str = (rt.snap.error_code == 0) ? "OK" : "ERR";

        char tgt_str[8] = "-", room_str[8] = "-";
        if (rt.is_online) {
          snprintf(tgt_str, sizeof(tgt_str), "%u C", rt.snap.target_temp);
          snprintf(room_str, sizeof(room_str), "%u C", rt.snap.room_temp);
        }
        const char *ip_str = (slot.is_connected && slot.target_ip[0]) ? slot.target_ip : "-";
        out.appendFormat("#%-5u %-7s %-6u %-16s %-6s %-6s %-6s %-6s %-5s %-5s %s\r\n",
                         s, slot.name, slot.target_port, ip_str,
                         pwr_str, mode_str, fan_str, swng_str, tgt_str, room_str, err_str);
      }
    }
    out.append(Fmt::DIV80EQ);
    out.append("\r\n");

    sendTelnetMsgLen(sock, out.buf, out.offset);
    return;
  }

  const char *sub = embeddedCliGetToken(args, 1);

  if (strcasecmp(sub, "frame") == 0) {
    if (argc < 4) {
      sendTelnetMsg(sock, "[ERROR] Usage: ew11 frame <slot:0-4> <stx:hex> <etx:hex> [len:dec]\r\n");
      return;
    }
    int slot = atoi(embeddedCliGetToken(args, 2));
    if (slot < 0 || slot >= Config::TCP::MAX_EW11_SLOTS) {
      sendTelnetMsgf(sock, "[ERROR] Slot index must be 0 to %d\r\n", Config::TCP::MAX_EW11_SLOTS - 1);
      return;
    }
    uint8_t stx = static_cast<uint8_t>(strtoul(embeddedCliGetToken(args, 3), nullptr, 16));
    uint8_t etx = static_cast<uint8_t>(strtoul(embeddedCliGetToken(args, 4), nullptr, 16));
    uint8_t len = 0;
    if (argc >= 5) {
      len = static_cast<uint8_t>(atoi(embeddedCliGetToken(args, 5)));
    }
    char ns[16], tag[16];
    snprintf(ns, sizeof(ns), "e%d_frame", slot);
    snprintf(tag, sizeof(tag), "EW11_#%d", slot);
    g_hub_slots[slot].tracker.setFixedLock(stx, etx, len);
    g_hub_slots[slot].tracker.saveToNvs(ns, tag);
    sendTelnetMsgf(sock, "[OK] EW11 Slot #%d framing permanently fixed to STX 0x%02X, ETX 0x%02X, Len %u.\r\n",
                   slot, stx, etx, len);
    return;
  }

  if (strcasecmp(sub, "reset") == 0) {
    if (argc < 2) {
      sendTelnetMsg(sock, "[ERROR] Usage: ew11 reset <slot:0-4>\r\n");
      return;
    }
    int slot = atoi(embeddedCliGetToken(args, 2));
    if (slot < 0 || slot >= Config::TCP::MAX_EW11_SLOTS) {
      sendTelnetMsgf(sock, "[ERROR] Slot index must be 0 to %d\r\n", Config::TCP::MAX_EW11_SLOTS - 1);
      return;
    }
    char ns[16], tag[16];
    snprintf(ns, sizeof(ns), "e%d_frame", slot);
    snprintf(tag, sizeof(tag), "EW11_#%d", slot);
    g_hub_slots[slot].tracker.clearNvs(ns, tag);
    sendTelnetMsgf(sock, "[OK] EW11 Slot #%d framing tracker reset to autonomous auto-probing.\r\n", slot);
    return;
  }

  if (strcasecmp(sub, "set") == 0) {
    if (argc < 2) {
      sendTelnetMsg(sock, "[ERROR] Usage: ew11 set <slot:0-4> [port] [allowed_ip] [name] [enable:1/0]\r\n");
      return;
    }
    int slot = atoi(embeddedCliGetToken(args, 2));
    if (slot < 0 || slot >= Config::TCP::MAX_EW11_SLOTS) {
      sendTelnetMsgf(sock, "[ERROR] Slot index must be 0 to %d\r\n", Config::TCP::MAX_EW11_SLOTS - 1);
      return;
    }

    uint16_t default_port = Config::TCP::EW11_SLOT_PORTS[slot];
    uint16_t port = g_hub_slots[slot].target_port > 0 ? g_hub_slots[slot].target_port : default_port;
    const char *ip_str = nullptr;
    const char *name_str = nullptr;
    bool enabled = g_hub_slots[slot].enabled;

    if (argc >= 3) {
      const char *tok3 = embeddedCliGetToken(args, 3);
      if (strchr(tok3, '.') != nullptr) {
        ip_str = (strcmp(tok3, "-") == 0 || strcmp(tok3, "none") == 0) ? "" : tok3;
      } else {
        int p_val = atoi(tok3);
        if (p_val > 0 && p_val <= 65535) port = static_cast<uint16_t>(p_val);
      }
    }

    if (argc >= 4) {
      const char *tok4 = embeddedCliGetToken(args, 4);
      if (strchr(tok4, '.') != nullptr) {
        ip_str = (strcmp(tok4, "-") == 0 || strcmp(tok4, "none") == 0) ? "" : tok4;
      } else if (port == default_port && atoi(tok4) > 0) {
        port = static_cast<uint16_t>(atoi(tok4));
      } else {
        name_str = tok4;
      }
    }

    if (argc >= 5) {
      const char *tok5 = embeddedCliGetToken(args, 5);
      if (name_str == nullptr && !isdigit(tok5[0])) {
        name_str = tok5;
      } else if (isdigit(tok5[0])) {
        enabled = (atoi(tok5) != 0);
      }
    }

    if (argc >= 6) {
      enabled = (atoi(embeddedCliGetToken(args, 6)) != 0);
    }

    if (Hub_SetSlot(static_cast<uint8_t>(slot), enabled, ip_str, port, name_str)) {
      sendTelnetMsgf(sock, "[OK] EW11 Slot #%d configured (Name: %s, Listen Port: %u, Allowed IP: %s, Enabled: %s) and saved to NVS!\r\n",
                     slot, g_hub_slots[slot].name, g_hub_slots[slot].target_port,
                     g_hub_slots[slot].target_ip[0] ? g_hub_slots[slot].target_ip : "Any",
                     g_hub_slots[slot].enabled ? "true" : "false");
    } else {
      sendTelnetMsg(sock, "[ERROR] Failed to configure EW11 slot.\r\n");
    }
    return;
  }

  if (strcasecmp(sub, "enable") == 0 || strcasecmp(sub, "disable") == 0) {
    if (argc < 2) {
      sendTelnetMsgf(sock, "[ERROR] Usage: ew11 %s <slot:0-4>\r\n", sub);
      return;
    }
    int slot = atoi(embeddedCliGetToken(args, 2));
    if (slot < 0 || slot >= Config::TCP::MAX_EW11_SLOTS) {
      sendTelnetMsgf(sock, "[ERROR] Slot index must be 0 to %d\r\n", Config::TCP::MAX_EW11_SLOTS - 1);
      return;
    }
    bool enable = (strcasecmp(sub, "enable") == 0);
    {
      MutexLocker lock(g_ch5_mutex);
      g_hub_slots[slot].enabled = enable;
      if (!enable && g_hub_slots[slot].sock >= 0) {
        close(g_hub_slots[slot].sock);
        g_hub_slots[slot].sock = -1;
        g_hub_slots[slot].is_connected = false;
        g_hub_slots[slot].rx_len = 0;
      }
    }
    Hub_SaveConfig();
    sendTelnetMsgf(sock, "[OK] EW11 Slot #%d %s and saved to NVS flash.\r\n", slot, enable ? "ENABLED" : "DISABLED");
    return;
  }

  sendTelnetMsg(sock, "Usage: ew11 [list | set <slot> [port] [allowed_ip] [name] [enable] | frame <slot> <stx> <etx> [len] | reset <slot> | enable <slot> | disable <slot>]\r\n");
}

void cmdRoutes(EmbeddedCli *cli, char *args, void *context) {
  int sock = getSock(context);
  int argc = embeddedCliGetTokenCount(args);

  if (argc == 1 && strcasecmp(embeddedCliGetToken(args, 1), "clear") == 0) {
    g_route_registry.clear();
    sendTelnetMsg(sock, "[OK] Dynamic device ingress routing table cleared.\r\n");
    return;
  }

  static DeviceRouteEntry entries[DeviceRouteRegistry::MAX_ROUTES];
  size_t count = g_route_registry.getRoutes(entries, DeviceRouteRegistry::MAX_ROUTES);

  g_cli_scratch_buf[0] = '\0';
  AppendBuf out{g_cli_scratch_buf, sizeof(g_cli_scratch_buf)};
  out.append("\r\n");
  out.append(Fmt::DIV80EQ);
  out.append("                 DYNAMIC DEVICE INGRESS ROUTING TABLE (Zero Hardcode)         \r\n");
  out.append(Fmt::DIV80EQ);
  out.appendFormat("%-22s %-25s %s\r\n", "Target [DevID:Sub1:Sub2]", "Egress Destination", "Last Seen");
  out.append(Fmt::DIV80);

  if (count == 0) {
    out.append("  (No device routes learned yet. Waiting for bus/EW11 packets...)\r\n");
  } else {
    uint32_t now = millis();
    for (size_t i = 0; i < count; i++) {
      const auto &e = entries[i];
      char tgt_str[24];
      snprintf(tgt_str, sizeof(tgt_str), "[0x%02X:%02X:%02X]", e.dev_id, e.sub1, e.sub2);

      char dst_str[32];
      if (e.endpoint.channel_id == 5 && e.endpoint.slot_idx >= 0) {
        snprintf(dst_str, sizeof(dst_str), "CH#5 Slot %d", e.endpoint.slot_idx);
      } else {
        snprintf(dst_str, sizeof(dst_str), "CH#%u", e.endpoint.channel_id);
      }

      char el_str[20];
      Fmt::FormatElapsed(now, e.endpoint.last_seen_ms, el_str, sizeof(el_str));

      out.appendFormat("  %-20s -> %-23s (%s ago)\r\n", tgt_str, dst_str, el_str);
    }
  }

  out.append(Fmt::DIV80EQ);
  out.append("\r\n");
  sendTelnetMsgLen(sock, out.buf, out.offset);
}

} // namespace ConfigCli

// ============================================================================
// From src/CLI/CliStatus.cpp
// ============================================================================

namespace WallpadCli {

void wallpadPrintStatus(AppendBuf &out) {
  auto *active = WallpadParserFactory::getActiveParser();
  auto desc = g_auto_probing_engine.getDescriptor();
  VendorProfileDescriptor active_prof;
  bool is_manual_prof = false;
  if (ProfileRepository::getActiveProfile(active_prof) && strcasecmp(active_prof.key, "auto") != 0) {
    is_manual_prof = true;
    desc.stx = active_prof.stx;
    desc.etx = active_prof.etx;
    desc.checksum_algo = active_prof.cs_algo;
    desc.opcode_offset = active_prof.opcode_offset;
    desc.query_opcode = active_prof.query_op;
    desc.control_opcode = active_prof.ctrl_op;
    desc.ack_opcode = active_prof.ack_op;
    desc.control_seen = (active_prof.ctrl_op != 0);
    desc.dev_id_offset = active_prof.dev_id_offset;
    desc.sub1_offset = active_prof.sub1_offset;
    desc.sub2_offset = active_prof.sub2_offset;
    desc.is_swapped_addr = (active_prof.is_swapped_addr != 0);
    desc.is_locked = true;
    desc.opcodes_locked = true;
    desc.offsets_locked = true;
    desc.payload_offset = std::max({active_prof.opcode_offset, active_prof.dev_id_offset,
                                    active_prof.sub1_offset, active_prof.sub2_offset}) + 1;
  }

  size_t active_targets = g_polling_targets.activeCount();
  size_t verified_targets = g_polling_targets.verifiedCount();
  size_t online_devs = g_device_repo.getOnlineCount();

  const char *phase_str = "Phase 1/3 (Framing Probing)";
  if (is_manual_prof || desc.offsets_locked) {
    phase_str = "Phase 3/3: Fully Locked";
  } else if (desc.is_locked) {
    phase_str = "Phase 2/3: Cache Syncing";
  }

  out.append("\r\n");
  out.append(Fmt::DIV80EQ);
  out.append("                    WALLPAD PROTOCOL & PROFILE DIAGNOSTICS                   \r\n");
  out.append(Fmt::DIV80EQ);
  char prof_key_buf[UniversalProtocolEngine::kProfileKeyMaxLen] = "Standard";
  if (active) {
    active->getActiveProfileKey(prof_key_buf, sizeof(prof_key_buf));
  }
  out.appendFormat("Active Profile  : %s (ID: %u)\r\n",
                   prof_key_buf,
                   static_cast<unsigned>(g_config.wallpad_profile));
  if (g_config.wallpad_profile == static_cast<uint8_t>(WallpadProfileIndex::ADAPTIVE)) {
    out.appendFormat("Profile Mode    : Auto Adaptive [%s]\r\n", phase_str);
  } else {
    out.appendFormat("Profile Mode    : Manual Fixed\r\n");
  }
  const auto *matched_p = ProfileMatcher::getActiveProfile();
  if (matched_p) {
    out.appendFormat("Catalog Match   : %s (%u Devices Spec Injected)\r\n",
                     matched_p->vendor_name, static_cast<unsigned>(matched_p->device_count));
  } else {
    out.append("Catalog Match   : None (Generic Framing Only)\r\n");
  }
  out.appendFormat("Devices Tracked : %u Active / %u Online (%u Offline) [%s]\r\n",
                   static_cast<unsigned>(active_targets), static_cast<unsigned>(online_devs),
                   static_cast<unsigned>(g_device_repo.count() >= online_devs ? (g_device_repo.count() - online_devs) : 0),
                   (online_devs >= active_targets && active_targets > 0) ? "100% Synced" : "Syncing");
  out.append(Fmt::DIV80);
  out.append("Packet Field    Parameter       Value / Layout Rule                       Status\r\n");
  out.append(Fmt::DIV80);

  char stx_buf[16], etx_buf[16], len_buf[32], pkt_len_buf[32];
  snprintf(stx_buf, sizeof(stx_buf), "%02X", active ? active->getStx() : 0xF7);
  snprintf(etx_buf, sizeof(etx_buf), "%02X", active ? active->getEtx() : 0xEE);
  snprintf(len_buf, sizeof(len_buf), "Byte #1");

  size_t total_tgts = g_polling_targets.totalCount();

  uint8_t q_lens[8], ack_lens[8];
  size_t q_len_cnt = 0, ack_len_cnt = 0;

  auto add_unique_len = [](uint8_t *arr, size_t &cnt, uint8_t len) {
    if (len >= 3 && len <= 64 && cnt < 8) {
      if (std::find(arr, arr + cnt, len) == arr + cnt) {
        arr[cnt++] = len;
      }
    }
  };

  constexpr uint8_t CH23_MASK = (1 << 2) | (1 << 3);
  for (size_t i = 0; i < total_tgts; ++i) {
    PollingTargetEntry entry;
    if (g_polling_targets.getEntry(i, entry)) {
      if ((entry.source_channels & CH23_MASK) == 0) continue;
      if (entry.raw_query_len > 0) add_unique_len(q_lens, q_len_cnt, entry.raw_query_len);
      if (entry.raw_ack_len > 0) add_unique_len(ack_lens, ack_len_cnt, entry.raw_ack_len);
    }
  }
  std::sort(q_lens, q_lens + q_len_cnt);
  std::sort(ack_lens, ack_lens + ack_len_cnt);

  auto format_lens = [](const uint8_t *arr, size_t cnt, char *out, size_t out_sz, const char *fallback) {
    if (cnt == 0) {
      snprintf(out, out_sz, "%s", fallback);
      return;
    }
    size_t off = 0;
    for (size_t i = 0; i < cnt; ++i) {
      off += snprintf(out + off, out_sz - off, "%s%u", (i == 0 ? "" : ", "), arr[i]);
    }
    snprintf(out + off, out_sz - off, " Byte");
  };

  char def_len_buf[16];
  if (desc.learned_query_len > 0) {
    snprintf(def_len_buf, sizeof(def_len_buf), "%u Byte", desc.learned_query_len);
  } else {
    snprintf(def_len_buf, sizeof(def_len_buf), "Waiting");
  }

  char q_str[32], ack_str[32];
  format_lens(q_lens, q_len_cnt, q_str, sizeof(q_str), def_len_buf);
  format_lens(ack_lens, ack_len_cnt, ack_str, sizeof(ack_str), def_len_buf);

  char len_prefix[16];
  if (desc.has_len_field) {
    snprintf(len_prefix, sizeof(len_prefix), "Byte #%u", desc.len_offset);
  } else {
    snprintf(len_prefix, sizeof(len_prefix), desc.is_locked ? "Fixed" : "Waiting");
  }

  char q_val[48], cmd_val[48], ack_val[48];
  snprintf(q_val, sizeof(q_val), "%s : %s", len_prefix, q_str);
  if (desc.ctrl_len_cnt > 0) {
    char cmd_lens_str[32];
    format_lens(desc.learned_ctrl_lens, desc.ctrl_len_cnt, cmd_lens_str, sizeof(cmd_lens_str), def_len_buf);
    snprintf(cmd_val, sizeof(cmd_val), "%s : %s", len_prefix, cmd_lens_str);
  } else {
    snprintf(cmd_val, sizeof(cmd_val), "%s : Waiting", len_prefix);
  }
  snprintf(ack_val, sizeof(ack_val), "%s : %s", len_prefix, ack_str);

  const char *len_status = desc.is_locked ? "[LOCKED]" : "[LEARNING]";
  const char *cmd_status = (desc.control_seen && desc.ctrl_len_cnt > 0) ? "[LOCKED]" : "[WAITING]";

  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "Header", "[ST] STX", stx_buf, len_status);
  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "", "[LN] Query", q_val, len_status);
  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "", "[LN] Command", cmd_val, cmd_status);
  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "", "[LN] Response", ack_val, len_status);
  out.append(Fmt::DIV80);

  uint8_t dev_ids[16], sub1_ids[16], sub2_ids[16];
  size_t dev_id_cnt = 0, sub1_cnt = 0, sub2_cnt = 0;

  for (size_t i = 0; i < total_tgts; ++i) {
    PollingTargetEntry entry;
    if (g_polling_targets.getEntry(i, entry)) {
      if ((entry.source_channels & CH23_MASK) == 0) continue;
      if (dev_id_cnt < 16 && std::find(dev_ids, dev_ids + dev_id_cnt, entry.dev_id) == dev_ids + dev_id_cnt) {
        dev_ids[dev_id_cnt++] = entry.dev_id;
      }
      if (sub1_cnt < 16 && std::find(sub1_ids, sub1_ids + sub1_cnt, entry.sub1) == sub1_ids + sub1_cnt) {
        sub1_ids[sub1_cnt++] = entry.sub1;
      }
      if (sub2_cnt < 16 && std::find(sub2_ids, sub2_ids + sub2_cnt, entry.sub2) == sub2_ids + sub2_cnt) {
        sub2_ids[sub2_cnt++] = entry.sub2;
      }
    }
  }

  std::sort(dev_ids, dev_ids + dev_id_cnt);
  std::sort(sub1_ids, sub1_ids + sub1_cnt);
  std::sort(sub2_ids, sub2_ids + sub2_cnt);

  auto format_hex_list = [](const uint8_t *arr, size_t cnt, const char *prefix, char *out, size_t out_sz) {
    if (cnt == 0) {
      snprintf(out, out_sz, "%s", prefix);
      return;
    }
    char hex_str[64] = {0};
    size_t off = 0;
    for (size_t d = 0; d < cnt; ++d) {
      if (off + 5 >= 24) {
        off += snprintf(hex_str + off, sizeof(hex_str) - off, ", ..");
        break;
      }
      off += snprintf(hex_str + off, sizeof(hex_str) - off, "%s%02X",
                      (d == 0 ? "" : ", "), arr[d]);
    }
    snprintf(out, out_sz, "%s : %s", prefix, hex_str);
  };

  char addr_mode_buf[48];
  if (desc.offsets_locked) {
    if (desc.is_swapped_addr) {
      snprintf(addr_mode_buf, sizeof(addr_mode_buf), "Swapped (GW:Byte#%u <-> ID:Byte#%u)",
               desc.gw_addr_offset, desc.dev_id_offset);
    } else {
      snprintf(addr_mode_buf, sizeof(addr_mode_buf), "Direct (Single Address)");
    }
  } else {
    snprintf(addr_mode_buf, sizeof(addr_mode_buf), "%s", desc.is_locked ? "Probing..." : "Waiting");
  }
  const char *addr_status = desc.offsets_locked ? "[LOCKED]" : (desc.is_locked ? "[LEARNING]" : "[WAITING]");

  char gw_val_buf[48];
  if (desc.offsets_locked) {
    if (desc.gw_addr_offset != 0xFF) {
      snprintf(gw_val_buf, sizeof(gw_val_buf), "Byte #%u : %02X", desc.gw_addr_offset, desc.gw_addr);
    } else {
      snprintf(gw_val_buf, sizeof(gw_val_buf), "Val: %02X", desc.gw_addr);
    }
  } else {
    snprintf(gw_val_buf, sizeof(gw_val_buf), "-");
  }

  char dev_off_label[32], sub1_off_label[32], sub2_off_label[32];
  if (desc.offsets_locked) {
    snprintf(dev_off_label, sizeof(dev_off_label), "Byte #%u", desc.dev_id_offset);
    snprintf(sub1_off_label, sizeof(sub1_off_label), "Byte #%u", desc.sub1_offset);
    snprintf(sub2_off_label, sizeof(sub2_off_label), "Byte #%u", desc.sub2_offset);
  } else {
    snprintf(dev_off_label, sizeof(dev_off_label), "Probing...");
    snprintf(sub1_off_label, sizeof(sub1_off_label), "Probing...");
    snprintf(sub2_off_label, sizeof(sub2_off_label), "Probing...");
  }

  char dev_list_buf[64], sub1_list_buf[64], sub2_list_buf[64];
  format_hex_list(dev_ids, dev_id_cnt, dev_off_label, dev_list_buf, sizeof(dev_list_buf));
  format_hex_list(sub1_ids, sub1_cnt, sub1_off_label, sub1_list_buf, sizeof(sub1_list_buf));
  format_hex_list(sub2_ids, sub2_cnt, sub2_off_label, sub2_list_buf, sizeof(sub2_list_buf));

  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "Addressing", "Addr Mode", addr_mode_buf, addr_status);
  if (desc.offsets_locked) {
    out.appendFormat("%-16s%-16s%-38s%10s\r\n", "", "[GW] Master", gw_val_buf, addr_status);
  }
  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "", "[ID] Device", dev_list_buf, addr_status);
  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "", "[S1] Sub Addr", sub1_list_buf, addr_status);
  if (desc.sub2_offset != 0xFF && desc.sub2_offset != desc.sub1_offset && sub2_cnt > 0) {
    out.appendFormat("%-16s%-16s%-38s%10s\r\n", "", "[S2] Sub Addr", sub2_list_buf, addr_status);
  }
  out.append(Fmt::DIV80);

  char op_line_buf[48];
  char ctl_hex[8];
  if (desc.control_seen && desc.control_opcode != 0) {
    snprintf(ctl_hex, sizeof(ctl_hex), "%02X", desc.control_opcode);
  } else {
    snprintf(ctl_hex, sizeof(ctl_hex), "??");
  }
  snprintf(op_line_buf, sizeof(op_line_buf), "Byte #%u : QRY:%02X, CTL:%s, ACK:%02X",
           desc.opcode_offset, desc.query_opcode, ctl_hex, desc.ack_opcode);

  const char *opcode_status;
  if (!desc.opcodes_locked) {
    opcode_status = "[LEARNING]";
  } else if (!desc.control_seen || desc.control_opcode == 0) {
    opcode_status = "[WAITING]";        // QRY+ACK 확정, CTL은 아직 미관측(대기)
  } else {
    opcode_status = "[LOCKED]";         // QRY+CTL+ACK 모두 확정
  }

  char seq_line_buf[32];
  if (desc.has_seq_counter) {
    snprintf(seq_line_buf, sizeof(seq_line_buf), "Byte #%u : +1 Counter", desc.seq_offset);
  } else {
    snprintf(seq_line_buf, sizeof(seq_line_buf), desc.offsets_locked ? "-" : "None");
  }
  const char *seq_status = desc.offsets_locked ? (desc.has_seq_counter ? "[LOCKED]" : "[UNUSED]") : "[WAITING]";

  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "Command", "[OP] Opcode", op_line_buf, opcode_status);
  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "", "Sequence", seq_line_buf, seq_status);
  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "", "[CX] Context", sub1_list_buf, addr_status);
  out.append(Fmt::DIV80);

  char payload_range_buf[48];
  char payload_len_buf[32];
  if (desc.offsets_locked) {
    snprintf(payload_range_buf, sizeof(payload_range_buf), "Byte #%u ~ #[N-3]", desc.payload_offset);
    snprintf(payload_len_buf, sizeof(payload_len_buf), "Data = [LEN - %u] Byte", desc.payload_offset + 2);
  } else {
    snprintf(payload_range_buf, sizeof(payload_range_buf), "Byte #7 ~ #[N-3] : Est");
    snprintf(payload_len_buf, sizeof(payload_len_buf), "Data = [LEN - 9] Byte : Est");
  }
  const char *payload_status = desc.offsets_locked ? "[LOCKED]" : "[ESTIMATE]";
  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "Payload", "[PL] Data Range", payload_range_buf, payload_status);
  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "", "[PL] Length", payload_len_buf, payload_status);
  out.append(Fmt::DIV80);

  char cs_algo_buf[48];
  snprintf(cs_algo_buf, sizeof(cs_algo_buf), "Byte #[N-2] : %s", AutoProbingEngine::getAlgoName(desc.checksum_algo));
  char etx_line_buf[32];
  snprintf(etx_line_buf, sizeof(etx_line_buf), "Byte #[N-1] : %s", etx_buf);

  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "Tail", "[CS] Checksum", cs_algo_buf, desc.is_locked ? "[LOCKED]" : "[LEARNING]");
  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "", "[ET] ETX", etx_line_buf, desc.is_locked ? "[LOCKED]" : "[LEARNING]");
  out.append(Fmt::DIV80);

  uint32_t b1 = g_config.uart_baud_rate;
  uint32_t b2 = g_config.ch2_baud_rate;
  uint32_t b3 = g_config.ch3_baud_rate;

  char baud_buf[48];
  if (b1 == b2 && b2 == b3) {
    snprintf(baud_buf, sizeof(baud_buf), "%u bps : CH1~3", static_cast<unsigned>(b1));
    out.appendFormat("%-16s%-16s%-38s%10s\r\n", "Bus Physical", "Baudrate", baud_buf, "[CONFIG]");
  } else {
    snprintf(baud_buf, sizeof(baud_buf), "CH1:%u, CH2:%u, CH3:%u",
             static_cast<unsigned>(b1), static_cast<unsigned>(b2), static_cast<unsigned>(b3));
    out.appendFormat("%-16s%-16s%-38s%10s\r\n", "Bus Physical", "Baudrate", baud_buf, "[CONFIG]");
  }
  char ipg_silence_buf[48];
  snprintf(ipg_silence_buf, sizeof(ipg_silence_buf), "%u ms : CH1~3",
           static_cast<unsigned>(Config::Timing::WALLPAD_AUTO_IPG_MS));
  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "", "IPG Silence", ipg_silence_buf, "[CONFIG]");
  out.append(Fmt::DIV80);

  Config::Doorphone::FramingStatus dp_status = g_doorphone_tracker.status.load(std::memory_order_relaxed);
  const char *dp_status_str = (dp_status == Config::Doorphone::FramingStatus::LOCKED)   ? "[LOCKED]"
                            : (dp_status == Config::Doorphone::FramingStatus::LEARNING) ? "[LEARNING]"
                            : (dp_status == Config::Doorphone::FramingStatus::NOISY)    ? "[NOISY]"
                                                                                        : "[WAITING]";

  char dp_frame_buf[48];
  if (dp_status == Config::Doorphone::FramingStatus::WAITING) {
    snprintf(dp_frame_buf, sizeof(dp_frame_buf), "-- .. --");
  } else {
    uint8_t dp_stx = g_doorphone_tracker.candidate_stx.load(std::memory_order_relaxed);
    uint8_t dp_etx = g_doorphone_tracker.candidate_etx.load(std::memory_order_relaxed);
    uint8_t dp_len = g_doorphone_tracker.candidate_len.load(std::memory_order_relaxed);
    if (dp_len > 0) {
      snprintf(dp_frame_buf, sizeof(dp_frame_buf), "%02X .. %02X (%u Bytes)", dp_stx, dp_etx, dp_len);
    } else {
      snprintf(dp_frame_buf, sizeof(dp_frame_buf), "%02X .. %02X", dp_stx, dp_etx);
    }
  }

  char dp_baud_buf[32];
  snprintf(dp_baud_buf, sizeof(dp_baud_buf), "%u bps", static_cast<unsigned>(g_config.doorphone_baud_rate));
  char dp_ipg_buf[32];
  snprintf(dp_ipg_buf, sizeof(dp_ipg_buf), "%u ms", static_cast<unsigned>(Config::Timing::DOORPHONE_IPG_MS));
  char dp_debounce_buf[32];
  snprintf(dp_debounce_buf, sizeof(dp_debounce_buf), "%u ms", static_cast<unsigned>(Config::Timing::DOORPHONE_DEBOUNCE_MS));

  uint8_t cur_dp_stx = g_doorphone_tracker.candidate_stx.load(std::memory_order_relaxed);
  uint8_t cur_dp_etx = g_doorphone_tracker.candidate_etx.load(std::memory_order_relaxed);
  uint8_t cur_dp_len = g_doorphone_tracker.candidate_len.load(std::memory_order_relaxed);
  const DoorphoneSpec *dp_prof =
      ProfileMatcher::matchDoorphone(cur_dp_stx, cur_dp_etx, cur_dp_len);

  char dp_match_buf[48];
  char dp_ops_f_buf[64];
  char dp_ops_l_buf[64];
  const char *dp_match_status = "[WAITING]";
  const char *dp_ops_status = "[WAITING]";

  if (dp_prof) {
    snprintf(dp_match_buf, sizeof(dp_match_buf), "%s", dp_prof->desc);
    dp_match_status = (dp_status == Config::Doorphone::FramingStatus::LOCKED) ? "[LOCKED]" : "[LEARNING]";
    dp_ops_status = dp_match_status;
    snprintf(dp_ops_f_buf, sizeof(dp_ops_f_buf), "Bell:%02X, Call:%02X, Open:%02X, End:%02X",
             dp_prof->bell_front, dp_prof->call_front, dp_prof->open_front, dp_prof->end_front);
    snprintf(dp_ops_l_buf, sizeof(dp_ops_l_buf), "Bell:%02X, Call:%02X, Open:%02X, End:%02X",
             dp_prof->bell_lobby, dp_prof->call_lobby, dp_prof->open_lobby, dp_prof->end_lobby);
  } else if (dp_status == Config::Doorphone::FramingStatus::WAITING) {
    snprintf(dp_match_buf, sizeof(dp_match_buf), "Waiting for traffic...");
    snprintf(dp_ops_f_buf, sizeof(dp_ops_f_buf), "Waiting...");
    snprintf(dp_ops_l_buf, sizeof(dp_ops_l_buf), "Waiting...");
    dp_match_status = "[WAITING]";
    dp_ops_status = "[WAITING]";
  } else {
    snprintf(dp_match_buf, sizeof(dp_match_buf), "No Catalog Match");
    snprintf(dp_ops_f_buf, sizeof(dp_ops_f_buf), "Bell:B5, Call:B9, Open:B4, End:B8");
    snprintf(dp_ops_l_buf, sizeof(dp_ops_l_buf), "Bell:5A, Call:5F, Open:61, End:60");
    dp_match_status = "[UNKNOWN]";
    dp_ops_status = "[UNKNOWN]";
  }

  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "Doorphone (CH4)", "Framing", dp_frame_buf, dp_status_str);
  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "", "Catalog Match", dp_match_buf, dp_match_status);
  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "", "Opcodes(F)", dp_ops_f_buf, dp_ops_status);
  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "", "Opcodes(L)", dp_ops_l_buf, dp_ops_status);
  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "", "Baudrate", dp_baud_buf, "[CONFIG]");
  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "", "Time-gap", dp_ipg_buf, "[CONFIG]");
  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "", "Debounce", dp_debounce_buf, "[CONFIG]");
  out.append(Fmt::DIV80);

  {
    MutexLocker lock(g_ch5_mutex);
    bool first_ew11 = true;
    for (int s = 0; s < Config::TCP::MAX_EW11_SLOTS; s++) {
      auto &slot = g_hub_slots[s];
      uint16_t listen_port = slot.target_port ? slot.target_port : Config::TCP::EW11_SLOT_PORTS[s];
      char port_title[24];
      snprintf(port_title, sizeof(port_title), "%u", listen_port);

      char slot_detail[48];
      const char *slot_status_str = "[UNUSED]";

      if (!slot.enabled && !slot.is_connected && slot.target_ip[0] == '\0') {
        snprintf(slot_detail, sizeof(slot_detail), "Disabled");
        slot_status_str = "[UNUSED]";
      } else {
        const char *ip_str = slot.target_ip[0] ? slot.target_ip : "-";
        if (slot.is_connected) {
          snprintf(slot_detail, sizeof(slot_detail), "Connect: %s", ip_str);
          slot_status_str = "[ACTIVE]";
        } else {
          snprintf(slot_detail, sizeof(slot_detail), "Listening");
          slot_status_str = "[WAITING]";
        }
      }

      out.appendFormat("%-16s%-16s%-38s%10s\r\n",
                       first_ew11 ? "EW11 (CH5)" : "",
                       port_title, slot_detail, slot_status_str);
      first_ew11 = false;
    }
  }
  out.append(Fmt::DIV80);

  char conv_buf[48];
  uint32_t conv_pct = active_targets ? (verified_targets * 100 / active_targets) : 0;
  snprintf(conv_buf, sizeof(conv_buf), "%u / %u Targets (%u%%)",
           static_cast<unsigned>(verified_targets), static_cast<unsigned>(active_targets),
           static_cast<unsigned>(conv_pct));
  const char *conv_status = (verified_targets >= active_targets && active_targets > 0)
                                ? "[SYNCED]"
                                : (desc.is_locked ? "[SYNCING]" : "[WAITING]");

  char cs_rate_buf[48];
  uint32_t cs_pct = desc.tested_packets ? (desc.matched_packets * 100 / desc.tested_packets) : 100;
  auto format_compact = [](char *buf, size_t sz, uint32_t count) {
    if (count >= 1000000) {
      snprintf(buf, sz, "%.1fM", count / 1000000.0);
    } else if (count >= 1000) {
      snprintf(buf, sz, "%.1fk", count / 1000.0);
    } else {
      snprintf(buf, sz, "%u", static_cast<unsigned>(count));
    }
  };
  char m_str[16], t_str[16];
  format_compact(m_str, sizeof(m_str), desc.matched_packets);
  format_compact(t_str, sizeof(t_str), desc.tested_packets);

  snprintf(cs_rate_buf, sizeof(cs_rate_buf), "%s / %s Packets (%u%%)",
           m_str, t_str, static_cast<unsigned>(cs_pct));
  const char *cs_status = (cs_pct >= 95) ? "[STABLE]" : (cs_pct >= 80 ? "[NOISY]" : "[ERROR]");

  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "Runtime Sync", "Cache Sync", conv_buf, conv_status);
  out.appendFormat("%-16s%-16s%-38s%10s\r\n", "", "CS Validation", cs_rate_buf, cs_status);
  out.append(Fmt::DIV80EQ);
  out.append("\r\n");
}

void wallpadListProfiles(AppendBuf &out) {
  out.append("\r\n");
  out.append(Fmt::DIV80EQ);
  out.append("                        WALLPAD PROTOCOL PROFILES                               \r\n");
  out.append(Fmt::DIV80EQ);
  out.append(" ID  | Profile Key | Protocol Specification / Description | Status\r\n");
  out.append(Fmt::DIV80);
  for (size_t i = 0; i < ProfileRepository::getProfileCount(); ++i) {
    VendorProfileDescriptor p_desc;
    if (ProfileRepository::getProfile(i, p_desc)) {
      bool is_current = (g_config.wallpad_profile == i);
      bool is_empty = (i > 0 && strncmp(p_desc.name, "[Empty", 6) == 0);
      out.appendFormat(" %2u  | %-11s | %-36s | %s\r\n",
                       static_cast<unsigned>(i), p_desc.key, p_desc.name,
                       is_current ? ">> ACTIVE <<" : (is_empty ? "Available" : "Saved (NVS)"));
    }
  }
  out.append(Fmt::DIV80EQ);
  out.append("\r\n");
}

void wallpadSaveProfile(int sock, const char *name) {
  if (!name || strlen(name) == 0) {
    sendTelnetMsg(sock, "[ERROR] Usage: wallpad save <profile_name> (e.g. 'wallpad save MyHome')\r\n");
    return;
  }
  size_t saved_slot = 0;
  if (ProfileRepository::saveCurrentAutoAs(name, saved_slot)) {
    sendTelnetMsgf(sock, "[OK] Successfully saved current Auto profile as '%s' (Slot #%u) in NVS!\r\n",
                   name, static_cast<unsigned>(saved_slot));
  } else {
    sendTelnetMsg(sock, "[ERROR] Failed to save profile to NVS.\r\n");
  }
}

void wallpadDeleteProfile(int sock, const char *target) {
  if (!target) {
    sendTelnetMsg(sock, "[ERROR] Usage: wallpad delete <name|id>\r\n");
    return;
  }
  char *endp = nullptr;
  long val = strtol(target, &endp, 10);
  size_t idx = 999;
  if (endp != target && *endp == '\0' && val >= 1 && val < static_cast<long>(ProfileRepository::getProfileCount())) {
    idx = static_cast<size_t>(val);
  } else {
    VendorProfileDescriptor pd;
    for (size_t i = 1; i < ProfileRepository::getProfileCount(); ++i) {
      if (ProfileRepository::getProfile(i, pd) && strcasecmp(pd.key, target) == 0) {
        idx = i;
        break;
      }
    }
  }
  if (idx >= 1 && idx < ProfileRepository::getProfileCount()) {
    ProfileRepository::deleteProfile(idx);
    sendTelnetMsgf(sock, "[OK] Custom profile (Slot #%u) reset to empty.\r\n", static_cast<unsigned>(idx));
  } else {
    sendTelnetMsgf(sock, "[ERROR] Cannot delete '%s' (Slot 0 is protected Auto slot).\r\n", target);
  }
}

void wallpadSetProfile(int sock, const char *key) {
  if (!key) {
    sendTelnetMsg(sock, "[ERROR] Usage: wallpad set <key|id>\r\n");
    return;
  }
  bool ok = false;
  char *endp = nullptr;
  long val = strtol(key, &endp, 10);
  if (endp != key && *endp == '\0' && val >= 0 &&
      val < static_cast<long>(ProfileRepository::getProfileCount())) {
    ok = ProfileRepository::setActiveProfileIndex(static_cast<size_t>(val));
  } else {
    ok = ProfileRepository::setActiveProfileByKey(key);
  }

  if (ok) {
    auto *new_p = WallpadParserFactory::getActiveParser();
    char v_name[UniversalProtocolEngine::kVendorNameMaxLen] = {0};
    char p_key[UniversalProtocolEngine::kProfileKeyMaxLen] = {0};
    if (new_p) {
      new_p->getVendorName(v_name, sizeof(v_name));
      new_p->getActiveProfileKey(p_key, sizeof(p_key));
    }
    sendTelnetMsgf(sock,
                   "[OK] Wallpad profile changed to '%s' (%s) and saved to NVS.\r\n",
                   v_name[0] ? v_name : key,
                   p_key[0] ? p_key : key);
  } else {
    sendTelnetMsgf(sock,
                   "[ERROR] Unknown vendor profile '%s'. Use 'wallpad list' to see available profiles.\r\n",
                   key);
  }
}

} // namespace WallpadCli

// ============================================================================
// From src/CLI/CliControl.cpp
// ============================================================================

namespace WallpadCli {

void cmdTrace(EmbeddedCli *cli, char *args, void *context) {
  int sock = getSock(context);
  int token_count = embeddedCliGetTokenCount(args);
  const char *sub = (token_count > 0) ? embeddedCliGetToken(args, 1) : "on";

  if (strcasecmp(sub, "off") == 0) {
    g_telnet_tracer.setTrace(false);
    sendTelnetMsg(sock, "Packet trace DISABLED.\r\n");
    return;
  }

  g_telnet_tracer.setClient(sock);
  g_telnet_tracer.setTrace(true);

  if (strcasecmp(sub, "on") == 0) {
    g_telnet_tracer.setFilter(TraceType::ALL);
    sendTelnetMsg(sock, "Packet trace ENABLED: ALL packets.\r\n");
  } else if (strcasecmp(sub, "ctl") == 0) {
    g_telnet_tracer.setFilter(TraceType::CTL);
    sendTelnetMsg(sock, "Packet trace ENABLED: CONTROL packets only.\r\n");
  } else if (strcasecmp(sub, "ack") == 0) {
    g_telnet_tracer.setFilter(TraceType::ACK);
    sendTelnetMsg(sock, "Packet trace ENABLED: ACK/Response packets only.\r\n");
  } else if (strcasecmp(sub, "pol") == 0) {
    g_telnet_tracer.setFilter(TraceType::QRY);
    sendTelnetMsg(sock, "Packet trace ENABLED: Polling queries only.\r\n");
  } else if (strcasecmp(sub, "rmt") == 0) {
    g_telnet_tracer.setFilter(TraceType::RMT);
    sendTelnetMsg(sock, "Packet trace ENABLED: Doorphone packets only.\r\n");
  } else if (strcasecmp(sub, "drp") == 0) {
    g_telnet_tracer.setFilter(TraceType::DRP);
    sendTelnetMsg(sock, "Packet trace ENABLED: Dropped packets only.\r\n");
  } else if (strcasecmp(sub, "ch") == 0 || (strncasecmp(sub, "ch", 2) == 0 && isdigit(static_cast<unsigned char>(sub[2])))) {
    uint8_t ch = 0;
    if (strcasecmp(sub, "ch") == 0 && token_count >= 2) {
      ch = static_cast<uint8_t>(atoi(embeddedCliGetToken(args, 2)));
    } else if (strncasecmp(sub, "ch", 2) == 0 && isdigit(static_cast<unsigned char>(sub[2]))) {
      ch = static_cast<uint8_t>(sub[2] - '0');
    }
    if (ch >= 1 && ch <= 6) {
      g_telnet_tracer.setFilter(TraceType::CH, ch);
      sendTelnetMsgf(sock, "Packet trace ENABLED: Channel %u only.\r\n", ch);
    } else {
      sendTelnetMsg(sock, "[ERROR] Usage: trace ch <1-6>\r\n");
    }
  } else if (strcasecmp(sub, "devid") == 0 || strncasecmp(sub, "0x", 2) == 0) {
    uint8_t id = 0;
    if (strcasecmp(sub, "devid") == 0 && token_count >= 2) {
      id = static_cast<uint8_t>(strtol(embeddedCliGetToken(args, 2), nullptr, 16));
    } else if (strncasecmp(sub, "0x", 2) == 0) {
      id = static_cast<uint8_t>(strtol(sub, nullptr, 16));
    }
    g_telnet_tracer.setFilter(TraceType::DEVID, id);
    sendTelnetMsgf(sock, "Packet trace ENABLED: Device ID 0x%02X only.\r\n", id);
  } else {
    sendTelnetMsg(sock, "Usage: trace [on | off | ctl | ack | pol | rmt | drp | ch <1-6> | devid <hex>]\r\n");
  }
}

void cmdStop(EmbeddedCli *cli, char *args, void *context) {
  int sock = getSock(context);
  g_telnet_tracer.setTrace(false);
  sendTelnetMsg(sock, "Packet trace DISABLED.\r\n");
}

static void formatSources(uint8_t src_mask, char *buf, size_t buf_len) {
  size_t idx = 0;
  if (src_mask & (1 << 2)) {
    idx += snprintf(buf + idx, buf_len - idx, "CH2");
  }
  if (src_mask & (1 << 3)) {
    if (idx > 0 && idx < buf_len)
      idx += snprintf(buf + idx, buf_len - idx, "+");
    idx += snprintf(buf + idx, buf_len - idx, "CH3");
  }
  if (src_mask & (1 << 5)) {
    if (idx > 0 && idx < buf_len)
      idx += snprintf(buf + idx, buf_len - idx, "+");
    idx += snprintf(buf + idx, buf_len - idx, "CH5");
  }
  if (src_mask & (1 << 6)) {
    if (idx > 0 && idx < buf_len)
      idx += snprintf(buf + idx, buf_len - idx, "+");
    idx += snprintf(buf + idx, buf_len - idx, "CH6");
  }
  if (idx == 0) {
    snprintf(buf, buf_len, "None");
  }
}

void devsPrintTier1Targets(AppendBuf &out, uint32_t now) {
  g_polling_targets.sweepExpired(Config::Timing::STALE_DEVICE_THRESHOLD_MS);
  size_t tgt_total = g_polling_targets.totalCount();
  size_t tgt_active = g_polling_targets.activeCount();

  const char *wc_src_str = (g_warm_cache_source == 1) ? "RTC SRAM" : (g_warm_cache_source == 2) ? "NVS Flash" : "Cold Start";

  out.append("\r\n");
  out.append(Fmt::DIV80EQ);
  out.append("          [1st-Tier Cache] Dynamic Polling Target Registry (Wallpad/App)      \r\n");
  out.append(Fmt::DIV80EQ);
  out.appendFormat("  Active Polling Targets: %zu | Total Tracked: %zu | Warm Cache: %s (%u)\r\n",
                   tgt_active, tgt_total, wc_src_str, static_cast<unsigned>(g_warm_cache_restored_count));
  out.append(Fmt::DIV80);
  out.append("No  Status   Last   Sources  Raw Query Frame (Template)\r\n");
  out.append(Fmt::DIV80);

  constexpr uint8_t ALLOWED_MASK = (1 << 2) | (1 << 3) | (1 << 5);

  if (tgt_total == 0) {
    out.append("  (No polling targets registered yet. Waiting for Wallpad/App queries...)\r\n");
  } else {
    unsigned int display_idx = 1;
    for (size_t i = 0; i < tgt_total; ++i) {
      PollingTargetEntry tgt;
      if (!g_polling_targets.getEntry(i, tgt))
        continue;

      // Display entries sourced from CH2, CH3, or CH5
      if (tgt.source_channels != 0 && (tgt.source_channels & ALLOWED_MASK) == 0) {
        continue;
      }

      char src_buf[32] = {0};
      formatSources(tgt.source_channels, src_buf, sizeof(src_buf));

      char last_req_str[16] = {0};
      Fmt::FormatElapsed(now, tgt.last_requested_ms, last_req_str, sizeof(last_req_str));

      char q_hex[64] = {0};
      if (tgt.raw_query_len > 0) {
        Fmt::FormatHex(tgt.raw_query_data.data(), tgt.raw_query_len, q_hex, sizeof(q_hex));
      } else {
        snprintf(q_hex, sizeof(q_hex), "[ %02X : %02X : %02X ]", tgt.dev_id,
                 tgt.sub1, tgt.sub2);
      }

      const char *status_str = !tgt.is_active ? "OFFLINE" : (!tgt.is_verified ? "UNVERIF" : "ONLINE");

      out.appendFormat(
          "%02u  %-7s  %-5s  %-7s  %s [%02X:%02X]\r\n",
          display_idx++, status_str, last_req_str,
          src_buf, q_hex, tgt.dev_id, tgt.sub1);
    }
  }
  out.append(Fmt::DIV80);
}

void devsPrintTier2Cache(AppendBuf &out, uint32_t now) {
  size_t total_count = g_device_repo.count();
  size_t online_count = g_device_repo.getOnlineCount();
  size_t tgt_total = g_polling_targets.totalCount();

  out.append("\r\n");
  out.append(Fmt::DIV80EQ);
  out.append("          [2nd-Tier Cache] Physical Device State & Health Monitor             \r\n");
  out.append(Fmt::DIV80EQ);
  out.appendFormat(
      "  Discovered Devices: %zu Nodes on Bus | Online [OK]: %zu | Offline: %zu\r\n",
      total_count, online_count, (total_count >= online_count) ? (total_count - online_count) : 0);
  out.append(Fmt::DIV80);
  out.append("No  Status   Last   Raw Physical ACK (Response Frame)\r\n");
  out.append(Fmt::DIV80);

  if (total_count == 0) {
    out.append("  (No physical devices discovered on RS-485 bus yet)\r\n");
  } else {
    for (size_t i = 0; i < total_count; ++i) {
      DeviceStateEntry dev;
      if (!g_device_repo.getSnapshot(i, dev) || dev.dev_id == 0)
        continue;

      char ack_hex[96] = {0};
      if (dev.last_ack_len > 0) {
        Fmt::FormatHex(dev.last_ack_data.data(), dev.last_ack_len, ack_hex, sizeof(ack_hex));
      } else {
        snprintf(ack_hex, sizeof(ack_hex), "(No ACK received from bus yet)");
      }

      char updated_str[16] = "-";
      if (dev.last_updated_ms > 0) {
        Fmt::FormatElapsed(now, dev.last_updated_ms, updated_str, sizeof(updated_str));
      }

      const char *status_str = dev.is_online ? "ONLINE" : "OFFLINE";

      out.appendFormat("%02u  %-7s  %-5s  %s [%02X:%02X]\r\n",
                       static_cast<unsigned int>(i + 1), status_str, updated_str, ack_hex,
                       dev.dev_id, dev.sub1);
    }
  }
  out.append(Fmt::DIV80);
  out.append(Fmt::DIV80EQ);
  out.append("\r\n");
}

void cmdDevs(EmbeddedCli *cli, char *args, void *context) {
  int client = getSock(context);
  uint32_t now = millis();
  int argc = embeddedCliGetTokenCount(args);

  bool show_tier1 = true;
  bool show_tier2 = true;

  if (argc > 0) {
    const char *sub = embeddedCliGetToken(args, 1);
    if (strcasecmp(sub, "1") == 0) {
      show_tier1 = true;
      show_tier2 = false;
    } else if (strcasecmp(sub, "2") == 0) {
      show_tier1 = false;
      show_tier2 = true;
    } else if (strcasecmp(sub, "clear") == 0) {
      g_polling_targets.clear();
      g_device_repo.clear();
      sendTelnetMsg(client, "All 1st-tier & 2nd-tier device caches CLEARED.\r\n");
      return;
    } else {
      sendTelnetMsg(client, "Usage: devs [1 | 2 | clear]\r\n");
      return;
    }
  }

  g_cli_scratch_buf[0] = '\0';
  AppendBuf out{g_cli_scratch_buf, sizeof(g_cli_scratch_buf)};

  if (show_tier1) {
    devsPrintTier1Targets(out, now);
  }
  if (show_tier2) {
    devsPrintTier2Cache(out, now);
  }

  sendTelnetMsgLen(client, out.buf, out.offset);
}



void cmdWallpad(EmbeddedCli *cli, char *args, void *context) {
  int sock = getSock(context);
  int argc = embeddedCliGetTokenCount(args);
  const char *sub = (argc > 0) ? embeddedCliGetToken(args, 1) : "status";

  if (argc == 0 || strcasecmp(sub, "status") == 0) {
    g_cli_scratch_buf[0] = '\0';
    AppendBuf out{g_cli_scratch_buf, sizeof(g_cli_scratch_buf)};
    wallpadPrintStatus(out);
    sendTelnetMsgLen(sock, out.buf, out.offset);
    return;
  }

  if (strcasecmp(sub, "list") == 0) {
    g_cli_scratch_buf[0] = '\0';
    AppendBuf out{g_cli_scratch_buf, sizeof(g_cli_scratch_buf)};
    wallpadListProfiles(out);
    sendTelnetMsgLen(sock, out.buf, out.offset);
  } else if (strcasecmp(sub, "set") == 0) {
    if (argc >= 2) {
      wallpadSetProfile(sock, embeddedCliGetToken(args, 2));
    } else {
      sendTelnetMsg(sock, "[ERROR] Usage: wallpad set <key|id>\r\n");
    }
  } else if (strcasecmp(sub, "save") == 0) {
    if (argc >= 2) {
      wallpadSaveProfile(sock, embeddedCliGetToken(args, 2));
    } else {
      sendTelnetMsg(sock, "[ERROR] Usage: wallpad save <name>\r\n");
    }
  } else if (strcasecmp(sub, "delete") == 0) {
    if (argc >= 2) {
      wallpadDeleteProfile(sock, embeddedCliGetToken(args, 2));
    } else {
      sendTelnetMsg(sock, "[ERROR] Usage: wallpad delete <id>\r\n");
    }
  } else if (strcasecmp(sub, "auto") == 0) {
    char dp_ns[16];
    Config::Doorphone::FramingTracker::getNvsNamespace(0, dp_ns, sizeof(dp_ns));
    ProfileRepository::setActiveProfileIndex(0);
    g_auto_probing_engine.reset();
    g_doorphone_tracker.clearNvs(dp_ns);
    sendTelnetMsg(sock, "[OK] Switched to Universal Auto-Probing mode (Wallpad & Doorphone framing reset).\r\n");
  } else if (strcasecmp(sub, "reset") == 0) {
    char dp_ns[16];
    Config::Doorphone::FramingTracker::getNvsNamespace(g_config.wallpad_profile, dp_ns, sizeof(dp_ns));
    g_auto_probing_engine.reset();
    g_doorphone_tracker.clearNvs(dp_ns);
    g_probe_convergence_reset.store(true, std::memory_order_release);
  } else if (strcasecmp(sub, "simulate") == 0) {
    if (argc < 2) {
      sendTelnetMsg(sock, "[ERROR] Usage: wallpad simulate <hex_bytes...> (e.g. wallpad simulate F7 0E 01 19 01 40 11 01 00 B6 EE)\r\n");
      return;
    }
    uint8_t sim_buf[64]{0};
    size_t sim_len = 0;
    for (int i = 2; i <= argc && sim_len < sizeof(sim_buf); ++i) {
      const char *tok = embeddedCliGetToken(args, i);
      if (!tok) break;
      char *endp = nullptr;
      unsigned long val = strtoul(tok, &endp, 16);
      if (endp != tok) {
        sim_buf[sim_len++] = static_cast<uint8_t>(val);
      }
    }
    if (sim_len < 3) {
      sendTelnetMsg(sock, "[ERROR] Simulated packet must be at least 3 bytes.\r\n");
      return;
    }
    g_auto_probing_engine.feedFrame(span<const uint8_t>(sim_buf, sim_len));
    sendTelnetMsgf(sock, "[OK] Fed %u simulated bytes into Auto-Probing Engine.\r\n", sim_len);
  } else if (strcasecmp(sub, "help") == 0 || strcasecmp(sub, "?") == 0) {
    g_cli_scratch_buf[0] = '\0';
    AppendBuf out{g_cli_scratch_buf, sizeof(g_cli_scratch_buf)};
    out.append("\r\n");
    out.append(Fmt::DIV80EQ);
    out.append("                    WALLPAD & PROTOCOL COMMAND REFERENCE                      \r\n");
    out.append(Fmt::DIV80EQ);
    out.append("Command                           Description\r\n");
    out.append(Fmt::DIV80);
    out.append("  wallpad [status]                Show auto-probing status, timings & locked profile\r\n");
    out.append("  wallpad list                    List available vendor & saved NVS custom profiles\r\n");
    out.append("  wallpad set <key|id>            Manually switch active wallpad vendor profile\r\n");
    out.append("  wallpad save <name>             Save current auto-learned profile to NVS slot\r\n");
    out.append("  wallpad delete <id>             Reset a saved custom profile slot in NVS\r\n");
    out.append("  wallpad auto                    Switch to Universal Auto-Probing mode\r\n");
    out.append("  wallpad reset                   Reset auto-probing engine and re-learn bus traffic\r\n");
    out.append("  wallpad simulate <hex...>       Inject raw hex packet into auto-probing engine\r\n");
    out.append(Fmt::DIV80EQ);
    out.append("Tip: Use 'ctl' for device control blueprints & learned slots.\r\n");
    out.append(Fmt::DIV80EQ);
    out.append("\r\n");
    sendTelnetMsgLen(sock, out.buf, out.offset);
  } else {
    sendTelnetMsg(sock, "Usage: wallpad [status | list | set <key|id> | save <name> | delete <id> | auto | reset | simulate <hex...> | help]\r\n");
  }
}

void cmdCtl(EmbeddedCli *cli, char *args, void *context) {
  auto *session = getSession(context);
  int sock = session ? session->sock : -1;
  if (sock < 0) return;

  if (session) {
    if (session->txLen > 0) {
      sendTelnetMsgLen(session->sock, session->txBuf, session->txLen);
      session->txLen = 0;
      session->needsSend = false;
    }
  }

  int argc = embeddedCliGetTokenCount(args);

  if (argc == 0) {
    g_cli_scratch_buf[0] = '\0';
    AppendBuf out{g_cli_scratch_buf, sizeof(g_cli_scratch_buf)};
    wallpadPrintControlTable(out);
    sendTelnetMsgLen(sock, out.buf, out.offset);
    if (session) {
      session->txLen = 0;
      session->needsSend = false;
    }
    return;
  }

  const char *sub = embeddedCliGetToken(args, 1);

  if (strcasecmp(sub, "table") == 0 || strcasecmp(sub, "list") == 0 || strcasecmp(sub, "view") == 0) {
    g_cli_scratch_buf[0] = '\0';
    AppendBuf out{g_cli_scratch_buf, sizeof(g_cli_scratch_buf)};
    wallpadPrintControlTable(out);
    sendTelnetMsgLen(sock, out.buf, out.offset);
  } else if (strcasecmp(sub, "reset") == 0) {
    if (argc < 2) {
      sendTelnetMsg(sock, "[ERROR] Usage: ctl reset <all | dev_id> (e.g. ctl reset all, ctl reset 0x18)\r\n");
      return;
    }
    const char *arg = embeddedCliGetToken(args, 2);
    if (strcasecmp(arg, "all") == 0) {
      g_control_registry.resetGroup(0, true);
      sendTelnetMsg(sock, "[OK] All control blueprints reset & re-synthesized from catalog specs.\r\n");
    } else {
      char *endp = nullptr;
      uint8_t dev_id = static_cast<uint8_t>(strtoul(arg, &endp, 0));
      if (endp == arg || dev_id == 0) {
        sendTelnetMsgf(sock, "[ERROR] Invalid target '%s'. Use 'ctl reset all' or 'ctl reset <dev_id>'.\r\n", arg);
        return;
      }
      g_control_registry.resetGroup(dev_id, false);
      sendTelnetMsgf(sock, "[OK] Control template for DevID 0x%02X action slots reset completed.\r\n", dev_id);
    }
  } else if (strcasecmp(sub, "name") == 0 || strcasecmp(sub, "setname") == 0) {
    if (argc >= 3) {
      uint8_t dev_id = static_cast<uint8_t>(strtoul(embeddedCliGetToken(args, 2), nullptr, 0));
      const char *name = embeddedCliGetToken(args, 3);
      if (dev_id == 0 || !name || strlen(name) == 0) {
        sendTelnetMsg(sock, "[ERROR] Usage: ctl name <dev_id> <custom_name> (e.g. ctl name 0x1B Gas)\r\n");
      } else {
        if (g_control_registry.setGroupName(dev_id, name)) {
          sendTelnetMsgf(sock, "[OK] DevID 0x%02X group name set to '%s' and saved to NVS flash.\r\n", dev_id, name);
        } else {
          sendTelnetMsgf(sock, "[ERROR] DevID 0x%02X not found in blueprint registry.\r\n", dev_id);
        }
      }
    } else {
      sendTelnetMsg(sock, "[ERROR] Usage: ctl name <dev_id> <custom_name> (e.g. ctl name 0x1B Gas)\r\n");
    }
  } else if (strcasecmp(sub, "class") == 0 || strcasecmp(sub, "setclass") == 0) {
    if (argc >= 3) {
      uint8_t dev_id = static_cast<uint8_t>(strtoul(embeddedCliGetToken(args, 2), nullptr, 0));
      const char *cls_str = embeddedCliGetToken(args, 3);
      const char *custom_name = (argc >= 4) ? embeddedCliGetToken(args, 4) : nullptr;
      struct DeviceClassEntry {
        std::string_view key;
        DeviceClass cls;
        const char *def_name;
      };
      static constexpr DeviceClassEntry kDeviceClassTable[] = {
          {"light",      DeviceClass::SWITCH,     "Light"},
          {"switch",     DeviceClass::SWITCH,     "Light"},
          {"outlet",     DeviceClass::SWITCH,     "Outlet"},
          {"vent",       DeviceClass::VENT,       "Vent"},
          {"fan",        DeviceClass::VENT,       "Vent"},
          {"thermo",     DeviceClass::THERMOSTAT, "Thermo"},
          {"thermostat", DeviceClass::THERMOSTAT, "Thermo"},
          {"heat",       DeviceClass::THERMOSTAT, "Thermo"},
          {"gas",        DeviceClass::GAS,        "Gas"},
          {"aircon",     DeviceClass::AIRCON,     "Aircon"},
          {"ac",         DeviceClass::AIRCON,     "Aircon"},
          {"ev",         DeviceClass::MOMENTARY,  "Elevator"},
          {"elevator",   DeviceClass::MOMENTARY,  "Elevator"},
      };

      char lower_cls[32] = {0};
      size_t c_len = 0;
      while (cls_str[c_len] && c_len < sizeof(lower_cls) - 1) {
        lower_cls[c_len] = static_cast<char>(tolower(static_cast<unsigned char>(cls_str[c_len])));
        c_len++;
      }
      lower_cls[c_len] = '\0';

      DeviceClass cls = DeviceClass::UNKNOWN;
      const char *def_name = cls_str;
      std::string_view sv{lower_cls};
      for (const auto &entry : kDeviceClassTable) {
        if (entry.key == sv) {
          cls = entry.cls;
          def_name = entry.def_name;
          break;
        }
      }

      if (dev_id == 0 || cls == DeviceClass::UNKNOWN) {
        sendTelnetMsg(sock, "[ERROR] Usage: ctl class <dev_id> <light|outlet|vent|thermo|gas|aircon|ev> [name]\r\n");
      } else {
        const char *final_name = (custom_name && strlen(custom_name) > 0) ? custom_name : def_name;
        if (g_control_registry.setGroupClass(dev_id, cls, final_name)) {
          sendTelnetMsgf(sock, "[OK] DevID 0x%02X class set to %s ('%s') and saved to NVS flash.\r\n", dev_id, cls_str, final_name);
        } else {
          sendTelnetMsgf(sock, "[ERROR] DevID 0x%02X not found in blueprint registry.\r\n", dev_id);
        }
      }
    } else {
      sendTelnetMsg(sock, "[ERROR] Usage: ctl class <dev_id> <light|outlet|vent|thermo|gas|aircon|ev> [name]\r\n");
    }
  } else if (strcasecmp(sub, "help") == 0 || strcasecmp(sub, "?") == 0) {
    g_cli_scratch_buf[0] = '\0';
    AppendBuf out{g_cli_scratch_buf, sizeof(g_cli_scratch_buf)};
    out.append("\r\n");
    out.append(Fmt::DIV80EQ);
    out.append("             DEVICE GROUP CONTROL BLUEPRINT COMMANDS                          \r\n");
    out.append(Fmt::DIV80EQ);
    out.append("Command                           Description\r\n");
    out.append("  ctl                             Display control blueprint table [view|list]\r\n");
    out.append("  ctl <dev_id>                    Inspect packet blueprint & slots (e.g. ctl 0x18)\r\n");
    out.append("  ctl name <dev_id> <name>        Set custom group name (e.g. Gas, Elevator)\r\n");
    out.append("  ctl reset <dev_id>              Reset action slots for specific device\r\n");
    out.append("  ctl reset all                   Factory wipe & re-inject blueprints from catalog\r\n");
    out.append(Fmt::DIV80EQ);
    out.append("\r\n");
    sendTelnetMsgLen(sock, out.buf, out.offset);
  } else {
    char *endp = nullptr;
    uint8_t dev_id = static_cast<uint8_t>(strtoul(sub, &endp, 0));
    if (endp != sub && dev_id != 0) {
      g_cli_scratch_buf[0] = '\0';
      AppendBuf out{g_cli_scratch_buf, sizeof(g_cli_scratch_buf)};
      wallpadPrintControlDetail(out, dev_id);
      sendTelnetMsgLen(sock, out.buf, out.offset);
    } else {
      sendTelnetMsg(sock, "Usage: ctl [<dev_id> | name <id> <name> | reset <all|id> | help]\r\n");
    }
  }
}

void wallpadPrintControlTable(AppendBuf &out) {
  out.append("\r\n");
  out.append(Fmt::DIV80EQ);
  out.append("                 DEVICE CONTROL BLUEPRINTS & ACTION SLOTS                     \r\n");
  out.append(Fmt::DIV80EQ);

  GroupControlTemplate grps[ControlTemplateRegistry::MAX_GROUPS];
  size_t count = g_control_registry.getGroupsSnapshot(grps, ControlTemplateRegistry::MAX_GROUPS);
  out.appendFormat("  Registered Blueprints: %zu Groups | Auto-Mapped & NVS Persisted               \r\n", count);
  out.append(Fmt::DIV80);
  out.append("DevID  Name        Class      CTL_Len  Power Action Slot   QRY_Len  Status Offsets\r\n");
  out.append(Fmt::DIV80);

  if (count == 0) {
    out.append("  (No control blueprints registered yet. Waiting for profile or learning...)\r\n");
    out.append(Fmt::DIV80);
    out.append(Fmt::DIV80EQ);
    out.append("\r\n");
    return;
  }

  for (size_t i = 0; i < count; ++i) {
    const GroupControlTemplate &grp = grps[i];
    if (grp.dev_id == 0) continue;

    const char *cls_str = DeviceClassToCliString(grp.coverage.dev_class);

    char name_safe[17] = {0};
    strncpy(name_safe, grp.group_name, sizeof(name_safe) - 1);

    char pwr_buf[24] = {0};
    if (grp.power_slot.discovered) {
      snprintf(pwr_buf, sizeof(pwr_buf), "#%u [0x%02X/0x%02X]",
               grp.power_slot.action_offset, grp.power_slot.on_val, grp.power_slot.off_val);
    } else {
      snprintf(pwr_buf, sizeof(pwr_buf), "-");
    }

    char ctl_len_str[12] = {0};
    if (grp.frame_len > 0) snprintf(ctl_len_str, sizeof(ctl_len_str), "%u Byte", grp.frame_len);
    else snprintf(ctl_len_str, sizeof(ctl_len_str), "-");

    char qry_len_str[12] = {0};
    if (grp.query_slots.expected_len > 0) snprintf(qry_len_str, sizeof(qry_len_str), "%u Byte", grp.query_slots.expected_len);
    else snprintf(qry_len_str, sizeof(qry_len_str), "-");

    char extra_slots[40] = {0};
    size_t e_off = 0;
    if (grp.query_slots.power_offset != 0xFF) {
      e_off += snprintf(extra_slots + e_off, sizeof(extra_slots) - e_off, "#%u", grp.query_slots.power_offset);
    } else {
      e_off += snprintf(extra_slots + e_off, sizeof(extra_slots) - e_off, "-");
    }

    if (grp.query_slots.target_temp_offset != 0xFF) {
      e_off += snprintf(extra_slots + e_off, sizeof(extra_slots) - e_off, " (TT:#%u)", grp.query_slots.target_temp_offset);
    }
    if (grp.query_slots.current_temp_offset != 0xFF) {
      e_off += snprintf(extra_slots + e_off, sizeof(extra_slots) - e_off, " (AT:#%u)", grp.query_slots.current_temp_offset);
    }
    if (grp.query_slots.fan_speed_offset != 0xFF) {
      e_off += snprintf(extra_slots + e_off, sizeof(extra_slots) - e_off, " (FS:#%u)", grp.query_slots.fan_speed_offset);
    }
    if (grp.query_slots.power_w_offset != 0xFF) {
      e_off += snprintf(extra_slots + e_off, sizeof(extra_slots) - e_off, " (W:#%u)", grp.query_slots.power_w_offset);
    }

    out.appendFormat("0x%02X   %-11s %-10s %-8s %-19s %-8s %s\r\n",
                     grp.dev_id, name_safe, cls_str,
                     ctl_len_str, pwr_buf,
                     qry_len_str, extra_slots);
  }

  out.append(Fmt::DIV80);
  out.append("  * TT: Target Temp, AT: Ambient Temp, FS: Fan Speed, W: Power Wattage\r\n");
  out.append(Fmt::DIV80EQ);
  out.append("\r\n");
}

void wallpadPrintControlDetail(AppendBuf &out, uint8_t dev_id) {
  GroupControlTemplate grp{};
  if (!g_control_registry.findGroup(dev_id, grp)) {
    out.appendFormat("[ERROR] Group 0x%02X not found in control blueprints.\r\n", dev_id);
    return;
  }

  char name_safe[17] = {0};
  strncpy(name_safe, grp.group_name, sizeof(name_safe) - 1);

  out.append("\r\n");
  out.append(Fmt::DIV80EQ);
  out.appendFormat("               DEVICE CONTROL BLUEPRINT DETAIL: 0x%02X (%s)                \r\n",
                   grp.dev_id, name_safe);
  out.append(Fmt::DIV80EQ);
  out.appendFormat("  Frame Specs     : CTL Length = %u Bytes | QRY Response Length = %u Bytes\r\n",
                   grp.frame_len, grp.query_slots.expected_len);
  out.appendFormat("  Addressing      : Sub1 = Offset #%u | Sub2 = Offset #%u (Override = 0x%02X)\r\n",
                   grp.sub1_offset, grp.sub2_offset, grp.ctl_sub1_override);
  out.append(Fmt::DIV80);

  out.append("[Outbound Action Slots]\r\n");
  if (grp.power_slot.discovered) {
    out.appendFormat("  Power Control : Offset #%u  [ ON: 0x%02X / OFF: 0x%02X ]\r\n",
                     grp.power_slot.action_offset, grp.power_slot.on_val, grp.power_slot.off_val);
  } else {
    out.append("  Power Control : None\r\n");
  }

  if (grp.temp_slot.discovered) {
    out.appendFormat("  Temp Control  : Offset #%u  [ Range: %u ~ %u C ]\r\n",
                     grp.temp_slot.action_offset, grp.temp_slot.min_val, grp.temp_slot.max_val);
  } else {
    out.append("  Temp Control  : None\r\n");
  }

  if (grp.speed_slot.discovered) {
    if (grp.speed_slot.level_count > 0) {
      char tok_str[64] = {0};
      for (uint8_t i = 0; i < grp.speed_slot.level_count; ++i) {
        char t_buf[16] = {0};
        snprintf(t_buf, sizeof(t_buf), "%sL%u:0x%02X", (i > 0 ? ", " : ""), i + 1, grp.speed_slot.level_tokens[i]);
        strncat(tok_str, t_buf, sizeof(tok_str) - strlen(tok_str) - 1);
      }
      out.appendFormat("  Speed Control : Offset #%u  [ Levels: %s ]\r\n",
                       grp.speed_slot.action_offset, tok_str);
    } else {
      out.appendFormat("  Speed Control : Offset #%u  [ Range: %u ~ %u ]\r\n",
                       grp.speed_slot.action_offset, grp.speed_slot.min_val, grp.speed_slot.max_val);
    }
  } else {
    out.append("  Speed Control : None\r\n");
  }

  if (grp.close_slot.discovered) {
    out.appendFormat("  Close Control : Offset #%u  [ Action: 0x%02X ]\r\n",
                     grp.close_slot.action_offset, grp.close_slot.off_val);
  } else {
    out.append("  Close Control : None\r\n");
  }

  out.append("\r\n[Inbound Status Slots]\r\n");
  auto format_slot = [](AppendBuf &b, const char *label, uint8_t off) {
    if (off != 0xFF) {
      b.appendFormat("  %-13s : Offset #%u\r\n", label, off);
    } else {
      b.appendFormat("  %-13s : None\r\n", label);
    }
  };

  format_slot(out, "Power State", grp.query_slots.power_offset);
  format_slot(out, "Target Temp", grp.query_slots.target_temp_offset);
  format_slot(out, "Ambient Temp", grp.query_slots.current_temp_offset);
  format_slot(out, "Fan Speed", grp.query_slots.fan_speed_offset);
  format_slot(out, "Power Wattage", grp.query_slots.power_w_offset);
  format_slot(out, "CTL ACK State", grp.ack_slots.power_offset);

  out.append(Fmt::DIV80EQ);
  out.append("\r\n");
}

} // namespace WallpadCli


