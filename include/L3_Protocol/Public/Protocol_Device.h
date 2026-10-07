#pragma once

// ============================================================================
// Device_Registry.h — L3 Routing / Protocol Layer
// Canonical L3 Device State SSOT (Single Source of Truth)
// Canonical 4+1 Layer: L3 — depends only on L2↓, L1↓, L0↓. Zero L4 includes.
// ============================================================================
//
// This header is the canonical L3 entry point for device state management.
// During phased migration (Step 3), it re-exports the legacy DeviceRegistry
// and adds the new Domain_VerbNoun accessor APIs that replace raw extern access.
//
// SSOT & Synchronization Invariants:
//   - All device state writes must go through Device_UpdateFromBus() or
//     Device_SetDesiredState() — never modify DeviceStateEntry fields directly.
//   - All reads must use snapshot copy APIs (Device_GetSnapshot, Device_FindCopy,
//     Device_GetSnapshotChunk) — NEVER return/store raw entry pointers (zero tearing).
//   - Lock Hierarchy & Synchronization Matrix (Single-Ownership Invariant):
//     +--------------------+-------------------+-------+----------------------------------+
//     | Lock Name          | Type / Primitive  | Layer | Protected Resource / Invariant   |
//     +--------------------+-------------------+-------+----------------------------------+
//     | s_uart0_mutex      | SemaphoreHandle_t | L2    | CH1 UART0 half-duplex TX/RX      |
//     | s_uart1_mutex      | SemaphoreHandle_t | L2    | CH2 UART1 half-duplex TX/RX      |
//     | s_uart2_mutex      | SemaphoreHandle_t | L2    | CH3 UART2 half-duplex TX/RX      |
//     | s_ctrl_queue_mutex | SemaphoreHandle_t | L2    | CH1 Control Packet FIFO Queue    |
//     | s_ch5_mutex        | SemaphoreHandle_t | L2    | CH5 Modbus RTU FCU half-duplex   |
//     | _cache_mutex       | StaticSemaphore_t | L3    | Device Registry 48 slots cache   |
//     | _mutex             | StaticSemaphore_t | L3    | Control Registry learned specs   |
//     | _nvs_mutex         | StaticSemaphore_t | L3    | Control Registry NVS profile     |
//     | s_save_mutex       | std::mutex        | L0    | System Config NVS save/reset     |
//     | _cli_mutex         | SemaphoreHandle_t | L4    | Telnet CLI session print locks   |
//     | s_telnet_tx_sem    | SemaphoreHandle_t | L4    | Telnet CLI socket TX pacing      |
//     | s_mgmt_mutex       | SemaphoreHandle_t | L4    | TCP Mgmt session multiplexing    |
//     +--------------------+-------------------+-------+----------------------------------+
//     * Invariant: Nested lock acquisition is STRICTLY FORBIDDEN. All locks follow
//       single-ownership. Callbacks and packet tracing execute 100% outside locks.
//   - g_device_repo is static-sealed in Device_Registry.cpp (Rule 17).
//     External callers use the functional API below, NOT the extern object.
// ============================================================================

#include "L0_Foundation/System_Buffer.h"
#include "L0_Foundation/System_Config.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

// ============================================================================
// CONTROL ACTION TYPES & SLOTS (Absorbed from ProtocolTypes)
// ============================================================================

enum class ControlActionType : uint8_t {
  POWER = 0,         // 전원 ON / OFF
  SET_TEMP,          // 설정 온도 변경 (난방/에어컨)
  FAN_SPEED,         // 풍량 변경 (환기/에어컨)
  VALVE_CLOSE,       // 밸브 닫기 (가스)
  MOMENTARY_TRIGGER, // 순간 호출 (엘리베이터 등)
  VENT_MODE, // 운전 모드 변경 (환기: 일반 0x01, 바이패스 0x02, 자동 0x03)
  UNKNOWN = 0xFF
};

struct ActionSlot {
  bool discovered{false};
  uint8_t category_offset{
      0xFF};                   // 카테고리/서브1 위치 (현대 0x45, 0x46 등 [CTX])
  uint8_t category_val{0x00};  // 해당 액션의 카테고리 바이트 값
  uint8_t action_offset{0xFF}; // 제어 파라미터가 위치하는 바이트 오프셋 ([VAL])
  uint8_t telemetry_offset{
      0xFF}; // 실시간 환경 센서 텔레메트리 바이트 오프셋 ([ENV] 현재온도 등)
  uint8_t on_val{0x01};       // ON / Active 토큰
  uint8_t off_val{0x02};      // OFF / Inactive 토큰
  uint8_t min_val{0};         // 최소값 (온도 15℃, 풍량 1 등)
  uint8_t max_val{0};         // 최대값 (온도 30℃, 풍량 3 등)
  uint8_t level_tokens[4]{0}; // 이산 단계별 토큰 (예: 풍량 L1/L2/L3 등)
  uint8_t level_count{0};     // 등록된 이산 단계 토큰 개수
  uint8_t ack_state_offset{
      0xFF}; // 이 컨텍스트 채널 ACK 내 운전/가동 상태 오프셋
  uint8_t ack_target_offset{0xFF}; // 이 컨텍스트 채널 ACK 내 설정/제어값 오프셋
  uint8_t ack_telemetry_offset{
      0xFF}; // 이 컨텍스트 채널 ACK 내 환경 센서(현재온도 등) 오프셋
  uint16_t sample_count{0};
};

// ============================================================================
// DEVICE CAPABILITY CLASSIFICATION & DATA MODELS
// ============================================================================

enum class FramingStatus : uint8_t {
  WAITING = 0,
  LEARNING = 1,
  LOCKED = 2,
  NOISY = 3
};

enum class DeviceClass : uint8_t {
  UNKNOWN = 0,
  SWITCH = 1, // 지속 릴레이 (ON/OFF) - 조명, 일괄소등
  OUTLET = 2, // 스마트 콘센트 (대기전력/소비전력 모니터링 포함)
  GAS = 3,    // 차단 밸브 (단방향 닫기 / 차단) - 가스 밸브
  MOMENTARY =
      4, // 단방향 순간 펄스 트리거 (호출) - 엘리베이터 호출, 현관문 열림
  THERMOSTAT = 5, // 연속 희망온도 파라미터 - 난방
  VENT = 6,       // 이산 다단계 풍량 파라미터 - 환기
  AIRCON = 7      // 온도 + 풍량 복합 파라미터 - 에어컨
};

struct SlotCoverage {
  DeviceClass dev_class{DeviceClass::UNKNOWN};
};

struct DecodedDeviceState {
  int power{0};
  int target_temp{0};
  int current_temp{0};
  int fan_speed{0};
  int vent_mode{1};
  float power_w{0.0f};
  int floor{1};
  int direction{0};
  int ho{0};
  char valve_state[8]{"closed"}; // 고정 8바이트 버퍼로 수명 안전 보장
  DeviceClass dev_class{DeviceClass::UNKNOWN};
  bool should_broadcast{false};
};

// String conversion helper declarations (implemented in Device_Registry.cpp)
const char *DeviceClassToName(DeviceClass cls) noexcept;
const char *DeviceClassToCliString(DeviceClass cls) noexcept;
const char *DeviceClassToTelemetryString(DeviceClass cls) noexcept;

// ============================================================================
// DEVICE KEY IDENTIFICATION
// ============================================================================

struct DeviceKey {
  uint8_t dev_id{0};
  uint8_t sub1{0};
  uint8_t sub2{0};

  constexpr bool operator==(const DeviceKey &o) const noexcept {
    return dev_id == o.dev_id && sub1 == o.sub1 && sub2 == o.sub2;
  }
  constexpr bool operator!=(const DeviceKey &o) const noexcept {
    return !(*this == o);
  }
};

struct DeviceStateEntry {
  uint8_t dev_id;
  uint8_t sub1, sub2;
  std::array<uint8_t, 64> last_ack_data;
  uint8_t last_ack_len{0};
  uint8_t last_target_temp{0};
  uint8_t last_current_temp{0};
  uint32_t last_updated_ms{0};
  mutable uint32_t last_stale_poll_ms{0};
  uint8_t timeout_count{0};
  bool is_online{false};

  [[nodiscard]] bool isStale() const noexcept {
    return last_updated_ms > 0 &&
           TimeUtils::isElapsed(last_updated_ms,
                                Config::Timing::STALE_DEVICE_THRESHOLD_MS);
  }
};

static_assert(std::is_trivially_copyable_v<DeviceStateEntry>,
              "DeviceStateEntry must be trivially copyable");

struct DeviceUpdateResult {
  bool updated{false};
  bool should_broadcast{false};
  uint8_t dev_id{0};
  uint8_t sub1{0};
  uint8_t sub2{0};
  DecodedDeviceState state{};
  uint8_t extra_count{0};
  struct ExtraDevice {
    uint8_t sub1{0};
    DecodedDeviceState state{};
  } extra[7]{};
};

/// Decode device state from snapshot via registered protocol decoder
bool Device_DecodeState(uint8_t dev_id,
                        const StaticPacket &ack,
                        const DeviceStateEntry *dev,
                        DecodedDeviceState &out) noexcept;

// ── Device State Query & Management API (L3 SSOT Snapshot Interface) ─────────

/// Initialise device repository tables.
void Device_Init() noexcept;

/// Clear all registered devices from the cache.
void Device_Clear() noexcept;

/// Returns a read-only snapshot of a device entry by (dev_id, sub1, sub2).
/// Thread-safe copy. Returns false if device not found.
/// @param out_copy  Filled with cached state on success.
[[nodiscard]] bool Device_GetSnapshot(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                                       DeviceStateEntry &out_copy) noexcept;

/// Returns snapshot of device at given index (for iteration).
/// Returns false if index out of range.
[[nodiscard]] bool Device_GetSnapshot(size_t index,
                                       DeviceStateEntry &out_copy) noexcept;
[[nodiscard]] bool Device_GetSnapshotAt(size_t index,
                                         DeviceStateEntry &out_copy) noexcept;

/// Chunked snapshot reader for safe iteration without stack exhaustion.
/// Copies up to max_count entries starting at start_idx within a single lock.
/// Note: Across multiple chunk iterations, a slight time-skew between chunks is
/// acceptable and intended for telemetry/reporting to keep lock holding minimal.
[[nodiscard]] size_t Device_GetSnapshotChunk(size_t start_idx,
                                             DeviceStateEntry *out_buf,
                                             size_t max_count) noexcept;

/// Returns total mutex timeout occurrences while accessing device cache.
[[nodiscard]] uint32_t Device_GetCacheLockTimeouts() noexcept;

/// Returns current online device count (all channels combined).
[[nodiscard]] size_t Device_GetOnlineCount() noexcept;

/// Returns total registered device count.
[[nodiscard]] size_t Device_GetCount() noexcept;

/// Find copy of device entry (thread-safe copy).
[[nodiscard]] bool Device_FindCopy(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                                   DeviceStateEntry &out_copy) noexcept;

/// Get copy of device entry at index (thread-safe copy).
[[nodiscard]] bool Device_GetAtCopy(size_t index,
                                    DeviceStateEntry &out_copy) noexcept;

/// Register FCU sub-device slot into repository cache.
void Device_RegisterFcu(uint8_t slot_idx) noexcept;

/// Synchronize FCU runtime snapshot state into repository cache.
void Device_SyncFcuState(uint8_t slot_idx, uint8_t target_temp, uint8_t room_temp,
                         bool is_online, const uint8_t *raw_pkt, size_t raw_len) noexcept;

/// Record polling timeout for device.
void Device_HandlePollingTimeout(uint8_t dev_id, uint8_t sub1, uint8_t sub2) noexcept;

/// Update last stale poll timestamp for device.
void Device_SetLastStalePollMs(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                              uint32_t ms) noexcept;
void Device_SetLastStalePollMsByIndex(size_t index, uint32_t ms) noexcept;

// ── Device State Update API (called by L3 Packet_Router after bus decode) ─────

/// Update device cached state from a raw bus ACK packet.
/// Parses, decodes, and stores into SSOT. Returns update result with event.
/// Zero heap — all processing on stack-local temporaries.
DeviceUpdateResult Device_UpdateFromBus(StaticPacket &ack_pkt) noexcept;

/// Set desired target temperature for a thermostat device.
/// Used by L4 service (CTL_Service) when SmartThings sends a setpoint command.
bool Device_SetTargetTemp(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                           uint8_t temp) noexcept;

/// Copy the last known virtual ACK frame for a device into out_pkt.
/// Used by L3 Packet_Router for cache virtual response (Fast-Path).
/// Returns false if no cached ACK available.
[[nodiscard]] bool Device_CopyVirtualAck(uint8_t dev_id, uint8_t sub1,
                                          uint8_t sub2,
                                          StaticPacket &out_pkt) noexcept;

// ── L4 State Change Listener Subscription API ────────────────────────────────
using DeviceStateListener    = void (*)(const DeviceUpdateResult &res) noexcept;
using DoorphoneEventListener = void (*)(bool front_bell, bool lobby_bell) noexcept;
using ElevatorEventListener  = void (*)(uint8_t sub1, uint8_t sub2, uint8_t floor,
                                       uint8_t ho, uint8_t power,
                                       bool is_arrival) noexcept;

void Device_RegisterStateListener(DeviceStateListener listener) noexcept;
void Device_RegisterDoorphoneListener(DoorphoneEventListener listener) noexcept;
void Device_RegisterElevatorListener(ElevatorEventListener listener) noexcept;

/// Process incoming bus ACK packet: updates SSOT cache and dispatches to registered listener.
void Device_ProcessBusPacket(StaticPacket &ack_pkt) noexcept;

/// Dispatch doorphone bell state change to registered listener.
void Device_NotifyDoorphoneEvent(bool front_bell, bool lobby_bell) noexcept;

/// Dispatch elevator state or arrival event to registered listener.
void Device_NotifyElevatorEvent(uint8_t sub1, uint8_t sub2, uint8_t floor,
                                uint8_t ho, uint8_t power,
                                bool is_arrival) noexcept;

// ── L3 Decoupled Protocol Parser & Decoder Registration API ──────────────────
using DeviceAckPacketCheckFn = bool (*)(std::span<const uint8_t> frame) noexcept;
using DeviceKeyExtractorFn   = bool (*)(std::span<const uint8_t> frame, uint8_t &dev_id, uint8_t &sub1, uint8_t &sub2) noexcept;
using DeviceStateDecoderFn   = bool (*)(uint8_t dev_id, const StaticPacket &ack, const DeviceStateEntry *dev, DecodedDeviceState &out) noexcept;
using DeviceNormSub1Fn       = uint8_t (*)(uint8_t dev_id, uint8_t sub1) noexcept;
using DoorphoneOpenHandler   = bool (*)(bool is_lobby) noexcept;

void Device_RegisterParserHooks(DeviceAckPacketCheckFn ack_check, DeviceKeyExtractorFn key_extract) noexcept;
void Device_RegisterStateDecoder(DeviceStateDecoderFn fn) noexcept;
void Device_RegisterNormSub1Hook(DeviceNormSub1Fn fn) noexcept;
void Device_RegisterDoorphoneOpenHandler(DoorphoneOpenHandler handler) noexcept;

// ── L4 Doorphone Control & State Facade API ──────────────────────────────────
[[nodiscard]] bool Device_DoorphoneOpen(bool is_lobby = false) noexcept;
void Device_DoorphoneGetState(bool &out_front_bell, bool &out_lobby_bell,
                              uint32_t &out_last_bell_ms) noexcept;

// ── FCU (Air Conditioner) Domain Public Interface ────────────────────────────
struct FcuDeviceSnapshot {
  bool power{false};
  uint16_t mode{1};
  uint16_t fan_speed{0};
  uint16_t swing{0};
  uint8_t target_temp{24};
  uint8_t room_temp{0};
  bool is_online{false};
};

bool Device_GetFcuSnapshot(uint8_t slot_idx, FcuDeviceSnapshot &out) noexcept;
bool Device_ControlFcu(uint8_t slot_idx, const char *action, int value,
                       uint16_t mode = 1, uint16_t fan = 4, uint16_t swing = 0,
                       uint8_t temp = 24) noexcept;
