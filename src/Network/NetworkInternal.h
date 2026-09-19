#pragma once

#include "Common.h"
#include "MgmtRpc.h"
#include "TelnetCli.h"
#include "WallpadParser.h"
#include "ControlTemplate.h"
#include <lwip/ip.h>
#include <lwip/tcp.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <WiFi.h>

extern const char *s_pending_reboot_reason;
extern EventGroupHandle_t g_wifi_event_group;
static constexpr EventBits_t WIFI_BIT_CONNECTED = BIT0;
static constexpr EventBits_t WIFI_BIT_DISCONNECTED = BIT1;
static constexpr EventBits_t WIFI_BIT_GOT_IP = BIT2;

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
void Tcp_PollAndReceive(SessionType (&sessions)[N],
                       SemaphoreHandle_t mux, fd_set &readfds,
                       fd_set &errorfds, DataHandler handler) {
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
                           int keepalive_intvl, int keepalive_cnt,
                           TcpSocketStats &stat) {
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
  stat.is_connected.store(true, std::memory_order_relaxed);
  stat.connection_count.fetch_add(1, std::memory_order_relaxed);
  return new_sock;
}

int Hub_AcceptClient(int slot_idx, int server_fd);
void Hub_Data(HubClientSlot *slot, const uint8_t *data, size_t len);
