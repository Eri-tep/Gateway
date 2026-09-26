#include "core/Platform.h"
#include "core/Config.h"
#include "core/Devices.h"
#include "WallpadParser.h"
#include <Preferences.h>

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
      static NvsEnvelope<RtcWarmCache> env;
      env.payload = rtc_warm_cache;
      env.seal();
      p.putBytes("wc_data", &env, sizeof(env));
      p.end();
      Serial.printf("[WARM CACHE] Synced %u targets to NVS Flash snapshot.\r\n",
                    rtc_warm_cache.count);
    }
  }
  g_warm_cache_dirty.store(false, std::memory_order_release);
}

void Cache_RestoreOnBoot() {
  uint32_t now = millis();
  esp_reset_reason_t reason = esp_reset_reason();

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
      Serial.printf("[WARM CACHE] Restored %u targets from RTC Fast SRAM (0ms "
                    "delay)!\r\n",
                    rtc_warm_cache.count);
      return;
    }
  }

  Preferences p;
  if (p.begin("wp_wc", true)) {
    if (p.isKey("wc_data")) {
      static NvsEnvelope<RtcWarmCache> env;
      size_t len = p.getBytesLength("wc_data");
      if (len == sizeof(env) &&
          p.getBytes("wc_data", &env, sizeof(env)) == sizeof(env)) {
        if (env.verify() && env.payload.count > 0 &&
            env.payload.count <= PollingTargetRegistry::MAX_TARGETS) {
          uint32_t computed_crc =
              FastCrc32(reinterpret_cast<const uint8_t *>(env.payload.entries),
                        sizeof(RtcWarmCacheEntry) * env.payload.count);
          if (computed_crc == env.payload.crc32) {
            g_polling_targets.loadFromWarmCache(env.payload.entries,
                                                env.payload.count, now);
            g_warm_cache_loaded = true;
            g_warm_cache_source = 2;
            g_warm_cache_restored_count = env.payload.count;
            Serial.printf(
                "[WARM CACHE] Restored %u targets from NVS Flash snapshot!\r\n",
                env.payload.count);
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
  Serial.println(
      F("[WARM CACHE] Cold start initialized (No prior cache found)."));
}

void Cache_CheckNvsDebounce() {
  if (g_warm_cache_dirty.load(std::memory_order_acquire)) {
    uint32_t dirty_ms = g_warm_cache_dirty_ms.load(std::memory_order_relaxed);
    if (dirty_ms > 0 &&
        TimeUtils::isElapsed(dirty_ms,
                             Config::Timing::WARM_CACHE_NVS_DEBOUNCE_MS)) {
      Cache_SaveToRtc();
      Cache_SaveToNvs();
    }
  }
}
