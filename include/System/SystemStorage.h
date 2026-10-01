#pragma once

#include <Arduino.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include "Base/SystemPlatform.h"
#include "Base/SystemConfig.h"
#include "Base/BufferUtils.h"

// ── 1st Tier Warm-Start Cache Data Structures ──

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
constexpr uint32_t RTC_MAGIC_CLEAN_RESTART = 0x434C4E52; // 'CLNR'
constexpr uint32_t RTC_MAGIC_RESCUE = 0x52455343; // 'RESC'
constexpr uint32_t RTC_MAGIC_WDT = 0x57445431; // 'WDT1'

// ── RTC Fast SRAM Retention Variables ──

extern uint32_t rtc_magic;
extern uint32_t rtc_last_alive_ms[Config::Task::TASK_COUNT];
extern volatile uint32_t g_telnet_stage;
#define TSTAGE(n)                                                              \
  (g_telnet_stage = (0xA5A50000u | (static_cast<uint32_t>(n) & 0xFFFFu)))
extern uint32_t rtc_rescue_magic;
extern uint32_t rtc_crash_counter;
extern uint32_t rtc_clean_restart_magic;
extern RtcWarmCache rtc_warm_cache;

// ── Warm Cache Status & Control ──

extern std::atomic<bool> g_rescue_mode;
extern bool g_rollback_detected;
extern bool g_warm_cache_loaded;
extern uint8_t g_warm_cache_source; // 0: None/Cold, 1: RTC SRAM, 2: NVS Flash
extern uint8_t g_warm_cache_restored_count;
extern std::atomic<bool> g_warm_cache_dirty;
extern std::atomic<uint32_t> g_warm_cache_dirty_ms;

void Cache_SaveToRtc();
void Cache_SaveToNvs();
void Cache_RestoreOnBoot();
void Cache_CheckNvsDebounce();
