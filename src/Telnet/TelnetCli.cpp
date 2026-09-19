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
      {"ew11", "CH5 EW11 multi-client hub config [list|set|enable|disable]", ConfigCli::cmdEw11},
      {"routes", "Show dynamic device ingress routing table [clear]", ConfigCli::cmdRoutes},
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
    } else if (session->wizard_step > 0) {
      // 대화형 학습 마법사 동작 중에는 전용 키 핸들러로 전달 (q: 취소, Enter: 다음 스킵)
      handleWizardInput(session, (char)c);
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
