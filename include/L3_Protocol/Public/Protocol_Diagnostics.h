#pragma once

// ============================================================================
// ProtocolDiagnostics: Level 3 Public Diagnostics & Engine Facade
// ============================================================================
// Provides thread-safe, decoupled diagnostic queries and management operations
// for L4 Services (CLI Console, Remote JSON-RPC, Telemetry) without leaking
// L3 Private internal singletons or raw packet codec engines.
// ============================================================================

#include "L0_Foundation/System_Buffer.h"
#include "L3_Protocol/Public/Device_Registry.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <sys/select.h>

// ── Diagnostic Snapshot Structures ──────────────────────────────────────────

struct AutoProbingDescriptorSnapshot {
  uint8_t stx{0};
  uint8_t etx{0};
  char checksum_algo_name[24]{"None"};
  uint8_t opcode_offset{0};
  uint8_t query_opcode{0};
  uint8_t control_opcode{0};
  uint8_t ack_opcode{0};
  bool control_seen{false};
  uint8_t dev_id_offset{0};
  uint8_t sub1_offset{0};
  uint8_t sub2_offset{0};
  uint8_t gw_addr_offset{0xFF};
  uint8_t gw_addr{0};
  bool is_swapped_addr{false};
  bool has_seq_counter{false};
  uint8_t seq_offset{0xFF};
  bool is_locked{false};
  bool opcodes_locked{false};
  bool offsets_locked{false};
  uint8_t payload_offset{0};
  bool is_manual{false};
  uint32_t matched_packets{0};
  uint32_t tested_packets{0};
};

struct PollingEntrySnapshot {
  uint8_t dev_id{0};
  uint8_t sub1{0};
  uint8_t sub2{0};
  uint8_t source_channels{0};
  uint32_t hits{0};
  uint32_t last_seen_ms{0};
  bool is_active{false};
  bool verified{false};
  uint8_t pkt_len{0};
  std::array<uint8_t, 64> pkt_data{};
};

struct ProfileInfoSnapshot {
  char key[16]{0};
  char name[48]{0};
  char vendor[32]{0};
  uint8_t stx{0};
  uint8_t etx{0};
  uint8_t query_op{0};
  uint8_t ctrl_op{0};
  uint8_t ack_op{0};
  bool is_active{false};
};

struct DoorphoneMatchSnapshot {
  bool matched{false};
  char desc[32]{0};
  uint8_t bell_front{0};
  uint8_t call_front{0};
  uint8_t open_front{0};
  uint8_t end_front{0};
  uint8_t bell_lobby{0};
  uint8_t call_lobby{0};
  uint8_t open_lobby{0};
  uint8_t end_lobby{0};
};

struct BlueprintSnapshot {
  uint8_t dev_id{0};
  char group_name[16]{"Unknown"};
  DeviceClass dev_class{DeviceClass::UNKNOWN};
  uint8_t frame_len{0};
  uint8_t sub1_offset{0xFF};
  uint8_t sub2_offset{0xFF};

  // Power slot
  bool power_discovered{false};
  uint8_t power_offset{0};
  uint8_t power_on{0};
  uint8_t power_off{0};

  // Temp slot
  bool temp_discovered{false};
  uint8_t temp_offset{0};
  uint8_t temp_min{0};
  uint8_t temp_max{0};

  // Speed slot
  bool speed_discovered{false};
  uint8_t speed_offset{0};
  uint8_t speed_levels{0};
  uint8_t speed_tokens[4]{0};
  uint8_t speed_min{0};
  uint8_t speed_max{0};

  // Close slot
  bool close_discovered{false};
  uint8_t close_offset{0};
  uint8_t close_val{0};

  // Mode slot
  bool mode_discovered{false};
  uint8_t mode_offset{0};

  // ACK slots
  bool ack_discovered{false};
  uint8_t ack_pwr_offset{0xFF};
  uint8_t ack_target_temp_offset{0xFF};
  uint8_t ack_curr_temp_offset{0xFF};
  uint8_t ack_fan_speed_offset{0xFF};
  uint8_t ack_valve_offset{0xFF};
  uint8_t ack_wattage_offset{0xFF};

  // QRY slots
  uint8_t qry_len{0};
  uint8_t qry_pwr_offset{0xFF};
  uint8_t qry_target_temp_offset{0xFF};
  uint8_t qry_curr_temp_offset{0xFF};
  uint8_t qry_fan_speed_offset{0xFF};
  uint8_t qry_valve_offset{0xFF};
  uint8_t qry_wattage_offset{0xFF};
};

// ── Cache & NVS Persistence Facade ──────────────────────────────────────────
void ProtocolDiag_WarmCacheSaveToNvs() noexcept;
void ProtocolDiag_WarmCacheCheckNvsDebounce() noexcept;
void ProtocolDiag_GetWarmCacheStatus(uint8_t &out_source, uint8_t &out_restored_count) noexcept;

// ── Polling & Auto-Probing Management Facade ────────────────────────────────
void ProtocolDiag_PollingResetHits() noexcept;
void ProtocolDiag_PollingClear() noexcept;
void ProtocolDiag_PollingSweepExpired(uint32_t threshold_ms) noexcept;
size_t ProtocolDiag_GetPollingTargetCount() noexcept;
bool ProtocolDiag_GetPollingEntry(size_t index, PollingEntrySnapshot &out_snap) noexcept;
size_t ProtocolDiag_GetPollingTargetsSnapshot(PollingEntrySnapshot *out_array, size_t max_count) noexcept;
void ProtocolDiag_PollingRegisterOrTouch(uint8_t ch, uint8_t dev_id, uint8_t sub1,
                                         uint8_t sub2, const uint8_t *pkt_data,
                                         size_t pkt_len) noexcept;
void ProtocolDiag_AutoProbingReset() noexcept;
void ProtocolDiag_AutoProbingFeedFrame(const uint8_t *data, size_t len) noexcept;
bool ProtocolDiag_GetAutoProbingDescriptor(AutoProbingDescriptorSnapshot &out) noexcept;
void ProtocolDiag_GetPollingStats(size_t &active, size_t &verified, size_t &total) noexcept;
void ProtocolDiag_GetActiveAddresses(uint8_t *dev_ids, size_t &dev_cnt,
                                     uint8_t *sub1_ids, size_t &sub1_cnt,
                                     uint8_t *sub2_ids, size_t &sub2_cnt,
                                     size_t max_items) noexcept;
uint32_t ProtocolDiag_GetStalePollCount() noexcept;

// ── Framing & Profile Helpers ────────────────────────────────────────────────
void ProtocolDiag_GetFramingNamespace(uint8_t profile_idx, char *out_buf,
                                      size_t buf_len) noexcept;
bool ProtocolDiag_ExtractDeviceKey(const uint8_t *data, size_t len,
                                   uint8_t &out_dev_id, uint8_t &out_sub1,
                                   uint8_t &out_sub2) noexcept;
void ProtocolDiag_GetActiveVendorName(char *out_buf, size_t max_len) noexcept;
void ProtocolDiag_GetProfileSummary(char *out_buf, size_t max_len) noexcept;
void ProtocolDiag_GetActiveProfileKey(char *out_buf, size_t max_len) noexcept;
bool ProtocolDiag_GetCatalogMatch(char *vendor_buf, size_t v_len, size_t &device_count) noexcept;
size_t ProtocolDiag_GetProfileCount() noexcept;
bool ProtocolDiag_GetProfileInfo(size_t idx, ProfileInfoSnapshot &out) noexcept;
bool ProtocolDiag_SetActiveProfile(size_t slot) noexcept;
bool ProtocolDiag_SetActiveProfileByKey(const char *key) noexcept;
bool ProtocolDiag_SaveCurrentProfileAs(const char *name, size_t &saved_slot) noexcept;
bool ProtocolDiag_DeleteProfile(size_t idx) noexcept;
size_t ProtocolDiag_GetMaxProfiles() noexcept;
void ProtocolDiag_WallpadReset() noexcept;
/// Request the CH1 worker to re-run auto-probing convergence (one-shot).
void ProtocolDiag_RequestRelearn() noexcept;
/// Background commit for auto-probing profile learning result (Task_Ch1 non-blocking).
bool ProtocolDiag_CommitAutoProfileNvsIfPending() noexcept;

// ── Doorphone Management Facade ──────────────────────────────────────────────
void ProtocolDiag_DoorphoneClearNvs(const char *nvs_ns) noexcept;
void ProtocolDiag_DoorphoneGetFraming(FramingStatus &out_status, uint8_t &out_stx,
                                      uint8_t &out_etx, uint8_t &out_len) noexcept;
bool ProtocolDiag_GetDoorphoneMatch(DoorphoneMatchSnapshot &out) noexcept;

// ── Control Template & Group Query Facade ────────────────────────────────────
bool ProtocolDiag_BuildControlPacket(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                                     ControlActionType act, int val,
                                     StaticPacket &out_req) noexcept;
size_t ProtocolDiag_GetGroupCount() noexcept;
const char *ProtocolDiag_GetGroupName(uint8_t dev_id) noexcept;
bool ProtocolDiag_GetBlueprintAt(size_t index, BlueprintSnapshot &out) noexcept;
size_t ProtocolDiag_GetBlueprintsSnapshot(BlueprintSnapshot *out_array, size_t max_count) noexcept;
bool ProtocolDiag_GetBlueprint(uint8_t dev_id, BlueprintSnapshot &out) noexcept;
bool ProtocolDiag_SetGroupName(uint8_t dev_id, const char *name) noexcept;
bool ProtocolDiag_SetGroupClass(uint8_t dev_id, DeviceClass cls, const char *name) noexcept;
void ProtocolDiag_ResetGroup(uint8_t dev_id, bool all) noexcept;

// ── Parser & Frame Decoding Helpers (for EW11 demux) ─────────────────────────
uint8_t ProtocolDiag_GetActiveStx() noexcept;
uint8_t ProtocolDiag_GetActiveEtx() noexcept;
int ProtocolDiag_ExtractPacketLength(const uint8_t *buf, size_t len, size_t offset) noexcept;
bool ProtocolDiag_ValidatePacket(const uint8_t *buf, size_t len) noexcept;
bool ProtocolDiag_IsQueryPacket(const uint8_t *buf, size_t len) noexcept;
uint8_t ProtocolDiag_CalculateChecksum(const uint8_t *data, size_t len) noexcept;

// ── L2 RS485 Dispatcher SPI Binding & Lifecycle ─────────────────────────────
struct RS485_PacketDispatcher;
void Protocol_BindDispatcher(RS485_PacketDispatcher &dispatcher) noexcept;
struct Bridge_PacketDispatcher;
void Protocol_BindBridgeDispatcher(Bridge_PacketDispatcher &dispatcher) noexcept;
void Protocol_DoorphoneInit() noexcept;
void Protocol_DoorphoneRestoreNvs(const char *dp_ns) noexcept;
void Protocol_WarmCacheRestoreOnBoot() noexcept;



// ── L3 TCP Reactor Participant Registration Facade (Rule 17) ─────────────────
using TcpPopulateFdsFn = void (*)(fd_set &readfds, fd_set &errorfds, int &max_fd) noexcept;
using TcpProcessEventsFn = void (*)(fd_set &readfds, fd_set &errorfds, bool ota_now) noexcept;
using TcpTickFn = void (*)(bool ota_now, uint32_t now_ms) noexcept;

struct ProtocolTcpParticipant {
  const char *name{nullptr};
  TcpPopulateFdsFn populateFds{nullptr};
  TcpProcessEventsFn processEvents{nullptr};
  TcpTickFn tick{nullptr};
};

bool ProtocolDiag_RegisterTcpParticipant(const ProtocolTcpParticipant &p) noexcept;


