#include "L0_Base/System_Platform.h"

// ── System Lifecycle & Synchronization Primitives ──
EventGroupHandle_t g_system_event_group = nullptr;
std::atomic<bool> g_ota_in_progress{false};
std::atomic<bool> g_probe_convergence_reset{false};

// ── Global Decoupled Diagnostic Trace Message Sink ──
static SystemTraceMessageFn s_trace_msg_sink = nullptr;
static SystemTracePacketFn s_trace_pkt_sink = nullptr;
static SystemConvergenceHookFn s_convergence_hook = nullptr;
static void (*s_stale_poll_sink)() = nullptr;

void System_RegisterTraceMessageSink(SystemTraceMessageFn fn) noexcept {
  s_trace_msg_sink = fn;
}

void System_RegisterTracePacketSink(SystemTracePacketFn fn) noexcept {
  s_trace_pkt_sink = fn;
}

void System_RegisterConvergenceHook(SystemConvergenceHookFn fn) noexcept {
  s_convergence_hook = fn;
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

void System_NotifyConvergence() noexcept {
  if (s_convergence_hook) {
    s_convergence_hook();
  }
}

void System_IncrementStalePollCount() noexcept {
  if (s_stale_poll_sink) {
    s_stale_poll_sink();
  }
}

void System_RegisterStalePollSink(void (*fn)()) noexcept {
  s_stale_poll_sink = fn;
}


