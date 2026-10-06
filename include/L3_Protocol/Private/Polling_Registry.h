#pragma once

// ============================================================================
// PollingRegistry: Level 3 1st-Tier Polling Target & Warm Cache Registry
// ============================================================================

#include "L0_Foundation/System_Config.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <freertos/FreeRTOS.h>

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
  bool is_verified{
      true}; // False if restored from warm cache until ACK/request seen
  std::array<uint8_t, 64> raw_query_data{};
  std::array<uint8_t, 64> raw_ack_data{};
};

// ── 1st Tier Warm-Start Cache Data Structures (Level 3 Wallpad Domain) ──
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

extern bool g_warm_cache_loaded;
extern uint8_t g_warm_cache_source; // 0: None/Cold, 1: RTC SRAM, 2: NVS Flash
extern uint8_t g_warm_cache_restored_count;
extern std::atomic<bool> g_warm_cache_dirty;
extern std::atomic<uint32_t> g_warm_cache_dirty_ms;

void WarmCache_SaveToRtc();
void WarmCache_SaveToNvs();
void WarmCache_RestoreOnBoot();
void WarmCache_CheckNvsDebounce();

class PollingTargetRegistry {
public:
  // 현대통신 세대망 환경(실제 28대 기기 수용 + 20대 여유 슬롯) 48대 설정
  static constexpr size_t MAX_TARGETS = 48;

  // 폴링 후보 선별을 위한 경량 메타데이터 구조체 (7 bytes)
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
  void updateResponse(const uint8_t *query_pkt, size_t query_len,
                      const uint8_t *ack_pkt, size_t ack_len);
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

  // Warm-Start Cache Interface
  void loadFromWarmCache(const RtcWarmCacheEntry *entries, size_t count,
                         uint32_t now_ms);
  size_t getWarmCacheEntries(RtcWarmCacheEntry *out_entries,
                             size_t max_count) const;
  void markVerified(uint8_t dev_id, uint8_t sub1, uint8_t sub2);
  size_t verifiedCount() const;
};

extern PollingTargetRegistry g_polling_targets;
