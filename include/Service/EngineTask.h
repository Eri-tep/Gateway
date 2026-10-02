#pragma once

// ============================================================================
// EngineTask: Level 4 RTOS Task Scheduling, Queues & RS-485 Engine (CH1~CH4)
// ============================================================================

#include "Base/BufferUtils.h"
#include "Base/SystemConfig.h"
#include "Base/SystemPlatform.h"
#include "Protocol/ControlTemplate.h"
#include "Protocol/DeviceRegistry.h"
#include "Protocol/WallpadProtocol.h"
#include "System/LockUtils.h"
#include "System/SystemDiagnostics.h"
#include "System/SystemStorage.h"
#include "Transport/DoorphoneTracker.h"
#include "Transport/NetworkRouter.h"

#include <array>
#include <atomic>
#include <shared_mutex>

// ── Packet Timing & Queuing Primitives ──
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

extern SemaphoreHandle_t g_ctrl_queue_mutex;

[[nodiscard]] bool Queue_EnqueueDropHead(QueueHandle_t queue,
                                         const StaticPacket &packet) noexcept;

struct WallpadChannelConfig {
  uart_port_t uart_num;
  QueueHandle_t *event_queue_ptr;
  uint8_t channel_id;
};

namespace PacketCodec {
uint8_t calculateChecksum(const uint8_t *data, size_t len) noexcept;
} // namespace PacketCodec

class ControlDispatcher {
public:
  bool dispatch(StaticPacket &req, StaticPacket &virtual_ack_out);
};

enum class Ch1State : uint8_t {
  IDLE,
  VIP_CONTROL,
  NORMAL_CONTROL,
  POLL_DEVICE
};

// ── Global Instances & RTOS Primitives ──
extern DeviceRepository g_device_repo;
extern ControlDispatcher g_control_dispatcher;
extern QueueHandle_t g_ch1_control_queue, g_ch1_vip_queue;
extern EventGroupHandle_t g_wifi_event_group;
extern QueueSetHandle_t g_ch1_queue_set;
extern QueueHandle_t g_uart0_event_queue, g_uart1_event_queue,
    g_uart2_event_queue;
extern QueueHandle_t g_ch4_passthrough_queue;
extern std::atomic<bool> g_initial_caching_complete;
extern uint32_t g_boot_start_ms;
extern SoftwareSerial g_doorphone_serial;
extern SemaphoreHandle_t g_tracer_sem;

extern SemaphoreHandle_t g_uart0_mutex, g_uart1_mutex, g_uart2_mutex;

// ── Lifecycle Initialization ──
void Engine_InitQueues();

// ── FreeRTOS Engine Task Functions ──
void Task_Ch1(void *pvParameters);
void Task_Ch2Ch3(void *pvParameters);
void Task_Ch4(void *pvParameters);

// ── Engine Event Listeners & Router Delegate ──
using DeviceStateListener = void (*)(const DeviceUpdateResult &res) noexcept;
using DoorphoneEventListener = void (*)(bool front_bell, bool lobby_bell) noexcept;
using Ch5ForwardHandler = bool (*)(uint8_t slot_idx, const StaticPacket &pkt,
                                   bool burst) noexcept;

void Engine_RegisterDeviceStateListener(DeviceStateListener listener) noexcept;
void Engine_RegisterDoorphoneListener(DoorphoneEventListener listener) noexcept;
void Engine_RegisterCh5ForwardHandler(Ch5ForwardHandler handler) noexcept;
