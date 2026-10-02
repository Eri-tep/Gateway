#pragma once

// ============================================================================
// TransportTypes: Level 2 Transport Pure Leaf POD Data Structures
// ============================================================================

#include <cstddef>
#include <cstdint>

// ── Virtual Hub Device Classification (0: Wallpad, 1: Air Conditioner) ──
enum class HubDeviceType : uint8_t {
  WALLPAD_COMPATIBLE = 0,
  AIR_CONDITIONER = 1
};

// ── EW11 Client Socket Slot Snapshot (Information Hiding POD) ──
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
  uint32_t dropped_pkts{0};
};

// ── Multi-Channel Route Endpoint ──
struct RouteEndpoint {
  uint8_t channel_id{1}; // Default channel: CH1 (Main Physical RS-485)
  int8_t slot_idx{-1};   // If CH5, EW11 slot index (0~4), otherwise -1
  uint32_t last_seen_ms{0};
};

// ── Physical Serial Port Configuration ──
struct SerialPortConfig {
  uint32_t baud_rate{9600};
  uint8_t data_bits{8};
  uint8_t stop_bits{1};
  char parity{'N'}; // 'N', 'E', 'O'
};

// ── Port 8900 Management TCP Session Snapshot ──
struct MgmtSessionSnapshot {
  bool is_active{false};
  char peer_ip[16]{""};
  uint32_t connected_at_ms{0};
  uint32_t last_activity_ms{0};
};
