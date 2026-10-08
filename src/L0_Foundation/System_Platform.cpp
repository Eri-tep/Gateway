#include "L0_Foundation/System_Platform.h"
#include <lwip/sockets.h>

// ── System Lifecycle & Synchronization Primitives ──
EventGroupHandle_t g_system_event_group = nullptr;
std::atomic<bool> g_ota_in_progress{false};

// ── Global Decoupled Diagnostic Trace Message Sink ──
static std::atomic<SystemTraceMessageFn> s_trace_msg_sink{nullptr};
static std::atomic<SystemTracePacketFn> s_trace_pkt_sink{nullptr};

void System_RegisterTraceMessageSink(SystemTraceMessageFn fn) noexcept {
  s_trace_msg_sink.store(fn, std::memory_order_release);
}

void System_RegisterTracePacketSink(SystemTracePacketFn fn) noexcept {
  s_trace_pkt_sink.store(fn, std::memory_order_release);
}

void System_TraceMessage(const char *msg) noexcept {
  auto sink = s_trace_msg_sink.load(std::memory_order_acquire);
  if (sink && msg) {
    sink(msg);
  }
}

void System_TracePacket(uint8_t channel, bool is_tx, TraceType type,
                        const StaticPacket &pkt) noexcept {
  auto sink = s_trace_pkt_sink.load(std::memory_order_acquire);
  if (sink) {
    sink(channel, is_tx, type, pkt);
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
extern "C" __attribute__((weak)) void Diagnostics_FeedWdt(size_t index) noexcept;

static std::atomic<WdtFeedHook> s_wdt_feed_hook{nullptr};

void System_RegisterWdtHook(WdtFeedHook hook) noexcept {
  s_wdt_feed_hook.store(hook, std::memory_order_release);
}

void System_FeedWdt(size_t index) noexcept {
  if (Diagnostics_FeedWdt) {
    Diagnostics_FeedWdt(index);
    return;
  }
  auto hook = s_wdt_feed_hook.load(std::memory_order_relaxed);
  if (hook) {
    hook(index);
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
  switch (rr) {
  case ESP_RST_UNKNOWN:    return "Unknown Reset";
  case ESP_RST_POWERON:    return "Power-On Reset";
  case ESP_RST_EXT:        return "Hardware Reset Pin (EXT)";
  case ESP_RST_SW:         return "Software Restart";
  case ESP_RST_PANIC:      return "CPU Panic / Crash Exception";
  case ESP_RST_INT_WDT:    return "Interrupt Watchdog Reset";
  case ESP_RST_TASK_WDT:   return "Task Watchdog Reset";
  case ESP_RST_WDT:        return "Other Watchdog Reset";
  case ESP_RST_DEEPSLEEP:  return "Deep Sleep Reset";
  case ESP_RST_BROWNOUT:   return "HW: Brownout Reset (Low Voltage)";
  case ESP_RST_SDIO:       return "HW: SDIO Reset";
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  case ESP_RST_USB:        return "HW: USB Reset";
  case ESP_RST_JTAG:       return "HW: JTAG Reset";
  case ESP_RST_EFUSE:      return "HW: eFuse Reset";
  case ESP_RST_PWR_GLITCH: return "HW: Power Glitch Reset";
  case ESP_RST_CPU_LOCKUP: return "HW: CPU Lockup Reset";
#endif
  }
  return "Unknown Hardware Reset";
}




