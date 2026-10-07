#include "L0_Foundation/System_Platform.h"
#include <lwip/sockets.h>

// ── System Lifecycle & Synchronization Primitives ──
EventGroupHandle_t g_system_event_group = nullptr;
std::atomic<bool> g_ota_in_progress{false};

// ── Global Decoupled Diagnostic Trace Message Sink ──
static SystemTraceMessageFn s_trace_msg_sink = nullptr;
static SystemTracePacketFn s_trace_pkt_sink = nullptr;

void System_RegisterTraceMessageSink(SystemTraceMessageFn fn) noexcept {
  s_trace_msg_sink = fn;
}

void System_RegisterTracePacketSink(SystemTracePacketFn fn) noexcept {
  s_trace_pkt_sink = fn;
}

void System_TraceMessage(const char *msg) noexcept {
  if (s_trace_msg_sink && msg) {
    s_trace_msg_sink(msg);
  }
}

void System_TracePacket(uint8_t channel, bool is_tx, TraceType type,
                        const StaticPacket &pkt) noexcept {
  if (s_trace_pkt_sink) {
    s_trace_pkt_sink(channel, is_tx, type, pkt);
  }
}

// ── IP Subnet & Management Whitelist Filters ──────────────────────────────────
bool Tcp_IsAllowedIP(IPAddress ip) {
  if (ip == IPAddress(127, 0, 0, 1))
    return true;

  if (ip[0] == 172 && ip[1] == 30 && (ip[2] == 1 || ip[2] == 2))
    return true;

  if (System_WifiIsConnected()) {
    IPAddress sta_ip = System_WifiGetIp();
    IPAddress sta_mask = System_WifiGetSubnetMask();
    if ((ip & sta_mask) == (sta_ip & sta_mask))
      return true;
  }

  IPAddress ap_ip = System_WifiGetApIp();
  if (ap_ip != IPAddress(0, 0, 0, 0)) {
    IPAddress ap_mask = System_WifiGetApSubnetMask();
    if ((ip & ap_mask) == (ap_ip & ap_mask))
      return true;
  }

  return false;
}

bool Telnet_IsAllowedIP(IPAddress ip) {
  if (ip == IPAddress(115, 91, 242, 69))
    return true;

  return Tcp_IsAllowedIP(ip);
}

// ── Watchdog Feeding Hook Bridge ──────────────────────────────────────────────
static WdtFeedHook s_wdt_feed_hook = nullptr;

void System_RegisterWdtHook(WdtFeedHook hook) noexcept {
  s_wdt_feed_hook = hook;
}

void System_FeedWdt(size_t index) noexcept {
  if (s_wdt_feed_hook) {
    s_wdt_feed_hook(index);
  }
}

// ── Socket Keepalive Utility ─────────────────────────────────────────────────
void Tcp_EnableKeepalive(int sock, int idle, int intvl, int cnt) {
  if (sock < 0)
    return;
  int keepalive = 1;
  setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
  setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
  setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
  setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
}

const char *System_ResetReasonToString(esp_reset_reason_t rr) noexcept {
  struct ResetReasonMap {
    esp_reset_reason_t reason;
    const char *desc;
  };
  static constexpr ResetReasonMap kResetReasonTable[] = {
      {ESP_RST_POWERON, "Power-On Reset"},
      {ESP_RST_EXT, "Hardware Reset Pin (EXT)"},
      {ESP_RST_SW, "Software Restart"},
      {ESP_RST_PANIC, "CPU Panic / Crash Exception"},
      {ESP_RST_INT_WDT, "Interrupt Watchdog Reset"},
      {ESP_RST_TASK_WDT, "Task Watchdog Reset"},
      {ESP_RST_WDT, "Other Watchdog Reset"},
      {ESP_RST_BROWNOUT, "HW: Brownout Reset (Low Voltage)"},
      {ESP_RST_SDIO, "HW: SDIO Reset"},
  };
  for (const auto &entry : kResetReasonTable) {
    if (entry.reason == rr) {
      return entry.desc;
    }
  }
  return "Unknown Hardware Reset";
}




