#include "Bridge.h"
#include "Core.h"
#include "Protocol.h"
#include "Service.h"
#include "Console.h"

#include <Preferences.h>
#include <WiFi.h>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <esp_log.h>
#include <esp_timer.h>
#include <fcntl.h>
#include <lwip/ip.h>
#include <lwip/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

// From src/Network/HubManager.cpp
// ============================================================================
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
  Tcp_EnableKeepalive(new_sock, 30,
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
    uint8_t stx = slot->tracker.candidate_stx.load(std::memory_order_relaxed);
    if (stx == 0)
      stx = PKT_STX;
    size_t stx_pos = 0;
    while (stx_pos < slot->rx_len && slot->rx_buf[stx_pos] != stx) {
      stx_pos++;
    }
    if (stx_pos > 0 && stx_pos < slot->rx_len) {
      memmove(slot->rx_buf, slot->rx_buf + stx_pos, slot->rx_len - stx_pos);
      slot->rx_len -= stx_pos;
    } else if (stx_pos >= slot->rx_len) {
      slot->rx_len = 0;
    }
  }

  size_t copy_len = std::min(len, sizeof(slot->rx_buf) - slot->rx_len);
  std::copy(data, data + copy_len, slot->rx_buf + slot->rx_len);
  slot->rx_len += copy_len;

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

  bool changed = (slot.enabled != enabled) ||
                 (strcmp(slot.target_ip, ip ? ip : "") != 0) ||
                 (port > 0 && slot.target_port != port) ||
                 (name && strlen(name) > 0 && strcmp(slot.name, name) != 0);

  if (!changed) {
    return true; // 변경 사항 없음
  }

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

// ============================================================================
// From src/Network/Ew11Manager.cpp
// ============================================================================
#include <esp_timer.h>
#include <esp_log.h>
#include <atomic>
#include <cstring>
#include <algorithm>

static const char *TAG = "Ew11Mgr";

namespace Ew11Manager {

namespace {

struct BurstTxFsm {
  portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
  StaticPacket pkt{};
  uint8_t target_slot{0};
  uint8_t remaining_count{0};
  uint32_t silence_ms{20};
  uint32_t last_tx_ms{0};
  esp_timer_handle_t timer{nullptr};
};

static BurstTxFsm s_burst_fsm;

static void onBurstTimer(void *arg) {
  (void)arg;
  StaticPacket tx_pkt{};
  uint8_t slot = 0;
  uint32_t silence_req_ms = 20;

  portENTER_CRITICAL(&s_burst_fsm.mux);
  if (s_burst_fsm.remaining_count == 0) {
    portEXIT_CRITICAL(&s_burst_fsm.mux);
    return;
  }
  tx_pkt = s_burst_fsm.pkt;
  slot = s_burst_fsm.target_slot;
  silence_req_ms = s_burst_fsm.silence_ms;
  uint32_t last_tx = s_burst_fsm.last_tx_ms;
  portEXIT_CRITICAL(&s_burst_fsm.mux);

  // 1. 선로 상태 확인 (RX 및 이전 TX 기준 20ms 침묵 여부 점검)
  uint32_t last_rx = 0;
  {
    MutexLocker lock(g_ch5_mutex);
    const auto &slot_info = g_hub_slots[slot];
    if (!slot_info.enabled || slot_info.sock < 0 || !slot_info.is_connected) {
      portENTER_CRITICAL(&s_burst_fsm.mux);
      s_burst_fsm.remaining_count = 0;
      portEXIT_CRITICAL(&s_burst_fsm.mux);
      return;
    }
    last_rx = slot_info.last_rx_ms;
  }

  uint32_t now = millis();
  uint32_t rx_elapsed = (now >= last_rx) ? (now - last_rx) : 0;
  uint32_t tx_elapsed = (now >= last_tx) ? (now - last_tx) : 0;

  uint32_t rx_rem_ms = (rx_elapsed < silence_req_ms) ? (silence_req_ms - rx_elapsed) : 0;
  uint32_t tx_rem_ms = (last_tx > 0 && tx_elapsed < silence_req_ms) ? (silence_req_ms - tx_elapsed) : 0;
  uint32_t wait_ms = std::max(rx_rem_ms, tx_rem_ms);

  // 2. 선로에 다른 패킷이 유입되었거나 이전 전송 후 20ms가 지나지 않은 경우 동적 연장 대기 (충돌 100% 회피)
  if (wait_ms > 0) {
    portENTER_CRITICAL(&s_burst_fsm.mux);
    if (s_burst_fsm.remaining_count > 0) {
      esp_timer_start_once(s_burst_fsm.timer, wait_ms * 1000);
    }
    portEXIT_CRITICAL(&s_burst_fsm.mux);
    return;
  }

  // 3. 선로가 20ms 이상 완전히 유휴인 상태 확인 -> 패킷 전송
  bool sent = Hub_SendPacket(slot, tx_pkt);
  g_telnet_tracer.trace(5, true, sent ? TraceType::CTL : TraceType::DRP, tx_pkt);

  portENTER_CRITICAL(&s_burst_fsm.mux);
  s_burst_fsm.last_tx_ms = millis();
  if (s_burst_fsm.remaining_count > 0) {
    s_burst_fsm.remaining_count--;
  }

  // 다음 회차가 남아있다면 20ms 후 동적 유휴 확인 타이머 스케줄
  if (s_burst_fsm.remaining_count > 0) {
    esp_timer_start_once(s_burst_fsm.timer, silence_req_ms * 1000);
  }
  portEXIT_CRITICAL(&s_burst_fsm.mux);
}

} // anonymous namespace

void init() {
  if (!s_burst_fsm.timer) {
    esp_timer_create_args_t timer_args{};
    timer_args.callback = onBurstTimer;
    timer_args.arg = nullptr;
    timer_args.name = "ew11_burst_timer";
    esp_timer_create(&timer_args, &s_burst_fsm.timer);
  }
}

bool sendBurstPacket(uint8_t slot_idx, const StaticPacket &pkt, uint8_t count, uint32_t silence_ms) {
  if (slot_idx >= Config::TCP::MAX_EW11_SLOTS || count == 0) return false;
  if (!s_burst_fsm.timer) init();

  // 기존 진행 중인 타이머 취소
  esp_timer_stop(s_burst_fsm.timer);

  {
    MutexLocker lock(g_ch5_mutex);
    const auto &slot = g_hub_slots[slot_idx];
    if (!slot.enabled || slot.sock < 0 || !slot.is_connected) {
      return false;
    }
  }

  portENTER_CRITICAL(&s_burst_fsm.mux);
  s_burst_fsm.pkt = pkt;
  s_burst_fsm.target_slot = slot_idx;
  s_burst_fsm.remaining_count = count;
  s_burst_fsm.silence_ms = silence_ms;
  s_burst_fsm.last_tx_ms = 0;
  portEXIT_CRITICAL(&s_burst_fsm.mux);

  // 즉시 onBurstTimer를 1ms 후 발화시켜 선로 20ms 유휴 검증 파이프라인으로 진입
  esp_timer_start_once(s_burst_fsm.timer, 1000); // 1ms (1000us)
  return true;
}

void processPacket(int slot_idx, const uint8_t *pkt_data, size_t pkt_len) {
  if (!pkt_data || pkt_len == 0) return;

  // --------------------------------------------------------------------------
  // Slot 0: 엘리베이터 (0x34) 전용 처리
  // --------------------------------------------------------------------------
  if (slot_idx == 0) {
    auto *parser = WallpadParserFactory::getActiveParser();
    if (!parser) return;

    span<const uint8_t> frame(pkt_data, pkt_len);
    uint8_t dev_id = 0, sub1 = 0, sub2 = 0;
    if (parser->extractDeviceKey(frame, dev_id, sub1, sub2) && dev_id == 0x34) {
      // EW11 Slot 0 수신 시 L2 라우터에 CH5 Slot 0 경로 자동 학습/등록
      g_route_registry.recordRoute(5, 0, 0x34, sub1, sub2);

      // 1) 11-byte 상태 응답 (호출 ACK / 대기 복귀)
      // data[4] == 0x04: Byte #8 == 0x06 (호출 이동 중 / ON), 0x00 (대기 복귀 / OFF)
      if (pkt_len == 11 && pkt_data[4] == 0x04) {
        static std::atomic<uint8_t> s_last_elev_pwr{0xFF};
        uint8_t new_pwr = (pkt_data[8] == 0x06) ? 1 : 0;
        uint8_t prev = s_last_elev_pwr.exchange(new_pwr, std::memory_order_acq_rel);
        if (prev != new_pwr) {
          ESP_LOGI(TAG, "Elevator State Changed -> Power: %u", new_pwr);
          Mgmt_BroadcastDeviceState(0x34, sub1, sub2, DeviceClass::MOMENTARY,
                                    new_pwr, 0, 0, 0, nullptr, 0.0f, 15, 0, 0);
        }
      }
      // 2) 13-byte 도착 감지 브로드캐스트
      // data[4] == 0x01 && data[8] == 0x01: Byte #9 = 댁내 층수, Byte #10 = 호기 번호
      else if (pkt_len == 13 && pkt_data[4] == 0x01 && pkt_data[8] == 0x01) {
        uint8_t floor = pkt_data[9];
        uint8_t ho = pkt_data[10];
        ESP_LOGI(TAG, "Elevator Arrived -> Floor: %u, Car: %u", floor, ho);
        Mgmt_BroadcastDeviceState(0x34, sub1, sub2, DeviceClass::MOMENTARY,
                                  0, 0, 0, 0, nullptr, 0.0f, floor, 0, ho);
      }
    }
    return;
  }

  // --------------------------------------------------------------------------
  // Slot 1~4: 시스템 에어컨 (1~4호) 독자 프로토콜 처리
  // --------------------------------------------------------------------------
#if CONFIG_LOG_DEFAULT_LEVEL >= ESP_LOG_DEBUG
  ESP_LOGD(TAG, "EW11 Slot %d RX len=%u", slot_idx, (unsigned)pkt_len);
#endif
}

void processStream(int slot_idx, HubClientSlot *slot) {
  if (!slot) return;

  if (slot_idx == 0) {
    // Slot 0 (엘리베이터): 현대통신 월패드 호환 프레이밍
    auto *parser = WallpadParserFactory::getActiveParser();
    uint8_t stx = parser ? parser->getStx() : PKT_STX;

    size_t p = 0;
    size_t loop_count = 0;
    while (p < slot->rx_len && ++loop_count < 256) {
      if (slot->rx_buf[p] != stx) {
        p++;
        continue;
      }

      int len_res = parser ? parser->extractPacketLength(slot->rx_buf, slot->rx_len, p) : -1;
      if (len_res == 0) break;
      if (len_res < 0) {
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
        std::copy(&slot->rx_buf[p], &slot->rx_buf[p + p_len], drp_pkt.data.begin());
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

  // Slot 1~4 (FCU 시스템 에어컨): Modbus-RTU 19B 상태 응답 or 8B ACK 처리
  // Modbus Slave는 0x01로 시작 (0x01 0x03 0x0E ... 19B 또는 0x01 0x06/0x10 ... 8B)
  size_t p = 0;
  while (p < slot->rx_len) {
    if (slot->rx_buf[p] != 0x01) {
      p++;
      continue;
    }

    size_t rem = slot->rx_len - p;
    // 19바이트 0x03 상태 응답
    if (rem >= 19 && slot->rx_buf[p + 1] == 0x03 && slot->rx_buf[p + 2] == 0x0E) {
      slot->rx_pkts++;
      g_pkt_stats.ch5.rx_pkts.fetch_add(1, std::memory_order_relaxed);
      Fcu::handleSlotRx(static_cast<uint8_t>(slot_idx), &slot->rx_buf[p], 19);
      p += 19;
      continue;
    }

    // 8바이트 0x06/0x10 제어 ACK
    if (rem >= 8 && (slot->rx_buf[p + 1] == 0x06 || slot->rx_buf[p + 1] == 0x10)) {
      slot->rx_pkts++;
      g_pkt_stats.ch5.rx_pkts.fetch_add(1, std::memory_order_relaxed);
      Fcu::handleSlotRx(static_cast<uint8_t>(slot_idx), &slot->rx_buf[p], 8);
      p += 8;
      continue;
    }

    // 아직 충분한 바이트가 도착하지 않은 경우 대기
    if (rem < 19) {
      break;
    }

    // 0x01이지만 Modbus FC가 아닌 경우 다음 바이트로 이동
    p++;
  }

  if (p > 0) {
    slot->rx_len -= p;
    if (slot->rx_len > 0) {
      memmove(slot->rx_buf, slot->rx_buf + p, slot->rx_len);
    }
  }
}

} // namespace Ew11Manager

// ============================================================================
// namespace Fcu 구현 (AP FCU Modbus-RTU 엔진)
// ============================================================================
namespace {

// ── FCU 슬롯별 독립 런타임 (인덱스 0은 엘리베이터 슬롯이므로 미사용) ──
static Fcu::SlotRuntime s_fcu_slots[Config::TCP::MAX_EW11_SLOTS]{};

// §4.1 Modbus-RTU CRC16 계산 (Zero-Heap)
static uint16_t Fcu_CalcCrc16(const uint8_t *buf, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t pos = 0; pos < len; pos++) {
    crc ^= static_cast<uint16_t>(buf[pos]);
    for (int i = 8; i != 0; i--) {
      if ((crc & 0x0001) != 0) {
        crc >>= 1;
        crc ^= 0xA001;
      } else {
        crc >>= 1;
      }
    }
  }
  return crc;
}

// §4.1 상태 조회 쿼리 (8B, CRC 포함)
static const uint8_t kFcuQueryPkt[8] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x07, 0x04, 0x08};

// §4.2 전원 OFF: 풍량(Reg 0x0002) = 0 (8B, CRC 포함)
static const uint8_t kFcuPowerOff[8] = {0x01, 0x06, 0x00, 0x02, 0x00, 0x00, 0x28, 0x0A};

// §4.2 전원 ON: Reg 0x0001(모드), 0x0002(풍량), 0x0003(스윙) 일괄 (15B 동적 생성)
// (Reg0x0000 절대 보존 — IR 리모컨 공존)
static size_t Fcu_BuildPowerOn(uint8_t *out_buf, uint16_t mode, uint16_t fan, uint16_t swing) {
  out_buf[0] = 0x01; // Slave ID
  out_buf[1] = 0x10; // FC 0x10 (Write Multiple Registers)
  out_buf[2] = 0x00; out_buf[3] = 0x01; // 시작 번지 0x0001 (Reg0 절대 보존)
  out_buf[4] = 0x00; out_buf[5] = 0x03; // 레지스터 개수 3개
  out_buf[6] = 0x06;                     // 바이트 수 6바이트
  out_buf[7] = static_cast<uint8_t>((mode >> 8) & 0xFF);
  out_buf[8] = static_cast<uint8_t>(mode & 0xFF);
  out_buf[9] = static_cast<uint8_t>((fan >> 8) & 0xFF);
  out_buf[10] = static_cast<uint8_t>(fan & 0xFF);
  out_buf[11] = static_cast<uint8_t>((swing >> 8) & 0xFF);
  out_buf[12] = static_cast<uint8_t>(swing & 0xFF);
  uint16_t crc = Fcu_CalcCrc16(out_buf, 13);
  out_buf[13] = static_cast<uint8_t>(crc & 0xFF);
  out_buf[14] = static_cast<uint8_t>((crc >> 8) & 0xFF);
  return 15;
}

// §4.3~§4.6 단일 레지스터 쓰기 (8B 동적 생성)
static size_t Fcu_BuildWriteSingle(uint8_t *out_buf, uint16_t reg, uint16_t val) {
  out_buf[0] = 0x01; // Slave ID
  out_buf[1] = 0x06; // FC 0x06 (Write Single Register)
  out_buf[2] = static_cast<uint8_t>((reg >> 8) & 0xFF);
  out_buf[3] = static_cast<uint8_t>(reg & 0xFF);
  out_buf[4] = static_cast<uint8_t>((val >> 8) & 0xFF);
  out_buf[5] = static_cast<uint8_t>(val & 0xFF);
  uint16_t crc = Fcu_CalcCrc16(out_buf, 6);
  out_buf[6] = static_cast<uint8_t>(crc & 0xFF);
  out_buf[7] = static_cast<uint8_t>((crc >> 8) & 0xFF);
  return 8;
}

// 19B 응답 파싱 및 CRC 검증
static bool Fcu_ParseQueryResponse(const uint8_t *data, size_t len, Fcu::Snapshot &out) {
  if (len < 19 || data[0] != 0x01 || data[1] != 0x03 || data[2] != 0x0E) return false;

  // CRC 검증: data[0..16] (17바이트) → CRC Little-Endian at data[17..18]
  uint16_t calc_crc = Fcu_CalcCrc16(data, 17);
  uint16_t pkt_crc = static_cast<uint16_t>(data[17]) | (static_cast<uint16_t>(data[18]) << 8);
  if (calc_crc != pkt_crc) return false;

  // 레지스터 언패킹 (Big-Endian)
  uint16_t reg1 = (static_cast<uint16_t>(data[5]) << 8) | data[6];   // 운전 모드
  uint16_t reg2 = (static_cast<uint16_t>(data[7]) << 8) | data[8];   // 풍량 (전원)
  uint16_t reg3 = (static_cast<uint16_t>(data[9]) << 8) | data[10];  // 스윙
  uint16_t reg4 = (static_cast<uint16_t>(data[11]) << 8) | data[12]; // 에러 코드
  uint16_t reg5 = (static_cast<uint16_t>(data[13]) << 8) | data[14]; // 희망 설정 온도
  uint16_t reg6 = (static_cast<uint16_t>(data[15]) << 8) | data[16]; // 실내 측정 온도

  out.mode = (reg1 >= 1 && reg1 <= 3) ? static_cast<Fcu::Mode>(reg1) : Fcu::Mode::Cool;
  out.fan_speed = (reg2 <= 4) ? static_cast<Fcu::FanSpeed>(reg2) : Fcu::FanSpeed::Off;
  out.swing = (reg3 == 2) ? Fcu::Swing::On : Fcu::Swing::Off;
  out.error_code = static_cast<uint8_t>(reg4 & 0xFF);
  out.target_temp = static_cast<uint8_t>(reg5 & 0xFF);
  out.room_temp = static_cast<uint8_t>(reg6 & 0xFF);
  out.power = (out.fan_speed != Fcu::FanSpeed::Off);
  return true;
}

// 슬롯 소켓에 직접 전송하거나, 선로 점유 중(waiting_response)이면 대기 큐에 보관 (Stop-and-Wait 규약)
static bool Fcu_SendRaw(uint8_t slot_idx, const uint8_t *pkt, size_t len) {
  if (slot_idx < 1 || slot_idx >= Config::TCP::MAX_EW11_SLOTS || !pkt || len == 0 || len > 16) return false;
  auto &rt = s_fcu_slots[slot_idx];

  // 선로가 응답 대기 중이거나 인터패킷 갭(15ms) 진행 중인 경우: 선로 충돌을 방지하기 위해 대기 큐에 보관 (Stop-and-Wait 규약)
  if (rt.waiting_response || rt.has_pending_temp || (millis() < rt.next_tx_ms)) {
    memcpy(rt.pending_cmd_buf, pkt, len);
    rt.pending_cmd_len = static_cast<uint8_t>(len);
    return true;
  }

  MutexLocker lock(g_ch5_mutex);
  HubClientSlot &slot = g_hub_slots[slot_idx];
  if (!slot.enabled || slot.sock < 0 || !slot.is_connected) return false;
  bool ok = (send(slot.sock, pkt, len, MSG_DONTWAIT) == static_cast<ssize_t>(len));
  if (ok) {
    slot.tx_pkts++;
    g_pkt_stats.ch5.tx_pkts.fetch_add(1, std::memory_order_relaxed);
    rt.waiting_response = true;
    rt.query_sent_ms = millis();
    rt.next_tx_ms = millis() + Config::FCU::INTER_PACKET_DELAY_MS; // CH1과 동일한 버스 인터패킷 안정 지연 (15ms)
  }
  return ok;
}

} // namespace

namespace Fcu {

void handleSlotRx(uint8_t slot_idx, const uint8_t *data, size_t len) {
  if (slot_idx < 1 || slot_idx >= Config::TCP::MAX_EW11_SLOTS) return;
  auto &rt = s_fcu_slots[slot_idx];

  // 제어 명령(0x06, 0x10) ACK 수신 확인 (8바이트 에코 응답)
  if (len >= 8 && data[0] == 0x01 && (data[1] == 0x06 || data[1] == 0x10)) {
    rt.waiting_response = false;
    rt.timeout_count = 0;
    rt.is_online = true;
    rt.next_tx_ms = millis() + Config::FCU::INTER_PACKET_DELAY_MS; // ACK 수신 후 버스 유휴 안정 지연 (CH1 동일 15ms)
    return;
  }

  // 19바이트 0x03 상태 응답 처리
  if (len >= 19 && data[0] == 0x01 && data[1] == 0x03 && data[2] == 0x0E) {
    Fcu::Snapshot new_snap;
    if (!Fcu_ParseQueryResponse(data, len, new_snap)) return; // CRC 불일치 시 무시

    rt.waiting_response = false;
    rt.timeout_count = 0;
    rt.is_online = true;

    // 냉방 또는 난방 중일 때만 유효 운전 모드로 기억 (송풍 건조 자동화로 인한 덮어쓰기 방어)
    if (new_snap.mode == Fcu::Mode::Cool || new_snap.mode == Fcu::Mode::Heat) {
      rt.last_active_mode = new_snap.mode;
      rt.has_active_record = true;
    }
    if (new_snap.fan_speed != Fcu::FanSpeed::Off) {
      rt.last_active_fan = new_snap.fan_speed;
    }
    rt.last_active_swing = new_snap.swing;

    // [펌웨어 레벨 스윙 자동 안착]
    // 복원 명령으로 회전(2)을 지시받았으나 모터 원점 복귀로 인해 swing != 2로 보고된 경우 1회 자동 보정
    // ※ Fcu_SendRaw 직접 호출 시 g_ch5_mutex 재귀 데드락(Self-Deadlock)이 발생하므로
    //   Stop-and-Wait 대기 큐(pending_cmd_buf)에 적재하여 handleSlotLoop에서 안전하게 방출
    if (new_snap.power && rt.pending_restore_swing == 2) {
      if (new_snap.swing != Fcu::Swing::On) {
        uint8_t swing_pkt[8];
        size_t s_len = Fcu_BuildWriteSingle(swing_pkt, 0x0003, 2);
        memcpy(rt.pending_cmd_buf, swing_pkt, s_len);
        rt.pending_cmd_len = static_cast<uint8_t>(s_len);
        rt.next_tx_ms = millis() + Config::FCU::INTER_PACKET_DELAY_MS;
      }
      rt.pending_restore_swing = 0; // 1회만 보정 수행
    }

    // ── 2nd-Tier Cache: 기존 Ch1Engine 패턴과 동일 ──
    DeviceStateEntry *dev = g_device_repo.findMutable(Config::FCU::DEV_ID, slot_idx, 0, true);
    if (dev) {
      bool byte_changed = (dev->last_ack_len != 19 ||
                           memcmp(dev->last_ack_data.data(), data, 19) != 0);

      dev->last_updated_ms = millis();
      dev->is_online = true;

      if (byte_changed) {
        memcpy(dev->last_ack_data.data(), data, 19);
        dev->last_ack_len = 19;
        dev->last_target_temp = new_snap.target_temp;
        dev->last_current_temp = new_snap.room_temp;

        rt.snap = new_snap; // SlotRuntime 스냅샷 갱신 (CLI 출력용)

        // 상태 변경 감지 즉시 SmartThings로 이벤트 JSON 브로드캐스트 (표준 device_state 형식)
        char json_buf[256];
        snprintf(json_buf, sizeof(json_buf),
                 "{\"event\":\"device_state\",\"dev_id\":%u,\"sub1\":%u,\"sub2\":0,\"class\":\"fcu\","
                 "\"power\":%u,\"mode\":%u,\"fan_speed\":%u,\"swing\":%u,"
                 "\"target_temp\":%u,\"room_temp\":%u,\"error\":%u}",
                 static_cast<unsigned>(Config::FCU::DEV_ID), slot_idx,
                 new_snap.power ? 1u : 0u,
                 static_cast<unsigned>(new_snap.mode),
                 static_cast<unsigned>(new_snap.fan_speed),
                 static_cast<unsigned>(new_snap.swing),
                 new_snap.target_temp,
                 new_snap.room_temp,
                 new_snap.error_code);
        Mgmt_BroadcastRawJson(json_buf);
      }
    }
  }
}

void handleSlotLoop(uint8_t slot_idx, HubClientSlot *slot, uint32_t now) {
  if (slot_idx < 1 || slot_idx >= Config::TCP::MAX_EW11_SLOTS || !slot) return;
  auto &rt = s_fcu_slots[slot_idx];

  if (!slot->enabled || slot->sock < 0 || !slot->is_connected) {
    rt.is_online = false;
    rt.waiting_response = false;
    rt.has_pending_temp = false;
    return;
  }

  // RS-485 Stop-and-Wait: 인터패킷 갭(15ms) 만료 시 대기 중인 목표 온도 패킷 송출
  if (rt.has_pending_temp) {
    if (now >= rt.next_tx_ms) {
      rt.has_pending_temp = false;
      uint8_t buf[8];
      size_t len = Fcu_BuildWriteSingle(buf, 0x0005, rt.pending_temp);
      if (send(slot->sock, buf, len, MSG_DONTWAIT) == static_cast<ssize_t>(len)) {
        slot->tx_pkts++;
        g_pkt_stats.ch5.tx_pkts.fetch_add(1, std::memory_order_relaxed);
        rt.waiting_response = true;
        rt.query_sent_ms = now;
        rt.next_tx_ms = now + Config::FCU::INTER_PACKET_DELAY_MS;
      }
    }
    return; // 인터패킷 갭 중에는 주기적 폴링을 억제하여 버스 간섭 완전 차단
  }

  // RS-485 Stop-and-Wait: 대기 중인 제어 명령 패킷 순차 방출 (CH1 규약과 동일)
  if (rt.pending_cmd_len > 0) {
    if (!rt.waiting_response && now >= rt.next_tx_ms) {
      uint8_t len = rt.pending_cmd_len;
      rt.pending_cmd_len = 0;
      if (send(slot->sock, rt.pending_cmd_buf, len, MSG_DONTWAIT) == static_cast<ssize_t>(len)) {
        slot->tx_pkts++;
        g_pkt_stats.ch5.tx_pkts.fetch_add(1, std::memory_order_relaxed);
        rt.waiting_response = true;
        rt.query_sent_ms = now;
        rt.next_tx_ms = now + Config::FCU::INTER_PACKET_DELAY_MS;
      }
    }
    return;
  }

  // 응답 대기 중 타임아웃 검사 (300ms)
  if (rt.waiting_response) {
    if (now - rt.query_sent_ms >= Config::FCU::RX_TIMEOUT_MS) {
      rt.waiting_response = false;
      rt.timeout_count++;
      if (rt.timeout_count >= Config::FCU::MAX_TIMEOUT_COUNT) {
        rt.is_online = false;
      }
    }
    return;
  }

  // 20초 주기 만료 검사 (슬롯별 독립 동작, 첫 진입 시 즉시 폴링)
  if (rt.last_poll_ms == 0 || (now - rt.last_poll_ms >= Config::FCU::POLL_INTERVAL_MS)) {
    rt.last_poll_ms = now;
    rt.query_sent_ms = now;
    rt.waiting_response = true;

    // 1st-Tier Polling Target 등록 (20초마다 갱신하여 스핀락 경합 100% 방지)
    g_polling_targets.registerOrTouch(5, Config::FCU::DEV_ID, slot_idx, 0,
                                     kFcuQueryPkt, sizeof(kFcuQueryPkt));

    if (send(slot->sock, kFcuQueryPkt, sizeof(kFcuQueryPkt), MSG_DONTWAIT) == static_cast<ssize_t>(sizeof(kFcuQueryPkt))) {
      slot->tx_pkts++;
      g_pkt_stats.ch5.tx_pkts.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

static void applyOptimisticState(uint8_t slot_idx, uint16_t mode, uint16_t fan, uint16_t swing, uint8_t temp) {
  if (slot_idx < 1 || slot_idx >= Config::TCP::MAX_EW11_SLOTS) return;
  auto &rt = s_fcu_slots[slot_idx];

  rt.snap.power = (fan != 0);
  if (mode >= 1 && mode <= 3) rt.snap.mode = static_cast<Mode>(mode);
  if (fan <= 4) rt.snap.fan_speed = static_cast<FanSpeed>(fan);
  if (swing == 0 || swing == 2) rt.snap.swing = static_cast<Swing>(swing);
  if (temp >= Config::FCU::TEMP_MIN && temp <= Config::FCU::TEMP_MAX) {
    rt.snap.target_temp = temp;
  }
  if (mode == 1 || mode == 2) {
    rt.last_active_mode = rt.snap.mode;
    rt.has_active_record = true;
  }
  if (fan != 0) {
    rt.last_active_fan = rt.snap.fan_speed;
  }
  rt.last_active_swing = rt.snap.swing;

  DeviceStateEntry *dev = g_device_repo.findMutable(Config::FCU::DEV_ID, slot_idx, 0, true);
  if (dev) {
    dev->last_ack_len = 19;
    dev->last_ack_data[6]  = static_cast<uint8_t>(rt.snap.mode);
    dev->last_ack_data[8]  = static_cast<uint8_t>(rt.snap.fan_speed);
    dev->last_ack_data[10] = static_cast<uint8_t>(rt.snap.swing);
    dev->last_ack_data[14] = rt.snap.target_temp;
    dev->last_target_temp  = rt.snap.target_temp;
    dev->last_updated_ms   = millis();
    dev->is_online         = true;
  }
}

bool RestorePower(uint8_t slot_idx, uint16_t mode, uint16_t fan, uint16_t swing, uint8_t temp) {
  if (slot_idx < 1 || slot_idx >= Config::TCP::MAX_EW11_SLOTS) return false;
  auto &rt = s_fcu_slots[slot_idx];

  if (mode != 1 && mode != 2 && mode != 3) mode = 1; // 기본 냉방
  if (fan == 0 || fan > 4) fan = 4;                  // 기본 자동
  if (swing != 0 && swing != 2) swing = 0;           // 기본 고정
  if (temp < Config::FCU::TEMP_MIN || temp > Config::FCU::TEMP_MAX) temp = 24;

  uint8_t buf[15];
  size_t len = Fcu_BuildPowerOn(buf, mode, fan, swing);
  if (!Fcu_SendRaw(slot_idx, buf, len)) return false;

  uint8_t prev_temp = rt.snap.target_temp;
  applyOptimisticState(slot_idx, mode, fan, swing, temp);

  if (prev_temp != temp) {
    rt.has_pending_temp = true;
    rt.pending_temp = temp;
    rt.next_tx_ms = millis() + Config::FCU::INTER_PACKET_DELAY_MS;
  }

  // 모터 캘리브레이션으로 인한 스윙 풀림 대비: 회전(2) 요구 시 pending_restore_swing 등록
  if (swing == 2) {
    rt.pending_restore_swing = 2;
  } else {
    rt.pending_restore_swing = 0;
  }

  return true;
}

bool SetPower(uint8_t slot_idx, bool on) {
  if (slot_idx < 1 || slot_idx >= Config::TCP::MAX_EW11_SLOTS) return false;
  auto &rt = s_fcu_slots[slot_idx];

  if (on) {
    Mode target_mode = rt.has_active_record ? rt.last_active_mode : Mode::Cool;
    FanSpeed target_fan = rt.has_active_record ? rt.last_active_fan : FanSpeed::Low;
    Swing target_swing = rt.has_active_record ? rt.last_active_swing : Swing::Off;

    if (target_fan == FanSpeed::Off) {
      target_fan = FanSpeed::Low;
    }

    if (target_swing == Swing::On) {
      rt.pending_restore_swing = 2;
    } else {
      rt.pending_restore_swing = 0;
    }

    uint8_t buf[15];
    size_t len = Fcu_BuildPowerOn(buf,
                   static_cast<uint16_t>(target_mode),
                   static_cast<uint16_t>(target_fan),
                   static_cast<uint16_t>(target_swing));
    bool ok = Fcu_SendRaw(slot_idx, buf, len);
    if (ok) {
      applyOptimisticState(slot_idx, static_cast<uint16_t>(target_mode),
                           static_cast<uint16_t>(target_fan),
                           static_cast<uint16_t>(target_swing), rt.snap.target_temp);
    }
    return ok;
  } else {
    rt.pending_restore_swing = 0;
    bool ok = Fcu_SendRaw(slot_idx, kFcuPowerOff, sizeof(kFcuPowerOff));
    if (ok) {
      applyOptimisticState(slot_idx, static_cast<uint16_t>(rt.snap.mode), 0,
                           static_cast<uint16_t>(rt.snap.swing), rt.snap.target_temp);
    }
    return ok;
  }
}

bool SetMode(uint8_t slot_idx, Mode m) {
  uint8_t buf[8];
  size_t len = Fcu_BuildWriteSingle(buf, 0x0001, static_cast<uint16_t>(m));
  bool ok = Fcu_SendRaw(slot_idx, buf, len);
  if (ok) {
    auto &rt = s_fcu_slots[slot_idx];
    applyOptimisticState(slot_idx, static_cast<uint16_t>(m),
                         static_cast<uint16_t>(rt.snap.fan_speed),
                         static_cast<uint16_t>(rt.snap.swing), rt.snap.target_temp);
  }
  return ok;
}

bool SetFanSpeed(uint8_t slot_idx, FanSpeed f) {
  uint8_t buf[8];
  size_t len = Fcu_BuildWriteSingle(buf, 0x0002, static_cast<uint16_t>(f));
  bool ok = Fcu_SendRaw(slot_idx, buf, len);
  if (ok) {
    auto &rt = s_fcu_slots[slot_idx];
    applyOptimisticState(slot_idx, static_cast<uint16_t>(rt.snap.mode),
                         static_cast<uint16_t>(f),
                         static_cast<uint16_t>(rt.snap.swing), rt.snap.target_temp);
  }
  return ok;
}

bool SetSwing(uint8_t slot_idx, Swing s) {
  uint8_t buf[8];
  size_t len = Fcu_BuildWriteSingle(buf, 0x0003, static_cast<uint16_t>(s));
  bool ok = Fcu_SendRaw(slot_idx, buf, len);
  if (ok) {
    auto &rt = s_fcu_slots[slot_idx];
    applyOptimisticState(slot_idx, static_cast<uint16_t>(rt.snap.mode),
                         static_cast<uint16_t>(rt.snap.fan_speed),
                         static_cast<uint16_t>(s), rt.snap.target_temp);
  }
  return ok;
}

bool SetTargetTemp(uint8_t slot_idx, uint8_t temp_c) {
  if (temp_c < Config::FCU::TEMP_MIN) temp_c = Config::FCU::TEMP_MIN;
  if (temp_c > Config::FCU::TEMP_MAX) temp_c = Config::FCU::TEMP_MAX;
  uint8_t buf[8];
  size_t len = Fcu_BuildWriteSingle(buf, 0x0005, static_cast<uint16_t>(temp_c));
  bool ok = Fcu_SendRaw(slot_idx, buf, len);
  if (ok) {
    auto &rt = s_fcu_slots[slot_idx];
    applyOptimisticState(slot_idx, static_cast<uint16_t>(rt.snap.mode),
                         static_cast<uint16_t>(rt.snap.fan_speed),
                         static_cast<uint16_t>(rt.snap.swing), temp_c);
  }
  return ok;
}

bool GetSlotRuntime(uint8_t slot_idx, SlotRuntime &out_rt) {
  if (slot_idx < 1 || slot_idx >= Config::TCP::MAX_EW11_SLOTS) return false;
  out_rt = s_fcu_slots[slot_idx];
  return true;
}

} // namespace Fcu

