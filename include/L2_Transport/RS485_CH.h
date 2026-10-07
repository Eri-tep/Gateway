#pragma once

// ============================================================================
// RS485_CH.h — L2 Transport / Data Link Channels
// Physical RS-485 Serial Channel Abstraction (CH1~CH4)
// Canonical 4+1 Layer: L2 — depends only on L1 Drivers and L0 Base.
// ============================================================================
//
// Channel Mapping:
//   CH1 = UART_NUM_0  Wallpad main RS-485 bus  (Task_Ch1,     Core 1)
//   CH2 = UART_NUM_1  Sub-device RS-485 bus    (Task_Ch2Ch3,  Core 1)
//   CH3 = UART_NUM_2  Ventilation/Temp bus     (Task_Ch2Ch3,  Core 1)
//   CH4 = SW Serial   Doorphone isolated line  (Task_Ch4,     Core 1)
//
// Design invariants:
//   - All FreeRTOS task entry points (Task_Ch1, Task_Ch2Ch3, Task_Ch4) are
//     declared here as the canonical L2 RS-485 channel interface.
//   - TX path:  upper layer (L3/L4) calls RS485_EnqueueTx() → Channel TX queue
//   - RX path:  upper layer (L3) calls RS485_PopRxQueue() → Pull from CH RX queue
//   - No upward includes ($L2 → L3+$): zero violation of AGENTS.md Rule 17.
//   - Implementation resides in EngineTask.cpp during phased migration (Step 4
//     will move Task bodies to RS485_CH.cpp). This header establishes the
//     canonical L2 interface identity ahead of the full body migration.
// ============================================================================

#include "L0_Foundation/System_Buffer.h"
#include "L0_Foundation/System_Config.h"
#include "L0_Foundation/System_Platform.h"
#include "L1_HAL/Uart_Driver.h"
#include <span>

// ── Packet Timing & Queuing Primitives ──────────────────────────────────────
struct TimestampedPacket {
  uint32_t due_ms;
  StaticPacket pkt;
};

template <size_t Capacity = 8> class TimestampedPacketQueue {
private:
  TimestampedPacket _elements[Capacity]{};
  size_t _head = 0;
  size_t _tail = 0;
  size_t _size = 0;
  portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;

public:
  bool enqueue(const StaticPacket &pkt, uint32_t due_ms) noexcept {
    CriticalSectionLocker lock(&_mux);
    if (_size >= Capacity) {
      return false;
    }
    _elements[_tail] = {due_ms, pkt};
    _tail = (_tail + 1) % Capacity;
    _size++;
    return true;
  }

  bool dequeue(StaticPacket &out_pkt, uint32_t &out_due_ms) noexcept {
    CriticalSectionLocker lock(&_mux);
    if (_size == 0) {
      return false;
    }
    out_due_ms = _elements[_head].due_ms;
    out_pkt = _elements[_head].pkt;
    _head = (_head + 1) % Capacity;
    _size--;
    return true;
  }

  bool peek(StaticPacket &out_pkt, uint32_t &out_due_ms) noexcept {
    CriticalSectionLocker lock(&_mux);
    if (_size == 0) {
      return false;
    }
    out_due_ms = _elements[_head].due_ms;
    out_pkt = _elements[_head].pkt;
    return true;
  }

  size_t size() const noexcept {
    CriticalSectionLocker lock(const_cast<portMUX_TYPE *>(&_mux));
    return _size;
  }
};

[[nodiscard]] bool Queue_EnqueueDropHead(QueueHandle_t queue,
                                         const StaticPacket &packet) noexcept;
[[nodiscard]] bool Queue_EnqueueDropTail(QueueHandle_t queue,
                                         const StaticPacket &packet) noexcept;

enum class Ch1State : uint8_t {
  IDLE,
  VIP_CONTROL,
  NORMAL_CONTROL,
  POLL_DEVICE
};

/// Enqueue control packet to CH1 local bus (VIP or normal queue).
[[nodiscard]] bool RS485_EnqueueControl(const StaticPacket &pkt, bool vip = false) noexcept;

// ── RS-485 Packet Dispatcher SPI (Canonical L2-L3 Inversion) ─────────────────
struct RS485_PacketDispatcher {
  bool (*onBuildPoll)(StaticPacket &out_pkt, uint8_t &poll_dev_id,
                      uint8_t &poll_sub1, uint8_t &poll_sub2) noexcept{nullptr};
  void (*onBusPacket)(uint8_t ch, const StaticPacket &pkt,
                      const StaticPacket *matching_query) noexcept{nullptr};
  void (*onTimeout)(uint8_t poll_dev_id, uint8_t poll_sub1,
                    uint8_t poll_sub2) noexcept{nullptr};
  bool (*onDispatchControl)(StaticPacket &req, StaticPacket &virtual_ack_out) noexcept{nullptr};
  uint32_t (*onGetPollIntervalMs)() noexcept{nullptr};
  uint8_t (*onGetStx)() noexcept{nullptr};
  bool (*onIsAutoUnlocked)() noexcept{nullptr};
  void (*onFeedAutoFrame)(std::span<const uint8_t> frame) noexcept{nullptr};
  int (*onExtractLength)(const uint8_t *stream, size_t stream_len,
                         size_t stx_idx) noexcept{nullptr};
  bool (*onValidatePacket)(std::span<const uint8_t> frame) noexcept{nullptr};
  bool (*onHandleSubBusQuery)(uint8_t ch, const StaticPacket &req,
                              StaticPacket &virtual_ack_out) noexcept{nullptr};
  void (*onFeedControlFrame)(std::span<const uint8_t> frame) noexcept{nullptr};
  void (*onDoorphonePacket)(const StaticPacket &pkt) noexcept{nullptr};
  void (*onDoorphoneReset)() noexcept{nullptr};
  bool (*onMatchDoorphoneLock)(uint8_t stx, uint8_t etx, uint8_t len,
                               uint8_t &out_fixed_len) noexcept{nullptr};
  bool (*onDoorphoneGetLockedFraming)(uint8_t &stx, uint8_t &etx, uint8_t &len) noexcept{nullptr};
  void (*onDoorphoneFrameDetected)(uint8_t stx, uint8_t etx, uint8_t len) noexcept{nullptr};
  bool (*onIsQueryPacket)(std::span<const uint8_t> frame) noexcept{nullptr};
};

void RS485_RegisterDispatcher(const RS485_PacketDispatcher &dispatcher) noexcept;

// ── Channel TX Enqueue (Downlink: L3/L4 → L2) ────────────────────────────────

/// Enqueue a packet for transmission on CH1 (Wallpad main bus).
/// Priority: normal control queue. Non-blocking — drops head if full.
/// @return true if enqueued successfully.
[[nodiscard]] bool RS485_EnqueueCh1Ctrl(const StaticPacket &pkt) noexcept;

/// Enqueue a high-priority VIP packet on CH1 (e.g. SmartThings direct command).
/// Bypasses normal queue head-of-line. Non-blocking.
[[nodiscard]] bool RS485_EnqueueCh1Vip(const StaticPacket &pkt) noexcept;

/// Enqueue a packet for transmission on CH4 (Doorphone SW Serial passthrough).
/// Non-blocking.
[[nodiscard]] bool RS485_EnqueueCh4Pass(const StaticPacket &pkt) noexcept;

// ── Channel Lifecycle (called once at boot from main.cpp / Engine_InitQueues) ─

/// Initialise all RS-485 channel FreeRTOS queues and semaphores.
/// Must be called before any FreeRTOS task is launched.
void RS485_InitQueues();

// ── FreeRTOS Task Entry Points (Core 1 — scheduled from main.cpp) ────────────
// Task naming follows MODERN_CPP_GUIDELINES.md §1.2 (Task_<Domain>).

/// CH1 Wallpad main bus task (UART_NUM_0, Core 1).
/// Implements full CH1 FSM: VIP control → normal control → poll device.
void Task_Ch1(void *pvParameters);

/// CH2 Sub-Wallpad #1 bus task (UART_NUM_1, Core 1).
/// Handles virtual ACK cache responses and U-turn bypass routing.
void Task_Ch2(void *pvParameters);

/// CH3 Sub-Wallpad #2 bus task (UART_NUM_2, Core 1).
/// Handles virtual ACK cache responses and U-turn bypass routing.
void Task_Ch3(void *pvParameters);

/// CH4 Doorphone SW Serial task (SoftwareSerial, Core 1).
/// Handles doorphone frame detection, bell events, and passthrough TX.
void Task_Ch4(void *pvParameters);

// ── Uart Event Queue Access (used by Uart_Driver boot init) ──────────────────

/// Returns the FreeRTOS UART event queue pointer for the given UART number.
/// Called by main.cpp Boot_InitHardwareAndDevices() → Uart_InitHw().
QueueHandle_t *RS485_GetUartEventQueuePtr(uint8_t uart_num) noexcept;

// ── Channel Config Struct (used by main.cpp task spawn descriptors) ───────────

/// Per-channel UART configuration passed as pvParameters to Task_Ch2Ch3.
struct WallpadChannelConfig {
  uart_port_t uart_num;       ///< UART port number (UART_NUM_1 or UART_NUM_2)
  QueueHandle_t *event_queue_ptr; ///< Pointer to UART event queue handle
  uint8_t channel_id;         ///< Logical channel ID (2 or 3)
};

// ── CH4 Doorphone Passthrough Queue Enqueue API ──────────────────────────────
void RS485_EnqueueCh4Passthrough(const StaticPacket &pkt) noexcept;

