#pragma once

#include "Common.h"
#include "CliCommands.h"

// ============================================================================
// SECTION 1: TELNET PROTOCOL & IAC ENUMS
// ============================================================================

namespace TelnetCmd {
static constexpr uint8_t IAC =
    255; // Interpret as Command (텔넷 프로토콜 제어 시작 바이트 0xFF)
static constexpr uint8_t SE = 240; // Sub-negotiation End (서브 협상 종료 0xF0)
static constexpr uint8_t SB =
    250; // Sub-negotiation Begin (서브 협상 시작 0xFA)
static constexpr uint8_t WILL = 251;   // Option 활성화 의사 표명 (0xFB)
static constexpr uint8_t WONT = 252;   // Option 비활성화 의사 표명 (0xFC)
static constexpr uint8_t DO = 253;     // 상대방에게 Option 활성화 요청 (0xFD)
static constexpr uint8_t DONT = 254;   // 상대방에게 Option 비활성화 요청 (0xFE)
static constexpr uint8_t OPT_ECHO = 1; // Telnet Local Echo 옵션 (0x01)
static constexpr uint8_t OPT_SUPPRESS_GA =
    3; // Telnet Suppress Go-Ahead 옵션 (0x03)
} // namespace TelnetCmd

enum class IacState : uint8_t {
  NORMAL,     // 일반 CLI 텍스트 데이터 수신 상태
  GOT_IAC,    // 0xFF(IAC) 수신 후 다음 텔넷 명령 바이트 대기
  GOT_OPTION, // WILL/WONT/DO/DONT 수신 후 옵션 ID 바이트 대기
  IN_SUBNEG,  // SB(250) 이후 SE(240) 수신 전까지 서브 협상 데이터 처리 중
};

// ============================================================================
// SECTION 2: TELNET MANAGER CLASS DEFINITION
// ============================================================================

class TelnetManager {
public:
  enum SessionState { AWAITING_PASSWORD, AUTHENTICATED };

  struct CliDeleter {
    void operator()(EmbeddedCli *p) const noexcept {
      if (p)
        embeddedCliFree(p);
    }
  };

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

    std::unique_ptr<EmbeddedCli, CliDeleter> cli;
    bool needsSend = false;
    char txBuf[256];
    size_t txLen = 0;

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
      needsSend = false;
      txLen = 0;
      cli.reset();
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

  void printSystemOverview(AppendBuf &out);

  enum class AuthResult : uint8_t {
    OK,
    WRONG_PASSWORD,
    LOCKED_OUT
  };

  static AuthResult evaluateAuth(const char *clean_pw, const char *stored_hash,
                                 AuthBlockEntry *blk, uint32_t now_ms);

  void onClientConnect(int new_sock, const struct sockaddr_in &client_addr,
                       uint32_t now);
  void onClientData(TelnetSession *session, const char *data, size_t len);
  void handleClientDisconnect(TelnetSession *session);
  bool handlePassword(TelnetSession *session, const char *password);

  static void writeCharToClient(EmbeddedCli *cli, char c);
  void bindCommands(TelnetSession *session);

  static void cmdExit(EmbeddedCli *cli, char *args, void *context);

public:
  explicit TelnetManager(uint16_t port = Config::TCP::TELNET_PORT);
  void startServer();
  void tick();
  void shutdownForReboot();
  void sendScanResult(const WifiScanReq &req, const char *result_str);
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

public:
  void setClient(int sock) noexcept {
    _client_fd.store(sock, std::memory_order_release);
  }
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
  bool passesFilter(uint8_t channel, TraceType type, const StaticPacket &pkt) const;
  void trace(uint8_t channel, bool is_tx, TraceType type, const StaticPacket &pkt);
  void trace(const char *fmt, ...);
  void flushToClient();
};

void sendTelnetMsg(int sock, const char *str);
void sendTelnetMsgLen(int sock, const char *str, size_t len);
void sendTelnetMsgf(int sock, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

extern TelnetManager g_telnet_manager;
extern TelnetTracer g_telnet_tracer;
extern std::atomic<bool> g_restart_pending;
extern const char *g_restart_reason;
extern TelnetManager::WifiScanReq g_wifi_scan_req;