// ============================================================================
// Bridge_CH: Level 2 EW11 TCP Bridge Transport Channel Implementation
// Transport Independent Leaf (AGENTS.md Rule 17)
// ============================================================================

#include "L2_Transport/Bridge_CH.h"
#include "L0_Foundation/System_Platform.h"

#include <Preferences.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <esp_log.h>
#include <esp_timer.h>
#include <fcntl.h>
#include <lwip/sockets.h>
#include <unistd.h>

static const char *TAG = "EW11";

struct HubClientSlot {
  bool enabled{false};
  char name[16]{""};
  char target_ip[16]{""};
  uint16_t target_port{8898};
  HubDeviceType dev_type{HubDeviceType::WALLPAD_COMPATIBLE};
  uint8_t frame_stx{0};
  uint8_t frame_etx{0};
  uint8_t frame_len{0};
  bool frame_locked{false};
  int sock{-1};
  bool is_connected{false};
  uint32_t last_reconnect_ms{0};
  uint8_t rx_buf[Config::TCP::HUB_RX_BUFFER_SIZE];
  size_t rx_len{0};
  uint32_t last_rx_ms{0};
  uint32_t rx_pkts{0};
  uint32_t tx_pkts{0};
  uint32_t crc_errors{0};
  uint32_t invalid_frames{0};
  uint32_t timeouts{0};
  uint32_t uncached_pkts{0};
  uint32_t dropped_pkts{0};
  uint8_t last_query_data[64]{0};
  uint8_t last_query_len{0};
};

static HubClientSlot s_hub_slots[Config::TCP::MAX_EW11_SLOTS];
static SemaphoreHandle_t s_ch5_mutex = nullptr;
static int s_ew11_server_fds[Config::TCP::MAX_EW11_SLOTS] = {-1, -1, -1, -1,
                                                             -1};

static Bridge_PacketDispatcher s_dispatcher{};
static BridgeSlotDriver s_slot_drivers[Config::TCP::MAX_EW11_SLOTS]{};

static void Hub_LoadConfig();
static void Hub_SaveConfig();
static bool Hub_SendPacket(uint8_t slot_idx, const StaticPacket &pkt);

void Bridge_RegisterDispatcher(
    const Bridge_PacketDispatcher &dispatcher) noexcept {
  s_dispatcher = dispatcher;
}

void Bridge_RegisterSlotDriver(uint8_t slot_idx,
                               const BridgeSlotDriver &driver) noexcept {
  if (slot_idx < Config::TCP::MAX_EW11_SLOTS) {
    s_slot_drivers[slot_idx] = driver;
  }
}

void Bridge_RegisterSlotRxCallback(BridgeRxCallback cb) noexcept {
  for (size_t i = 1; i < Config::TCP::MAX_EW11_SLOTS; ++i) {
    s_slot_drivers[i].onRxStream = cb;
  }
}

void Bridge_RegisterSlotTickCallback(BridgeTickCallback cb) noexcept {
  for (size_t i = 1; i < Config::TCP::MAX_EW11_SLOTS; ++i) {
    s_slot_drivers[i].onTick = cb;
  }
}

bool Bridge_GetSlotSnapshot(uint8_t slot_idx, HubClientSlotSnapshot &out) {
  if (slot_idx >= Config::TCP::MAX_EW11_SLOTS) {
    return false;
  }
  MutexLocker lock(s_ch5_mutex);
  const auto &s = s_hub_slots[slot_idx];
  out.enabled = s.enabled;
  out.is_connected = s.is_connected;
  strncpy(out.name, s.name, sizeof(out.name));
  out.name[sizeof(out.name) - 1] = '\0';
  strncpy(out.target_ip, s.target_ip, sizeof(out.target_ip));
  out.target_ip[sizeof(out.target_ip) - 1] = '\0';
  out.target_port = s.target_port;
  out.dev_type = s.dev_type;
  out.last_rx_ms = s.last_rx_ms;
  out.rx_pkts = s.rx_pkts;
  out.tx_pkts = s.tx_pkts;
  out.crc_errors = s.crc_errors;
  out.invalid_frames = s.invalid_frames;
  out.timeouts = s.timeouts;
  out.uncached_pkts = s.uncached_pkts;
  out.dropped_pkts = s.dropped_pkts;
  return true;
}

bool System_GetBridgeSlotSnapshot(uint8_t slot_idx,
                                  HubClientSlotSnapshot &out) noexcept {
  return Bridge_GetSlotSnapshot(slot_idx, out);
}

bool Bridge_IsSlotOnline(uint8_t slot_idx) noexcept {
  if (slot_idx >= Config::TCP::MAX_EW11_SLOTS) {
    return false;
  }
  MutexLocker lock(s_ch5_mutex);
  return s_hub_slots[slot_idx].enabled && s_hub_slots[slot_idx].is_connected;
}

bool Bridge_SetSlotEnabled(uint8_t slot_idx, bool enabled) {
  if (slot_idx >= Config::TCP::MAX_EW11_SLOTS) {
    return false;
  }
  {
    MutexLocker lock(s_ch5_mutex);
    s_hub_slots[slot_idx].enabled = enabled;
    if (!enabled && s_hub_slots[slot_idx].sock >= 0) {
      close(s_hub_slots[slot_idx].sock);
      s_hub_slots[slot_idx].sock = -1;
      s_hub_slots[slot_idx].is_connected = false;
      s_hub_slots[slot_idx].rx_len = 0;
    }
  }
  Hub_SaveConfig();
  return true;
}

bool Bridge_SetFramingLock(uint8_t slot_idx, uint8_t stx, uint8_t etx,
                           uint8_t len) {
  if (slot_idx >= Config::TCP::MAX_EW11_SLOTS) {
    return false;
  }
  char ns[16];
  snprintf(ns, sizeof(ns), "e%d_frame", slot_idx);
  MutexLocker lock(s_ch5_mutex);
  s_hub_slots[slot_idx].frame_stx = stx;
  s_hub_slots[slot_idx].frame_etx = etx;
  s_hub_slots[slot_idx].frame_len = len;
  s_hub_slots[slot_idx].frame_locked = true;

  Preferences p;
  p.begin(ns, false);
  p.putUChar("stx", stx);
  p.putUChar("etx", etx);
  p.putUChar("len", len);
  p.putBool("locked", true);
  p.end();
  return true;
}

bool Bridge_ResetFramingTracker(uint8_t slot_idx) {
  if (slot_idx >= Config::TCP::MAX_EW11_SLOTS) {
    return false;
  }
  char ns[16];
  snprintf(ns, sizeof(ns), "e%d_frame", slot_idx);
  MutexLocker lock(s_ch5_mutex);
  s_hub_slots[slot_idx].frame_stx = 0;
  s_hub_slots[slot_idx].frame_etx = 0;
  s_hub_slots[slot_idx].frame_len = 0;
  s_hub_slots[slot_idx].frame_locked = false;

  Preferences p;
  p.begin(ns, false);
  p.clear();
  p.end();
  return true;
}

bool Bridge_SendRaw(uint8_t slot_idx, const uint8_t *data,
                    size_t len) noexcept {
  if (slot_idx >= Config::TCP::MAX_EW11_SLOTS || !data || len == 0) {
    return false;
  }
  MutexLocker lock(s_ch5_mutex);
  auto &slot = s_hub_slots[slot_idx];
  if (slot.sock < 0 || !slot.is_connected) {
    return false;
  }
  ssize_t sent = send(slot.sock, data, len, MSG_DONTWAIT);
  if (sent == static_cast<ssize_t>(len)) {
    slot.tx_pkts++;
    return true;
  }
  return false;
}

bool Bridge_SendRaw(uint8_t slot_idx, std::span<const uint8_t> data) noexcept {
  return Bridge_SendRaw(slot_idx, data.data(), data.size());
}

void Bridge_RecordSlotRx(uint8_t slot_idx) noexcept {
  if (slot_idx < Config::TCP::MAX_EW11_SLOTS) {
    s_hub_slots[slot_idx].rx_pkts++;
  }
  System_RecordCh5Rx();
}

void Bridge_RecordSlotCrcError(uint8_t slot_idx) noexcept {
  if (slot_idx < Config::TCP::MAX_EW11_SLOTS) {
    MutexLocker lock(s_ch5_mutex);
    s_hub_slots[slot_idx].crc_errors++;
  }
}

void Bridge_RecordSlotInvalidFrame(uint8_t slot_idx) noexcept {
  if (slot_idx < Config::TCP::MAX_EW11_SLOTS) {
    MutexLocker lock(s_ch5_mutex);
    s_hub_slots[slot_idx].invalid_frames++;
  }
}

void Bridge_RecordSlotTimeout(uint8_t slot_idx) noexcept {
  if (slot_idx < Config::TCP::MAX_EW11_SLOTS) {
    MutexLocker lock(s_ch5_mutex);
    s_hub_slots[slot_idx].timeouts++;
  }
}

void Bridge_RecordSlotUncached(uint8_t slot_idx) noexcept {
  if (slot_idx < Config::TCP::MAX_EW11_SLOTS) {
    MutexLocker lock(s_ch5_mutex);
    s_hub_slots[slot_idx].uncached_pkts++;
  }
}

void Bridge_ResetStats() noexcept {
  MutexLocker lock(s_ch5_mutex);
  for (int i = 0; i < Config::TCP::MAX_EW11_SLOTS; ++i) {
    s_hub_slots[i].rx_pkts = 0;
    s_hub_slots[i].tx_pkts = 0;
    s_hub_slots[i].crc_errors = 0;
    s_hub_slots[i].invalid_frames = 0;
    s_hub_slots[i].timeouts = 0;
    s_hub_slots[i].uncached_pkts = 0;
    s_hub_slots[i].dropped_pkts = 0;
  }
}

void System_ResetBridgeStats() noexcept {
  Bridge_ResetStats();
}

namespace {

inline void consumeRxBuffer(HubClientSlot *slot, size_t consumed) {
  if (!slot || consumed == 0)
    return;
  if (consumed >= slot->rx_len) {
    slot->rx_len = 0;
  } else {
    size_t remaining = slot->rx_len - consumed;
    memmove(slot->rx_buf, slot->rx_buf + consumed, remaining);
    slot->rx_len = remaining;
  }
}

static void Hub_ProcessPacket(HubClientSlot *slot, const uint8_t *pkt_data,
                              size_t pkt_len);

void demuxPacketStream(HubClientSlot *slot) {
  uint8_t stx = s_dispatcher.onGetStx ? s_dispatcher.onGetStx() : PKT_STX;

  size_t p = 0;
  size_t loop_count = 0;
  while (p < slot->rx_len && ++loop_count < 256) {
    if (slot->rx_buf[p] != stx) {
      const void *stx_ptr = memchr(&slot->rx_buf[p], stx, slot->rx_len - p);
      if (!stx_ptr) {
        break;
      }
      p = static_cast<const uint8_t *>(stx_ptr) - slot->rx_buf;
    }

    int len_res =
        s_dispatcher.onExtractLength
            ? s_dispatcher.onExtractLength(slot->rx_buf, slot->rx_len, p)
            : 0;
    if (len_res == 0) [[unlikely]]
      break;

    if (len_res < 0) [[unlikely]] {
      StaticPacket drp_pkt{5, 1};
      drp_pkt.data[0] = slot->rx_buf[p];
      System_TracePacket(5, false, TraceType::DRP, drp_pkt);
      System_RecordCh5Dropped();
      slot->invalid_frames++;
      slot->dropped_pkts++;
      p++;
      continue;
    }

    uint8_t p_len = static_cast<uint8_t>(len_res);
    if (p_len == 0) [[unlikely]] {
      p++;
      continue;
    }

    if (s_dispatcher.onValidatePacket &&
        !s_dispatcher.onValidatePacket(&slot->rx_buf[p], p_len)) [[unlikely]] {
      StaticPacket drp_pkt{5, p_len};
      std::copy(&slot->rx_buf[p], &slot->rx_buf[p + p_len],
                drp_pkt.data.begin());
      System_TracePacket(5, false, TraceType::DRP, drp_pkt);
      System_RecordCh5Dropped();
      slot->crc_errors++;
      slot->dropped_pkts++;
      p += p_len;
      continue;
    }

    Hub_ProcessPacket(slot, &slot->rx_buf[p], p_len);
    p += p_len;
  }

  consumeRxBuffer(slot, p);
}

// ── 비동기 버스트 전송 FSM ──
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
  uint32_t last_tx = 0;

  {
    CriticalSectionLocker lock(&s_burst_fsm.mux);
    if (s_burst_fsm.remaining_count == 0) {
      return;
    }
    tx_pkt = s_burst_fsm.pkt;
    slot = s_burst_fsm.target_slot;
    silence_req_ms = s_burst_fsm.silence_ms;
    last_tx = s_burst_fsm.last_tx_ms;
  }

  uint32_t last_rx = 0;
  {
    MutexLocker lock(s_ch5_mutex);
    const auto &slot_info = s_hub_slots[slot];
    if (!slot_info.enabled || slot_info.sock < 0 || !slot_info.is_connected) {
      CriticalSectionLocker lock_burst(&s_burst_fsm.mux);
      s_burst_fsm.remaining_count = 0;
      return;
    }
    last_rx = slot_info.last_rx_ms;
  }

  uint32_t now = millis();
  uint32_t rx_elapsed = (now >= last_rx) ? (now - last_rx) : 0;
  uint32_t tx_elapsed = (now >= last_tx) ? (now - last_tx) : 0;

  uint32_t rx_rem_ms =
      (rx_elapsed < silence_req_ms) ? (silence_req_ms - rx_elapsed) : 0;
  uint32_t tx_rem_ms = (last_tx > 0 && tx_elapsed < silence_req_ms)
                           ? (silence_req_ms - tx_elapsed)
                           : 0;
  uint32_t wait_ms = std::max(rx_rem_ms, tx_rem_ms);

  if (wait_ms > 0) {
    esp_timer_start_once(s_burst_fsm.timer,
                         static_cast<uint64_t>(wait_ms) * 1000);
    return;
  }

  const bool sent = Hub_SendPacket(slot, tx_pkt);
  now = millis();
  System_TracePacket(5, true, sent ? TraceType::CTL : TraceType::DRP, tx_pkt);

  bool schedule_next = false;
  uint32_t next_gap_ms = 20;
  {
    CriticalSectionLocker lock(&s_burst_fsm.mux);
    s_burst_fsm.last_tx_ms = now;
    if (s_burst_fsm.remaining_count > 0) {
      s_burst_fsm.remaining_count--;
    }
    schedule_next = (s_burst_fsm.remaining_count > 0);
    next_gap_ms = s_burst_fsm.silence_ms;
  }

  if (schedule_next) {
    esp_timer_start_once(s_burst_fsm.timer,
                         static_cast<uint64_t>(next_gap_ms) * 1000);
  }
}

namespace Ew11Manager {

void init() {
  if (!s_burst_fsm.timer) {
    esp_timer_create_args_t timer_args{};
    timer_args.callback = onBurstTimer;
    timer_args.arg = nullptr;
    timer_args.name = "ew11_burst_timer";
    esp_timer_create(&timer_args, &s_burst_fsm.timer);
  }
}

bool sendBurstPacket(uint8_t slot_idx, const StaticPacket &pkt, uint8_t count,
                     uint32_t silence_ms) {
  if (slot_idx >= Config::TCP::MAX_EW11_SLOTS || count == 0)
    return false;
  if (!s_burst_fsm.timer)
    init();

  esp_timer_stop(s_burst_fsm.timer);

  {
    MutexLocker lock(s_ch5_mutex);
    const auto &slot = s_hub_slots[slot_idx];
    if (!slot.enabled || slot.sock < 0 || !slot.is_connected) {
      return false;
    }
  }

  {
    CriticalSectionLocker lock(&s_burst_fsm.mux);
    s_burst_fsm.pkt = pkt;
    s_burst_fsm.target_slot = slot_idx;
    s_burst_fsm.remaining_count = count;
    s_burst_fsm.silence_ms = silence_ms;
    s_burst_fsm.last_tx_ms = 0;
  }

  esp_timer_start_once(s_burst_fsm.timer, 1000);
  return true;
}

void processStream(int slot_idx, HubClientSlot *slot) {
  if (!slot)
    return;
  if (slot_idx > 0 && slot_idx < static_cast<int>(Config::TCP::MAX_EW11_SLOTS) &&
      s_slot_drivers[slot_idx].onRxStream) {
    size_t consumed =
        s_slot_drivers[slot_idx].onRxStream(
            static_cast<uint8_t>(slot_idx),
            std::span<const uint8_t>(slot->rx_buf, slot->rx_len));
    consumeRxBuffer(slot, consumed);
  } else if (slot_idx == 0) {
    demuxPacketStream(slot);
  }
}

} // namespace Ew11Manager

static void configureClientSocket(int sock) {
  int flags = fcntl(sock, F_GETFL, 0);
  fcntl(sock, F_SETFL, flags | O_NONBLOCK);
  int nodelay = 1;
  setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
  int sockbuf = Config::TCP::SOCKET_BUFFER_SIZE;
  setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &sockbuf, sizeof(sockbuf));
  setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sockbuf, sizeof(sockbuf));
  Tcp_EnableKeepalive(sock, 30, Config::TCP::DEFAULT_KEEPALIVE_INTVL_SEC,
                      Config::TCP::DEFAULT_KEEPALIVE_CNT);
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

  if (heap_caps_get_free_size(MALLOC_CAP_8BIT) < 32768) {
    close(new_sock);
    return -1;
  }

  const uint8_t *b = reinterpret_cast<const uint8_t *>(&caddr.sin_addr.s_addr);
  IPAddress remote_ip(b[0], b[1], b[2], b[3]);
  if (!Tcp_IsAllowedIP(remote_ip)) {
    close(new_sock);
    return -1;
  }

  char client_ip_str[16];
  snprintf(client_ip_str, sizeof(client_ip_str), "%u.%u.%u.%u", b[0], b[1],
           b[2], b[3]);

  MutexLocker lock(s_ch5_mutex);
  auto &slot = s_hub_slots[slot_idx];

  if (slot.target_ip[0] != '\0' && strcmp(slot.target_ip, client_ip_str) != 0) {
    ESP_LOGW(TAG,
             "[CH5] Client IP mismatch for Slot %d (%s): got %s, expected %s",
             slot_idx, slot.name, client_ip_str, slot.target_ip);
    close(new_sock);
    return -1;
  }

  configureClientSocket(new_sock);

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

  System_RecordCh5Connection();
  ESP_LOGI(TAG, "[CH5] Accepted EW11 client %s on port %u -> Slot %d (%s)",
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
  System_RecordCh5Rx();
  System_TracePacket(5, false, TraceType::RMT, pkt);

  int slot_idx = static_cast<int>(slot - s_hub_slots);
  if (s_dispatcher.onPacketReceived) {
    s_dispatcher.onPacketReceived(static_cast<uint8_t>(slot_idx), pkt);
  }
}

void Hub_Data(HubClientSlot *slot, const uint8_t *data, size_t len) {
  if (!slot || slot->sock < 0 || !data || len == 0)
    return;

  slot->last_rx_ms = millis();

  if (slot->rx_len + len > sizeof(slot->rx_buf)) {
    uint8_t stx = slot->frame_stx;
    if (stx == 0)
      stx = PKT_STX;
    const void *hit = memchr(slot->rx_buf, stx, slot->rx_len);
    size_t stx_pos = hit ? static_cast<size_t>(static_cast<const uint8_t *>(hit) - slot->rx_buf)
                         : slot->rx_len;
    consumeRxBuffer(slot, stx_pos);
  }

  size_t copy_len = std::min(len, sizeof(slot->rx_buf) - slot->rx_len);
  std::copy(data, data + copy_len, slot->rx_buf + slot->rx_len);
  slot->rx_len += copy_len;

  int slot_idx = static_cast<int>(slot - s_hub_slots);
  Ew11Manager::processStream(slot_idx, slot);
}

} // anonymous namespace

bool Bridge_ForwardPacket(uint8_t slot_idx, const StaticPacket &pkt,
                          bool burst) noexcept {
  if (burst || slot_idx == 0) {
    return Ew11Manager::sendBurstPacket(slot_idx, pkt, 2, 20);
  } else {
    const bool sent = Hub_SendPacket(slot_idx, pkt);
    System_TracePacket(5, true, sent ? TraceType::CTL : TraceType::DRP, pkt);
    return sent;
  }
}

static void Hub_LoadConfig() {
  Preferences p;
  p.begin("ew11-config", true);
  MutexLocker lock(s_ch5_mutex);

  // Slot 0 (엘리베이터)
  s_hub_slots[0].enabled = p.getBool("e0_en", true);
  p.getString("e0_name", "Elevator")
      .toCharArray(s_hub_slots[0].name, sizeof(s_hub_slots[0].name));
  p.getString("e0_ip", "172.30.1.245")
      .toCharArray(s_hub_slots[0].target_ip, sizeof(s_hub_slots[0].target_ip));
  uint16_t p0 = p.getUShort("e0_port", 8898);
  if (p0 == 0 || p0 == 8899)
    p0 = 8898;
  s_hub_slots[0].target_port = p0;
  s_hub_slots[0].dev_type = HubDeviceType::WALLPAD_COMPATIBLE;
  s_hub_slots[0].sock = -1;
  s_hub_slots[0].is_connected = false;
  s_hub_slots[0].rx_len = 0;

  // Slot 1~4 (FCU 에어컨)
  for (int i = 1; i < Config::TCP::MAX_EW11_SLOTS; i++) {
    char k_en[8], k_nm[8], k_ip[8], k_pt[8], def_nm[16];
    snprintf(k_en, sizeof(k_en), "e%d_en", i);
    snprintf(k_nm, sizeof(k_nm), "e%d_name", i);
    snprintf(k_ip, sizeof(k_ip), "e%d_ip", i);
    snprintf(k_pt, sizeof(k_pt), "e%d_port", i);
    snprintf(def_nm, sizeof(def_nm), "AC_%d", i);

    s_hub_slots[i].enabled = p.getBool(k_en, false);
    p.getString(k_nm, def_nm)
        .toCharArray(s_hub_slots[i].name, sizeof(s_hub_slots[i].name));
    p.getString(k_ip, "").toCharArray(s_hub_slots[i].target_ip,
                                      sizeof(s_hub_slots[i].target_ip));
    uint16_t def_slot_port = Config::TCP::EW11_SLOT_PORTS[i];
    uint16_t pi = p.getUShort(k_pt, def_slot_port);
    if (pi == 0 || pi == 8899)
      pi = def_slot_port;
    s_hub_slots[i].target_port = pi;
    s_hub_slots[i].dev_type = HubDeviceType::AIR_CONDITIONER;
    s_hub_slots[i].sock = -1;
    s_hub_slots[i].is_connected = false;
    s_hub_slots[i].rx_len = 0;
  }

  for (int s = 0; s < Config::TCP::MAX_EW11_SLOTS; s++) {
    char ns[16];
    snprintf(ns, sizeof(ns), "e%d_frame", s);
    Preferences fp;
    fp.begin(ns, true);
    if (fp.getBool("locked", false)) {
      s_hub_slots[s].frame_stx = fp.getUChar("stx", 0);
      s_hub_slots[s].frame_etx = fp.getUChar("etx", 0);
      s_hub_slots[s].frame_len = fp.getUChar("len", 0);
      s_hub_slots[s].frame_locked = true;
    }
    fp.end();
  }
  p.end();
}

static void Hub_SaveConfig() {
  Preferences p;
  p.begin("ew11-config", false);
  MutexLocker lock(s_ch5_mutex);

  for (int i = 0; i < Config::TCP::MAX_EW11_SLOTS; i++) {
    char k_en[8], k_nm[8], k_ip[8], k_pt[8];
    snprintf(k_en, sizeof(k_en), "e%d_en", i);
    snprintf(k_nm, sizeof(k_nm), "e%d_name", i);
    snprintf(k_ip, sizeof(k_ip), "e%d_ip", i);
    snprintf(k_pt, sizeof(k_pt), "e%d_port", i);

    p.putBool(k_en, s_hub_slots[i].enabled);
    p.putString(k_nm, s_hub_slots[i].name);
    p.putString(k_ip, s_hub_slots[i].target_ip);
    p.putUShort(k_pt, s_hub_slots[i].target_port);
  }
  p.end();
}

bool Bridge_SetSlot(uint8_t slot_idx, bool enabled, const char *ip,
                    uint16_t port, const char *name) {
  if (slot_idx >= Config::TCP::MAX_EW11_SLOTS)
    return false;

  MutexLocker lock(s_ch5_mutex);
  auto &slot = s_hub_slots[slot_idx];

  bool changed = (slot.enabled != enabled) ||
                 (strcmp(slot.target_ip, ip ? ip : "") != 0) ||
                 (port > 0 && slot.target_port != port) ||
                 (name && strlen(name) > 0 && strcmp(slot.name, name) != 0);

  if (!changed)
    return true;

  bool reconnect_needed = (slot.enabled != enabled) ||
                          (strcmp(slot.target_ip, ip ? ip : "") != 0) ||
                          (port > 0 && slot.target_port != port);

  slot.enabled = enabled;
  if (ip) {
    strncpy(slot.target_ip, ip, sizeof(slot.target_ip) - 1);
    slot.target_ip[sizeof(slot.target_ip) - 1] = '\0';
  } else {
    slot.target_ip[0] = '\0';
  }
  if (port > 0)
    slot.target_port = port;
  if (name && strlen(name) > 0) {
    strncpy(slot.name, name, sizeof(slot.name) - 1);
    slot.name[sizeof(slot.name) - 1] = '\0';
  }

  if (reconnect_needed && slot.sock >= 0) {
    close(slot.sock);
    slot.sock = -1;
    slot.is_connected = false;
    slot.rx_len = 0;
    slot.last_reconnect_ms = 0;
  }

  Hub_SaveConfig();
  return true;
}

static bool Hub_SendPacket(uint8_t slot_idx, const StaticPacket &pkt) {
  if (slot_idx >= Config::TCP::MAX_EW11_SLOTS)
    return false;
  MutexLocker lock(s_ch5_mutex);
  auto &slot = s_hub_slots[slot_idx];
  if (!slot.enabled || slot.sock < 0 || !slot.is_connected)
    return false;

  int s = send(slot.sock, pkt.data.data(), pkt.length, MSG_DONTWAIT);
  if (s == static_cast<int>(pkt.length)) {
    slot.tx_pkts++;
    System_RecordCh5Tx();
    return true;
  }
  return false;
}

void Bridge_PopulateFds(fd_set &readfds, fd_set &errorfds,
                        int &max_fd) noexcept {
  auto add_fd = [&](int fd) {
    if (fd >= 0) {
      FD_SET(fd, &readfds);
      FD_SET(fd, &errorfds);
      if (fd > max_fd)
        max_fd = fd;
    }
  };

  for (int s = 0; s < Config::TCP::MAX_EW11_SLOTS; s++) {
    add_fd(s_ew11_server_fds[s]);
  }

  MutexLocker lock(s_ch5_mutex);
  for (int s = 0; s < Config::TCP::MAX_EW11_SLOTS; s++) {
    add_fd(s_hub_slots[s].sock);
  }
}

void Bridge_ProcessEvents(fd_set &readfds, fd_set &errorfds,
                          bool ota_now) noexcept {
  for (int s = 0; s < Config::TCP::MAX_EW11_SLOTS; s++) {
    if (!ota_now && s_ew11_server_fds[s] >= 0 &&
        FD_ISSET(s_ew11_server_fds[s], &readfds)) {
      Hub_AcceptClient(s, s_ew11_server_fds[s]);
    }
  }

  if (!ota_now) {
    struct PendingChunk {
      int slot_idx{-1};
      uint8_t buf[Config::TCP::POLL_RX_CHUNK_SIZE];
      int len{0};
    };
    PendingChunk pending[Config::TCP::MAX_EW11_SLOTS];
    size_t pending_count = 0;

    {
      MutexLocker lock(s_ch5_mutex);
      for (int s = 0; s < Config::TCP::MAX_EW11_SLOTS; s++) {
        auto &slot = s_hub_slots[s];
        if (slot.sock < 0)
          continue;

        if (FD_ISSET(slot.sock, &errorfds)) {
          close(slot.sock);
          slot.sock = -1;
          slot.is_connected = false;
          slot.rx_len = 0;
          ESP_LOGW("EW11", "[CH5] Slot %d (%s) socket error detected. Closed.",
                   s, slot.name);
          continue;
        }

        if (FD_ISSET(slot.sock, &readfds)) {
          uint8_t temp_buf[Config::TCP::POLL_RX_CHUNK_SIZE];
          int r = recv(slot.sock, temp_buf, sizeof(temp_buf), 0);
          if (r > 0) {
            if (pending_count < Config::TCP::MAX_EW11_SLOTS) {
              pending[pending_count].slot_idx = s;
              pending[pending_count].len = r;
              std::copy(temp_buf, temp_buf + r, pending[pending_count].buf);
              pending_count++;
            }
          } else if (r == 0 ||
                     (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            close(slot.sock);
            slot.sock = -1;
            slot.is_connected = false;
            slot.rx_len = 0;
          }
        }
      }
    } // s_ch5_mutex 해제 완료

    // 패킷 스트림 파싱 및 L3 비즈니스 콜백은 뮤텍스 밖에서 안전하게 디스패치
    for (size_t i = 0; i < pending_count; ++i) {
      int s = pending[i].slot_idx;
      if (s >= 0 && s < Config::TCP::MAX_EW11_SLOTS) {
        Hub_Data(&s_hub_slots[s], pending[i].buf, pending[i].len);
      }
    }
  }
}

void Bridge_StartServer() noexcept {
  if (System_IsRescueMode()) {
    return;
  }

  for (int s = 0; s < Config::TCP::MAX_EW11_SLOTS; s++) {
    if (s_ew11_server_fds[s] >= 0) {
      continue;
    }

    uint16_t listen_port = s_hub_slots[s].target_port;
    if (listen_port == 0) {
      listen_port = Config::TCP::EW11_SLOT_PORTS[s];
      s_hub_slots[s].target_port = listen_port;
    }

    int sfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sfd >= 0) {
      int opt = 1;
      setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

      int flags = fcntl(sfd, F_GETFL, 0);
      fcntl(sfd, F_SETFL, flags | O_NONBLOCK);

      struct sockaddr_in saddr;
      memset(&saddr, 0, sizeof(saddr));
      saddr.sin_family = AF_INET;
      saddr.sin_addr.s_addr = htonl(INADDR_ANY);
      saddr.sin_port = htons(listen_port);
      if (bind(sfd, reinterpret_cast<struct sockaddr *>(&saddr),
               sizeof(saddr)) < 0 ||
          listen(sfd, 1) < 0) {
        ESP_LOGE("EW11",
                 "Failed to bind/listen EW11 slot %d on port %u: errno %d", s,
                 listen_port, errno);
        close(sfd);
        sfd = -1;
      } else {
        ESP_LOGI("EW11", "[CH5] Listening for EW11 slot %d (%s) on port %u", s,
                 s_hub_slots[s].name, listen_port);
      }
    }
    s_ew11_server_fds[s] = sfd;
  }
}

void Bridge_StopServer() noexcept {
  Bridge_ShutdownSockets();
  ESP_LOGI("EW11", "[CH5] EW11 Bridge TCP Sockets stopped");
}

void Bridge_Tick(bool ota_now, uint32_t now_ms) noexcept {
  static bool s_last_net_ready = false;
  const bool current_net_ready = System_IsNetworkReady();

  if (current_net_ready != s_last_net_ready) {
    s_last_net_ready = current_net_ready;
    if (current_net_ready) {
      Bridge_StartServer();
    } else {
      Bridge_StopServer();
    }
  }

  if (!ota_now) {
    for (size_t i = 1; i < Config::TCP::MAX_EW11_SLOTS; ++i) {
      if (s_slot_drivers[i].onTick) {
        s_slot_drivers[i].onTick(now_ms);
      }
    }
  }
}

void Bridge_Init() {
  if (!s_ch5_mutex) {
    s_ch5_mutex = xSemaphoreCreateMutex();
    assert(s_ch5_mutex != nullptr);
  }
  Ew11Manager::init();
  Hub_LoadConfig();

  for (int s = 0; s < Config::TCP::MAX_EW11_SLOTS; s++) {
    s_ew11_server_fds[s] = -1;
    s_hub_slots[s].sock = -1;
    s_hub_slots[s].is_connected = false;
    s_hub_slots[s].rx_len = 0;
  }
}

void Bridge_ShutdownSockets() noexcept {
  MutexLocker lock(s_ch5_mutex);
  for (int s = 0; s < Config::TCP::MAX_EW11_SLOTS; s++) {
    if (s_hub_slots[s].sock >= 0) {
      close(s_hub_slots[s].sock);
      s_hub_slots[s].sock = -1;
      s_hub_slots[s].is_connected = false;
      s_hub_slots[s].rx_len = 0;
    }
    if (s_ew11_server_fds[s] >= 0) {
      close(s_ew11_server_fds[s]);
      s_ew11_server_fds[s] = -1;
    }
  }
}
