// ============================================================================
// ConsoleCli: Level 4 Telnet Socket Server & Interactive Console Engine
// Implementation
// ============================================================================

#include "L4_Services/CLI_Service.h"
#include "L0_Foundation/System_Platform.h"
#include "L3_Protocol/Public/Protocol_Facade.h"
#include "L4_Services/CLI_Commands.h"
#if defined(BENCHMARK_BUILD)
#include "L4_Services/Benchmark_Harness.h"
#endif

#include <WiFi.h>
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#include <initializer_list>
#include <lwip/sockets.h>
#include <span>
#include <string_view>

// ============================================================================
// From src/Telnet/Telnet.cpp
// ============================================================================

// ============================================================================
// GLOBAL INSTANCES & SYNCHRONIZATION OBJECTS
// ============================================================================

static TelnetManager s_telnet_manager(Config::TCP::TELNET_PORT);
static TelnetTracer s_telnet_tracer;
static SemaphoreHandle_t s_telnet_tx_sem = nullptr;
static SemaphoreHandle_t s_tracer_sem = nullptr;
static std::atomic<bool> s_restart_pending{false};
static const char *s_restart_reason = nullptr;
static TelnetManager::WifiScanReq s_wifi_scan_req;

TelnetManager &CLI_GetTelnetManager() noexcept { return s_telnet_manager; }
TelnetTracer &CLI_GetTracer() noexcept { return s_telnet_tracer; }
void CLI_RequestRestart(const char *reason) noexcept {
  s_restart_reason = reason;
  s_restart_pending.store(true, std::memory_order_release);
}
TelnetManager::WifiScanReq &CLI_GetWifiScanReq() noexcept { return s_wifi_scan_req; }

// ============================================================================
// TELNET LOW-LEVEL OUTPUT HELPERS
// ============================================================================

namespace CliIO {

static inline bool valid(int sock, const char *data, size_t len) noexcept {
  return sock >= 0 && data != nullptr && len != 0;
}

static void write(int sock, const char *data, size_t len) noexcept {
  if (!valid(sock, data, len))
    return;

  // Bulkhead Protection: Free Heap이 비상 한계선(25KB) 미만이면 비핵심 텔넷 출력 즉시 드랍-테일
  if (esp_get_free_heap_size() < Config::Memory::MIN_HEAP_THRESHOLD_KB * 1024) {
    return;
  }

  if (!s_telnet_tx_sem)
    return;

  MutexLocker lock(s_telnet_tx_sem, pdMS_TO_TICKS(100));
  if (!lock.isLocked())
    return;

  size_t sent = 0;
  constexpr uint32_t MAX_WRITE_TIMEOUT_MS = 2000;
  const uint32_t start_ms = millis();

  while (sent < len) {
    const size_t to_send = std::min<size_t>(len - sent, 1024);
    const int r = send(sock, data + sent, to_send, MSG_DONTWAIT);
    if (r > 0) {
      sent += static_cast<size_t>(r);
      continue;
    }
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      if (TimeUtils::isElapsed(start_ms, MAX_WRITE_TIMEOUT_MS)) {
        break; // 클라이언트 단선/행 방지 데드라인
      }
      fd_set wfds;
      FD_ZERO(&wfds);
      FD_SET(sock, &wfds);
      struct timeval tv { .tv_sec = 0, .tv_usec = 50000 };
      int sel_res = select(sock + 1, nullptr, &wfds, nullptr, &tv);
      if (sel_res > 0 && FD_ISSET(sock, &wfds)) {
        continue; // 커널이 ACK 수신 후 버퍼 확보 시 마이크로초 단위 즉시 재개
      }
      if (sel_res < 0 && errno != EINTR) {
        break; // 소켓 오류/연결 종료
      }
      System_FeedWdt(Config::Task::WDT_ID_TELNET);
      continue;
    }
    break;
  }
  System_FeedWdt(Config::Task::WDT_ID_TELNET);
}

static inline void text(int sock, const char *s) noexcept {
  if (s)
    write(sock, s, strlen(s));
}

} // namespace CliIO

void sendTelnetMsg(int sock, const char *str) { CliIO::text(sock, str); }
void sendTelnetMsgLen(int sock, const char *str, size_t len) {
  CliIO::write(sock, str, len);
}
void sendTelnetMsgf(int sock, const char *fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n > 0)
    CliIO::write(sock, buf,
                 static_cast<size_t>(
                     n < static_cast<int>(sizeof(buf)) ? n : sizeof(buf) - 1));
}

void CliWriter::write(const char *data, size_t len) {
  if (buf) {
    buf->append(std::string_view(data, len));
    return;
  }
  if (sock >= 0 && data && len > 0) {
    CliIO::write(sock, data, len);
  }
}

void CliWriter::text(const char *s) {
  if (s)
    write(s, strlen(s));
}

void CliWriter::printf(const char *fmt, ...) {
  char b[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  if (n > 0)
    write(b, static_cast<size_t>(
                 n < static_cast<int>(sizeof(b)) ? n : sizeof(b) - 1));
}

void TableRenderer::writeRaw(const char *data, size_t len) {
  if (buf) {
    buf->append(std::string_view(data, len));
    return;
  }
  if (w) {
    w->write(data, len);
  }
}

void TableRenderer::separator(char ch) {
  char sep[128];
  size_t off = 0;
  sep[off++] = '+';
  for (size_t i = 0; i < count && off + cols[i].width + 4 < sizeof(sep); ++i) {
    memset(sep + off, ch, cols[i].width + 2);
    off += cols[i].width + 2;
    sep[off++] = '+';
  }
  sep[off++] = '\r';
  sep[off++] = '\n';
  writeRaw(sep, off);
}

int TableRenderer::formatCell(char *dst, size_t sz, const char *txt,
                              uint8_t width, Align align) {
  int len = txt ? static_cast<int>(strlen(txt)) : 0;
  if (len > width)
    len = width;
  if (align == Align::CENTER) {
    int l = (width - len) / 2;
    return snprintf(dst, sz, "%*s%.*s%*s", l, "", len, txt ? txt : "",
                    width - len - l, "");
  }
  return snprintf(dst, sz, (align == Align::RIGHT) ? "%*.*s" : "%-*.*s", width,
                  width, txt ? txt : "");
}

void TableRenderer::header(bool top_sep) {
  if (top_sep)
    separator('-');
  char line[128];
  size_t off = 0;
  line[off++] = '|';
  for (size_t i = 0; i < count && off + cols[i].width + 4 < sizeof(line); ++i) {
    line[off++] = ' ';
    int n = formatCell(line + off, sizeof(line) - off, cols[i].title,
                       cols[i].width, cols[i].header_align);
    if (n > 0)
      off += static_cast<size_t>(n);
    line[off++] = ' ';
    line[off++] = '|';
  }
  line[off++] = '\r';
  line[off++] = '\n';
  writeRaw(line, off);
  separator('-');
}

void TableRenderer::row(std::initializer_list<const char *> cells) {
  char line[128];
  size_t off = 0;
  line[off++] = '|';
  size_t col_idx = 0;
  for (const char *cell : cells) {
    if (col_idx >= count)
      break;
    const Column &c = cols[col_idx++];
    line[off++] = ' ';
    int n = formatCell(line + off, sizeof(line) - off, cell, c.width, c.align);
    if (n > 0)
      off += static_cast<size_t>(n);
    line[off++] = ' ';
    line[off++] = '|';
  }
  line[off++] = '\r';
  line[off++] = '\n';
  writeRaw(line, off);
}

void TableRenderer::empty(const char *msg) {
  char line[96];
  int len = msg ? static_cast<int>(strlen(msg)) : 0;
  if (len > 78)
    len = 78;
  int l = (78 - len) / 2;
  int r = 78 - len - l;
  int n = snprintf(line, sizeof(line), "|%*s%.*s%*s|\r\n", l, "", len,
                   msg ? msg : "", r, "");
  if (n > 0)
    writeRaw(line, static_cast<size_t>(n));
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
    result = result | (a[i] ^ b[i]);
  return result == 0;
}

namespace {
constexpr uint32_t AUTH_FAIL_PENALTY_MS = 5000;
} // namespace

TelnetManager::AuthResult TelnetManager::evaluateAuth(const char *clean_pw,
                                                      const char *stored_hash,
                                                      AuthBlockEntry *blk,
                                                      uint32_t now_ms) {
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
    Preferences p;
    if (p.begin("runtime-config", false)) {
      p.putString("telnet_hash", input_hash);
      p.end();
    }
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

  // 슬롯 없으면 LRU(가장 오래된 슬롯) 재사용 — null blk로 인한 락아웃 bypass
  // 차단 [C-4]
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

  AuthResult res =
      evaluateAuth(clean_pw, Config_Get().telnet_pass_hash, blk, now);

  if (res == AuthResult::LOCKED_OUT) {
    sendTelnetMsg(
        session->sock,
        "\r\n[SECURITY] Too many failed attempts. Try again in 5 seconds.\r\n");
  }

  if (res == AuthResult::OK) {
    session->sessionState = SessionState::AUTHENTICATED;
    sendTelnetMsg(session->sock, "\r\nAuthentication successful.\r\n"
                                 "Welcome to Gateway Bridge Diagnostics!\r\n"
                                 "Type 'help' for available commands, "
                                 "or press [TAB] to auto-complete.\r\n\r\n");

    if (System_IsRollbackDetected()) {
      char warn_msg[384];
      const esp_partition_t *cur = esp_ota_get_running_partition();
      const esp_partition_t *other = esp_ota_get_next_update_partition(nullptr);
      snprintf(
          warn_msg, sizeof(warn_msg),
          "===================================================================="
          "============\r\n"
          " ⚠️  [SYSTEM AUTO-ROLLBACK NOTICE]\r\n"
          " ⚠️  Firmware automatically rolled back to stable partition '%s'!\r\n"
          " ⚠️  Failed partition '%s' crashed during boot and was "
          "invalidated.\r\n"
          "===================================================================="
          "============\r\n\r\n",
          cur ? cur->label : "app0", other ? other->label : "app1");
      sendTelnetMsg(session->sock, warn_msg);
    }
    if (System_IsRescueMode()) {
      sendTelnetMsg(session->sock,
                    "=========================================================="
                    "======================\r\n"
                    " 🚨  [RESCUE SAFE MODE ACTIVE]\r\n"
                    " 🚨  Connected via Emergency SoftAP (Sweet_Home_Rescue). "
                    "RS-485 tasks bypassed.\r\n"
                    " 🚨  Use 'ota status' or 'coredump' to diagnose and "
                    "upload new firmware.\r\n"
                    "=========================================================="
                    "======================\r\n\r\n");
    }

    session->sessionState = SessionState::AUTHENTICATED;
    session->lineLen = 0;
    sendTelnetMsg(session->sock, "\r\n> ");
    return true;
  }

  sendTelnetMsg(session->sock, "\r\nInvalid password.\r\n");
  return false;
}

// ============================================================================
// TELNET CLI TERMINAL DRIVER & COMMAND BINDINGS
// ============================================================================

TelnetManager::TelnetManager(uint16_t port) : _port(port) {}

// ============================================================================
// Shared CLI 5KB Scratch Buffer Implementation
// ============================================================================

static char s_cli_scratch_buf[5120];

char *Cli_GetScratchBuffer() {
  return s_cli_scratch_buf;
}

size_t Cli_GetScratchBufferSize() {
  return sizeof(s_cli_scratch_buf);
}

void withScratchBufInternal(int sock, std::function<void(AppendBuf &)> fn) {
  s_cli_scratch_buf[0] = '\0';
  AppendBuf out{s_cli_scratch_buf, sizeof(s_cli_scratch_buf)};
  fn(out);
  if (sock >= 0 && out.offset > 0) {
    sendTelnetMsgLen(sock, out.buf, out.offset);
  }
}

// ============================================================================
// Top-Level Unified Command Table Dispatch Definition
// ============================================================================

constexpr CommandDef kConsoleCmds[] = {
#if defined(BENCHMARK_BUILD)
    {"bench", BenchmarkCli::cmdBench, 0,
     "ESP32-S3 cycle-accurate benchmark harness [run|health]"},
#endif
    {"config", ConfigCli::cmdConfig, 0,
     "View or modify runtime configuration [set|reset]"},
    {"coredump", SystemCli::cmdCoreDump, 0,
     "Show crash core dump summary or erase partition [clear]"},
    {"ctl", WallpadCli::cmdCtl, 0,
     "Device control blueprints [table|<dev_id>|name|class|reset]"},
    {"devs", WallpadCli::cmdDevs, 0,
     "Show device registry & cache [1|2|all|clear]"},
    {"ew11", ConfigCli::cmdEw11, 0,
     "CH5 EW11 hub sockets & FCU [list|set|frame|reset|enable|disable]"},
    {"exit", TelnetManager::cmdExit, 0,
     "Disconnect current Telnet CLI session"},
    {"help", SystemCli::cmdHelp, 0,
     "Display comprehensive command reference and usage examples"},
    {"logview", SystemCli::cmdLogView, 0,
     "Persistent reboot history & crash logs [list|<1-20>|last|clear]"},
    {"nvs", SystemCli::cmdNvs, 0,
     "Decoupled NVS management & stress testing [stress|flush]"},
    {"ota", SystemCli::cmdOta, 0,
     "Dual-partition OTA & rollback [status|rollback|validate|cloud]"},
    {"q", WallpadCli::cmdStop, 0,
     "Stop active packet tracing (shortcut for 'trace off')"},
    {"reboot", SystemCli::cmdReboot, 0,
     "Perform hardware system reboot with safe shutdown"},
    {"routes", ConfigCli::cmdRoutes, 0,
     "Show dynamic device ingress routing table [clear]"},
    {"save", ConfigCli::cmdSave, 0,
     "Save current runtime configuration to NVS flash"},
    {"stats", SystemCli::cmdStats, 0,
     "Show real-time HW metrics & traffic stats [clear]"},
    {"sup", SystemCli::cmdSup, 0,
     "Task supervisor status, logs & fault injection [status|inject]"},
    {"trace", WallpadCli::cmdTrace, 0,
     "Packet monitoring [on|off|ctl|ack|pol|rmt|drp|ch|devid]"},
    {"wallpad", WallpadCli::cmdWallpad, 0,
     "Wallpad protocol & auto-probing [status|list|set|save|delete|auto|reset|simulate]"},
    {"wifi", WifiCli::cmdWifi, 0,
     "Manage WiFi connection [status|scan|connect|disconnect]"},
};

constexpr size_t kConsoleCmdsCount = std::size(kConsoleCmds);
static_assert(kConsoleCmdsCount > 0, "kConsoleCmds table cannot be empty");

constexpr int constexpr_strcmp(const char *s1, const char *s2) noexcept {
  while (*s1 && (*s1 == *s2)) {
    s1++;
    s2++;
  }
  return static_cast<unsigned char>(*s1) - static_cast<unsigned char>(*s2);
}

template <size_t N>
constexpr bool isTableSorted(const CommandDef (&table)[N]) noexcept {
  for (size_t i = 1; i < N; ++i) {
    if (constexpr_strcmp(table[i - 1].name, table[i].name) >= 0) {
      return false;
    }
  }
  return true;
}

static_assert(isTableSorted(kConsoleCmds), "kConsoleCmds must be sorted alphabetically by name");

static const CommandDef *findCommand(const char *cmd) noexcept {
  if (!cmd || !*cmd)
    return nullptr;
  int low = 0;
  int high = static_cast<int>(kConsoleCmdsCount) - 1;
  while (low <= high) {
    int mid = low + (high - low) / 2;
    int cmp = strcasecmp(cmd, kConsoleCmds[mid].name);
    if (cmp == 0)
      return &kConsoleCmds[mid];
    if (cmp < 0)
      high = mid - 1;
    else
      low = mid + 1;
  }
  return nullptr;
}

static void dispatchCommand(CliContext &ctx) {
  if (ctx.args.argc == 0)
    return;
  const char *cmd = ctx.args.argv[0];
  if (strcmp(cmd, "?") == 0) {
    SystemCli::cmdHelp(ctx);
    return;
  }
  const CommandDef *entry = findCommand(cmd);
  if (!entry) {
    ctx.out.printf("Unknown command: '%s'. Type 'help' for usage.\r\n", cmd);
    return;
  }
  if (ctx.args.argc - 1 < entry->min_args) {
    ctx.out.printf("Usage: %s %s\r\n", entry->name, entry->help);
    return;
  }
  entry->handler(ctx);
}

static inline bool consumeIac(TelnetManager::TelnetSession *sess, uint8_t c) {
  switch (sess->iacState) {
  case IacState::GOT_IAC:
    switch (c) {
    case TelnetCmd::WILL:
    case TelnetCmd::WONT:
    case TelnetCmd::DO:
    case TelnetCmd::DONT:
      sess->iacState = IacState::GOT_OPTION;
      break;
    case TelnetCmd::SB:
      sess->iacState = IacState::IN_SUBNEG;
      break;
    default:
      sess->iacState = IacState::NORMAL;
      break;
    }
    return true;

  case IacState::GOT_OPTION:
    sess->iacState = IacState::NORMAL;
    return true;

  case IacState::IN_SUBNEG:
    if (c == TelnetCmd::IAC)
      sess->iacState = IacState::GOT_IAC;
    return true;

  case IacState::NORMAL:
  default:
    if (c == TelnetCmd::IAC) {
      sess->iacState = IacState::GOT_IAC;
      return true;
    }
    return false;
  }
}

static void redrawLine(TelnetManager::TelnetSession *sess,
                       const char *new_text) {
  size_t len = new_text ? strlen(new_text) : 0;
  if (len >= sizeof(sess->lineBuf))
    len = sizeof(sess->lineBuf) - 1;
  if (new_text && len > 0)
    memcpy(sess->lineBuf, new_text, len);
  sess->lineBuf[len] = '\0';
  sess->lineLen = static_cast<uint8_t>(len);
  sendTelnetMsgf(sess->sock, "\r> \x1B[K%s", sess->lineBuf);
}

static void handleHistoryNav(TelnetManager::TelnetSession *sess, bool is_up) {
  if (sess->hist_count == 0)
    return;

  if (is_up) {
    sess->browse_idx = (sess->browse_idx == -1)
                           ? (sess->hist_count - 1)
                           : std::max(0, sess->browse_idx - 1);
  } else {
    if (sess->browse_idx == -1)
      return;
    sess->browse_idx++;
    if (sess->browse_idx >= sess->hist_count) {
      sess->browse_idx = -1;
      redrawLine(sess, "");
      return;
    }
  }

  // Ring buffer mapping: index 0 is oldest, hist_count - 1 is latest
  uint8_t slot = (sess->hist_head + TelnetManager::TelnetSession::HISTORY_MAX -
                  sess->hist_count + sess->browse_idx) %
                 TelnetManager::TelnetSession::HISTORY_MAX;
  redrawLine(sess, sess->history[slot]);
}

static void handleTabCompletion(TelnetManager::TelnetSession *sess) {
  if (sess->lineLen == 0)
    return;
  // If line already contains spaces, skip completion for simplicity
  if (memchr(sess->lineBuf, ' ', sess->lineLen) != nullptr)
    return;

  const char *matches[32]{};
  size_t match_count = 0;

  for (size_t i = 0; i < kConsoleCmdsCount && match_count < 32; ++i) {
    if (strncasecmp(sess->lineBuf, kConsoleCmds[i].name, sess->lineLen) == 0) {
      matches[match_count++] = kConsoleCmds[i].name;
    }
  }

  if (match_count == 1) {
    // Exact single match: complete command with a trailing space
    char completed[TelnetManager::TelnetSession::CMD_MAX_LEN];
    snprintf(completed, sizeof(completed), "%s ", matches[0]);
    redrawLine(sess, completed);
    return;
  }
  if (match_count > 1) {
    // Multiple matches: list all candidates on new line, re-prompt current
    // buffer
    sendTelnetMsg(sess->sock, "\r\n");
    for (size_t i = 0; i < match_count; ++i) {
      sendTelnetMsgf(sess->sock, "  %-12s", matches[i]);
      if ((i + 1) % 4 == 0 || i + 1 == match_count)
        sendTelnetMsg(sess->sock, "\r\n");
    }
    sendTelnetMsgf(sess->sock, "> %s", sess->lineBuf);
    return;
  }
  sendTelnetMsg(sess->sock, "\a"); // Bell
}

static bool handlePasswordInput(TelnetManager::TelnetSession *session,
                                uint8_t c, bool &should_close) {
  if (c == '\r' || c == '\n') {
    if (session->pwLen > 0) {
      session->pwBuffer[session->pwLen] = '\0';
      if (!session->pwLen ||
          !s_telnet_manager.handlePassword(session, session->pwBuffer)) {
        should_close = true;
        return true;
      }
      session->pwLen = 0;
    }
    return false;
  }
  if (c == '\b' || c == 0x7F) {
    if (session->pwLen > 0)
      session->pwLen--;
    return false;
  }
  if (isprint(c) && session->pwLen < sizeof(session->pwBuffer) - 1) {
    session->pwBuffer[session->pwLen++] = c;
  }
  return false;
}

static void handleAuthenticatedInput(TelnetManager::TelnetSession *session,
                                     uint8_t c) {
  // ── Enter: execute command & record history ──
  if (c == '\r' || c == '\n') {
    if (session->lineLen == 0) {
      if (c == '\r') {
        sendTelnetMsg(session->sock, "\r\n> ");
      }
      return;
    }

    session->lineBuf[session->lineLen] = '\0';
    session->addHistory(session->lineBuf);
    session->browse_idx = -1;

    s_telnet_tracer.pause();
    sendTelnetMsg(session->sock, "\r\n");

    Args args;
    char *p = session->lineBuf;
    while (*p && args.argc < 8) {
      while (*p && isspace((unsigned char)*p))
        ++p;
      if (!*p)
        break;
      args.argv[args.argc++] = p;
      while (*p && !isspace((unsigned char)*p))
        ++p;
      if (*p) {
        *p = '\0';
        ++p;
      }
    }

    if (args.argc > 0) {
      CliWriter writer(session->sock);
      CliContext ctx{*session, session->sock, args, writer};
      dispatchCommand(ctx);
    }
    session->lineLen = 0;
    sendTelnetMsg(session->sock, "\r\n> ");
    s_telnet_tracer.resume();
    return;
  }

  // ── Tab: Auto-complete ──
  if (c == '\t') {
    handleTabCompletion(session);
    return;
  }

  // ── Backspace ──
  if (c == '\b' || c == 0x7F) {
    if (session->lineLen > 0) {
      session->lineLen--;
      sendTelnetMsg(session->sock, "\b \b");
    }
    return;
  }

  // ── ANSI ESC sequence decoding (Up/Down arrow history) ──
  if (c == 0x1B) {
    session->esc_state = TelnetManager::TelnetSession::EscState::GOT_ESC;
    return;
  }
  if (session->esc_state == TelnetManager::TelnetSession::EscState::GOT_ESC) {
    session->esc_state = (c == '[')
                             ? TelnetManager::TelnetSession::EscState::IN_CSI
                             : TelnetManager::TelnetSession::EscState::NORMAL;
    return;
  }
  if (session->esc_state == TelnetManager::TelnetSession::EscState::IN_CSI) {
    session->esc_state = TelnetManager::TelnetSession::EscState::NORMAL;
    if (c == 'A' || c == 'B') {
      handleHistoryNav(session, c == 'A');
    }
    return;
  }

  // ── Printable character echo ──
  if (isprint(c) && session->lineLen < sizeof(session->lineBuf) - 1) {
    session->lineBuf[session->lineLen++] = c;
    char echo[1] = {static_cast<char>(c)};
    sendTelnetMsgLen(session->sock, echo, 1);
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
    if (consumeIac(session, c))
      continue;

    if (session->sessionState == SessionState::AWAITING_PASSWORD) {
      if (handlePasswordInput(session, c, should_close))
        break;
      continue;
    }
    if (session->sessionState == SessionState::AUTHENTICATED) {
      handleAuthenticatedInput(session, c);
    }
  }

  if (should_close) {
    handleClientDisconnect(session);
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

void TelnetManager::cmdExit(CliContext &ctx) {
  sendTelnetMsg(ctx.sock, "Goodbye!\r\n");
  s_telnet_manager.handleClientDisconnect(&ctx.session);
}

// ============================================================================
// TELNET SERVER LIFE-CYCLE & CONNECTION MANAGEMENT
// ============================================================================

void TelnetManager::onClientConnect(int new_sock,
                                    const struct sockaddr_in &client_addr,
                                    uint32_t now) {
  if (new_sock < 0)
    return;

  const uint8_t *ip_bytes =
      reinterpret_cast<const uint8_t *>(&client_addr.sin_addr.s_addr);
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
  int sndbuf = 2048; // 2 KB Bounded Buffer (Bulkhead)
  setsockopt(new_sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

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
        if (_sessions[i].sessionState == SessionState::AWAITING_PASSWORD) {
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
        const char *busy_msg = "\r\n[SYSTEM] Server busy: Maximum "
                               "authenticated sessions reached.\r\n";
        send(new_sock, busy_msg, strlen(busy_msg), 0);
        close(new_sock);
        return;
      }
    }

    _sessions[emptySlot].reset();
    _sessions[emptySlot].sock = new_sock;
    _sessions[emptySlot].clientIp = remote_ip;
    _sessions[emptySlot].sessionState = SessionState::AWAITING_PASSWORD;
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

  if (s_telnet_tracer.isClient(session->sock)) {
    s_telnet_tracer.setTrace(false);
    s_telnet_tracer.setClient(-1);
  }
  session->reset();
}

void TelnetManager::shutdownForReboot() {
  s_telnet_tracer.setTrace(false);
  s_telnet_tracer.setClient(-1);
  MutexLocker cliLock(_cli_mutex);
  for (int i = 0; i < Config::TCP::MAX_TELNET_CLIENTS; ++i) {
    _sessions[i].reset();
  }
  if (_server_fd >= 0) {
    close(_server_fd);
    _server_fd = -1;
  }
}

void TelnetManager::stopServer() noexcept {
  s_telnet_tracer.setTrace(false);
  s_telnet_tracer.setClient(-1);

  if (_cli_mutex) {
    MutexLocker cliLock(_cli_mutex);
    for (auto &session : _sessions) {
      if (session.sock >= 0) {
        close(session.sock);
        session.sock = -1;
      }
      session.reset();
    }
  }

  if (_server_fd >= 0) {
    close(_server_fd);
    _server_fd = -1;
  }
  Serial.println("[TELNET] Server stopped (Network disconnected)");
}

void TelnetManager::startServer() {
  if (!_cli_mutex)
    _cli_mutex = xSemaphoreCreateMutex();

  if (!s_telnet_tx_sem)
    s_telnet_tx_sem = xSemaphoreCreateMutex();

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

      int b =
          bind(_server_fd, reinterpret_cast<struct sockaddr *>(&server_addr),
               sizeof(server_addr));
      int l = listen(_server_fd, Config::TCP::MAX_TELNET_CLIENTS);
      Serial.printf("[TELNET] Server initialized on port %u (bind=%d, "
                    "listen=%d, fd=%d, errno=%d)\r\n",
                    _port, b, l, _server_fd, errno);
    } else {
      Serial.printf(
          "[TELNET] ERROR: Failed to create server socket! errno=%d\r\n",
          errno);
    }
  }
}

void TelnetManager::tick() {
  fd_set readfds, errorfds;
  FD_ZERO(&readfds);
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
  int activity = select(max_fd + 1, &readfds, nullptr, &errorfds, &tv);

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
    int new_sock =
        accept(_server_fd, reinterpret_cast<struct sockaddr *>(&client_addr), &client_len);
    if (new_sock >= 0) {
      if (heap_caps_get_free_size(MALLOC_CAP_8BIT) < 32768) {
        close(new_sock);
      } else {
        onClientConnect(new_sock, client_addr, now);
      }
    }
  }

  for (int i = 0; i < Config::TCP::MAX_TELNET_CLIENTS; ++i) {
    TelnetSession &s = _sessions[i];
    if (s.sock < 0)
      continue;

    uint32_t session_timeout =
        (s.sessionState == SessionState::AWAITING_PASSWORD)
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

    if (FD_ISSET(s.sock, &readfds)) {
      char rx_buf[128];
      TSTAGE(9);
      int len = recv(s.sock, rx_buf, sizeof(rx_buf) - 1, 0);
      if (len > 0) {
        rx_buf[len] = '\0';
        onClientData(&s, rx_buf, len);
        continue;
      }
      if (len == 0 ||
          (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        handleClientDisconnect(&s);
        continue;
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
  if (!s_tracer_sem)
    s_tracer_sem = xSemaphoreCreateBinary();

  SystemTraceSink sink;
  sink.trace_packet = [](uint8_t ch, bool tx, TraceType ty,
                         const StaticPacket &pkt) {
    s_telnet_tracer.trace(ch, tx, ty, pkt);
  };
  sink.trace_msg = [](const char *msg) { s_telnet_tracer.trace(msg); };
  System_RegisterTraceSink(sink);

  System_RegisterShutdownHook([]() noexcept {
    s_telnet_tracer.setTrace(false);
    s_telnet_tracer.setClient(-1);
    s_telnet_manager.shutdownForReboot();
  });
  TSTAGE(3);

  static bool first_feed = true;
  static uint32_t s_last_twdt_feed_ms = 0;
  static uint32_t s_max_twdt_interval_ms = 0;
  bool ota_notice_sent = false;

  auto feed_twdt = [&]() noexcept {
    if (twdt_registered) {
      const uint32_t now_ms = millis();
      if (first_feed) {
        s_last_twdt_feed_ms = now_ms;
        first_feed = false;
      } else {
        const uint32_t interval = now_ms - s_last_twdt_feed_ms;
        if (interval > s_max_twdt_interval_ms) {
          s_max_twdt_interval_ms = interval;
        }
        s_last_twdt_feed_ms = now_ms;
      }
      esp_task_wdt_reset();
    }
    System_FeedWdt(Config::Task::WDT_ID_TELNET);
  };

  for (;;) {
    TSTAGE(5);
    feed_twdt();

    // ⭐️ [FreeRTOS Event-Driven] 오프라인 시 이벤트 비트 대기 (CPU 점유 0%
    // 슬립)
    if (!System_IsNetworkReady()) {
      s_telnet_manager.stopServer();

      // 네트워크 비트가 켜질 때까지 슬립 (WDT 피딩을 위해 1초 단위 블로킹 대기)
      while (!System_IsNetworkReady()) {
        feed_twdt();
        if (g_system_event_group) {
          xEventGroupWaitBits(g_system_event_group, SYS_EVT_NETWORK_READY,
                              pdFALSE, pdTRUE, pdMS_TO_TICKS(1000));
        } else {
          vTaskDelay(pdMS_TO_TICKS(100));
        }
      }

      s_telnet_manager.startServer();
    }

    // ── 아래부터는 무조건 '네트워크 가용(Online)' 상태 보장 ──

    // 1. OTA 진행 가드
    if (g_ota_in_progress.load(std::memory_order_acquire)) {
      TSTAGE(4);
      if (!ota_notice_sent) {
        ota_notice_sent = s_telnet_manager.broadcastNoticeNonBlocking(
            "\r\n[OTA] Firmware update in progress. Telnet CLI paused...\r\n");
      }
      feed_twdt();
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    ota_notice_sent = false;

    // 2. 시스템 재부팅 가드
    if (s_restart_pending.load(std::memory_order_acquire)) {
      s_restart_pending.store(false, std::memory_order_relaxed);
      ProtocolDiag_ForceFlushAllNvs(50);
      System_Restart(s_restart_reason ? s_restart_reason : "Telnet Command");
    }

    // 3. 온라인 메인 I/O 처리
    s_telnet_manager.tick();

    // 4. 비동기 NVS 저장 커밋 (Task_Ch1으로부터 분리된 저우선순위 백그라운드 플러시: 라운드로빈 틱당 최대 1건)
    ProtocolDiag_CommitPendingNvs();

    TSTAGE(11);
    const bool has_clients = s_telnet_manager.hasActiveClients();
    if (has_clients) {
      s_telnet_tracer.flushToClient();
    }
    TSTAGE(14);
    xSemaphoreTake(s_tracer_sem, has_clients ? pdMS_TO_TICKS(5) : 0);
  }
}

// ============================================================================
// TELNET PACKET TRACER
// ============================================================================

bool TelnetTracer::passesFilter(uint8_t channel, TraceType type,
                                const StaticPacket &pkt) const {
  uint8_t ch_mask = getChannelMask();
  if (ch_mask != 0 && channel >= 1 && channel <= 6 &&
      !((1 << channel) & ch_mask))
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
      ProtocolDiag_ExtractDeviceKey(pkt.data.data(), pkt.length, pkt_dev_id,
                                    dummy_s1, dummy_s2);
      return (pkt_dev_id == target);
    }
    if (pkt.length == 5 && pkt.data[0] == 0x7F) {
      pkt_dev_id = pkt.data[1];
      return (pkt_dev_id == target);
    }
    return false;
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

  if (s_tracer_sem)
    xSemaphoreGive(s_tracer_sem);
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

  if (s_tracer_sem)
    xSemaphoreGive(s_tracer_sem);
}

namespace {
struct ChannelTracker {
  uint8_t dev_id = 0;
  struct timeval tv = {0, 0};
  bool is_query = false;
  bool is_control = false;
  bool active = false;
};

static ChannelTracker s_trackers[7] = {};
static struct timeval s_last_pkt_tv = {0, 0};
static const char *const kCmdTags[7] = {
    nullptr, nullptr, "CMD_CH2", "CMD_CH3", "PASSTHRU", nullptr, "CMD_CH6"};
} // anonymous namespace

void TelnetTracer::resetTrackers() noexcept {
  memset(s_trackers, 0, sizeof(s_trackers));
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

  constexpr size_t BATCH_SIZE = 4;
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

  for (size_t i = 0; i < batch_count; ++i) {
    TracePacketEntry &entry = local_batch[i];
    uint8_t dev_id = 0, sub1 = 0, sub2 = 0;
    if (entry.len >= 5 && entry.data[0] == PKT_STX) {
      ProtocolDiag_ExtractDeviceKey(entry.data.data(), entry.len, dev_id, sub1,
                                    sub2);
    }

    long delay_ms = -1;
    bool is_new_req = false;
    const char *delay_tag = nullptr;

    uint8_t ch = entry.channel;
    auto find_wch = [&](const char *tag) {
      for (uint8_t wch : {2, 3, 6}) {
        if (s_trackers[wch].active && s_trackers[wch].dev_id == dev_id) {
          delay_ms = TimeUtils::elapsedMs(entry.tv, s_trackers[wch].tv);
          delay_tag = tag;
          s_trackers[wch].active = false;
          break;
        }
      }
    };

    switch (ch) {
    case 2:
    case 3:
    case 6:
      if (!entry.is_tx) {
        is_new_req = true;
        s_trackers[ch] = {dev_id, entry.tv, (entry.type == TraceType::QRY),
                          (entry.type == TraceType::CTL), true};
        if (entry.type == TraceType::CTL) {
          delay_tag = kCmdTags[ch];
          delay_ms = -2;
        }
      } else {
        if (entry.type == TraceType::ACK) {
          if (ch == 6 && s_trackers[5].active && s_trackers[5].dev_id == dev_id) {
            delay_ms = TimeUtils::elapsedMs(entry.tv, s_trackers[5].tv);
            delay_tag = "PASSTHRU";
            s_trackers[5].active = false;
          } else {
            if (s_trackers[ch].active) {
              delay_ms = TimeUtils::elapsedMs(entry.tv, s_trackers[ch].tv);
              delay_tag = s_trackers[ch].is_query ? "CACHE  " : "FWD ACK";
              s_trackers[ch].active = false;
            }
          }
        }
      }
      break;

    case 4:
      if (!entry.is_tx) {
        is_new_req = true;
        s_trackers[4] = {dev_id, entry.tv, false, false, true};
        delay_tag = "PASSTHRU";
        delay_ms = -2;
      } else {
        delay_tag = "INJECT ";
        if (s_trackers[4].active) {
          delay_ms = TimeUtils::elapsedMs(entry.tv, s_trackers[4].tv);
          s_trackers[4].active = false;
        } else {
          delay_ms = -2;
        }
      }
      break;

    case 5:
      if (!entry.is_tx) {
        is_new_req = true;
        s_trackers[5] = {dev_id, entry.tv, false, false, true};
      } else {
        find_wch("INJECT ");
      }
      break;

    case 1:
      if (entry.is_tx) {
        is_new_req = true;
        s_trackers[1] = {dev_id, entry.tv, false,
                         (entry.type == TraceType::CTL), true};
        if (entry.type == TraceType::CTL)
          find_wch("GW FWD ");
      } else {
        if (entry.type == TraceType::ACK) {
          if (s_trackers[1].active && s_trackers[1].dev_id == dev_id) {
            delay_ms = TimeUtils::elapsedMs(entry.tv, s_trackers[1].tv);
            delay_tag = "DEV ACK";
            s_trackers[1].active = false;
          }
        }
      }
      break;

    default:
      break;
    }

    if (is_new_req && s_last_pkt_tv.tv_sec > 0) {
      long gap = TimeUtils::elapsedMs(entry.tv, s_last_pkt_tv);
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

    size_t idx = snprintf(line_buf, sizeof(line_buf), "%02d:%02d:%02d.%03ld   ",
                          timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec,
                          entry.tv.tv_usec / 1000);
    if (idx >= sizeof(line_buf))
      idx = sizeof(line_buf) - 1;

    if (entry.type == TraceType::MSG) {
      auto safe_append = [&](const char *fmt, auto... args) {
        if (idx >= sizeof(line_buf))
          return;
        int n = snprintf(line_buf + idx, sizeof(line_buf) - idx, fmt, args...);
        if (n > 0)
          idx = std::min(idx + (size_t)n, sizeof(line_buf) - 1);
      };
      safe_append("[SYSTEM MSG]  ");
      if (idx < sizeof(line_buf) - entry.len) {
        memcpy(line_buf + idx, entry.data.data(), entry.len);
        idx += entry.len;
      }
    } else {
      const char *type_str = (entry.type == TraceType::QRY)   ? "QRY"
                             : (entry.type == TraceType::CTL) ? "CTL"
                             : (entry.type == TraceType::ACK) ? "ACK"
                             : (entry.type == TraceType::DRP) ? "DRP"
                             : (entry.channel == 5)           ? "TCP"
                                                              : "RMT";
      idx +=
          snprintf(line_buf + idx, sizeof(line_buf) - idx, "[CH#%u]  %s %s   ",
                   entry.channel, entry.is_tx ? "==>" : "<==", type_str);

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

      if (delay_ms >= 0 || delay_ms == -2) {
        idx += (delay_ms >= 0)
                   ? snprintf(line_buf + idx, sizeof(line_buf) - idx,
                              "[%s : +%3ldms]", delay_tag, delay_ms)
                   : snprintf(line_buf + idx, sizeof(line_buf) - idx, "[%s]",
                              delay_tag);
      }
    }

    idx += snprintf(line_buf + idx, sizeof(line_buf) - idx, "\r\n");
    TSTAGE(13);
    send(c_fd, line_buf, idx, MSG_DONTWAIT);
  }
}
