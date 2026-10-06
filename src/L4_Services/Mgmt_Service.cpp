#include "L4_Services/Mgmt_Service.h"
#include "L4_Services/Mgmt/Mgmt_Internal.h"
#include "L3_Protocol/Public/Protocol_Diagnostics.h"
constexpr uint32_t POST_BOOT_LOG_DELAY_MS = 5000;

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <lwip/sockets.h>
#include <unistd.h>

WifiFallbackGuard g_wifi_guard;
static MgmtSession s_mgmt_sessions[Config::TCP::MAX_MGMT_CLIENTS];
static SemaphoreHandle_t s_mgmt_mutex = nullptr;
static DeviceControlHandler s_control_handler = nullptr;
static int s_mgmt_server_fd = -1;
static uint32_t s_chk_ms = 0, s_met_ms = 0, s_tcp_ms = 0;
static uint32_t s_last_sta_retry_ms = 0;
static bool s_last_net_ready = false;
static uint32_t s_sta_retry_interval_ms = Config::Timing::WIFI_BACKGROUND_RETRY_INTERVAL_MS;
constexpr uint32_t kMaxStaRetryIntervalMs = 60000;

MgmtSession *Remote_GetSessions() { return s_mgmt_sessions; }
SemaphoreHandle_t Remote_GetSessionMutex() { return s_mgmt_mutex; }
DeviceControlHandler Remote_GetControlHandler() { return s_control_handler; }

void Remote_RegisterControlHandler(DeviceControlHandler handler) noexcept {
  s_control_handler = handler;
}

IPAddress Remote_GetClientIp(int sock) {
  struct sockaddr_in peer;
  socklen_t len = sizeof(peer);
  if (getpeername(sock, reinterpret_cast<struct sockaddr *>(&peer), &len) == 0) {
    const uint8_t *b = reinterpret_cast<const uint8_t *>(&peer.sin_addr.s_addr);
    return IPAddress(b[0], b[1], b[2], b[3]);
  }
  return IPAddress(0, 0, 0, 0);
}

void Mgmt_Init() {
  if (!s_mgmt_mutex) {
    s_mgmt_mutex = xSemaphoreCreateMutex();
  }
  for (size_t i = 0; i < Config::TCP::MAX_MGMT_CLIENTS; i++) {
    s_mgmt_sessions[i].sock = -1;
    s_mgmt_sessions[i].len = 0;
    s_mgmt_sessions[i].connected_at_ms = 0;
  }
  TimingConfig_Load();
}

template <typename SessionType, size_t N>
void Tcp_CloseAllSessions(SessionType (&sessions)[N],
                          SemaphoreHandle_t mux) noexcept {
  MutexLocker lock(mux);
  for (size_t i = 0; i < N; i++) {
    if (sessions[i].sock >= 0) {
      close(sessions[i].sock);
      sessions[i].sock = -1;
      sessions[i].len = 0;
    }
  }
}

template <typename SessionType, size_t N>
[[nodiscard]] bool Tcp_HasActiveSession(SessionType (&sessions)[N],
                                        SemaphoreHandle_t mux) noexcept {
  MutexLocker lock(mux);
  for (size_t i = 0; i < N; i++) {
    if (sessions[i].sock >= 0)
      return true;
  }
  return false;
}

template <typename SessionType, size_t N, typename DataHandler>
void Tcp_PollAndReceive(SessionType (&sessions)[N], SemaphoreHandle_t mux,
                        fd_set &readfds, fd_set &errorfds,
                        DataHandler handler) {
  MutexLocker lock(mux);
  for (size_t i = 0; i < N; i++) {
    int s = sessions[i].sock;
    if (s < 0)
      continue;

    if (FD_ISSET(s, &errorfds)) {
      close(s);
      sessions[i].sock = -1;
      sessions[i].len = 0;
      continue;
    }

    if (FD_ISSET(s, &readfds)) {
      uint8_t rx_buf[Config::TCP::POLL_RX_CHUNK_SIZE];
      int r = recv(s, rx_buf, sizeof(rx_buf), 0);
      if (r > 0) {
        handler(&sessions[i], rx_buf, r);
      } else if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        close(s);
        sessions[i].sock = -1;
        sessions[i].len = 0;
      }
    }
  }
}

void Tcp_EnableKeepalive(int sock, int idle, int intvl, int cnt);

template <typename SessionType, size_t N>
int Tcp_AcceptAndAssignSlot(int server_fd, SessionType (&sessions)[N],
                            SemaphoreHandle_t mux, int keepalive_idle,
                            int keepalive_intvl, int keepalive_cnt) {
  struct sockaddr_in caddr;
  socklen_t clen = sizeof(caddr);
  int new_sock =
      accept(server_fd, reinterpret_cast<struct sockaddr *>(&caddr), &clen);
  if (new_sock < 0)
    return -1;

  const uint8_t *b = reinterpret_cast<const uint8_t *>(&caddr.sin_addr.s_addr);
  IPAddress remote_ip(b[0], b[1], b[2], b[3]);
  if (!Tcp_IsAllowedIP(remote_ip)) {
    close(new_sock);
    return -1;
  }

  int flags = fcntl(new_sock, F_GETFL, 0);
  fcntl(new_sock, F_SETFL, flags | O_NONBLOCK);
  int nodelay = 1;
  setsockopt(new_sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
  int sockbuf = Config::TCP::SOCKET_BUFFER_SIZE;
  setsockopt(new_sock, SOL_SOCKET, SO_RCVBUF, &sockbuf, sizeof(sockbuf));
  setsockopt(new_sock, SOL_SOCKET, SO_SNDBUF, &sockbuf, sizeof(sockbuf));
  Tcp_EnableKeepalive(new_sock, keepalive_idle, keepalive_intvl, keepalive_cnt);

  MutexLocker lock(mux);
  int slot = -1;
  for (size_t i = 0; i < N; i++) {
    if (sessions[i].sock < 0) {
      slot = static_cast<int>(i);
      break;
    }
  }
  if (slot == -1) {
    uint32_t oldest_time = 0xFFFFFFFF;
    int oldest_idx = 0;
    for (size_t i = 0; i < N; i++) {
      if (sessions[i].connected_at_ms < oldest_time) {
        oldest_time = sessions[i].connected_at_ms;
        oldest_idx = static_cast<int>(i);
      }
    }
    close(sessions[oldest_idx].sock);
    sessions[oldest_idx].sock = -1;
    sessions[oldest_idx].len = 0;
    slot = oldest_idx;
  }
  sessions[slot].sock = new_sock;
  sessions[slot].len = 0;
  sessions[slot].connected_at_ms = millis();
  System_RecordCh6Connection();
  return new_sock;
}


static void Network_HandleMaintenance(uint32_t &t_chk, uint32_t &t_met,
                                      uint32_t &t_tcp, uint32_t now) {
  if (TimeUtils::isElapsed(t_chk, Config::Timing::SYSTEM_MONITOR_INTERVAL_MS)) {
    t_chk = now;
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    if (!System_IsHttpOtaInProgress()) {
      if (free_heap < Config::Memory::MIN_HEAP_THRESHOLD_KB * 1024) {
        System_Restart("Low Heap Memory");
      }
    } else {
      if (free_heap < 8 * 1024) {
        System_Restart("Low Heap Memory (OTA)");
      }
    }
  }

  if (TimeUtils::isElapsed(t_met, Config::Metrics::SAMPLE_INTERVAL_MS)) {
    t_met = now;
    uint16_t used_ram = (heap_caps_get_total_size(MALLOC_CAP_8BIT) -
                         heap_caps_get_free_size(MALLOC_CAP_8BIT)) /
                        1024;
    System_RecordMetricsSample(used_ram);
  }

  if (TimeUtils::isElapsed(t_tcp, Config::TCP::CLEANUP_INTERVAL_MS)) {
    t_tcp = now;
    System_SetCh6Connected(Tcp_HasActiveSession(s_mgmt_sessions, s_mgmt_mutex));
  }
}

void Remote_PopulateFds(fd_set &readfds, fd_set &errorfds, int &max_fd) noexcept {
  auto add_fd = [&](int fd) {
    if (fd >= 0) {
      FD_SET(fd, &readfds);
      FD_SET(fd, &errorfds);
      if (fd > max_fd)
        max_fd = fd;
    }
  };

  add_fd(s_mgmt_server_fd);

  MutexLocker lock(s_mgmt_mutex);
  for (int m = 0; m < Config::TCP::MAX_MGMT_CLIENTS; m++) {
    add_fd(s_mgmt_sessions[m].sock);
  }
}

void Remote_ProcessEvents(fd_set &readfds, fd_set &errorfds,
                          bool /*ota_now*/) noexcept {
  if (s_mgmt_server_fd >= 0 && FD_ISSET(s_mgmt_server_fd, &readfds)) {
    Tcp_AcceptAndAssignSlot(s_mgmt_server_fd, s_mgmt_sessions, s_mgmt_mutex,
                            Config::TCP::DEFAULT_KEEPALIVE_IDLE_SEC,
                            Config::TCP::DEFAULT_KEEPALIVE_INTVL_SEC,
                            Config::TCP::DEFAULT_KEEPALIVE_CNT);
  }

  Tcp_PollAndReceive(s_mgmt_sessions, s_mgmt_mutex, readfds, errorfds,
                     [](MgmtSession *sess, const uint8_t *data, size_t len) {
                       Mgmt_Data(sess, data, len);
                     });
}

namespace {

enum class NetworkFsmState : uint8_t {
  DISCONNECTED, // Wi-Fi STA disconnected, backing off
  TESTING,      // Testing newly switched Wi-Fi credentials (15s guard)
  OPERATIONAL,  // Connected and sockets active
};

NetworkFsmState s_net_fsm = NetworkFsmState::DISCONNECTED;

void handleFsmOperational(uint32_t now, EventBits_t bits) noexcept {
  // Check if disconnected
  if (!System_IsNetworkReady() || (bits & WIFI_BIT_DISCONNECTED)) {
    Remote_StopServer();
    s_net_fsm = NetworkFsmState::DISCONNECTED;
    s_last_sta_retry_ms = now;
    s_sta_retry_interval_ms = Config::Timing::WIFI_BACKGROUND_RETRY_INTERVAL_MS;
    return;
  }
}

void handleFsmTesting(uint32_t now, EventBits_t bits) noexcept {
  // Success check: got IP while testing
  if (System_IsNetworkReady() || (bits & WIFI_BIT_GOT_IP)) {
    g_wifi_guard.testing.store(false, std::memory_order_release);
    Serial.println(
        F("[WIFI] ★ New Wi-Fi connected successfully! Saved to NVS."));
    Remote_StartServer();
    s_net_fsm = NetworkFsmState::OPERATIONAL;
    return;
  }

  // Timeout check: failed to connect within 15 seconds
  if (TimeUtils::isElapsed(g_wifi_guard.start_ms, 15000)) {
    g_wifi_guard.testing.store(false, std::memory_order_release);
    const auto &cfg = Config_Get();
    Serial.printf(
        "[WIFI] ⚠️ New Wi-Fi failed within 15s! Reverting to '%s'...\r\n",
        cfg.wifi_ssid);
    Config_SaveStaged(cfg, TimingConfig_Get());
    WiFi.disconnect(false);
    vTaskDelay(pdMS_TO_TICKS(100));
    WiFi.begin(cfg.wifi_ssid, cfg.wifi_password);
    s_net_fsm = NetworkFsmState::DISCONNECTED;
    s_last_sta_retry_ms = now;
  }
}

void handleFsmDisconnected(uint32_t now, EventBits_t bits) noexcept {
  // If credential testing started, transition immediately
  if (g_wifi_guard.testing.load(std::memory_order_acquire)) {
    s_net_fsm = NetworkFsmState::TESTING;
    return;
  }

  // If network came up (IP acquired)
  if (System_IsNetworkReady() || (bits & WIFI_BIT_GOT_IP)) {
    Remote_StartServer();
    s_net_fsm = NetworkFsmState::OPERATIONAL;
    s_sta_retry_interval_ms = Config::Timing::WIFI_BACKGROUND_RETRY_INTERVAL_MS;
    return;
  }

  // Periodic exponential backoff STA reconnection
  if (TimeUtils::isElapsed(s_last_sta_retry_ms, s_sta_retry_interval_ms)) {
    s_last_sta_retry_ms = now;
    Serial.printf("[WIFI] Reconnection attempt (interval: %u ms)...\r\n",
                  static_cast<unsigned>(s_sta_retry_interval_ms));
    System_WifiReconnect();
    s_sta_retry_interval_ms =
        std::min(s_sta_retry_interval_ms * 2, kMaxStaRetryIntervalMs);
  }
}

} // namespace

void Remote_Tick(bool /*ota_now*/, uint32_t now) noexcept {
  if (!System_IsRescueMode()) {
    const EventBits_t bits = System_WifiGetEventBits();

    switch (s_net_fsm) {
    case NetworkFsmState::OPERATIONAL:
      handleFsmOperational(now, bits);
      break;
    case NetworkFsmState::TESTING:
      handleFsmTesting(now, bits);
      break;
    case NetworkFsmState::DISCONNECTED:
      handleFsmDisconnected(now, bits);
      break;
    default:
      std::unreachable();
    }
  }

  if (now > POST_BOOT_LOG_DELAY_MS) {
    if (const char *reason = System_ConsumePendingRebootReason()) {
      System_WriteRebootLog(reason);
    }
  }

  Network_HandleMaintenance(s_chk_ms, s_met_ms, s_tcp_ms, now);
  ProtocolDiag_WarmCacheCheckNvsDebounce();
}

void Remote_StartServer() noexcept {
  if (System_IsRescueMode()) {
    Serial.println(F("[RESCUE] CH6 TCP server port disabled in Rescue Mode."));
    return;
  }

  if (s_mgmt_server_fd >= 0) {
    return; // Already listening
  }

  s_mgmt_server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s_mgmt_server_fd >= 0) {
    int opt = 1;
    setsockopt(s_mgmt_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    int flags = fcntl(s_mgmt_server_fd, F_GETFL, 0);
    fcntl(s_mgmt_server_fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_addr.s_addr = htonl(INADDR_ANY);
    saddr.sin_port = htons(Config::TCP::MGMT_PORT);
    if (bind(s_mgmt_server_fd, reinterpret_cast<struct sockaddr *>(&saddr),
             sizeof(saddr)) < 0 ||
        listen(s_mgmt_server_fd, Config::TCP::MAX_MGMT_CLIENTS) < 0) {
      ESP_LOGE("NET", "Failed to bind/listen mgmt server (8900): errno %d",
               errno);
      close(s_mgmt_server_fd);
      s_mgmt_server_fd = -1;
    } else {
      ESP_LOGI("NET", "[CH6] Management TCP Server listening on port %u",
               Config::TCP::MGMT_PORT);
    }
  }
}

void Remote_StopServer() noexcept {
  if (!s_mgmt_mutex)
    return;

  MutexLocker lock(s_mgmt_mutex);
  for (int i = 0; i < Config::TCP::MAX_MGMT_CLIENTS; i++) {
    if (s_mgmt_sessions[i].sock >= 0) {
      close(s_mgmt_sessions[i].sock);
      s_mgmt_sessions[i].sock = -1;
      s_mgmt_sessions[i].len = 0;
    }
  }

  if (s_mgmt_server_fd >= 0) {
    close(s_mgmt_server_fd);
    s_mgmt_server_fd = -1;
  }
  ESP_LOGI("NET", "[CH6] Management TCP Server stopped");
}

void Remote_Init() {
  // Phase 1: Pure static resource and timing initialization (Zero socket calls)
  s_mgmt_server_fd = -1;
  for (int i = 0; i < Config::TCP::MAX_MGMT_CLIENTS; i++) {
    s_mgmt_sessions[i].sock = -1;
    s_mgmt_sessions[i].len = 0;
    s_mgmt_sessions[i].connected_at_ms = 0;
  }

  s_chk_ms = millis();
  s_met_ms = millis();
  s_tcp_ms = millis();

  ProtocolTcpParticipant p;
  p.name = "RemoteService";
  p.populateFds = Remote_PopulateFds;
  p.processEvents = Remote_ProcessEvents;
  p.tick = Remote_Tick;
  ProtocolDiag_RegisterTcpParticipant(p);
}
