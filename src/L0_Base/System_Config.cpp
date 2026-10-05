#include "L0_Base/System_Config.h"
#include <Arduino.h>
#include <Preferences.h>
#include <cstring>
#include <esp_idf_version.h>
#include <mbedtls/sha256.h>

RuntimeConfig g_config;
std::shared_mutex g_config_rw;
portMUX_TYPE g_config_mux = portMUX_INITIALIZER_UNLOCKED;
std::atomic<bool> g_config_dirty{false};

// ── RTC Fast SRAM Retention Variables ──
RTC_NOINIT_ATTR uint32_t rtc_magic;
RTC_NOINIT_ATTR uint32_t rtc_last_alive_ms[Config::Task::TASK_COUNT];
RTC_NOINIT_ATTR volatile uint32_t g_telnet_stage = 0;
RTC_NOINIT_ATTR uint32_t rtc_rescue_magic;
RTC_NOINIT_ATTR uint32_t rtc_crash_counter;
RTC_NOINIT_ATTR uint32_t rtc_clean_restart_magic;

// ── System Boot & Rescue Status ──
std::atomic<bool> g_rescue_mode{false};
bool g_rollback_detected = false;


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
    sprintf(output + (i * 2), "%02x", hash[i]);
  }
  output[64] = '\0';
}

void Config_Load() {
  Preferences p;
  p.begin("runtime-config", true);
  std::unique_lock lock(g_config_rw);
  auto &c = g_config;

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
  c.wallpad_profile =
      p.getUChar("w_prof", static_cast<uint8_t>(WallpadProfileIndex::ADAPTIVE));
  p.end();

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
}

void Config_Save() {
  if (!g_config_dirty.load(std::memory_order_acquire))
    return;

  RuntimeConfig snapshot;
  {
    std::unique_lock lock(g_config_rw);
    snapshot = g_config;
    g_config_dirty.store(false, std::memory_order_release);
  }

  Preferences p;
  p.begin("runtime-config", false);

  p.putULong("uart_baud", snapshot.uart_baud_rate);
  p.putULong("ch2_baud", snapshot.ch2_baud_rate);
  p.putULong("ch3_baud", snapshot.ch3_baud_rate);
  p.putULong("door_baud", snapshot.doorphone_baud_rate);
  p.putString("wifi_ssid", snapshot.wifi_ssid);
  p.putString("wifi_pass", snapshot.wifi_password);
  p.putString("ap_ssid", snapshot.ap_ssid);
  p.putString("ap_pass", snapshot.ap_password);
  p.putString("telnet_hash", snapshot.telnet_pass_hash);

  p.putUChar("u_parity", snapshot.uart_parity);
  p.putUChar("u_sbits", snapshot.uart_stop_bits);
  p.putUChar("u_dbits", snapshot.uart_data_bits);
  p.putUChar("ch2_parity", snapshot.ch2_parity);
  p.putUChar("ch2_sbits", snapshot.ch2_stop_bits);
  p.putUChar("ch2_dbits", snapshot.ch2_data_bits);
  p.putUChar("ch3_parity", snapshot.ch3_parity);
  p.putUChar("ch3_sbits", snapshot.ch3_stop_bits);
  p.putUChar("ch3_dbits", snapshot.ch3_data_bits);
  p.putUChar("d_dbits", snapshot.doorphone_data_bits);
  p.putUChar("d_parity", snapshot.doorphone_parity);
  p.putUChar("d_sbits", snapshot.doorphone_stop_bits);
  p.putUShort("w_tout", snapshot.wifi_connect_timeout_s);
  p.putUChar("w_prof", snapshot.wallpad_profile);

  p.end();
}

void Config_ResetDefaults() {
  std::unique_lock lock(g_config_rw);
  g_config = RuntimeConfig{};
  g_config_dirty.store(true, std::memory_order_release);
}

RuntimeTimingConfig g_timing_config{};

void TimingConfig_Load() {
  Preferences p;
  if (p.begin("timing_cfg", true)) {
    g_timing_config.ch1_poll_interval_ms = p.getUShort("ch1_poll", 1000);
    g_timing_config.ch2_cache_delay_ms = p.getUShort("ch2_del", 30);
    g_timing_config.ch3_cache_delay_ms = p.getUShort("ch3_del", 240);
    p.end();
  } else {
    g_timing_config.ch1_poll_interval_ms = 1000;
    g_timing_config.ch2_cache_delay_ms = 30;
    g_timing_config.ch3_cache_delay_ms = 240;
  }

  if (g_timing_config.ch1_poll_interval_ms < 200 ||
      g_timing_config.ch1_poll_interval_ms > 5000)
    g_timing_config.ch1_poll_interval_ms = 1000;
  if (g_timing_config.ch2_cache_delay_ms < 5 ||
      g_timing_config.ch2_cache_delay_ms > 300)
    g_timing_config.ch2_cache_delay_ms = 30;
  if (g_timing_config.ch3_cache_delay_ms < 20 ||
      g_timing_config.ch3_cache_delay_ms > 1000)
    g_timing_config.ch3_cache_delay_ms = 240;

  ::Serial.printf(
      "[TIMING] Loaded: CH1 Poll %u ms, CH2 Delay %u ms, CH3 Delay %u ms\r\n",
      g_timing_config.ch1_poll_interval_ms, g_timing_config.ch2_cache_delay_ms,
      g_timing_config.ch3_cache_delay_ms);
}

void TimingConfig_Save() {
  Preferences p;
  if (p.begin("timing_cfg", false)) {
    p.putUShort("ch1_poll", g_timing_config.ch1_poll_interval_ms);
    p.putUShort("ch2_del", g_timing_config.ch2_cache_delay_ms);
    p.putUShort("ch3_del", g_timing_config.ch3_cache_delay_ms);
    p.end();
    ::Serial.printf("[TIMING] Saved to NVS: CH1 Poll %u ms, CH2 Delay %u ms, "
                    "CH3 Delay %u ms\r\n",
                    g_timing_config.ch1_poll_interval_ms,
                    g_timing_config.ch2_cache_delay_ms,
                    g_timing_config.ch3_cache_delay_ms);
  }
}

std::atomic<bool> g_mgmt_client_connected{false};
