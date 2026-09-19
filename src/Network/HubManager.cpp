#include "NetworkInternal.h"
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

  // 1) 특정 허용 IP가 지정되어 있는 경우 일치 여부 검사 (미지정이거나 비어있으면 모든 사설 IP 허용)
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

  // 기존 연결이 열려있다면 안전하게 닫고 새 연결로 교체
  if (slot.sock >= 0) {
    close(slot.sock);
  }
  slot.sock = new_sock;
  slot.is_connected = true;
  slot.rx_len = 0;
  slot.last_rx_ms = millis();
  // IP 필터가 비어있던 경우 접속된 EW11 IP 자동 등록
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

static void Hub_ProcessPacket(HubClientSlot *slot, const uint8_t *pkt_data,
                               size_t pkt_len) {
  if (!slot || !pkt_data || pkt_len == 0)
    return;

  StaticPacket pkt{5, static_cast<uint8_t>(pkt_len)};
  std::copy(pkt_data, pkt_data + pkt_len, pkt.data.begin());

  slot->rx_pkts++;
  g_pkt_stats.ch5.rx_pkts.fetch_add(1, std::memory_order_relaxed);
  g_telnet_tracer.trace(5, false, TraceType::RMT, pkt);

  // [동적 라우팅 학습 및 1차/2차 캐시 페어링 & 위저드 파이프라인]
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
      bool is_ack = parser->isAckPacket(frame);

      if (is_query) {
        // 1차 캐시 등록 및 갱신 (위저드로 절대 보내지 않음!)
        g_polling_targets.registerOrTouch(5, dev_id, sub1, sub2, pkt_data,
                                          pkt_len);
        slot->last_query_len = static_cast<uint8_t>(
            std::min<size_t>(pkt_len, sizeof(slot->last_query_data)));
        memcpy(slot->last_query_data, pkt_data, slot->last_query_len);
      } else if (is_ack) {
        // 2차 캐시 페어링 및 디바이스 레포지토리 최신화
        if (slot->last_query_len > 0) {
          g_polling_targets.updateResponse(
              slot->last_query_data, slot->last_query_len, pkt_data, pkt_len);
          g_polling_targets.markVerified(dev_id, sub1, sub2);
        }

        // [CH5/EW11 제어 트랜잭션 수렴] 방금 전송된 제어 명령(last_ctrl)에
        // 대한 ACK 응답인 경우 학습기에 주입!
        if (slot->last_ctrl_len > 0 &&
            millis() - slot->last_ctrl_tx_ms < 2000) {
          StaticPacket ctrl_pkt{5, slot->last_ctrl_len};
          memcpy(ctrl_pkt.data.data(), slot->last_ctrl_data,
                 slot->last_ctrl_len);

          uint8_t c_dev = 0, c_s1 = 0, c_s2 = 0;
          if (parser->extractDeviceKey(
                  span<const uint8_t>(ctrl_pkt.data.data(), ctrl_pkt.length),
                  c_dev, c_s1, c_s2) &&
              c_dev == dev_id) {
            StaticPacket ack_before{};
            const auto *cached = g_device_repo.find(dev_id, sub1, sub2);
            if (cached && cached->last_ack_len > 0) {
              ack_before.channel_id = 5;
              ack_before.length = cached->last_ack_len;
              std::copy(cached->last_ack_data.begin(),
                        cached->last_ack_data.begin() + cached->last_ack_len,
                        ack_before.data.begin());
            }
            AckSlotHint hint = g_telnet_manager.peekWizardHint(dev_id);
            g_control_registry.onControlTransaction(ctrl_pkt, ack_before, pkt,
                                                    hint);
            g_telnet_manager.notifyControlTransaction(dev_id);
            slot->last_ctrl_len = 0; // 1회 트랜잭션 소비 완료
          }
        } else {
          // [Causal Differential Event] 게이트웨이 송신 제어가 아니더라도,
          // 외부(월패드/물리버튼) 물리 조작으로 인해 직전 상태와 바이트 차이(Diff)가 발생한 경우
          // 특정 기기 ID나 토큰 하드코딩 없이 비주기 돌발 이벤트로 자동 학습 및 위저드에 전달!
          const auto *cached = g_device_repo.find(dev_id, sub1, sub2);
          if (cached && cached->last_ack_len > 0) {
            bool state_changed = (cached->last_ack_len != pkt_len ||
                                  memcmp(cached->last_ack_data.data(), pkt_data, pkt_len) != 0);
            if (state_changed) {
              StaticPacket ack_before{};
              ack_before.channel_id = 5;
              ack_before.length = cached->last_ack_len;
              std::copy(cached->last_ack_data.begin(),
                        cached->last_ack_data.begin() + cached->last_ack_len,
                        ack_before.data.begin());
              AckSlotHint hint = g_telnet_manager.peekWizardHint(dev_id);
              g_control_registry.onControlTransaction(pkt, ack_before, pkt, hint);
              g_telnet_manager.notifyControlTransaction(dev_id);
            }
          }
        }

        g_device_repo.updateFromBus(pkt);
      } else {
        // QRY도 ACK도 아닌 비표준 제어(CTL) 또는 돌발 이벤트
        AckSlotHint hint = g_telnet_manager.peekWizardHint(dev_id);
        StaticPacket dummy_before{};
        g_control_registry.onControlTransaction(pkt, dummy_before, pkt, hint);
        g_telnet_manager.notifyControlTransaction(dev_id);
      }
    }
  }
}

void Hub_Data(HubClientSlot *slot, const uint8_t *data, size_t len) {
  if (!slot || slot->sock < 0 || !data || len == 0)
    return;

  slot->last_rx_ms = millis();

  // 슬롯 버퍼 오버플로우 방어
  if (slot->rx_len + len > sizeof(slot->rx_buf)) {
    slot->rx_len = 0;
  }
  std::copy(data, data + len, slot->rx_buf + slot->rx_len);
  slot->rx_len += len;

  int slot_idx = static_cast<int>(slot - g_hub_slots);

  // ★ [소켓 0: 월패드 서브기기 통신] 채널 1, 2, 3, 6과 100% 동일한 공통 파서 및
  // 무결성 검증 파이프라인
  if (slot_idx == 0) {
    auto *parser = WallpadParserFactory::getActiveParser();
    uint8_t stx = parser ? parser->getStx() : PKT_STX;

    size_t p = 0;
    size_t loop_count = 0;
    while (p < slot->rx_len && ++loop_count < 256) {
      if (slot->rx_buf[p] != stx) {
        p++;
        continue;
      }

      int len_res =
          parser ? parser->extractPacketLength(slot->rx_buf, slot->rx_len, p)
                 : -1;
      if (len_res == 0) {
        // 미완성 패킷 (추가 데이터 수신 대기)
        break;
      }
      if (len_res < 0) {
        // 헤더 규격 불일치/노이즈
        p++;
        continue;
      }

      uint8_t p_len = static_cast<uint8_t>(len_res);
      if (p_len == 0) {
        p++;
        continue;
      }

      span<const uint8_t> frame(&slot->rx_buf[p], p_len);
      if (!parser->validatePacket(frame)) {
        StaticPacket drp_pkt{5, p_len};
        std::copy(&slot->rx_buf[p], &slot->rx_buf[p + p_len],
                  drp_pkt.data.begin());
        g_telnet_tracer.trace(5, false, TraceType::DRP, drp_pkt);
        g_pkt_stats.ch5.dropped_pkts.fetch_add(1, std::memory_order_relaxed);
        p += p_len;
        continue;
      }

      Hub_ProcessPacket(slot, &slot->rx_buf[p], p_len);
      p += p_len;
    }

    if (p > 0) {
      slot->rx_len -= p;
      if (slot->rx_len > 0) {
        memmove(slot->rx_buf, slot->rx_buf + p, slot->rx_len);
      }
    }
    return;
  }

  // ★ [소켓 1~4: 향후 에어컨 등 이종 프로토콜 전용 트래커]
  char ns[16], tag[16];
  snprintf(ns, sizeof(ns), "e%d_frame", slot_idx);
  snprintf(tag, sizeof(tag), "EW11_#%d", slot_idx);

  auto f_status = slot->tracker.status.load(std::memory_order_relaxed);
  uint8_t c_stx = slot->tracker.candidate_stx.load(std::memory_order_relaxed);
  uint8_t c_etx = slot->tracker.candidate_etx.load(std::memory_order_relaxed);
  uint8_t c_len = slot->tracker.candidate_len.load(std::memory_order_relaxed);

  uint8_t target_stx = (c_stx != 0) ? c_stx : PKT_STX;
  uint8_t target_etx = (c_etx != 0) ? c_etx : PKT_ETX;

  size_t p = 0;
  while (p < slot->rx_len) {
    if (slot->rx_buf[p] != target_stx) {
      p++;
      continue;
    }

    // 1) 고정 길이 후보가 학습된 경우
    if (c_len >= 3 && (p + c_len) <= slot->rx_len) {
      if (slot->rx_buf[p + c_len - 1] == target_etx) {
        slot->tracker.processFrame(target_stx, target_etx, c_len, ns, tag);
        Hub_ProcessPacket(slot, &slot->rx_buf[p], c_len);
        p += c_len;
        continue;
      }
    }

    // 2) ETX 기반 패킷 길이 탐색 (최대 64B)
    size_t end_idx = 0;
    bool found_frame = false;
    for (size_t k = p + 2; k < slot->rx_len && (k - p) < 64; ++k) {
      if (slot->rx_buf[k] == target_etx) {
        end_idx = k;
        found_frame = true;
        break;
      }
    }

    if (found_frame) {
      uint8_t frame_len = static_cast<uint8_t>(end_idx - p + 1);
      slot->tracker.processFrame(target_stx, target_etx, frame_len, ns, tag);
      Hub_ProcessPacket(slot, &slot->rx_buf[p], frame_len);
      p += frame_len;
    } else {
      if (slot->rx_len - p < 64) {
        break;
      } else {
        p++;
      }
    }
  }

  if (p > 0) {
    slot->rx_len -= p;
    if (slot->rx_len > 0) {
      memmove(slot->rx_buf, slot->rx_buf + p, slot->rx_len);
    }
  }
}

void Hub_LoadConfig() {
  Preferences p;
  p.begin("ew11-config", true);
  MutexLocker lock(g_ch5_mutex);

  // 기본값 설정
  // Slot 0: Elevator (기본 포트: 8898)
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

  // Slot 1~4: AC 1~4 (기본 포트: 8891~8894)
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
