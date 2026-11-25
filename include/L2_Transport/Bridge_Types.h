#pragma once

// ============================================================================
// Bridge_Types: Level 2 EW11 Bridge Pure Snapshot & Value Types (Leaf Header)
// ============================================================================
// Decouples snapshot POD definitions from Bridge_CH transport control functions
// so L3 Protocol_Facade and L4 Services can inspect bridge slot state without
// pulling in L2 transport mutation functions.
// ============================================================================

#include <cstdint>

enum class HubDeviceType : uint8_t {
  WALLPAD_COMPATIBLE = 0,
  AIR_CONDITIONER = 1
};

struct HubClientSlotSnapshot {
  bool enabled{false};
  bool is_connected{false};
  char name[16]{""};
  char target_ip[16]{""};
  uint16_t target_port{0};
  HubDeviceType dev_type{HubDeviceType::WALLPAD_COMPATIBLE};
  uint32_t last_rx_ms{0};
  uint32_t rx_pkts{0};
  uint32_t tx_pkts{0};
  uint32_t crc_errors{0};
  uint32_t invalid_frames{0};
  uint32_t timeouts{0};
  uint32_t uncached_pkts{0};
  uint32_t dropped_pkts{0};
};
