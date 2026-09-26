#include "TelnetCli.h"
#include "CliCommands.h"
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <sys/socket.h>

TelnetManager g_telnet_manager(Config::TCP::TELNET_PORT);
std::atomic<bool> g_restart_pending{false};
const char *g_restart_reason = nullptr;
TelnetManager::WifiScanReq g_wifi_scan_req;


void sendTelnetMsg(int sock, const char *str) {
  if (sock >= 0 && str) {
    sendTelnetMsgLen(sock, str, strlen(str));
  }
}

SemaphoreHandle_t g_telnet_tx_sem = nullptr;

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

TelnetManager::TelnetManager(uint16_t port) : _port(port) {}

void TelnetManager::writeCharToClient(EmbeddedCli *cli, char c) {
  if (!cli)
    return;
  auto *session = static_cast<TelnetSession *>(cli->appContext);
  if (!session || session->sock < 0)
    return;

  // Strip ANSI/VT100 escape sequences (ESC [ ... letter)
  // These are EmbeddedCli's own cursor-positioning codes that corrupt dumb terminals.
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
    // CSI sequence ends at any letter A-Z or a-z
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
      session->esc_state = EscState::NORMAL;
    }
    return; // drop all CSI bytes including the terminating letter
  }

  // Drop all remaining non-printable control chars except CR, LF, and TAB (\t).
  // EmbeddedCli uses \t for help command description indentation.
  if ((c < 0x20 && c != '\r' && c != '\n' && c != '\t') || c == 0x7F) {
    return;
  }

  if (c == '\t') {
    // Expand TAB to 4 spaces for consistent terminal alignment
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
      {"ctl", "Device control blueprints & view [view|send|reset]", WallpadCli::cmdCtl},
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
      // embeddedCliNew registers an internal "help" at binding index 0.
      // Override binding index 0 so our full SystemCli::cmdHelp reference is called.
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
        // IAC SE: 서브협상 정상 종료 (RFC 854)
        session->iacState = IacState::NORMAL;
      } else {
        // 기타 2-byte IAC 명령 (IAC NOP, IAC DM 등) → 바이트 소비 후 NORMAL
        session->iacState = IacState::NORMAL;
      }
      continue;
    } else if (session->iacState == IacState::GOT_OPTION) {
      session->iacState = IacState::NORMAL;
      continue;
    } else if (session->iacState == IacState::IN_SUBNEG) {
      if (c == TelnetCmd::IAC) {
        // RFC 854: 서브협상 종료는 IAC SE. IAC 수신 시 SE 대기 상태로 전환.
        session->iacState = IacState::GOT_IAC;
      }
      // SE 이전의 서브협상 데이터 바이트는 모두 버림 (continue)
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
