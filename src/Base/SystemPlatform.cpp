#include "SystemPlatform.h"

SoftwareSerialConfig Door_SerialConfig(uint8_t data_bits, uint8_t parity,
                                       uint8_t stop_bits) {
  if (data_bits == 7 && stop_bits == 1) {
    if (parity == 1)
      return SWSERIAL_7E1;
    if (parity == 2)
      return SWSERIAL_7O1;
  } else if (data_bits == 8) {
    if (stop_bits == 1) {
      if (parity == 1)
        return SWSERIAL_8E1;
      if (parity == 2)
        return SWSERIAL_8O1;
    } else if (stop_bits == 2 && parity == 0) {
      return SWSERIAL_8N2;
    }
  }
  return SWSERIAL_8N1;
}

// ── System Lifecycle & Synchronization Primitives ──
EventGroupHandle_t g_system_event_group = nullptr;
std::atomic<bool> g_ota_in_progress{false};
std::atomic<bool> g_probe_convergence_reset{false};
