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
#include "Supervisor.h"
#include "ThreadSafety.h"

// ── Type-Safe Clang TSA Capability Wrapper for FreeRTOS Spinlocks ──
class CAPABILITY("mutex") PortMuxLock {
private:
  portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;

public:
  constexpr PortMuxLock() noexcept = default;
  [[nodiscard]] inline portMUX_TYPE *get() noexcept { return &_mux; }
  [[nodiscard]] inline const portMUX_TYPE *get() const noexcept { return &_mux; }
  [[nodiscard]] inline portMUX_TYPE &raw() noexcept { return _mux; }
  [[nodiscard]] inline const portMUX_TYPE &raw() const noexcept { return _mux; }
  inline operator portMUX_TYPE *() noexcept { return &_mux; }
  inline operator const portMUX_TYPE *() const noexcept { return &_mux; }
  inline operator portMUX_TYPE &() noexcept { return _mux; }
  inline operator const portMUX_TYPE &() const noexcept { return _mux; }
};

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

class [[nodiscard]] SCOPED_CAPABILITY SpinlockLocker {
private:
  PortMuxLock &_mux;

public:
  explicit SpinlockLocker(PortMuxLock &mux) noexcept ACQUIRE(mux) : _mux(mux) {
    portENTER_CRITICAL(mux.get());
  }
  ~SpinlockLocker() noexcept RELEASE() {
    portEXIT_CRITICAL(_mux.get());
  }
  SpinlockLocker(const SpinlockLocker &) = delete;
  SpinlockLocker &operator=(const SpinlockLocker &) = delete;
  SpinlockLocker(SpinlockLocker &&) = delete;
  SpinlockLocker &operator=(SpinlockLocker &&) = delete;
};

class [[nodiscard]] SCOPED_CAPABILITY MutexLocker {
private:
  SemaphoreHandle_t _mutex{nullptr};
  bool _locked{false};
  uint32_t _acquired_ms{0};

public:
  explicit MutexLocker(SemaphoreHandle_t mutex,
                       TickType_t timeout = portMAX_DELAY) noexcept
      ACQUIRE(mutex)
      : _mutex(mutex) {
    if (_mutex) {
      if (xSemaphoreTake(_mutex, 0) == pdTRUE) {
        _locked = true;
        _acquired_ms = millis();
        return;
      }
      if (timeout == 0) {
        return;
      }
      Supervisor::noteWaitBegin(_mutex);
      _locked = (xSemaphoreTake(_mutex, timeout) == pdTRUE);
      Supervisor::noteWaitEnd();
      if (_locked) {
        _acquired_ms = millis();
      }
    }
  }
  ~MutexLocker() noexcept RELEASE() {
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

[[nodiscard]] inline uint32_t FastCrc32(std::span<const uint8_t> data) noexcept {
  return ~esp_rom_crc32_le(~0U, data.data(), data.size());
}

[[nodiscard]] inline uint32_t FastCrc32(const uint8_t *data, size_t len) noexcept {
  return FastCrc32(std::span<const uint8_t>(data, len));
}

template <typename T> struct alignas(4) NvsEnvelope {
  static_assert(std::is_trivially_copyable_v<T>, "NvsEnvelope payload must be trivially copyable");
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

enum class NvsValueType : uint8_t {
  I8 = 0,
  U8,
  I16,
  U16,
  I32,
  U32,
  I64,
  U64,
  STR,
  BLOB,
  INVALID
};

/// L0 RAII NVS storage handle encapsulating ESP-IDF NVS (zero heap / zero Arduino Preferences leak).
class SystemNvs {
private:
  uint32_t _handle{0};
  bool _started{false};
  bool _readOnly{false};

public:
  SystemNvs() noexcept = default;
  ~SystemNvs() noexcept;
  SystemNvs(const SystemNvs &) = delete;
  SystemNvs &operator=(const SystemNvs &) = delete;
  SystemNvs(SystemNvs &&) = delete;
  SystemNvs &operator=(SystemNvs &&) = delete;

  bool begin(const char *name, bool readOnly = false) noexcept;
  void end() noexcept;
  bool clear() noexcept;
  bool remove(const char *key) noexcept;

  [[nodiscard]] bool isKey(const char *key) noexcept;
  [[nodiscard]] NvsValueType getType(const char *key) noexcept;

  size_t putChar(const char *key, int8_t value) noexcept;
  size_t putUChar(const char *key, uint8_t value) noexcept;
  size_t putShort(const char *key, int16_t value) noexcept;
  size_t putUShort(const char *key, uint16_t value) noexcept;
  size_t putLong(const char *key, int32_t value) noexcept;
  size_t putULong(const char *key, uint32_t value) noexcept;
  size_t putUInt(const char *key, uint32_t value) noexcept;
  size_t putBool(const char *key, bool value) noexcept;
  size_t putString(const char *key, const char *value) noexcept;
  size_t putBytes(const char *key, const void *value, size_t len) noexcept;

  int8_t getChar(const char *key, int8_t defaultValue = 0) noexcept;
  uint8_t getUChar(const char *key, uint8_t defaultValue = 0) noexcept;
  int16_t getShort(const char *key, int16_t defaultValue = 0) noexcept;
  uint16_t getUShort(const char *key, uint16_t defaultValue = 0) noexcept;
  int32_t getLong(const char *key, int32_t defaultValue = 0) noexcept;
  uint32_t getULong(const char *key, uint32_t defaultValue = 0) noexcept;
  uint32_t getUInt(const char *key, uint32_t defaultValue = 0) noexcept;
  bool getBool(const char *key, bool defaultValue = false) noexcept;
  size_t getString(const char *key, char *value, size_t maxLen) noexcept;
  size_t getBytesLength(const char *key) noexcept;
  size_t getBytes(const char *key, void *buf, size_t maxLen) noexcept;
};

template <class T>
inline bool nvsGetEnv(SystemNvs &p, const char *key, T &out) {
  NvsEnvelope<T> env{};
  if (p.getBytesLength(key) != sizeof(env))
    return false;
  if (p.getBytes(key, &env, sizeof(env)) != sizeof(env) || !env.verify())
    return false;
  out = env.payload;
  return true;
}

template <class T>
inline bool nvsPutEnv(SystemNvs &p, const char *key, const T &v) {
  NvsEnvelope<T> env{};
  env.payload = v;
  env.seal();
  return (p.putBytes(key, &env, sizeof(env)) == sizeof(env));
}

template <class T>
inline bool nvsGetEnvNs(const char *ns, const char *key, T &out) {
  SystemNvs p;
  if (!p.begin(ns, true))
    return false;
  bool ok = nvsGetEnv(p, key, out);
  p.end();
  return ok;
}

template <class T>
inline bool nvsPutEnvNs(const char *ns, const char *key, const T &v) {
  SystemNvs p;
  if (!p.begin(ns, false))
    return false;
  bool ok = nvsPutEnv(p, key, v);
  p.end();
  return ok;
}

inline bool nvsClearNs(const char *ns) noexcept {
  SystemNvs p;
  if (!p.begin(ns, false))
    return false;
  bool ok = p.clear();
  p.end();
  return ok;
}

[[nodiscard]] bool System_NvsPutUInt(const char *ns, const char *key,
                                     uint32_t val) noexcept;

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
[[nodiscard]] inline std::expected<T, esp_err_t> nvsReadPrimitive(SystemNvs &p, const char *key) noexcept {
  if (!p.isKey(key)) {
    return std::unexpected(ESP_ERR_NVS_NOT_FOUND);
  }
  using Underlying = typename UnderlyingTypeHelper<T>::type;
  NvsValueType expected_type = NvsValueType::INVALID;
  if constexpr (sizeof(Underlying) == 1) {
    expected_type = std::is_signed_v<Underlying> ? NvsValueType::I8 : NvsValueType::U8;
  } else if constexpr (sizeof(Underlying) == 2) {
    expected_type = std::is_signed_v<Underlying> ? NvsValueType::I16 : NvsValueType::U16;
  } else if constexpr (sizeof(Underlying) == 4) {
    expected_type = std::is_signed_v<Underlying> ? NvsValueType::I32 : NvsValueType::U32;
  }

  const NvsValueType actual_type = p.getType(key);
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
  uint32_t exc_vaddr{0};
  uint32_t bt[16]{};
  uint8_t bt_depth{0};
  bool bt_corrupted{false};
};

void System_GetCoreDumpSnapshot(CoreDumpInfo &out) noexcept;
[[nodiscard]] bool System_GetCoreDumpLiveSummary(CoreDumpInfo &out) noexcept;
void System_ClearCoreDumpInfo() noexcept;
void System_EraseCoreDump() noexcept;

void Tcp_EnableKeepalive(int sock, int idle, int intvl, int cnt);

// ── Fixed-Size Packet Structures ──
constexpr uint8_t PKT_STX = 0xF7;
constexpr uint8_t PKT_ETX = 0xEE;

struct StaticPacket {
  uint8_t channel_id;
  uint8_t length;
  std::array<uint8_t, 64> data;
};

// ── System Lifecycle & Synchronization Primitives (100% Static-Sealed) ───────
void System_EnsureEventGroup() noexcept;
[[nodiscard]] EventGroupHandle_t System_GetEventGroup() noexcept;
void System_SetEventGroup(EventGroupHandle_t eg) noexcept;
void System_SetEventBits(EventBits_t bits) noexcept;
void System_ClearEventBits(EventBits_t bits) noexcept;
[[nodiscard]] EventBits_t System_GetEventBits() noexcept;
bool System_WaitEventBits(EventBits_t bits, TickType_t ticks_to_wait,
                          EventBits_t *out_bits = nullptr) noexcept;

[[nodiscard]] bool System_IsOtaInProgress() noexcept;
void System_SetOtaInProgress(bool in_prog) noexcept;

inline void System_DelayMs(uint32_t ms) noexcept {
  vTaskDelay(pdMS_TO_TICKS(ms) > 0 ? pdMS_TO_TICKS(ms) : 1);
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

[[nodiscard]] bool System_IsNetworkReady() noexcept;

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
using ShutdownHookFn = void (*)() noexcept;

enum class SystemLifecycleEvent : uint8_t {
  PRE_SHUTDOWN,
  PRE_OTA,
  POST_OTA,
  PANIC
};
using SystemLifecycleHookFn = void (*)() noexcept;

void System_RegisterLifecycleHook(SystemLifecycleEvent evt, SystemLifecycleHookFn hook) noexcept;
void System_TriggerLifecycle(SystemLifecycleEvent evt) noexcept;

void System_RegisterTraceSink(const SystemTraceSink &sink) noexcept;
void System_RegisterShutdownHook(ShutdownHookFn hook) noexcept;

// ── System Snapshots & Diagnostics DTOs (L0 Universal Platform) ──────────────


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
  uint32_t queue_full{0};
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

struct LatencySnapshot {
  static constexpr unsigned kBuckets = 20;
  uint32_t count{0};
  uint32_t max_cycles{0};
  uint32_t max_age_ms{0};
  uint32_t hist[kBuckets]{};
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
void System_GetCh1Latency(LatencySnapshot &lat) noexcept;
void System_GetNvsOverlapStats(uint32_t &out_hits, uint32_t &out_max_us) noexcept;
void System_ResetNvsOverlapStats() noexcept;
void System_SetNvsBusy(bool busy) noexcept;
void System_ReadCpuPct(uint8_t &cpu0_out, uint8_t &cpu1_out) noexcept;
int8_t System_ReadTempC() noexcept;
void System_GetCpuAndTemp(uint8_t &cpu0, uint8_t &cpu1, int8_t &temp_c) noexcept;
void System_GetCh1Metrics(uint32_t &poll_cnt, uint32_t &vip_cnt, uint32_t &normal_cnt) noexcept;
void System_RecordMetricsSample(uint16_t used_ram_kb) noexcept;

// ── Traffic Counters & Connection Status ─────────────────────────────────────
void System_ResetTrafficStats() noexcept;
void System_ResetBridgeStats() noexcept;
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
[[nodiscard]] bool System_TaskWdtAddSelf() noexcept;
void System_TaskWdtResetSelf() noexcept;
void System_FormatTaskStacks(AppendBuf &out, const StackSnapshot &st) noexcept;

// ── Persistent Reboot Log & Boot Lifecycle ───────────────────────────────────
[[nodiscard]] size_t System_GetRebootLogCount() noexcept;
[[nodiscard]] bool System_GetRebootLogEntry(size_t index, LogEntry &out_entry) noexcept;
void System_WriteRebootLog(const char *reason) noexcept;
void System_ClearRebootLog() noexcept;
[[nodiscard]] const char *System_ConsumePendingRebootReason() noexcept;
[[nodiscard]] const char *System_ResetReasonToString(esp_reset_reason_t rr) noexcept;
[[nodiscard]] uint32_t System_GetBootTimeMs() noexcept;

// ── NVS Flash Storage Diagnostics (Cold Path Reliability) ───────────────────
void System_GetNvsStats(uint32_t &err_count, uint32_t &last_sync_ms,
                        uint32_t *last_duration_ms = nullptr) noexcept;
void System_ResetNvsStats() noexcept;

// ── HTTP / Cloud Firmware OTA Engine & Partition Contract ────────────────────
enum class OtaImgState : uint8_t {
  NEW = 0,
  PENDING_VERIFY,
  VALID,
  INVALID,
  ABORTED,
  UNDEFINED
};

struct OtaPartitionInfo {
  bool valid{false};
  char label[17]{};
  uint32_t address{0};
  uint32_t size{0};
  OtaImgState state{OtaImgState::UNDEFINED};
};

void System_StartHttpOta(const char *url) noexcept;
void System_GetHttpOtaSnapshot(HttpOtaSnapshot &out) noexcept;
[[nodiscard]] bool System_IsHttpOtaInProgress() noexcept;
void System_GetOtaPartitions(OtaPartitionInfo &running_out,
                             OtaPartitionInfo &next_out) noexcept;
[[nodiscard]] esp_err_t System_OtaMarkValidCancelRollback() noexcept;
[[nodiscard]] esp_err_t System_OtaRollbackAndReboot() noexcept;

// ── Network & Wi-Fi Platform Services (L0 Universal Contract) ────────────────
constexpr EventBits_t WIFI_BIT_CONNECTED = BIT0;
constexpr EventBits_t WIFI_BIT_DISCONNECTED = BIT1;
constexpr EventBits_t WIFI_BIT_GOT_IP = BIT2;

enum class WifiOpMode : uint8_t { OFF = 0, STA = 1, AP = 2, AP_STA = 3 };
enum class WifiAuthMode : uint8_t {
  OPEN = 0,
  WEP,
  WPA_PSK,
  WPA2_PSK,
  WPA_WPA2_PSK,
  WPA2_ENTERPRISE,
  WPA3_PSK,
  WPA2_WPA3_PSK,
  OTHER
};

struct WifiScanEntry {
  char ssid[33]{};
  int32_t rssi{0};
  uint8_t channel{0};
  WifiAuthMode auth{WifiAuthMode::OPEN};
};

void System_WifiInit() noexcept;
[[nodiscard]] bool System_WifiIsConnected() noexcept;
[[nodiscard]] IPAddress System_WifiGetIp() noexcept;
[[nodiscard]] IPAddress System_WifiGetSubnetMask() noexcept;
[[nodiscard]] IPAddress System_WifiGetApIp() noexcept;
[[nodiscard]] IPAddress System_WifiGetApSubnetMask() noexcept;
[[nodiscard]] int8_t System_WifiGetRssi() noexcept;
void System_WifiGetSsid(char *out_buf, size_t max_len) noexcept;
[[nodiscard]] WifiOpMode System_WifiGetMode() noexcept;
void System_WifiSetMode(WifiOpMode mode) noexcept;
void System_WifiConnect(const char *ssid, const char *pass) noexcept;
void System_WifiDisconnect() noexcept;
void System_WifiReconnect() noexcept;
[[nodiscard]] int System_WifiScanStart(bool async, bool show_hidden) noexcept;
[[nodiscard]] bool System_WifiScanGetEntry(int index, WifiScanEntry &out) noexcept;
[[nodiscard]] int32_t System_WifiScanGetRssi(int index) noexcept;
void System_WifiScanDelete() noexcept;
[[nodiscard]] EventBits_t System_WifiGetEventBits() noexcept;
void System_WifiStartFallbackAp() noexcept;
[[nodiscard]] bool System_WifiIsApActive() noexcept;



// ── IP Subnet & Management Whitelist Filters (Global Security Policy) ────────
[[nodiscard]] bool Tcp_IsAllowedIP(IPAddress ip) noexcept;
[[nodiscard]] bool Telnet_IsAllowedIP(IPAddress ip) noexcept;

