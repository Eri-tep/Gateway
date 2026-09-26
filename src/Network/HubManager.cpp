#include "NetworkInternal.h"
#include "Ew11Manager.h"
#include <esp_log.h>
#include <cstring>
#include <algorithm>

void Tcp_EnableKeepalive(int sock, int idle, int intvl, int cnt) {
  if (sock < 0)
    return;
  int keepalive = 1;
  setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
  setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
  setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
  setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
}

int Hub_AcceptClient(int slot_idx, int server_fd) {
  if (slot_idx < 0 || slot_idx >= Config::TCP::MAX_EW11_SLOTS || server_fd < 0)
    return -1;

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

  char client_ip_str[16];
  snprintf(client_ip_str, sizeof(client_ip_str), "%u.%u.%u.%u", b[0], b[1], b[2],
           b[3]);

  MutexLocker lock(g_ch5_mutex);
  auto &slot = g_hub_slots[slot_idx];

  if (slot.target_ip[0] != '\0' && strcmp(slot.target_ip, client_ip_str) != 0) {
    ESP_LOGW("EW11", "[CH5] Client IP mismatch for Slot %d (%s): got %s, expected %s",
             slot_idx, slot.name, client_ip_str, slot.target_ip);
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
  Tcp_EnableKeepalive(new_sock, Config::TCP::DEFAULT_KEEPALIVE_IDLE_SEC,
                      Config::TCP::DEFAULT_KEEPALIVE_INTVL_SEC,
                      Config::TCP::DEFAULT_KEEPALIVE_CNT);

  if (slot.sock >= 0) {
    close(slot.sock);
  }
  slot.sock = new_sock;
  slot.is_connected = true;
  slot.rx_len = 0;
  slot.last_rx_ms = millis();
  if (slot.target_ip[0] == '\0') {
    strncpy(slot.target_ip, client_ip_str, sizeof(slot.target_ip) - 1);
    slot.target_ip[sizeof(slot.target_ip) - 1] = '\0';
  }

  g_pkt_stats.ch5.is_connected.store(true, std::memory_order_relaxed);
  g_pkt_stats.ch5.connection_count.fetch_add(1, std::memory_order_relaxed);
  ESP_LOGI("EW11", "[CH5] Accepted EW11 client %s on port %u -> Slot %d (%s)",
           client_ip_str, slot.target_port, slot_idx, slot.name);
  return new_sock;
}

void Hub_ProcessPacket(HubClientSlot *slot, const uint8_t *pkt_data,
                        size_t pkt_len) {
  if (!slot || !pkt_data || pkt_len == 0)
    return;

  StaticPacket pkt{5, static_cast<uint8_t>(pkt_len)};
  std::copy(pkt_data, pkt_data + pkt_len, pkt.data.begin());

  slot->rx_pkts++;
  g_pkt_stats.ch5.rx_pkts.fetch_add(1, std::memory_order_relaxed);
  g_telnet_tracer.trace(5, false, TraceType::RMT, pkt);

  auto *parser = WallpadParserFactory::getActiveParser();
  if (parser) {
    span<const uint8_t> frame(pkt_data, pkt_len);
    uint8_t dev_id = 0, sub1 = 0, sub2 = 0;
    if (parser->extractDeviceKey(frame, dev_id, sub1, sub2) && dev_id != 0 &&
        dev_id != parser->getStx() && dev_id != parser->getEtx() &&
        dev_id != 0xFF) {
      int8_t s_idx = static_cast<int8_t>(slot - g_hub_slots);
      g_route_registry.recordRoute(5, s_idx, dev_id, sub1, sub2);

      bool is_query = parser->isQueryPacket(frame);
      bool is_ack   = (pkt_len >= 5 && frame[4] == 0x04); // 표준 ACK Opcode(0x04)

      // 0x2A (신발장 서브 패널 / 원격검침)는 폴링 대상 및 단말 제어 기기가 아니므로 캐시에서 완전 제외
      if (dev_id != 0x2A) {
        if (is_query) {
          // CH5 쿼리 패킷: Cache1 (타겟 레지스트리) 등록 (CH1 물리 폴링 대상에서는 자동 제외)
          g_polling_targets.registerOrTouch(5, dev_id, sub1, sub2, pkt_data, pkt_len);
        }
        if (is_ack) {
          // CH5 응답 패킷: Cache2 (기기 상태 레포지토리) 등록 및 상태 브로드캐스트
          g_device_repo.updateFromBus(pkt);
        }
      }
    }
  }

  // EW11 격리 매니저 호출 (Slot 0 엘리베이터 상태 전환/도착 감지 & Slot 1~4 에어컨)
  int slot_idx = static_cast<int>(slot - g_hub_slots);
  Ew11Manager::processPacket(slot_idx, pkt_data, pkt_len);
}


void Hub_Data(HubClientSlot *slot, const uint8_t *data, size_t len) {
  if (!slot || slot->sock < 0 || !data || len == 0)
    return;

  slot->last_rx_ms = millis();

  if (slot->rx_len + len > sizeof(slot->rx_buf)) {
    slot->rx_len = 0;
  }
  std::copy(data, data + len, slot->rx_buf + slot->rx_len);
  slot->rx_len += len;

  int slot_idx = static_cast<int>(slot - g_hub_slots);
  Ew11Manager::processStream(slot_idx, slot);
}

void Hub_LoadConfig() {
  Preferences p;
  p.begin("ew11-config", true);
  MutexLocker lock(g_ch5_mutex);

  g_hub_slots[0].enabled = p.getBool("e0_en", true);
  p.getString("e0_name", "Elevator")
      .toCharArray(g_hub_slots[0].name, sizeof(g_hub_slots[0].name));
  p.getString("e0_ip", "172.30.1.245")
      .toCharArray(g_hub_slots[0].target_ip,
                   sizeof(g_hub_slots[0].target_ip));
  uint16_t p0 = p.getUShort("e0_port", 8898);
  if (p0 == 0 || p0 == 8899) p0 = 8898; // 기존 구버전 기본값 마이그레이션
  g_hub_slots[0].target_port = p0;
  g_hub_slots[0].dev_type = HubDeviceType::WALLPAD_COMPATIBLE;
  g_hub_slots[0].sock = -1;
  g_hub_slots[0].is_connected = false;
  g_hub_slots[0].rx_len = 0;

  for (int i = 1; i < Config::TCP::MAX_EW11_SLOTS; i++) {
    char k_en[8], k_nm[8], k_ip[8], k_pt[8], def_nm[16];
    snprintf(k_en, sizeof(k_en), "e%d_en", i);
    snprintf(k_nm, sizeof(k_nm), "e%d_name", i);
    snprintf(k_ip, sizeof(k_ip), "e%d_ip", i);
    snprintf(k_pt, sizeof(k_pt), "e%d_port", i);
    snprintf(def_nm, sizeof(def_nm), "AC_%d", i);

    g_hub_slots[i].enabled = p.getBool(k_en, false);
    p.getString(k_nm, def_nm)
        .toCharArray(g_hub_slots[i].name, sizeof(g_hub_slots[i].name));
    p.getString(k_ip, "").toCharArray(g_hub_slots[i].target_ip,
                                      sizeof(g_hub_slots[i].target_ip));
    uint16_t def_slot_port = Config::TCP::EW11_SLOT_PORTS[i]; // 8891, 8892, 8893, 8894
    uint16_t pi = p.getUShort(k_pt, def_slot_port);
    if (pi == 0 || pi == 8899) pi = def_slot_port; // 기존 구버전 기본값 마이그레이션
    g_hub_slots[i].target_port = pi;
    g_hub_slots[i].dev_type = HubDeviceType::AIR_CONDITIONER;
    g_hub_slots[i].sock = -1;
    g_hub_slots[i].is_connected = false;
    g_hub_slots[i].rx_len = 0;
  }
  for (int s = 0; s < Config::TCP::MAX_EW11_SLOTS; s++) {
    char ns[16], tag[16];
    snprintf(ns, sizeof(ns), "e%d_frame", s);
    snprintf(tag, sizeof(tag), "EW11_#%d", s);
    g_hub_slots[s].tracker.restoreFromNvs(ns, tag);
  }
  p.end();
}

void Hub_SaveConfig() {
  Preferences p;
  p.begin("ew11-config", false);
  MutexLocker lock(g_ch5_mutex);

  for (int i = 0; i < Config::TCP::MAX_EW11_SLOTS; i++) {
    char k_en[8], k_nm[8], k_ip[8], k_pt[8];
    snprintf(k_en, sizeof(k_en), "e%d_en", i);
    snprintf(k_nm, sizeof(k_nm), "e%d_name", i);
    snprintf(k_ip, sizeof(k_ip), "e%d_ip", i);
    snprintf(k_pt, sizeof(k_pt), "e%d_port", i);

    p.putBool(k_en, g_hub_slots[i].enabled);
    p.putString(k_nm, g_hub_slots[i].name);
    p.putString(k_ip, g_hub_slots[i].target_ip);
    p.putUShort(k_pt, g_hub_slots[i].target_port);
  }
  p.end();
}

bool Hub_SetSlot(uint8_t slot_idx, bool enabled, const char *ip, uint16_t port,
                 const char *name) {
  if (slot_idx >= Config::TCP::MAX_EW11_SLOTS)
    return false;

  MutexLocker lock(g_ch5_mutex);
  auto &slot = g_hub_slots[slot_idx];

  bool reconnect_needed = false;
  if (slot.enabled != enabled || strcmp(slot.target_ip, ip ? ip : "") != 0 ||
      slot.target_port != port) {
    reconnect_needed = true;
  }

  slot.enabled = enabled;
  if (ip) {
    strncpy(slot.target_ip, ip, sizeof(slot.target_ip) - 1);
    slot.target_ip[sizeof(slot.target_ip) - 1] = '\0';
  } else {
    slot.target_ip[0] = '\0';
  }
  if (port > 0) {
    slot.target_port = port;
  }
  if (name && strlen(name) > 0) {
    strncpy(slot.name, name, sizeof(slot.name) - 1);
    slot.name[sizeof(slot.name) - 1] = '\0';
  }

  if (reconnect_needed && slot.sock >= 0) {
    close(slot.sock);
    slot.sock = -1;
    slot.is_connected = false;
    slot.rx_len = 0;
    slot.last_reconnect_ms = 0; // 즉시 재연결 유도
  }

  Hub_SaveConfig();
  return true;
}

bool Hub_SendPacket(uint8_t slot_idx, const StaticPacket &pkt) {
  if (slot_idx >= Config::TCP::MAX_EW11_SLOTS)
    return false;
  MutexLocker lock(g_ch5_mutex);
  auto &slot = g_hub_slots[slot_idx];
  if (!slot.enabled || slot.sock < 0 || !slot.is_connected)
    return false;

  int s = send(slot.sock, pkt.data.data(), pkt.length, MSG_DONTWAIT);
  if (s == static_cast<int>(pkt.length)) {
    slot.tx_pkts++;
    g_pkt_stats.ch5.tx_pkts.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  return false;
}
