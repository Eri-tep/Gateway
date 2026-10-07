#pragma once

// ============================================================================
// SystemPlatform: Level 0 Pure Base Infrastructure Definitions
// ============================================================================

// ── Standard Library Includes ──
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

// ── Platform & ESP-IDF Includes ──
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include <Arduino.h>
#include <IPAddress.h>
#include <Preferences.h>

// ── RAII FreeRTOS Synchronization Primitives ──
class [[nodiscard]] CriticalSectionLocker {
private:
  portMUX_TYPE *_mux{nullptr};

public:
  explicit CriticalSectionLocker(portMUX_TYPE *mux) noexcept : _mux(mux) {
    if (_mux)
      portENTER_CRITICAL(_mux);
  }
  explicit CriticalSectionLocker(portMUX_TYPE &mux) noexcept : _mux(&mux) {
    portENTER_CRITICAL(_mux);
  }
  ~CriticalSectionLocker() noexcept {
    if (_mux)
      portEXIT_CRITICAL(_mux);
  }
  CriticalSectionLocker(const CriticalSectionLocker &) = delete;
  CriticalSectionLocker &operator=(const CriticalSectionLocker &) = delete;
  CriticalSectionLocker(CriticalSectionLocker &&) = delete;
  CriticalSectionLocker &operator=(CriticalSectionLocker &&) = delete;
};

class [[nodiscard]] MutexLocker {
private:
  SemaphoreHandle_t _mutex{nullptr};
  bool _locked{false};
  uint32_t _acquired_ms{0};

public:
  explicit MutexLocker(SemaphoreHandle_t mutex,
                       TickType_t timeout = portMAX_DELAY) noexcept
      : _mutex(mutex) {
    if (_mutex) {
      _locked = (xSemaphoreTake(_mutex, timeout) == pdTRUE);
      if (_locked) {
        _acquired_ms = millis();
      }
    }
  }
  ~MutexLocker() noexcept {
    if (_mutex && _locked) {
      uint32_t hold_ms = millis() - _acquired_ms;
      if (hold_ms >= 50) { // Config::Timing::MAX_LOCK_HOLD_MS (50ms)
        ESP_LOGW("LOCK", "Mutex held for %u ms (>= 50 ms threshold)",
                 static_cast<unsigned>(hold_ms));
      }
      xSemaphoreGive(_mutex);
    }
  }
  [[nodiscard]] bool isLocked() const noexcept { return _locked; }
  explicit operator bool() const noexcept { return _locked; }
  MutexLocker(const MutexLocker &) = delete;
  MutexLocker &operator=(const MutexLocker &) = delete;
  MutexLocker(MutexLocker &&) = delete;
  MutexLocker &operator=(MutexLocker &&) = delete;
};

#ifndef LIKELY
#define LIKELY(x) __builtin_expect(!!(x), 1)
#endif
#ifndef UNLIKELY
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif

using std::span;
using std::string_view;
using std::to_underlying;

[[nodiscard]] inline uint32_t FastCrc32(std::span<const uint8_t> data) noexcept {
  return ~esp_rom_crc32_le(~0U, data.data(), data.size());
}

[[nodiscard]] inline uint32_t FastCrc32(const uint8_t *data, size_t len) noexcept {
  return FastCrc32(std::span<const uint8_t>(data, len));
}

template <typename T> struct NvsEnvelope {
  uint32_t magic{0x4757484D}; // "GWHM"
  uint16_t version{1};
  uint16_t data_len{sizeof(T)};
  uint32_t crc32{0};
  T payload{};

  void seal() noexcept {
    crc32 = FastCrc32(reinterpret_cast<const uint8_t *>(&payload), sizeof(T));
  }

  bool verify() const noexcept {
    if (magic != 0x4757484D || data_len != sizeof(T))
      return false;
    uint32_t computed =
        FastCrc32(reinterpret_cast<const uint8_t *>(&payload), sizeof(T));
    return (computed == crc32);
  }
};

template <class T>
inline bool nvsGetEnv(Preferences &p, const char *key, T &out) {
  NvsEnvelope<T> env{};
  if (p.getBytesLength(key) != sizeof(env))
    return false;
  if (p.getBytes(key, &env, sizeof(env)) != sizeof(env) || !env.verify())
    return false;
  out = env.payload;
  return true;
}

template <class T>
inline bool nvsPutEnv(Preferences &p, const char *key, const T &v) {
  NvsEnvelope<T> env{};
  env.payload = v;
  env.seal();
  return (p.putBytes(key, &env, sizeof(env)) == sizeof(env));
}

template <class T>
inline bool nvsGetEnvNs(const char *ns, const char *key, T &out) {
  Preferences p;
  if (!p.begin(ns, true))
    return false;
  bool ok = nvsGetEnv(p, key, out);
  p.end();
  return ok;
}

template <class T>
inline bool nvsPutEnvNs(const char *ns, const char *key, const T &v) {
  Preferences p;
  if (!p.begin(ns, false))
    return false;
  bool ok = nvsPutEnv(p, key, v);
  p.end();
  return ok;
}

template <typename U, bool = std::is_enum_v<U>>
struct UnderlyingTypeHelper {
  using type = std::underlying_type_t<U>;
};

template <typename U>
struct UnderlyingTypeHelper<U, false> {
  using type = U;
};

template <typename T>
  requires ((std::is_integral_v<T> && sizeof(T) <= 4 && !std::is_same_v<T, bool>) || std::is_enum_v<T>)
[[nodiscard]] inline std::expected<T, esp_err_t> nvsReadPrimitive(Preferences &p, const char *key) noexcept {
  if (!p.isKey(key)) {
    return std::unexpected(ESP_ERR_NVS_NOT_FOUND);
  }
  using Underlying = typename UnderlyingTypeHelper<T>::type;
  PreferenceType expected_type = PT_INVALID;
  if constexpr (sizeof(Underlying) == 1) {
    expected_type = std::is_signed_v<Underlying> ? PT_I8 : PT_U8;
  } else if constexpr (sizeof(Underlying) == 2) {
    expected_type = std::is_signed_v<Underlying> ? PT_I16 : PT_U16;
  } else if constexpr (sizeof(Underlying) == 4) {
    expected_type = std::is_signed_v<Underlying> ? PT_I32 : PT_U32;
  }

  const PreferenceType actual_type = p.getType(key);
  if (actual_type != expected_type) {
    return std::unexpected(ESP_ERR_NVS_TYPE_MISMATCH);
  }

  Underlying val{};
  if constexpr (sizeof(Underlying) == 1) {
    if constexpr (std::is_signed_v<Underlying>) {
      val = p.getChar(key);
    } else {
      val = p.getUChar(key);
    }
  } else if constexpr (sizeof(Underlying) == 2) {
    if constexpr (std::is_signed_v<Underlying>) {
      val = p.getShort(key);
    } else {
      val = p.getUShort(key);
    }
  } else if constexpr (sizeof(Underlying) == 4) {
    if constexpr (std::is_signed_v<Underlying>) {
      val = p.getLong(key);
    } else {
      val = p.getULong(key);
    }
  }
  return static_cast<T>(val);
}

template <size_t N> inline void setStr(char (&dst)[N], const char *src) {
  snprintf(dst, N, "%s", src ? src : "");
}

struct CoreDumpInfo {
  bool valid{false};
  char task_name[16]{};
  uint32_t exc_pc{0};
  uint32_t exc_cause{0};
  uint32_t bt[16]{};
  uint8_t bt_depth{0};
  bool bt_corrupted{false};
};

extern CoreDumpInfo g_coredump_info;

void Tcp_EnableKeepalive(int sock, int idle, int intvl, int cnt);

// ── Fixed-Size Packet Structures ──
constexpr uint8_t PKT_STX = 0xF7;
constexpr uint8_t PKT_ETX = 0xEE;

struct StaticPacket {
  uint8_t channel_id;
  uint8_t length;
  std::array<uint8_t, 64> data;
};

// ── System Lifecycle & Synchronization Primitives ──
extern EventGroupHandle_t g_system_event_group;
extern std::atomic<bool> g_ota_in_progress;

inline EventGroupHandle_t System_GetEventGroup() noexcept {
  return g_system_event_group;
}
inline void System_SetEventGroup(EventGroupHandle_t eg) noexcept {
  g_system_event_group = eg;
}

inline bool System_IsOtaInProgress() noexcept {
  return g_ota_in_progress.load(std::memory_order_relaxed);
}
inline void System_SetOtaInProgress(bool in_prog) noexcept {
  g_ota_in_progress.store(in_prog, std::memory_order_relaxed);
}

// ── Sealed System State Contracts (implemented in L1 Diagnostics_Driver) ──
/// Crash breadcrumb in RTC SRAM; read/cleared by System_DiagnoseStuck().
void System_MarkStage(uint32_t stage) noexcept;
bool System_IsRescueMode() noexcept;
void System_SetRollbackDetected() noexcept;
bool System_IsRollbackDetected() noexcept;
uint32_t System_GetCrashCounter() noexcept;
constexpr EventBits_t SYS_EVT_OTA_IDLE = (1 << 0);
constexpr EventBits_t SYS_EVT_CACHE_READY = (1 << 1);
constexpr EventBits_t SYS_EVT_SYSTEM_RUNNING = (1 << 2);
constexpr EventBits_t SYS_EVT_NETWORK_READY = (1 << 3);

inline bool System_IsNetworkReady() noexcept {
  return (g_system_event_group != nullptr) &&
         ((xEventGroupGetBits(g_system_event_group) & SYS_EVT_NETWORK_READY) != 0);
}

void System_Restart(const char *reason);
void System_CheckOtaHealth();

enum class TraceType : uint8_t;

// ── Global Decoupled Diagnostic Trace & Event Sinks ──
using SystemTraceMessageFn = void (*)(const char *msg);
using SystemTracePacketFn = void (*)(uint8_t channel, bool is_tx, TraceType type,
                                    const StaticPacket &pkt);

void System_RegisterTraceMessageSink(SystemTraceMessageFn fn) noexcept;
void System_RegisterTracePacketSink(SystemTracePacketFn fn) noexcept;

void System_TraceMessage(const char *msg) noexcept;
void System_TracePacket(uint8_t channel, bool is_tx, TraceType type,
                        const StaticPacket &pkt) noexcept;

struct SystemTraceSink {
  SystemTracePacketFn trace_packet{nullptr};
  SystemTraceMessageFn trace_msg{nullptr};
};
using ShutdownHookFn = void (*)();
using ShutdownHook = ShutdownHookFn;

void System_RegisterTraceSink(const SystemTraceSink &sink) noexcept;
void System_RegisterShutdownHook(ShutdownHookFn hook) noexcept;

// ── System Snapshots & Diagnostics DTOs (L0 Universal Platform) ──────────────
enum class HubDeviceType : uint8_t {
  WALLPAD_COMPATIBLE = 0,
  AIR_CONDITIONER = 1
};

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

struct MgmtSessionSnapshot {
  bool is_active{false};
  char peer_ip[16]{""};
  uint32_t connected_at_ms{0};
  uint32_t last_activity_ms{0};
};

struct HttpOtaSnapshot {
  bool in_progress{false};
  char status[64]{"Idle"};
  uint8_t progress_pct{0};
  char last_error[64]{""};
};

struct SysSnapshot {
  uint32_t free_heap{0};
  uint32_t min_free_heap{0};
  uint32_t total_heap{0};
  uint32_t sketch_size_kb{0};
  uint32_t flash_total_kb{0};
  uint32_t uptime_ms{0};
  bool wifi_connected{false};
  int8_t wifi_rssi{0};
  char wifi_ip[16]{""};
};

struct HwSnapshot {
  uint8_t cpu0_cur{0}, cpu0_15m_avg{0}, cpu0_15m_peak{0}, cpu0_24h_avg{0}, cpu0_24h_peak{0};
  uint8_t cpu1_cur{0}, cpu1_15m_avg{0}, cpu1_15m_peak{0}, cpu1_24h_avg{0}, cpu1_24h_peak{0};
  uint16_t ram_cur{0}, ram_15m_avg{0}, ram_15m_peak{0}, ram_24h_avg{0}, ram_24h_peak{0};
  int8_t temp_cur{0}, temp_15m_avg{0}, temp_15m_peak{0}, temp_24h_avg{0}, temp_24h_peak{0};
};

struct StackSnapshot {
  uint16_t ch1_stack{0}, ch2_stack{0}, ch3_stack{0}, ch4_stack{0}, net_stack{0}, telnet_stack{0};
};

struct ChanStats {
  uint32_t rx_pkts{0};
  uint32_t tx_pkts{0};
  uint32_t crc_errors{0};
  uint32_t invalid_frames{0};
  uint32_t timeouts{0};
  uint32_t lock_timeouts{0};
  uint32_t uncached_pkts{0};
  uint32_t last_activity_ms{0};
};

struct TcpChanStats {
  uint32_t rx_pkts{0};
  uint32_t tx_pkts{0};
  uint32_t dropped_pkts{0};
  uint32_t uncached_pkts{0};
  uint16_t connection_count{0};
  bool is_connected{false};
};

struct PktSnapshot {
  ChanStats ch1;
  ChanStats ch2;
  ChanStats ch3;
  ChanStats ch4;
  TcpChanStats ch5;
  TcpChanStats ch6;
};

struct LogEntry {
  uint32_t timestamp{0};
  char reason[32]{""};
  SysSnapshot stats_snapshot;
  HwSnapshot hw_snapshot;
  StackSnapshot stack_snapshot;
  PktSnapshot packet_stats_snapshot;
};

struct AppendBuf;

void System_TakeSnapshot(SysSnapshot &sys, HwSnapshot &hw, StackSnapshot &st,
                         PktSnapshot &pkt) noexcept;
void System_GetPktSnapshot(PktSnapshot &pkt) noexcept;
void System_ReadCpuPct(uint8_t &cpu0_out, uint8_t &cpu1_out) noexcept;
int8_t System_ReadTempC() noexcept;
void System_GetCpuAndTemp(uint8_t &cpu0, uint8_t &cpu1, int8_t &temp_c) noexcept;
void System_GetCh1Metrics(uint32_t &poll_cnt, uint32_t &vip_cnt, uint32_t &normal_cnt) noexcept;
void System_RecordMetricsSample(uint16_t used_ram_kb) noexcept;

// ── Traffic Counters & Connection Status ─────────────────────────────────────
void System_ResetTrafficStats() noexcept;
void System_RecordCh5Tx() noexcept;
void System_RecordCh5Rx() noexcept;
void System_RecordCh5Dropped() noexcept;
void System_SetCh5Connected(bool conn) noexcept;
void System_RecordCh5Connection() noexcept;

void System_RecordCh6Tx() noexcept;
void System_RecordCh6Rx() noexcept;
void System_SetCh6Connected(bool conn) noexcept;
void System_RecordCh6Connection() noexcept;

// ── Watchdog & Task Health ───────────────────────────────────────────────────
using WdtFeedHook = void (*)(size_t) noexcept;
void System_RegisterWdtHook(WdtFeedHook hook) noexcept;
void System_FeedWdt(size_t index) noexcept;
void System_FormatTaskStacks(AppendBuf &out, const StackSnapshot &st) noexcept;

// ── Persistent Reboot Log & Boot Lifecycle ───────────────────────────────────
[[nodiscard]] size_t System_GetRebootLogCount() noexcept;
[[nodiscard]] bool System_GetRebootLogEntry(size_t index, LogEntry &out_entry) noexcept;
void System_WriteRebootLog(const char *reason) noexcept;
void System_ClearRebootLog() noexcept;
[[nodiscard]] const char *System_ConsumePendingRebootReason() noexcept;
[[nodiscard]] const char *System_ResetReasonToString(esp_reset_reason_t rr) noexcept;
[[nodiscard]] uint32_t System_GetBootTimeMs() noexcept;

// ── HTTP / Cloud Firmware OTA Engine ─────────────────────────────────────────
void System_StartHttpOta(const char *url) noexcept;
void System_GetHttpOtaSnapshot(HttpOtaSnapshot &out) noexcept;
[[nodiscard]] bool System_IsHttpOtaInProgress() noexcept;

// ── Network & Wi-Fi Platform Services (L0 Universal Contract) ────────────────
constexpr EventBits_t WIFI_BIT_CONNECTED = BIT0;
constexpr EventBits_t WIFI_BIT_DISCONNECTED = BIT1;
constexpr EventBits_t WIFI_BIT_GOT_IP = BIT2;

void System_WifiInit() noexcept;
[[nodiscard]] bool System_WifiIsConnected() noexcept;
[[nodiscard]] IPAddress System_WifiGetIp() noexcept;
[[nodiscard]] IPAddress System_WifiGetSubnetMask() noexcept;
[[nodiscard]] IPAddress System_WifiGetApIp() noexcept;
[[nodiscard]] IPAddress System_WifiGetApSubnetMask() noexcept;
[[nodiscard]] int8_t System_WifiGetRssi() noexcept;
void System_WifiReconnect() noexcept;
[[nodiscard]] EventBits_t System_WifiGetEventBits() noexcept;

// ── Bridge Transport Channel Slot Snapshot Contract ──────────────────────────
bool System_GetBridgeSlotSnapshot(uint8_t slot_idx, HubClientSlotSnapshot &out) noexcept;

// ── IP Subnet & Management Whitelist Filters (Global Security Policy) ────────
[[nodiscard]] bool Tcp_IsAllowedIP(IPAddress ip);
[[nodiscard]] bool Telnet_IsAllowedIP(IPAddress ip);

