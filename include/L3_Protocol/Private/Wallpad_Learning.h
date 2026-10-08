#pragma once

// ============================================================================
// WallpadRegistry: Level 3 Wallpad Device Learning, Polling & Control SSOT Domain
// ============================================================================
// Collocates Polling Target Registry, Auto-Probing Engine, and Group Control
// Template Registry into a single, cohesive private domain for Level 3 Protocol.
// ============================================================================

#include "L0_Foundation/System_Config.h"
#include "L0_Foundation/System_Platform.h"
#include "L3_Protocol/Public/Protocol_Device.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <span>

// ============================================================================
// 1. CHECKSUM ALGORITHM & AUTO-PROBING ENGINE DEFINITIONS
// ============================================================================

enum class ChecksumAlgo : uint8_t {
  UNKNOWN = 0,
  XOR_ALL = 1,         // XOR from 0 to N-3 (Hyundai HT, EzVille, etc.)
  XOR_NO_STX = 2,      // XOR from 1 to N-3 (Kocom)
  SUM_ALL = 3,         // Sum 0 to N-3 modulo 256 (Commax Legacy)
  SUM_NO_STX = 4,      // Sum 1 to N-3 modulo 256 (Commax Modern)
  TWOS_COMPLEMENT = 5, // (0x100 - Sum[1..N-3]) % 256 (Samsung SDS / EZON)
  ONES_COMPLEMENT = 6, // (~Sum[0..N-3]) % 256
  CRC8_MAXIM = 7,      // CRC-8 (poly 0x31, init 0x00)
  NONE = 8             // Pure framing without checksum byte (Doorphone 0x02..0x03)
};

struct AutoProbeDescriptor {
  uint8_t stx{0xF7};
  uint8_t etx{0xEE};
  uint8_t min_len{3};
  uint8_t max_len{64};
  ChecksumAlgo checksum_algo{ChecksumAlgo::XOR_ALL};
  uint8_t opcode_offset{4};
  uint8_t query_opcode{0x01};
  uint8_t control_opcode{0x00};
  uint8_t ack_opcode{0x04};
  bool opcodes_locked{false};
  bool control_seen{false};
  uint8_t dev_id_offset{3};
  uint8_t sub1_offset{5};
  uint8_t sub2_offset{6};
  uint8_t payload_offset{8};
  bool is_swapped_addr{false};
  bool offsets_locked{false};
  uint8_t gw_addr_offset{2};
  uint8_t gw_addr{0x01};
  uint8_t learned_query_len{11};
  uint8_t len_offset{0xFF};
  bool has_len_field{false};
  uint8_t seq_offset{0xFF};
  bool has_seq_counter{false};
  uint8_t ack_flag_offset{0xFF};
  uint8_t learned_ctrl_lens[4]{0};
  uint8_t ctrl_len_cnt{0};
  uint32_t matched_packets{0};
  uint32_t tested_packets{0};
  bool is_locked{false};
  char description[64]{"Probing bus traffic..."};
};

class AutoProbingEngine {
private:
  AutoProbeDescriptor _desc;
  uint16_t _stx_counts[256]{};
  uint16_t _etx_counts[256]{};
  uint16_t _algo_matches[9]{};
  uint16_t _consecutive_matches{0};
  uint16_t _consecutive_mismatches{0};
  ChecksumAlgo _candidate_algo{ChecksumAlgo::UNKNOWN};
  uint16_t _diff_idx_counts[16]{};
  uint8_t _control_matches{0};
  uint8_t _candidate_ctrl_op{0};
  mutable portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;

public:
  AutoProbingEngine();
  void initFromNvs();
  void feedFrame(std::span<const uint8_t> raw_frame);
  void feedOpcodePair(std::span<const uint8_t> req, std::span<const uint8_t> ack);
  void feedControlFrame(std::span<const uint8_t> ctrl_frame);
  bool isLocked() const;
  bool isOffsetsLocked() const;
  AutoProbeDescriptor getDescriptor() const;
  void reset();
  void injectControlSpec(uint8_t ctrl_op, uint8_t ctrl_len);
  bool analyzeCacheMatrix();
  uint8_t calculateChecksum(ChecksumAlgo algo,
                            std::span<const uint8_t> data) const noexcept;
  uint8_t calculateChecksum(ChecksumAlgo algo, const uint8_t *data,
                            size_t len) const;
  static const char *getAlgoName(ChecksumAlgo algo);
};

AutoProbingEngine &AutoProbe_GetEngine() noexcept;

// ============================================================================
// 2. POLLING TARGET REGISTRY & WARM-START CACHE DEFINITIONS
// ============================================================================

constexpr uint8_t kWallpadChMask = (1 << 2) | (1 << 3); // CH2, CH3 (월패드 유래)
constexpr uint32_t EXPIRED_TARGET_EVICTION_TIMEOUT_MS = 600000;

struct PollingTargetEntry {
  uint32_t last_requested_ms{0};
  uint32_t last_interval_ms{0};
  uint32_t restored_ms{0};
  uint16_t hit_count{0};
  uint8_t dev_id{0};
  uint8_t sub1{0};
  uint8_t sub2{0};
  uint8_t source_channels{0}; // Bitmask: bit 2=CH2, bit 3=CH3, bit 6=CH6
  uint8_t raw_query_len{0};
  uint8_t raw_ack_len{0};
  bool is_active{false};
  bool is_verified{true}; // False if restored from warm cache until ACK/request seen
  std::array<uint8_t, 64> raw_query_data{};
  std::array<uint8_t, 64> raw_ack_data{};
};

struct RtcWarmCacheEntry {
  uint8_t dev_id;
  uint8_t sub1;
  uint8_t sub2;
  uint8_t source_channels;
  uint8_t raw_len;
  uint8_t raw_query[32];
};

struct RtcWarmCache {
  uint32_t magic; // 0x57415243 ('WARC')
  uint8_t count;
  uint8_t reserved[3];
  RtcWarmCacheEntry entries[48];
  uint32_t crc32;
};

constexpr uint32_t RTC_MAGIC_WARM_CACHE = 0x57415243; // 'WARC'

struct WarmCacheStatus {
  bool loaded{false};
  uint8_t source{0}; // 0: None/Cold, 1: RTC SRAM, 2: NVS Flash
  uint8_t restored_count{0};
  bool dirty{false};
  uint32_t dirty_ms{0};
};

WarmCacheStatus WarmCache_GetStatus() noexcept;
void WarmCache_SaveToRtc();
void WarmCache_SaveToNvs();
void WarmCache_RestoreOnBoot();
void WarmCache_CheckNvsDebounce();

class PollingTargetRegistry {
public:
  static constexpr size_t MAX_TARGETS = 48;

  struct PollingCandidate {
    uint8_t dev_id{0};
    uint8_t sub1{0};
    uint8_t sub2{0};
    uint8_t source_channels{0};
    uint8_t raw_ack_len{0};
    uint8_t raw_query_len{0};
    uint8_t entry_idx{0};
  };

private:
  PollingTargetEntry _entries[MAX_TARGETS]{};
  size_t _count{0};
  mutable portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;

public:
  void registerOrTouch(uint8_t ch, uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                       const uint8_t *raw_pkt = nullptr, size_t raw_len = 0);
  void registerOrTouch(uint8_t ch, uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                       std::span<const uint8_t> raw_pkt) {
    registerOrTouch(ch, dev_id, sub1, sub2, raw_pkt.data(), raw_pkt.size());
  }
  void updateResponse(const uint8_t *query_pkt, size_t query_len,
                      const uint8_t *ack_pkt, size_t ack_len);
  void updateResponse(std::span<const uint8_t> query_pkt,
                      std::span<const uint8_t> ack_pkt) {
    updateResponse(query_pkt.data(), query_pkt.size(), ack_pkt.data(),
                   ack_pkt.size());
  }
  void reindexWithOffsets(uint8_t dev_id_offset, uint8_t sub1_offset,
                          uint8_t sub2_offset);
  void sweepExpired(uint32_t ttl_ms = 30000);
  size_t getActiveTargets(PollingTargetEntry *out_buf, size_t max_count);
  size_t getActiveCandidates(PollingCandidate *out_cands, size_t max_count);
  bool getQueryData(uint8_t entry_idx, const uint8_t *&out_data,
                    uint8_t &out_len) const;
  size_t activeCount() const;
  size_t totalCount() const;
  size_t ackedCount() const;
  bool getEntry(size_t index, PollingTargetEntry &out) const;
  void resetHits();
  void clear();

  void loadFromWarmCache(const RtcWarmCacheEntry *entries, size_t count,
                         uint32_t now_ms);
  size_t getWarmCacheEntries(RtcWarmCacheEntry *out_entries,
                             size_t max_count) const;
  void markVerified(uint8_t dev_id, uint8_t sub1, uint8_t sub2);
  size_t verifiedCount() const;
};

PollingTargetRegistry &Polling_GetRegistry() noexcept;

// ============================================================================
// 3. GROUP CONTROL TEMPLATE REGISTRY DEFINITIONS
// ============================================================================

struct PacketSnapshot {
  uint8_t len{0};
  uint8_t raw[32]{0};
};

struct AckStateSlots {
  bool discovered{false};
  uint8_t power_offset{0xFF};
  uint8_t target_temp_offset{0xFF};
  uint8_t current_temp_offset{0xFF};
  uint8_t fan_speed_offset{0xFF};
  uint8_t valve_state_offset{0xFF};
  uint8_t sample_count{0};
  uint32_t power_changed_mask{0};
};

struct QueryStateSlots {
  bool discovered{false};
  uint8_t expected_len{0};
  uint8_t power_offset{0xFF};
  uint8_t target_temp_offset{0xFF};
  uint8_t current_temp_offset{0xFF};
  uint8_t fan_speed_offset{0xFF};
  uint8_t power_w_offset{0xFF};
  uint8_t valve_state_offset{0xFF};
  bool is_bitmap_power{false};
};

struct GroupControlTemplate {
  uint8_t dev_id{0x00};
  char group_name[16]{"Unknown"};
  uint8_t frame_len{0};
  uint8_t raw_template[32]{0};

  uint8_t sub1_offset{0xFF};
  uint8_t sub2_offset{0xFF};
  uint8_t ctl_sub1_override{0xFF};

  ActionSlot power_slot;
  ActionSlot temp_slot;
  ActionSlot speed_slot;
  ActionSlot mode_slot;
  ActionSlot close_slot;

  SlotCoverage coverage;
  uint8_t away_mode_token{0xFF};

  AckStateSlots ack_slots;
  QueryStateSlots query_slots;

  uint8_t getPowerOffset(uint8_t pkt_len = 0) const noexcept;
  uint8_t getTargetTempOffset(uint8_t pkt_len = 0) const noexcept;
  uint8_t getCurrentTempOffset(uint8_t pkt_len = 0) const noexcept;
  uint8_t getFanSpeedOffset(uint8_t pkt_len = 0) const noexcept;
  uint8_t decodeFanSpeed(uint8_t raw_token) const noexcept;
  uint8_t decodeVentMode(uint8_t raw_byte) const noexcept;
  uint8_t getValveStateOffset(uint8_t pkt_len = 0) const noexcept;
  uint8_t getWattageOffset(uint8_t pkt_len = 0) const noexcept;
  bool isUnidirectional() const noexcept;
};

static_assert(sizeof(GroupControlTemplate) == 180,
              "NVS ABI break: GroupControlTemplate size changed");

class ControlTemplateRegistry {
public:
  static constexpr size_t MAX_GROUPS = 8;
  static constexpr TickType_t kQueryLockTimeout = pdMS_TO_TICKS(5);
  static constexpr TickType_t kManageLockTimeout = pdMS_TO_TICKS(50);

  ControlTemplateRegistry();
  void init();
  void clear();
  void synthesizeFromConvergedCache();

  bool findGroup(uint8_t dev_id, GroupControlTemplate &out,
                 TickType_t timeout = kQueryLockTimeout) const;
  size_t getGroupsSnapshot(GroupControlTemplate *out_buf, size_t max_count,
                           TickType_t timeout = kQueryLockTimeout) const;
  size_t getGroupCount() const;
  bool getGroupByIndex(size_t index, GroupControlTemplate &out) const;
  bool resetGroup(uint8_t dev_id, bool full_reset = false);
  bool setGroupName(uint8_t dev_id, const char *name);
  bool setGroupClass(uint8_t dev_id, DeviceClass cls,
                     const char *name = nullptr);

  template <typename Func>
  bool modifyOrCreateGroup(uint8_t dev_id, Func &&mutator,
                           const char *initial_name = nullptr,
                           TickType_t timeout = kManageLockTimeout) {
    if (dev_id == 0)
      return false;
    MutexLocker lock(_mutex, timeout);
    if (!lock.isLocked())
      return false;
    GroupControlTemplate *grp = registerOrTouchUnlocked(dev_id, initial_name);
    if (!grp)
      return false;
    mutator(*grp);
    rebuildNormSub1LutLocked();
    return true;
  }

  bool buildControlPacket(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                          ControlActionType action, int value,
                          StaticPacket &out) const;

  void applyProfile(const struct WallpadProfile *profile);
  void matchAndInject(const struct AutoProbeDescriptor &ad);

  bool saveToNvs();
  void loadFromNvs();
  bool saveToNvsForProfile(uint8_t prof_idx);
  void loadFromNvsForProfile(uint8_t prof_idx);
  void onProfileChanged(uint8_t old_prof_idx, uint8_t new_prof_idx);

private:
  GroupControlTemplate _groups[MAX_GROUPS];
  size_t _group_count{0};
  mutable StaticSemaphore_t _mutex_storage{};
  mutable SemaphoreHandle_t _mutex{nullptr};
  mutable StaticSemaphore_t _nvs_mutex_storage{};
  mutable SemaphoreHandle_t _nvs_mutex{nullptr};

  void rebuildNormSub1LutLocked() noexcept;
  void autoAssignGroupName(GroupControlTemplate &group);
  GroupControlTemplate *registerOrTouchUnlocked(uint8_t dev_id,
                                                const char *name = nullptr);
};

ControlTemplateRegistry &Control_GetRegistry() noexcept;

namespace ControlTemplateUtils {
inline void getControlNamespace(char *out_ns, size_t max_len,
                                uint8_t prof_idx) {
  snprintf(out_ns, max_len, "ctl_p%u", prof_idx);
}

inline uint8_t getCurrentProfileIndex() {
  return Config_GetWallpadProfile();
}
} // namespace ControlTemplateUtils

void ControlTemplate_DecodeDeviceState(const GroupControlTemplate &grp,
                                       const StaticPacket &ack,
                                       const DeviceStateEntry *dev,
                                       DecodedDeviceState &out) noexcept;

bool ControlTemplate_DecodeByDevId(uint8_t dev_id,
                                   const StaticPacket &ack,
                                   const DeviceStateEntry *dev,
                                   DecodedDeviceState &out) noexcept;

uint8_t ControlTemplate_NormSub1(uint8_t dev_id, uint8_t sub1) noexcept;
