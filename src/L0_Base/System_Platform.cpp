#include "L0_Base/System_Platform.h"

// ── System Lifecycle & Synchronization Primitives ──
EventGroupHandle_t g_system_event_group = nullptr;
std::atomic<bool> g_ota_in_progress{false};
std::atomic<bool> g_probe_convergence_reset{false};


