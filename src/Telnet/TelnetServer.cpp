#include "TelnetCli.h"
#include "esp_task_wdt.h"
#include <cstdio>
#include <cstring>
#include <sys/socket.h>
#include <fcntl.h>

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

    bool did_read = false;
    if (FD_ISSET(s.sock, &readfds)) {
      char rx_buf[128];
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

    // readfds를 처리한 tick에서는 writefds 전송을 건너뜀:
    // onClientData(→ cmdCtl → sendTelnetMsgLen)가 이미 소켓에 출력했고,
    // select 반환 당시의 txLen(stale EmbeddedCli echo bytes)이 테이블 사이로
    // 끼어드는 것을 방지한다.
    if (!did_read && FD_ISSET(s.sock, &writefds) && s.txLen > 0) {
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

    if (g_restart_pending.load(std::memory_order_acquire)) {
      g_restart_pending.store(false, std::memory_order_relaxed);
      System_Restart(g_restart_reason ? g_restart_reason : "Telnet Command");
    }

    g_telnet_manager.tick();

    if (g_telnet_manager.hasActiveClients()) {
      g_telnet_tracer.flushToClient();
      xSemaphoreTake(g_tracer_sem, pdMS_TO_TICKS(5));
    } else {
      xSemaphoreTake(g_tracer_sem, 0);
    }
  }
}

