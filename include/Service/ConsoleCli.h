#pragma once

// ============================================================================
// ConsoleCli: Level 4 Telnet Socket Server & Interactive Console Engine
// ============================================================================

#include "Service/EngineTask.h"
#include "Service/RemoteService.h"

// ============================================================================
// SECTION 1: TELNET PROTOCOL & IAC ENUMS
// ============================================================================

namespace TelnetCmd {
static constexpr uint8_t IAC = 255;
static constexpr uint8_t SE = 240;
static constexpr uint8_t SB = 250;
static constexpr uint8_t WILL = 251;
static constexpr uint8_t WONT = 252;
static constexpr uint8_t DO = 253;
static constexpr uint8_t DONT = 254;
static constexpr uint8_t OPT_ECHO = 1;
static constexpr uint8_t OPT_SUPPRESS_GA = 3;
} // namespace TelnetCmd

enum class IacState : uint8_t {
  NORMAL,
  GOT_IAC,
  GOT_OPTION,
  IN_SUBNEG,
};

// Forward declaration
class CliWriter;
struct CliContext;

struct Args {
  int argc = 0;
  const char *argv[8] = {nullptr};

  int count() const noexcept { return argc > 1 ? argc - 1 : 0; }
  const char *get(int idx) const noexcept {
    return (idx >= 0 && idx < argc && argv[idx]) ? argv[idx] : "";
  }
  bool is(int idx, const char *val) const noexcept {
    return (idx >= 0 && idx < argc && argv[idx] && val) &&
           (strcasecmp(argv[idx], val) == 0);
  }
};

extern SemaphoreHandle_t g_telnet_tx_sem;
void sendTelnetMsg(int sock, const char *str);
void sendTelnetMsgLen(int sock, const char *str, size_t len);
void sendTelnetMsgf(int sock, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

class CliWriter {
public:
  int sock = -1;
  AppendBuf *buf = nullptr;

  explicit CliWriter(int s = -1, AppendBuf *b = nullptr) : sock(s), buf(b) {}

  void write(const char *data, size_t len);
  void text(const char *s);
  void printf(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
  void line(const char *fmt = nullptr, ...);
  void ok(const char *msg);
  void error(const char *msg);
  void warn(const char *msg);
  void header(const char *title);
  void subtitle(const char *sub);
  void footer(const char *tip = nullptr);
  void separator(char ch = '-');
  void cardRow2(const char *key, const char *val);

private:
  void centerBox(const char *str);
};

enum class Align : uint8_t { LEFT, CENTER, RIGHT };

struct Column {
  const char *title;
  uint8_t width;
  Align align = Align::LEFT;
  Align header_align = Align::CENTER;
};

class TableRenderer {
public:
  AppendBuf *buf = nullptr;
  CliWriter *w = nullptr;
  const Column *cols;
  size_t count;

  TableRenderer(AppendBuf &b, const Column *c, size_t n)
      : buf(&b), cols(c), count(n) {}
  TableRenderer(CliWriter &writer, const Column *c, size_t n)
      : w(&writer), cols(c), count(n) {}

  void writeRaw(const char *data, size_t len);
  void separator(char ch = '-');
  static int formatCell(char *dst, size_t sz, const char *txt, uint8_t width,
                        Align align);
  void header(bool top_sep = false);
  void row(std::initializer_list<const char *> cells);
  void empty(const char *msg);
  void end(char ch = '-') { separator(ch); }
};

// ============================================================================
// SECTION 2: TELNET MANAGER CLASS DEFINITION
// ============================================================================

class TelnetManager {
public:
  enum SessionState { AWAITING_PASSWORD, AUTHENTICATED };

  struct TelnetSession {
    int sock = -1;
    IacState iacState = IacState::NORMAL;
    std::atomic<bool> wasConnected{false};
    SessionState sessionState = AWAITING_PASSWORD;
    uint32_t connected_at_ms = 0;
    uint32_t last_activity_ms = 0;
    IPAddress clientIp{0, 0, 0, 0};

    char pwBuffer[64];
    size_t pwLen = 0;
    uint32_t sessionId = 0;

    char lineBuf[128];
    uint8_t lineLen = 0;

    enum class EscState : uint8_t {
      NORMAL,
      GOT_ESC,
      IN_CSI
    } esc_state{EscState::NORMAL};

    static constexpr uint8_t HISTORY_MAX = 8;
    static constexpr uint8_t CMD_MAX_LEN = 64;
    char history[HISTORY_MAX][CMD_MAX_LEN]{{0}};
    uint8_t hist_count = 0;
    uint8_t hist_head = 0;
    int8_t browse_idx = -1;

    void addHistory(const char *cmd) {
      if (!cmd || !*cmd)
        return;
      if (hist_count > 0) {
        uint8_t prev = (hist_head + HISTORY_MAX - 1) % HISTORY_MAX;
        if (strncmp(history[prev], cmd, CMD_MAX_LEN - 1) == 0)
          return;
      }
      strncpy(history[hist_head], cmd, CMD_MAX_LEN - 1);
      history[hist_head][CMD_MAX_LEN - 1] = '\0';
      hist_head = (hist_head + 1) % HISTORY_MAX;
      if (hist_count < HISTORY_MAX)
        hist_count++;
    }

    void reset() {
      if (sock >= 0) {
        close(sock);
        sock = -1;
      }
      iacState = IacState::NORMAL;
      wasConnected.store(false, std::memory_order_relaxed);
      sessionState = AWAITING_PASSWORD;
      connected_at_ms = 0;
      last_activity_ms = 0;
      clientIp = IPAddress(0, 0, 0, 0);
      memset(pwBuffer, 0, sizeof(pwBuffer));
      pwLen = 0;
      memset(lineBuf, 0, sizeof(lineBuf));
      lineLen = 0;
      esc_state = EscState::NORMAL;
      memset(history, 0, sizeof(history));
      hist_count = 0;
      hist_head = 0;
      browse_idx = -1;
    }
  };

  struct AuthBlockEntry {
    IPAddress ip;
    uint8_t failedCount = 0;
    uint32_t lastFailedMs = 0;
  };

  struct WifiScanReq {
    IPAddress clientIp;
    uint32_t sessionId;
  };

private:
  int _server_fd = -1;
  uint16_t _port = Config::TCP::TELNET_PORT;
  TelnetSession _sessions[Config::TCP::MAX_TELNET_CLIENTS];
  SemaphoreHandle_t _cli_mutex = nullptr;
  AuthBlockEntry _authBlocks[4];
  uint32_t _nextSessionId = 1;

  enum class AuthResult : uint8_t { OK, WRONG_PASSWORD, LOCKED_OUT };

  static AuthResult evaluateAuth(const char *clean_pw, const char *stored_hash,
                                 AuthBlockEntry *blk, uint32_t now_ms);

  void onClientConnect(int new_sock, const struct sockaddr_in &client_addr,
                       uint32_t now);
  void onClientData(TelnetSession *session, const char *data, size_t len);
  void handleClientDisconnect(TelnetSession *session);

public:
  bool handlePassword(TelnetSession *session, const char *password);
  static void cmdExit(CliContext &ctx);
  explicit TelnetManager(uint16_t port = Config::TCP::TELNET_PORT);
  void startServer();
  void tick();
  void shutdownForReboot();
  void sendScanResult(const WifiScanReq &req, const char *result_str);
  bool broadcastNoticeNonBlocking(const char *msg);
  bool hasActiveClients() const noexcept {
    for (int i = 0; i < Config::TCP::MAX_TELNET_CLIENTS; ++i) {
      if (_sessions[i].sock >= 0)
        return true;
    }
    return false;
  }

  TelnetSession *getSession(int index) {
    if (index >= 0 && index < Config::TCP::MAX_TELNET_CLIENTS)
      return &_sessions[index];
    return nullptr;
  }
};

// ============================================================================
// SECTION 3: TELNET TRACER CLASS DEFINITION
// ============================================================================

class TelnetTracer {
private:
  static constexpr size_t RING_CAP = 64;
  static constexpr size_t RING_MASK = RING_CAP - 1;

  struct TraceSlot {
    std::atomic<uint32_t> seq{0};
    TracePacketEntry entry{};
  };

  std::atomic<int> _client_fd{-1};
  std::atomic<bool> _traceEnabled{false};
  std::atomic<uint8_t> _filterMode{static_cast<uint8_t>(TraceType::ALL)};
  std::atomic<uint8_t> _filterTargetVal{0};
  std::atomic<uint8_t> _channelMask{0};

  TraceSlot _traceRing[RING_CAP]{};
  std::atomic<uint32_t> _head{0};
  uint32_t _tail{0};

  std::atomic<bool> _paused{false};

public:
  void resetTrackers() noexcept;
  void pause() noexcept { _paused.store(true, std::memory_order_release); }
  void resume() noexcept { _paused.store(false, std::memory_order_release); }
  bool isPaused() const noexcept {
    return _paused.load(std::memory_order_acquire);
  }
  void setClient(int sock) noexcept;
  int getClient() const noexcept {
    return _client_fd.load(std::memory_order_acquire);
  }
  bool isClient(int sock) const noexcept {
    return (_client_fd.load(std::memory_order_acquire) == sock && sock >= 0);
  }
  void setTrace(bool enabled) noexcept {
    _traceEnabled.store(enabled, std::memory_order_release);
  }
  bool isTraceEnabled() const noexcept {
    return _traceEnabled.load(std::memory_order_acquire);
  }
  void setFilter(TraceType mode, uint8_t targetVal = 0) {
    _filterMode.store(static_cast<uint8_t>(mode), std::memory_order_release);
    _filterTargetVal.store(targetVal, std::memory_order_release);
    if (mode == TraceType::CH && targetVal >= 1 && targetVal <= 6) {
      _channelMask.store(1 << targetVal, std::memory_order_release);
    } else {
      _channelMask.store(0, std::memory_order_release);
    }
  }
  void setChannelMask(uint8_t mask) noexcept {
    _channelMask.store(mask, std::memory_order_release);
  }
  uint8_t getChannelMask() const noexcept {
    return _channelMask.load(std::memory_order_acquire);
  }
  TraceType getFilterMode() const {
    return static_cast<TraceType>(_filterMode.load(std::memory_order_acquire));
  }
  uint8_t getFilterTargetVal() const {
    return _filterTargetVal.load(std::memory_order_acquire);
  }
  bool passesFilter(uint8_t channel, TraceType type,
                    const StaticPacket &pkt) const;
  void trace(uint8_t channel, bool is_tx, TraceType type,
             const StaticPacket &pkt);
  void trace(const char *fmt, ...);
  void flushToClient();
};

extern TelnetManager g_telnet_manager;
extern TelnetTracer g_telnet_tracer;
extern std::atomic<bool> g_restart_pending;
extern const char *g_restart_reason;
extern TelnetManager::WifiScanReq g_wifi_scan_req;

struct CliContext {
  TelnetManager::TelnetSession &session;
  int sock;
  const Args &args;
  CliWriter &out;
};
