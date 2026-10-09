#pragma once

// ============================================================================
// SystemConfig: Level 0 Immutables, Pinmaps, Timings & NVS Runtime Config
// ============================================================================

#include "L0_Foundation/System_Platform.h"
#include <atomic>
#include <expected>
#include <shared_mutex>

namespace Config {
constexpr const char *FIRMWARE_VERSION = "v2.5.4";
} // namespace Config

namespace Config::Task {
constexpr size_t STACK_SIZE_CORE1 = 5888;
constexpr size_t STACK_SIZE_CH2 = 5824;
constexpr size_t STACK_SIZE_CH3 = 4160;
constexpr size_t STACK_SIZE_CH4 = 3328;
constexpr size_t STACK_SIZE_CORE0 = 6528;
constexpr size_t STACK_SIZE_TELNET = 6176;
constexpr size_t TASK_COUNT = 6;
constexpr uint8_t WDT_ID_CH1 = 0;
constexpr uint8_t WDT_ID_CH2 = 1;
constexpr uint8_t WDT_ID_CH3 = 2;
constexpr uint8_t WDT_ID_CH4 = 3;
constexpr uint8_t WDT_ID_NET = 4;
constexpr uint8_t WDT_ID_TELNET = 5;
static_assert(WDT_ID_TELNET < TASK_COUNT, "WDT_ID_TELNET out of range");
} // namespace Config::Task

namespace Config::Queue {
constexpr size_t POOL_SIZE_CONTROL = 32;
constexpr size_t POOL_SIZE_VIP = 16;
constexpr size_t POOL_SIZE_CH4_PASS = 16;
constexpr size_t UART_EVENT_QUEUE_SIZE = 16;
} // namespace Config::Queue

namespace Config::Timing {
constexpr uint32_t CH1_POLL_TIMEOUT_MS = 200;
constexpr uint32_t MAX_LOCK_HOLD_MS = 300;
constexpr uint32_t UPTIME_24H_MS = 86400000;
constexpr uint32_t CH1_INTER_PACKET_DELAY_MS = 15;
constexpr uint32_t UART_TX_DONE_TIMEOUT_MS = 20;
constexpr uint32_t CH1_POLL_INTERVAL_MS = 1000;
constexpr uint32_t STALE_DEVICE_THRESHOLD_MS = 180000;
constexpr uint32_t CH1_STALE_POLL_INTERVAL_MS = 10000;
constexpr uint32_t CACHE_CONVERGENCE_STABLE_MS = 1500;
constexpr uint32_t INITIAL_CACHING_GRACE_PERIOD_MS = 5000;
constexpr uint32_t SYSTEM_MONITOR_INTERVAL_MS = 15000;
constexpr uint32_t DOORPHONE_DEBOUNCE_MS = 500;
constexpr uint32_t DEVICE_BROADCAST_THROTTLE_MS = 50;
constexpr uint32_t DOORPHONE_BELL_TIMEOUT_MS = 30000;
constexpr uint32_t WALLPAD_AUTO_IPG_MS = 20;
constexpr uint32_t DOORPHONE_IPG_MS = 25;
constexpr uint32_t DEFAULT_DOORPHONE_INTER_BYTE_TIMEOUT_MS = 16;
uint32_t getDoorphoneInterByteTimeoutMs(uint32_t baud) noexcept;
constexpr uint32_t CH2_CACHE_DELAY_MS = 30;
constexpr uint32_t CH3_CACHE_DELAY_MS = 240;
constexpr uint32_t OTA_VALIDATION_PERIOD_MS = 120000;
constexpr uint32_t RESCUE_BUTTON_HOLD_MS = 2500;
constexpr uint32_t WIFI_BACKGROUND_RETRY_INTERVAL_MS = 60000;
constexpr uint32_t WARM_CACHE_NVS_DEBOUNCE_MS = 60000;
constexpr uint32_t WARM_CACHE_VERIFY_TIMEOUT_MS = 60000;
} // namespace Config::Timing

namespace Config::Network {
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 30000;
} // namespace Config::Network

namespace Config::OTA {
constexpr uint32_t DOWNLOAD_DEADLINE_MS = 300000;
constexpr uint32_t STALL_TIMEOUT_MS = 25000;
constexpr uint32_t IDLE_DELAY_MS = 2;
} // namespace Config::OTA

namespace Config::Metrics {
constexpr uint32_t SAMPLE_INTERVAL_MS = 5000;
} // namespace Config::Metrics

namespace Config::Memory {
constexpr uint32_t MIN_HEAP_THRESHOLD_KB = 25;
} // namespace Config::Memory

namespace Config::Packet {
constexpr uint8_t MIN_LEN = 3;
constexpr uint8_t MAX_LEN = 64;
constexpr uint16_t UART_HW_RX_BUF_SIZE = 2048;
constexpr size_t UART_READ_CHUNK = 64;
constexpr size_t MAX_STREAM_BUF = 128;
} // namespace Config::Packet

namespace Config::GPIO {
constexpr int BTN_PIN = 41;
constexpr int TX_GPIO = 39;
constexpr int RX_GPIO = 38;
} // namespace Config::GPIO

namespace Config::TCP {
constexpr uint16_t TELNET_PORT = 23;
constexpr uint16_t EW11_PORT = 8898;
constexpr uint16_t MGMT_PORT = 8900;
constexpr uint8_t MAX_TELNET_CLIENTS = 3;
constexpr uint8_t MAX_MGMT_CLIENTS = 3;
constexpr uint8_t MAX_EW11_SLOTS = 5;
constexpr uint16_t EW11_SLOT_PORTS[MAX_EW11_SLOTS] = {8898, 8891, 8892, 8893,
                                                      8894};
constexpr int SOCKET_BUFFER_SIZE = 4096;
constexpr size_t HUB_RX_BUFFER_SIZE = 1024;
constexpr size_t MGMT_BUFFER_SIZE = 512;
constexpr size_t POLL_RX_CHUNK_SIZE = 128;
constexpr uint32_t TELNET_SESSION_TIMEOUT_MS = 600000;
constexpr uint32_t CLEANUP_INTERVAL_MS = 30000;
constexpr uint32_t DEFAULT_KEEPALIVE_IDLE_SEC = 60;
constexpr uint32_t DEFAULT_KEEPALIVE_INTVL_SEC = 5;
constexpr uint32_t DEFAULT_KEEPALIVE_CNT = 3;
} // namespace Config::TCP

namespace Config::FCU {
constexpr uint32_t POLL_INTERVAL_MS = 1000;
constexpr uint32_t RX_TIMEOUT_MS = 500;
constexpr uint32_t INTER_PACKET_DELAY_MS = 50;
constexpr uint8_t MAX_TIMEOUT_COUNT = 3;
constexpr uint8_t TEMP_MIN = 18;
constexpr uint8_t TEMP_MAX = 30;
constexpr uint8_t DEV_ID = 0x2C;
} // namespace Config::FCU

namespace Config::Serial {
constexpr uint32_t DEFAULT_DOORPHONE_BAUD = 3860;
constexpr uint8_t DEFAULT_DOORPHONE_PARITY = 1;
constexpr uint8_t DEFAULT_DOORPHONE_DATABITS = 8;
constexpr uint8_t DEFAULT_DOORPHONE_STOPBITS = 1;
} // namespace Config::Serial

enum class WallpadProfileIndex : uint8_t {
  ADAPTIVE = 0,
  CUSTOM1 = 1,
  CUSTOM2 = 2,
  CUSTOM3 = 3,
  COUNT = 4
};

template <typename E>
[[nodiscard]] constexpr std::optional<E> toEnum(std::underlying_type_t<E> v,
                                                E first, E last) noexcept {
  static_assert(std::is_enum_v<E>, "toEnum requires an enum type");
  if (std::to_underlying(first) > std::to_underlying(last)) {
    return std::nullopt;
  }
  return (v >= std::to_underlying(first) && v <= std::to_underlying(last))
             ? std::optional<E>{static_cast<E>(v)}
             : std::nullopt;
}

static_assert(std::to_underlying(WallpadProfileIndex::CUSTOM3) == 3,
              "WallpadProfileIndex::CUSTOM3 must be 3");
static_assert(toEnum(0, WallpadProfileIndex::ADAPTIVE,
                     WallpadProfileIndex::CUSTOM3)
                  .has_value(),
              "toEnum 0 check failed");
static_assert(toEnum(3, WallpadProfileIndex::ADAPTIVE,
                     WallpadProfileIndex::CUSTOM3)
                  .has_value(),
              "toEnum 3 check failed");
static_assert(!toEnum(4, WallpadProfileIndex::ADAPTIVE,
                      WallpadProfileIndex::CUSTOM3)
                   .has_value(),
              "toEnum 4 check failed");

constexpr uint8_t kWallpadProfileMax =
    std::to_underlying(WallpadProfileIndex::COUNT) - 1;

struct RuntimeConfig {
  uint32_t uart_baud_rate{9600};
  uint32_t ch2_baud_rate{9600};
  uint32_t ch3_baud_rate{9600};
  uint32_t doorphone_baud_rate{Config::Serial::DEFAULT_DOORPHONE_BAUD};
  char wifi_ssid[64]{0};
  char wifi_password[64]{0};
  char ap_ssid[64]{0};
  char ap_password[64]{0};
  char telnet_pass_hash[68]{0};
  uint16_t wifi_connect_timeout_s{30};
  uint8_t uart_data_bits{8};
  uint8_t uart_parity{0};
  uint8_t uart_stop_bits{1};
  uint8_t ch2_data_bits{8};
  uint8_t ch2_parity{0};
  uint8_t ch2_stop_bits{1};
  uint8_t ch3_data_bits{8};
  uint8_t ch3_parity{0};
  uint8_t ch3_stop_bits{1};
  uint8_t doorphone_data_bits{Config::Serial::DEFAULT_DOORPHONE_DATABITS};
  uint8_t doorphone_parity{Config::Serial::DEFAULT_DOORPHONE_PARITY};
  uint8_t doorphone_stop_bits{Config::Serial::DEFAULT_DOORPHONE_STOPBITS};
  uint8_t wallpad_profile{0};
};

struct RuntimeTimingConfig {
  uint16_t ch1_poll_interval_ms{1000};
  uint16_t ch2_cache_delay_ms{30};
  uint16_t ch3_cache_delay_ms{240};
};

[[nodiscard]] const RuntimeConfig &Config_Get() noexcept;
[[nodiscard]] const RuntimeTimingConfig &TimingConfig_Get() noexcept;

void Config_Freeze() noexcept;
[[nodiscard]] bool Config_IsFrozen() noexcept;

[[nodiscard]] uint8_t Config_GetWallpadProfile() noexcept;
bool Config_SetWallpadProfile(uint8_t profile) noexcept;
bool Config_SaveWallpadProfile() noexcept;

bool Config_SaveStaged(const RuntimeConfig &cfg,
                       const RuntimeTimingConfig &timing) noexcept;

struct FramingConfig {
  uint8_t data_bits{8};
  uint8_t parity{0};
  uint8_t stop_bits{1};
};

enum class FramingParseError : uint8_t { InvalidLength, UnsupportedFormat };

const char *formatFramingStr(uint8_t data_bits, uint8_t parity,
                             uint8_t stop_bits) noexcept;

[[nodiscard]] std::expected<FramingConfig, FramingParseError>
parseFramingStr(std::string_view str) noexcept;

bool parseFramingStr(const char *str, uint8_t &data_bits, uint8_t &parity,
                     uint8_t &stop_bits) noexcept;

bool Config_SetUartFraming(uint8_t ch, uint32_t baud, uint8_t data_bits,
                           uint8_t parity, uint8_t stop_bits) noexcept;

void Config_Load();
void Config_Save();
void Config_ResetDefaults();
void System_Sha256ToHex(const char *input, char *output);

void TimingConfig_Load();
void TimingConfig_Save();

enum class TraceType : uint8_t {
  ALL = 0,
  QRY,
  CTL,
  ACK,
  DRP,
  RMT,
  MSG,
  CH,
  DEVID
};

// ── RTC Fast SRAM Retention Constants ──
constexpr uint32_t RTC_MAGIC_CLEAN_RESTART = 0x434C4E52; // 'CLNR'
constexpr uint32_t RTC_MAGIC_RESCUE = 0x52455343;        // 'RESC'
constexpr uint32_t RTC_MAGIC_WDT = 0x57445431;           // 'WDT1'

// Stage breadcrumb is sealed in L1 (System_MarkStage); macro keeps call sites.
#define TSTAGE(n)                                                              \
  System_MarkStage(0xA5A50000u | (static_cast<uint32_t>(n) & 0xFFFFu))
