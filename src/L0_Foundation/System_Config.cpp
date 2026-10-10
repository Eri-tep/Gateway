#include "L0_Foundation/System_Config.h"
#include "L0_Foundation/System_Buffer.h"
#include "L0_Foundation/System_Platform.h"
#include <Arduino.h>
#include <Preferences.h>
#include <cstring>
#include <esp_idf_version.h>
#include <mbedtls/sha256.h>

static RuntimeConfig s_config{};
alignas(64) static RuntimeConfig s_config_banks[2]{};
static std::atomic<uint8_t> s_active_config_idx{0};
static std::shared_mutex s_config_rw;
static std::atomic<bool> s_config_dirty{false};

static void updateActiveConfig(const RuntimeConfig &new_cfg) noexcept {
  const uint8_t curr = s_active_config_idx.load(std::memory_order_relaxed);
  const uint8_t next_idx = (curr == 0) ? 1 : 0;
  s_config_banks[next_idx] = new_cfg;
  s_active_config_idx.store(next_idx, std::memory_order_release);
}

static std::atomic<bool> s_frozen{false};
static std::atomic<uint8_t> s_active_wallpad_profile{0};
static std::mutex s_save_mutex;

static std::atomic<uint32_t> s_nvs_write_errors{0};
static std::atomic<uint32_t> s_nvs_last_sync_ms{0};
static std::atomic<uint32_t> s_nvs_last_duration_ms{0};
static uint8_t s_active_bank{0};
static RuntimeConfig s_persisted_config{};
static bool s_has_persisted_config{false};

namespace {
// Pillar 4 Table-Driven NVS Delta Save Definitions
struct PropU32 {
  const char *key;
  uint32_t RuntimeConfig::*member;
};
struct PropU16 {
  const char *key;
  uint16_t RuntimeConfig::*member;
};
struct PropU8 {
  const char *key;
  uint8_t RuntimeConfig::*member;
};
struct PropStr {
  const char *key;
  size_t offset;
  size_t max_len;
};

#define PROP_STR_ENTRY(k, field) \
  { k, offsetof(RuntimeConfig, field), sizeof(RuntimeConfig::field) }

inline constexpr PropU32 kPropsU32[] = {
    {"uart_baud", &RuntimeConfig::uart_baud_rate},
    {"ch2_baud", &RuntimeConfig::ch2_baud_rate},
    {"ch3_baud", &RuntimeConfig::ch3_baud_rate},
    {"door_baud", &RuntimeConfig::doorphone_baud_rate},
};

inline constexpr PropU16 kPropsU16[] = {
    {"w_tout", &RuntimeConfig::wifi_connect_timeout_s},
};

inline constexpr PropU8 kPropsU8[] = {
    {"u_parity", &RuntimeConfig::uart_parity},
    {"u_sbits", &RuntimeConfig::uart_stop_bits},
    {"u_dbits", &RuntimeConfig::uart_data_bits},
    {"ch2_parity", &RuntimeConfig::ch2_parity},
    {"ch2_sbits", &RuntimeConfig::ch2_stop_bits},
    {"ch2_dbits", &RuntimeConfig::ch2_data_bits},
    {"ch3_parity", &RuntimeConfig::ch3_parity},
    {"ch3_sbits", &RuntimeConfig::ch3_stop_bits},
    {"ch3_dbits", &RuntimeConfig::ch3_data_bits},
    {"d_dbits", &RuntimeConfig::doorphone_data_bits},
    {"d_parity", &RuntimeConfig::doorphone_parity},
    {"d_sbits", &RuntimeConfig::doorphone_stop_bits},
    {"w_prof", &RuntimeConfig::wallpad_profile},
};

inline constexpr PropStr kPropsStr[] = {
    PROP_STR_ENTRY("wifi_ssid", wifi_ssid),
    PROP_STR_ENTRY("wifi_pass", wifi_password),
    PROP_STR_ENTRY("ap_ssid", ap_ssid),
    PROP_STR_ENTRY("ap_pass", ap_password),
    PROP_STR_ENTRY("telnet_hash", telnet_pass_hash),
};

// Static asserts for NVS 15-character key limit
static_assert([] {
  for (const auto &p : kPropsU32) {
    if (std::string_view(p.key).size() > 15) return false;
  }
  for (const auto &p : kPropsU16) {
    if (std::string_view(p.key).size() > 15) return false;
  }
  for (const auto &p : kPropsU8) {
    if (std::string_view(p.key).size() > 15) return false;
  }
  for (const auto &p : kPropsStr) {
    if (std::string_view(p.key).size() > 15) return false;
  }
  return true;
}(), "NVS key length exceeds 15 characters limit");

void saveDeltaProperties(Preferences &p, const RuntimeConfig &target,
                         RuntimeConfig &shadow, bool has_shadow) {
  for (const auto &prop : kPropsU32) {
    const uint32_t val = target.*(prop.member);
    if (!has_shadow || val != shadow.*(prop.member)) {
      if (p.putULong(prop.key, val) > 0) {
        shadow.*(prop.member) = val;
      }
    }
  }
  for (const auto &prop : kPropsU16) {
    const uint16_t val = target.*(prop.member);
    if (!has_shadow || val != shadow.*(prop.member)) {
      if (p.putUShort(prop.key, val) > 0) {
        shadow.*(prop.member) = val;
      }
    }
  }
  for (const auto &prop : kPropsU8) {
    const uint8_t val = target.*(prop.member);
    if (!has_shadow || val != shadow.*(prop.member)) {
      if (p.putUChar(prop.key, val) > 0) {
        shadow.*(prop.member) = val;
      }
    }
  }
  const auto *target_bytes = reinterpret_cast<const char *>(&target);
  auto *shadow_bytes = reinterpret_cast<char *>(&shadow);
  for (const auto &prop : kPropsStr) {
    const char *target_str = target_bytes + prop.offset;
    char *shadow_str = shadow_bytes + prop.offset;
    if (!has_shadow || strncmp(target_str, shadow_str, prop.max_len) != 0) {
      if (p.putString(prop.key, target_str) > 0) {
        strncpy(shadow_str, target_str, prop.max_len - 1);
        shadow_str[prop.max_len - 1] = '\0';
      }
    }
  }
}
} // namespace

void System_GetNvsStats(uint32_t &err_count, uint32_t &last_sync_ms,
                        uint32_t *last_duration_ms) noexcept {
  err_count = s_nvs_write_errors.load(std::memory_order_relaxed);
  last_sync_ms = s_nvs_last_sync_ms.load(std::memory_order_relaxed);
  if (last_duration_ms) {
    *last_duration_ms = s_nvs_last_duration_ms.load(std::memory_order_relaxed);
  }
}

void System_ResetNvsStats() noexcept {
  s_nvs_write_errors.store(0, std::memory_order_relaxed);
  s_nvs_last_sync_ms.store(0, std::memory_order_relaxed);
  s_nvs_last_duration_ms.store(0, std::memory_order_relaxed);
}



namespace Config::Timing {
uint32_t getDoorphoneInterByteTimeoutMs(uint32_t baud) noexcept {
  if (baud == 0)
    return DEFAULT_DOORPHONE_INTER_BYTE_TIMEOUT_MS;
  const uint32_t timeout = (60000UL + baud - 1) / baud;
  return (timeout < 6) ? 6 : (timeout > 20 ? 20 : timeout);
}
} // namespace Config::Timing

const char *formatFramingStr(uint8_t data_bits, uint8_t parity,
                             uint8_t stop_bits) noexcept {
  if (data_bits == 8) [[likely]] {
    if (parity == 0 && stop_bits == 1)
      return "8N1";
    if (parity == 1 && stop_bits == 1)
      return "8E1";
    if (parity == 2 && stop_bits == 1)
      return "8O1";
    if (parity == 0 && stop_bits == 2)
      return "8N2";
  }
  return "8N1";
}

[[nodiscard]] std::expected<FramingConfig, FramingParseError>
parseFramingStr(std::string_view str) noexcept {
  if (str.size() != 3) {
    return std::unexpected(FramingParseError::InvalidLength);
  }
  char d = str[0];
  char p = static_cast<char>(toupper(static_cast<unsigned char>(str[1])));
  char s = str[2];

  if (d == '8' && p == 'N' && s == '1') {
    return FramingConfig{8, 0, 1};
  }
  if (d == '8' && p == 'E' && s == '1') {
    return FramingConfig{8, 1, 1};
  }
  if (d == '8' && p == 'O' && s == '1') {
    return FramingConfig{8, 2, 1};
  }
  if (d == '8' && p == 'N' && s == '2') {
    return FramingConfig{8, 0, 2};
  }
  return std::unexpected(FramingParseError::UnsupportedFormat);
}

bool parseFramingStr(const char *str, uint8_t &data_bits, uint8_t &parity,
                     uint8_t &stop_bits) noexcept {
  if (!str) [[unlikely]]
    return false;
  return parseFramingStr(std::string_view(str))
      .transform([&](const FramingConfig &cfg) noexcept {
        data_bits = cfg.data_bits;
        parity = cfg.parity;
        stop_bits = cfg.stop_bits;
        return true;
      })
      .value_or(false);
}

void System_Sha256ToHex(const char *input, char *output) {
  if (!input || !output)
    return;
  uint8_t hash[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
#if defined(ESP_IDF_VERSION) && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(
      &ctx, reinterpret_cast<const unsigned char *>(input), strlen(input));
  mbedtls_sha256_finish(&ctx, hash);
#else
  mbedtls_sha256_starts_ret(&ctx, 0);
  mbedtls_sha256_update_ret(
      &ctx, reinterpret_cast<const unsigned char *>(input), strlen(input));
  mbedtls_sha256_finish_ret(&ctx, hash);
#endif
  mbedtls_sha256_free(&ctx);

  for (size_t i = 0; i < 32; i++) {
    const auto &hex_chars = HexLUT::LUT[hash[i]];
    output[i * 2] = hex_chars[0];
    output[i * 2 + 1] = hex_chars[1];
  }
  output[64] = '\0';
}

void Config_Load() {
  Preferences p;
  const bool p_opened = p.begin("runtime-config", true);
  std::unique_lock lock(s_config_rw);
  auto &c = s_config;

  if (p_opened) {
    // 1. Try atomic sealed A/B Ping-Pong envelope first (Power-cut resilient)
    RuntimeConfig env_cfg{};
    const uint8_t active_bank = p.getUChar("cfg_act", 0);
    const char *primary_key = (active_bank == 1) ? "cfg_bin_1" : "cfg_bin_0";
    const char *secondary_key = (active_bank == 1) ? "cfg_bin_0" : "cfg_bin_1";

    auto load_bank = [&]() {
      if (nvsGetEnv(p, primary_key, env_cfg)) {
        c = env_cfg;
        s_active_bank = active_bank;
        ESP_LOGI("CONFIG", "Loaded atomic A/B bank %u (CRC-32 verified)", active_bank);
        return true;
      }
      if (nvsGetEnv(p, secondary_key, env_cfg)) {
        c = env_cfg;
        s_active_bank = (active_bank == 1) ? 0 : 1;
        ESP_LOGW("CONFIG", "Primary bank %u corrupted; recovered from alternate bank %u (CRC-32 verified)",
                 active_bank, s_active_bank);
        return true;
      }
      if (nvsGetEnv(p, "cfg_bin", env_cfg)) {
        c = env_cfg;
        s_active_bank = 0;
        ESP_LOGI("CONFIG", "Loaded legacy cfg_bin (CRC-32 verified)");
        return true;
      }
      return false;
    };
    bool loaded = load_bank();

    if (!loaded) {
      // 2. Fallback to individual legacy NVS keys
      c.uart_baud_rate = p.getULong("uart_baud", 9600);
      c.ch2_baud_rate = p.getULong("ch2_baud", 9600);
      c.ch3_baud_rate = p.getULong("ch3_baud", 9600);
      c.doorphone_baud_rate =
          p.getULong("door_baud", Config::Serial::DEFAULT_DOORPHONE_BAUD);

      c.wifi_ssid[0] = '\0';
      p.getString("wifi_ssid", c.wifi_ssid, sizeof(c.wifi_ssid));
      c.wifi_password[0] = '\0';
      p.getString("wifi_pass", c.wifi_password, sizeof(c.wifi_password));
      c.ap_ssid[0] = '\0';
      p.getString("ap_ssid", c.ap_ssid, sizeof(c.ap_ssid));
      c.ap_password[0] = '\0';
      p.getString("ap_pass", c.ap_password, sizeof(c.ap_password));
      c.telnet_pass_hash[0] = '\0';
      p.getString("telnet_hash", c.telnet_pass_hash, sizeof(c.telnet_pass_hash));

      c.uart_parity = p.getUChar("u_parity", 0);
      c.uart_stop_bits = p.getUChar("u_sbits", 1);
      c.uart_data_bits = p.getUChar("u_dbits", 8);
      c.ch2_parity = p.getUChar("ch2_parity", 0);
      c.ch2_stop_bits = p.getUChar("ch2_sbits", 1);
      c.ch2_data_bits = p.getUChar("ch2_dbits", 8);
      c.ch3_parity = p.getUChar("ch3_parity", 0);
      c.ch3_stop_bits = p.getUChar("ch3_sbits", 1);
      c.ch3_data_bits = p.getUChar("ch3_dbits", 8);
      c.doorphone_data_bits =
          p.getUChar("d_dbits", Config::Serial::DEFAULT_DOORPHONE_DATABITS);
      c.doorphone_parity =
          p.getUChar("d_parity", Config::Serial::DEFAULT_DOORPHONE_PARITY);
      c.doorphone_stop_bits =
          p.getUChar("d_sbits", Config::Serial::DEFAULT_DOORPHONE_STOPBITS);
      c.wifi_connect_timeout_s = p.getUShort("w_tout", 30);
      auto raw_prof = nvsReadPrimitive<uint8_t>(p, "w_prof");
      if (!raw_prof && raw_prof.error() != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW("CONFIG", "w_prof: %s, using default ADAPTIVE", esp_err_to_name(raw_prof.error()));
      }
      const auto prof = toEnum(raw_prof.value_or(0),
                               WallpadProfileIndex::ADAPTIVE,
                               WallpadProfileIndex::CUSTOM3);
      if (raw_prof && !prof) {
        ESP_LOGW("CONFIG", "w_prof out of range (%u), using default ADAPTIVE",
                 static_cast<unsigned>(*raw_prof));
      }
      c.wallpad_profile = std::to_underlying(prof.value_or(WallpadProfileIndex::ADAPTIVE));
    }
    s_active_wallpad_profile.store(c.wallpad_profile, std::memory_order_relaxed);
    p.end();
  } else {
    ESP_LOGW("CONFIG", "Failed to open NVS runtime-config (read-only); using defaults");
  }

  // Defensive Range Validation & Clamping (Pillar 2 Safe Fallback)
  auto clampBaud = [](uint32_t baud, uint32_t def_val) -> uint32_t {
    return (baud >= 1200 && baud <= 921600) ? baud : def_val;
  };
  c.uart_baud_rate = clampBaud(c.uart_baud_rate, 9600);
  c.ch2_baud_rate = clampBaud(c.ch2_baud_rate, 9600);
  c.ch3_baud_rate = clampBaud(c.ch3_baud_rate, 9600);
  c.doorphone_baud_rate = clampBaud(c.doorphone_baud_rate, Config::Serial::DEFAULT_DOORPHONE_BAUD);

  auto clampBits = [](uint8_t bits, uint8_t def_val) -> uint8_t {
    return (bits >= 5 && bits <= 8) ? bits : def_val;
  };
  c.uart_data_bits = clampBits(c.uart_data_bits, 8);
  c.ch2_data_bits = clampBits(c.ch2_data_bits, 8);
  c.ch3_data_bits = clampBits(c.ch3_data_bits, 8);
  c.doorphone_data_bits = clampBits(c.doorphone_data_bits, Config::Serial::DEFAULT_DOORPHONE_DATABITS);

  auto clampParity = [](uint8_t par, uint8_t def_val) -> uint8_t {
    return (par <= 2) ? par : def_val;
  };
  c.uart_parity = clampParity(c.uart_parity, 0);
  c.ch2_parity = clampParity(c.ch2_parity, 0);
  c.ch3_parity = clampParity(c.ch3_parity, 0);
  c.doorphone_parity = clampParity(c.doorphone_parity, Config::Serial::DEFAULT_DOORPHONE_PARITY);

  auto clampStop = [](uint8_t stop, uint8_t def_val) -> uint8_t {
    return (stop >= 1 && stop <= 2) ? stop : def_val;
  };
  c.uart_stop_bits = clampStop(c.uart_stop_bits, 1);
  c.ch2_stop_bits = clampStop(c.ch2_stop_bits, 1);
  c.ch3_stop_bits = clampStop(c.ch3_stop_bits, 1);
  c.doorphone_stop_bits = clampStop(c.doorphone_stop_bits, Config::Serial::DEFAULT_DOORPHONE_STOPBITS);

  if (c.wifi_connect_timeout_s < 5 || c.wifi_connect_timeout_s > 300) {
    c.wifi_connect_timeout_s = 30;
  }
  if (c.wallpad_profile > kWallpadProfileMax) {
    c.wallpad_profile = 0;
  }

  uint16_t mac_suffix = static_cast<uint16_t>(ESP.getEfuseMac() >> 32);

  if (strlen(c.wifi_ssid) == 0) {
#ifdef WIFI_SSID
    strncpy(c.wifi_ssid, WIFI_SSID, sizeof(c.wifi_ssid) - 1);
    c.wifi_ssid[sizeof(c.wifi_ssid) - 1] = '\0';
#endif
  }

  if (strlen(c.wifi_password) == 0) {
#ifdef WIFI_PASSWORD
    strncpy(c.wifi_password, WIFI_PASSWORD, sizeof(c.wifi_password) - 1);
    c.wifi_password[sizeof(c.wifi_password) - 1] = '\0';
#endif
  }

  if (strlen(c.ap_ssid) == 0) {
    snprintf(c.ap_ssid, sizeof(c.ap_ssid), "Gateway-Setup-%04X", mac_suffix);
  }

  if (strlen(c.ap_password) < 8) {
#ifdef EMERGENCY_AP_PASS
    strncpy(c.ap_password, EMERGENCY_AP_PASS, sizeof(c.ap_password) - 1);
    c.ap_password[sizeof(c.ap_password) - 1] = '\0';
#else
    strncpy(c.ap_password, "9dnjf1!DLF", sizeof(c.ap_password) - 1);
    c.ap_password[sizeof(c.ap_password) - 1] = '\0';
#endif
  }

  if (strlen(c.telnet_pass_hash) == 0) {
#ifdef DEFAULT_TELNET_PASS
    System_Sha256ToHex(DEFAULT_TELNET_PASS, c.telnet_pass_hash);
#endif
  }

  s_persisted_config = c;
  s_has_persisted_config = true;
  updateActiveConfig(c);
}

void Config_Save() {
  if (!s_config_dirty.load(std::memory_order_acquire))
    return;

  std::lock_guard<std::mutex> save_lock(s_save_mutex);

  RuntimeConfig snapshot;
  {
    std::unique_lock lock(s_config_rw);
    snapshot = s_config;
    snapshot.wallpad_profile =
        s_active_wallpad_profile.load(std::memory_order_relaxed);
    s_config_dirty.store(false, std::memory_order_release);
  }

  Preferences p;
  if (!p.begin("runtime-config", false)) {
    ESP_LOGE("CONFIG", "Failed to open NVS runtime-config for write; restoring dirty flag");
    s_config_dirty.store(true, std::memory_order_release);
    s_nvs_write_errors.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  const uint32_t t0 = millis();

  // 1. A/B Ping-Pong Atomic sealed binary envelope write (Power-cut resilient)
  const uint8_t target_bank = (s_active_bank == 0) ? 1 : 0;
  const char *target_key = (target_bank == 1) ? "cfg_bin_1" : "cfg_bin_0";

  if (!nvsPutEnv(p, target_key, snapshot)) {
    ESP_LOGE("CONFIG", "Failed to write atomic %s envelope; restoring dirty flag", target_key);
    s_config_dirty.store(true, std::memory_order_release);
    s_nvs_write_errors.fetch_add(1, std::memory_order_relaxed);
    p.end();
    return;
  }

  p.putUChar("cfg_act", target_bank);
  s_active_bank = target_bank;

  // 2. Backward compatibility Delta Write (Table-Driven Dispatch)
  saveDeltaProperties(p, snapshot, s_persisted_config, s_has_persisted_config);

  p.end();

  s_has_persisted_config = true;

  const uint32_t dur = millis() - t0;
  s_nvs_last_duration_ms.store(dur, std::memory_order_relaxed);
  s_nvs_last_sync_ms.store(millis(), std::memory_order_relaxed);
}

void Config_ResetDefaults() {
  std::unique_lock lock(s_config_rw);
  s_config = RuntimeConfig{};
  updateActiveConfig(s_config);
  s_config_dirty.store(true, std::memory_order_release);
}

bool Config_SetUartFraming(uint8_t ch, uint32_t baud, uint8_t data_bits,
                           uint8_t parity, uint8_t stop_bits) noexcept {
  if (baud < 1200 || baud > 921600)
    return false;

  std::unique_lock lock(s_config_rw);
  switch (ch) {
  case 1:
    s_config.uart_baud_rate = baud;
    s_config.uart_data_bits = data_bits;
    s_config.uart_parity = parity;
    s_config.uart_stop_bits = stop_bits;
    break;
  case 2:
    s_config.ch2_baud_rate = baud;
    s_config.ch2_data_bits = data_bits;
    s_config.ch2_parity = parity;
    s_config.ch2_stop_bits = stop_bits;
    break;
  case 3:
    s_config.ch3_baud_rate = baud;
    s_config.ch3_data_bits = data_bits;
    s_config.ch3_parity = parity;
    s_config.ch3_stop_bits = stop_bits;
    break;
  case 4:
    s_config.doorphone_baud_rate = baud;
    s_config.doorphone_data_bits = data_bits;
    s_config.doorphone_parity = parity;
    s_config.doorphone_stop_bits = stop_bits;
    break;
  default:
    return false;
  }
  updateActiveConfig(s_config);
  s_config_dirty.store(true, std::memory_order_release);
  return true;
}

static RuntimeTimingConfig s_timing_config{};

void TimingConfig_Load() {
  Preferences p;
  if (p.begin("timing_cfg", true)) {
    RuntimeTimingConfig env_timing{};
    if (nvsGetEnv(p, "tm_bin", env_timing)) {
      s_timing_config = env_timing;
    } else {
      s_timing_config.ch1_poll_interval_ms = p.getUShort("ch1_poll", 1000);
      s_timing_config.ch2_cache_delay_ms = p.getUShort("ch2_del", 30);
      s_timing_config.ch3_cache_delay_ms = p.getUShort("ch3_del", 240);
    }
    p.end();
  } else {
    s_timing_config.ch1_poll_interval_ms = 1000;
    s_timing_config.ch2_cache_delay_ms = 30;
    s_timing_config.ch3_cache_delay_ms = 240;
  }

  if (s_timing_config.ch1_poll_interval_ms < 200 ||
      s_timing_config.ch1_poll_interval_ms > 5000)
    s_timing_config.ch1_poll_interval_ms = 1000;
  if (s_timing_config.ch2_cache_delay_ms < 5 ||
      s_timing_config.ch2_cache_delay_ms > 300)
    s_timing_config.ch2_cache_delay_ms = 30;
  if (s_timing_config.ch3_cache_delay_ms < 20 ||
      s_timing_config.ch3_cache_delay_ms > 1000)
    s_timing_config.ch3_cache_delay_ms = 240;

  ::Serial.printf(
      "[TIMING] Loaded: CH1 Poll %u ms, CH2 Delay %u ms, CH3 Delay %u ms\r\n",
      s_timing_config.ch1_poll_interval_ms, s_timing_config.ch2_cache_delay_ms,
      s_timing_config.ch3_cache_delay_ms);
}

void TimingConfig_Save() {
  Preferences p;
  if (p.begin("timing_cfg", false)) {
    nvsPutEnv(p, "tm_bin", s_timing_config);
    p.putUShort("ch1_poll", s_timing_config.ch1_poll_interval_ms);
    p.putUShort("ch2_del", s_timing_config.ch2_cache_delay_ms);
    p.putUShort("ch3_del", s_timing_config.ch3_cache_delay_ms);
    p.end();
    s_nvs_last_sync_ms.store(millis(), std::memory_order_relaxed);
    ::Serial.printf("[TIMING] Saved to NVS: CH1 Poll %u ms, CH2 Delay %u ms, "
                    "CH3 Delay %u ms\r\n",
                    s_timing_config.ch1_poll_interval_ms,
                    s_timing_config.ch2_cache_delay_ms,
                    s_timing_config.ch3_cache_delay_ms);
  } else {
    s_nvs_write_errors.fetch_add(1, std::memory_order_relaxed);
  }
}

static_assert(std::is_trivially_copyable_v<RuntimeConfig>,
              "RuntimeConfig must be trivially copyable for staging");
static_assert(std::is_trivially_copyable_v<RuntimeTimingConfig>,
              "RuntimeTimingConfig must be trivially copyable");

const RuntimeConfig &Config_Get() noexcept {
#if defined(DEBUG) || !defined(NDEBUG)
  assert(s_frozen.load(std::memory_order_relaxed) &&
         "[ASSERT] Config_Get() called before Config_Freeze()!");
#endif
  return s_config_banks[s_active_config_idx.load(std::memory_order_acquire) & 1U];
}

const RuntimeTimingConfig &TimingConfig_Get() noexcept {
  return s_timing_config;
}

void Config_Freeze() noexcept {
  s_frozen.store(true, std::memory_order_release);
}

bool Config_IsFrozen() noexcept {
  return s_frozen.load(std::memory_order_acquire);
}

uint8_t Config_GetWallpadProfile() noexcept {
  return s_active_wallpad_profile.load(std::memory_order_relaxed);
}

bool Config_SetWallpadProfile(uint8_t profile) noexcept {
  if (profile > kWallpadProfileMax) {
    return false;
  }
  std::unique_lock lock(s_config_rw);
  s_active_wallpad_profile.store(profile, std::memory_order_relaxed);
  s_config.wallpad_profile = profile;
  updateActiveConfig(s_config);
  return true;
}

bool Config_SaveWallpadProfile() noexcept {
  std::lock_guard<std::mutex> lock(s_save_mutex);
  Preferences p;
  if (!p.begin("runtime-config", false)) {
    s_nvs_write_errors.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  const uint32_t t0 = millis();
  RuntimeConfig env_cfg{};
  const uint8_t active_bank = p.getUChar("cfg_act", 0);
  const char *primary_key = (active_bank == 1) ? "cfg_bin_1" : "cfg_bin_0";
  if (!nvsGetEnv(p, primary_key, env_cfg)) {
    nvsGetEnv(p, "cfg_bin", env_cfg);
  }
  env_cfg.wallpad_profile = s_active_wallpad_profile.load(std::memory_order_relaxed);

  const uint8_t target_bank = (active_bank == 0) ? 1 : 0;
  const char *target_key = (target_bank == 1) ? "cfg_bin_1" : "cfg_bin_0";
  if (nvsPutEnv(p, target_key, env_cfg)) {
    p.putUChar("cfg_act", target_bank);
    s_active_bank = target_bank;
  }
  p.putUChar("w_prof",
             s_active_wallpad_profile.load(std::memory_order_relaxed));
  p.end();

  const uint32_t dur = millis() - t0;
  s_nvs_last_duration_ms.store(dur, std::memory_order_relaxed);
  s_nvs_last_sync_ms.store(millis(), std::memory_order_relaxed);
  s_persisted_config = env_cfg;
  s_has_persisted_config = true;
  return true;
}

static bool Config_ValidateStaged(const RuntimeConfig &cfg,
                                  const RuntimeTimingConfig &timing) noexcept {
  auto validBaud = [](uint32_t b) { return b >= 1200 && b <= 921600; };
  if (!validBaud(cfg.uart_baud_rate) || !validBaud(cfg.ch2_baud_rate) ||
      !validBaud(cfg.ch3_baud_rate) || !validBaud(cfg.doorphone_baud_rate)) {
    return false;
  }
  if (cfg.wifi_connect_timeout_s < 5 || cfg.wifi_connect_timeout_s > 120) {
    return false;
  }
  if (timing.ch1_poll_interval_ms < 200 || timing.ch1_poll_interval_ms > 5000) {
    return false;
  }
  if (timing.ch2_cache_delay_ms < 5 || timing.ch2_cache_delay_ms > 300) {
    return false;
  }
  if (timing.ch3_cache_delay_ms < 20 || timing.ch3_cache_delay_ms > 1000) {
    return false;
  }
  return true;
}

bool Config_SaveStaged(const RuntimeConfig &cfg,
                       const RuntimeTimingConfig &timing) noexcept {
  if (!Config_ValidateStaged(cfg, timing)) {
    return false;
  }

  std::lock_guard<std::mutex> lock(s_save_mutex);
  Preferences p;
  if (!p.begin("runtime-config", false)) {
    s_nvs_write_errors.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  const uint32_t t0 = millis();

  // 1. Atomic sealed A/B binary envelope write (Power-cut resilient)
  const uint8_t target_bank = (s_active_bank == 0) ? 1 : 0;
  const char *target_key = (target_bank == 1) ? "cfg_bin_1" : "cfg_bin_0";

  if (!nvsPutEnv(p, target_key, cfg)) {
    s_nvs_write_errors.fetch_add(1, std::memory_order_relaxed);
  } else {
    p.putUChar("cfg_act", target_bank);
    s_active_bank = target_bank;
  }

  // 2. Backward compatibility Delta Write (Table-Driven Dispatch)
  saveDeltaProperties(p, cfg, s_persisted_config, s_has_persisted_config);

  p.end();

  Preferences pt;
  if (pt.begin("timing_cfg", false)) {
    nvsPutEnv(pt, "tm_bin", timing);
    if (timing.ch1_poll_interval_ms != s_timing_config.ch1_poll_interval_ms)
      pt.putUShort("ch1_poll", timing.ch1_poll_interval_ms);
    if (timing.ch2_cache_delay_ms != s_timing_config.ch2_cache_delay_ms)
      pt.putUShort("ch2_del", timing.ch2_cache_delay_ms);
    if (timing.ch3_cache_delay_ms != s_timing_config.ch3_cache_delay_ms)
      pt.putUShort("ch3_del", timing.ch3_cache_delay_ms);
    pt.end();
  }

  const uint32_t dur = millis() - t0;
  s_nvs_last_duration_ms.store(dur, std::memory_order_relaxed);
  s_nvs_last_sync_ms.store(millis(), std::memory_order_relaxed);
  s_persisted_config = cfg;
  s_has_persisted_config = true;
  {
    std::unique_lock rw_lock(s_config_rw);
    s_config = cfg;
    s_timing_config = timing;
    updateActiveConfig(cfg);
  }
  return true;
}

