#include "System/SystemStorage.h"
#include "Protocol/WallpadProtocol.h"
#include <Arduino.h>
#include <Preferences.h>

RTC_NOINIT_ATTR uint32_t rtc_magic;
RTC_NOINIT_ATTR uint32_t rtc_last_alive_ms[Config::Task::TASK_COUNT];
RTC_NOINIT_ATTR volatile uint32_t g_telnet_stage = 0;
RTC_NOINIT_ATTR uint32_t rtc_rescue_magic;
RTC_NOINIT_ATTR uint32_t rtc_crash_counter;
RTC_NOINIT_ATTR uint32_t rtc_clean_restart_magic;
RTC_NOINIT_ATTR RtcWarmCache rtc_warm_cache;

std::atomic<bool> g_rescue_mode{false};
bool g_rollback_detected = false;

bool g_warm_cache_loaded = false;
uint8_t g_warm_cache_source = 0; // 0: None/Cold, 1: RTC SRAM, 2: NVS Flash
uint8_t g_warm_cache_restored_count = 0;
std::atomic<bool> g_warm_cache_dirty{false};
std::atomic<uint32_t> g_warm_cache_dirty_ms{0};

namespace {
// Save와 Restore가 절대 동시 실행되지 않으므로 단일 정적 봉투를 공유하여 RAM
// 절감
static NvsEnvelope<RtcWarmCache> s_warm_cache_env;
} // anonymous namespace

void Cache_SaveToRtc() {
  memset(&rtc_warm_cache, 0, sizeof(rtc_warm_cache));
  rtc_warm_cache.magic = RTC_MAGIC_WARM_CACHE;
  rtc_warm_cache.count =
      static_cast<uint8_t>(g_polling_targets.getWarmCacheEntries(
          rtc_warm_cache.entries, PollingTargetRegistry::MAX_TARGETS));
  if (rtc_warm_cache.count > 0) {
    rtc_warm_cache.crc32 =
        FastCrc32(reinterpret_cast<const uint8_t *>(rtc_warm_cache.entries),
                  sizeof(RtcWarmCacheEntry) * rtc_warm_cache.count);
  }
}

void Cache_SaveToNvs() {
  Cache_SaveToRtc();
  if (rtc_warm_cache.count > 0) {
    Preferences p;
    if (p.begin("wp_wc", false)) {
      s_warm_cache_env.payload = rtc_warm_cache;
      s_warm_cache_env.seal();
      p.putBytes("wc_data", &s_warm_cache_env, sizeof(s_warm_cache_env));
      p.end();
      ::Serial.printf(
          "[WARM CACHE] Synced %u targets to NVS Flash snapshot.\r\n",
          rtc_warm_cache.count);
    }
  }
  g_warm_cache_dirty.store(false, std::memory_order_release);
}

void Cache_RestoreOnBoot() {
  uint32_t now = millis();
  esp_reset_reason_t reason = esp_reset_reason();

  // 1순위: RTC FAST SRAM에서 0ms 즉시 복원
  if (reason != ESP_RST_POWERON &&
      rtc_warm_cache.magic == RTC_MAGIC_WARM_CACHE &&
      rtc_warm_cache.count > 0 &&
      rtc_warm_cache.count <= PollingTargetRegistry::MAX_TARGETS) {
    uint32_t computed_crc =
        FastCrc32(reinterpret_cast<const uint8_t *>(rtc_warm_cache.entries),
                  sizeof(RtcWarmCacheEntry) * rtc_warm_cache.count);
    if (computed_crc == rtc_warm_cache.crc32) {
      g_polling_targets.loadFromWarmCache(rtc_warm_cache.entries,
                                          rtc_warm_cache.count, now);
      g_warm_cache_loaded = true;
      g_warm_cache_source = 1;
      g_warm_cache_restored_count = rtc_warm_cache.count;
      ::Serial.printf("[WARM CACHE] Restored %u targets from RTC Fast SRAM "
                      "(0ms delay)!\r\n",
                      rtc_warm_cache.count);
      return;
    }
  }

  // 2순위: NVS Flash 스냅샷에서 무결성 검증 후 복원
  Preferences p;
  if (p.begin("wp_wc", true)) {
    if (p.isKey("wc_data")) {
      size_t len = p.getBytesLength("wc_data");
      if (len == sizeof(s_warm_cache_env) &&
          p.getBytes("wc_data", &s_warm_cache_env, sizeof(s_warm_cache_env)) ==
              sizeof(s_warm_cache_env)) {
        if (s_warm_cache_env.verify() && s_warm_cache_env.payload.count > 0 &&
            s_warm_cache_env.payload.count <=
                PollingTargetRegistry::MAX_TARGETS) {
          uint32_t computed_crc = FastCrc32(
              reinterpret_cast<const uint8_t *>(
                  s_warm_cache_env.payload.entries),
              sizeof(RtcWarmCacheEntry) * s_warm_cache_env.payload.count);
          if (computed_crc == s_warm_cache_env.payload.crc32) {
            g_polling_targets.loadFromWarmCache(
                s_warm_cache_env.payload.entries,
                s_warm_cache_env.payload.count, now);
            g_warm_cache_loaded = true;
            g_warm_cache_source = 2;
            g_warm_cache_restored_count = s_warm_cache_env.payload.count;
            ::Serial.printf(
                "[WARM CACHE] Restored %u targets from NVS Flash snapshot!\r\n",
                s_warm_cache_env.payload.count);
            p.end();
            return;
          }
        }
      }
    }
    p.end();
  }

  g_warm_cache_loaded = false;
  g_warm_cache_source = 0;
  g_warm_cache_restored_count = 0;
  ::Serial.println(
      F("[WARM CACHE] Cold start initialized (No prior cache found)."));
}

void Cache_CheckNvsDebounce() {
  if (g_warm_cache_dirty.load(std::memory_order_acquire)) {
    uint32_t dirty_ms = g_warm_cache_dirty_ms.load(std::memory_order_relaxed);
    if (dirty_ms > 0 &&
        TimeUtils::isElapsed(dirty_ms,
                             Config::Timing::WARM_CACHE_NVS_DEBOUNCE_MS)) {
      Cache_SaveToNvs();
    }
  }
}
