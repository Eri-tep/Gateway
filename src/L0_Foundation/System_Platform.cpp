#include "L0_Foundation/System_Platform.h"

// ── System Lifecycle & Synchronization Primitives ──
EventGroupHandle_t g_system_event_group = nullptr;
std::atomic<bool> g_ota_in_progress{false};
std::atomic<bool> g_probe_convergence_reset{false};

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
#include <WiFi.h>

bool Tcp_IsAllowedIP(IPAddress ip) {
  if (ip == IPAddress(127, 0, 0, 1))
    return true;

  if (ip[0] == 172 && ip[1] == 30 && (ip[2] == 1 || ip[2] == 2))
    return true;

  if (WiFi.isConnected()) {
    IPAddress sta_ip = WiFi.localIP();
    IPAddress sta_mask = WiFi.subnetMask();
    if ((ip & sta_mask) == (sta_ip & sta_mask))
      return true;
  }

  if (WiFi.getMode() == WIFI_MODE_AP || WiFi.getMode() == WIFI_MODE_APSTA) {
    IPAddress ap_ip = WiFi.softAPIP();
    IPAddress ap_mask = WiFi.softAPSubnetMask();
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
#include <lwip/sockets.h>

void Tcp_EnableKeepalive(int sock, int idle, int intvl, int cnt) {
  if (sock < 0)
    return;
  int keepalive = 1;
  setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
  setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
  setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
  setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
}



