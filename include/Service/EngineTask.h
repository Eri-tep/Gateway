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

struct TracePacketEntry {
  struct timeval tv;
  uint8_t channel;
  bool is_tx;
  TraceType type;
  uint8_t len;
  std::array<uint8_t, 64> data;
};

enum class Ch1State : uint8_t {
  IDLE,
  VIP_CONTROL,
  NORMAL_CONTROL,
  POLL_DEVICE
};

struct Ch1StateMetrics {
  std::atomic<uint32_t> poll_cnt{0}, vip_cnt{0}, normal_cnt{0},
      stale_poll_cnt{0};
  std::atomic<Ch1State> last_from_state{Ch1State::IDLE};
  std::atomic<Ch1State> last_to_state{Ch1State::IDLE};
  std::atomic<uint32_t> last_transition_ms{0};
};

// ── Global Instances & RTOS Buffers ──
extern Ch1StateMetrics g_ch1_state_metrics;
extern DeviceRepository g_device_repo;
extern ControlDispatcher g_control_dispatcher;
extern QueueHandle_t g_ch1_control_queue, g_ch1_vip_queue;
extern StaticQueue_t g_ch1_ctrl_queue_buf, g_ch4_pass_queue_buf,
    g_ch1_vip_queue_buf;
extern uint8_t
    g_ch1_ctrl_storage[Config::Queue::POOL_SIZE_CONTROL * sizeof(StaticPacket)];
extern uint8_t g_ch4_pass_storage[Config::Queue::POOL_SIZE_CH4_PASS *
                                  sizeof(StaticPacket)];
extern uint8_t
    g_ch1_vip_storage[Config::Queue::POOL_SIZE_VIP * sizeof(StaticPacket)];

extern StaticTask_t g_task_core1_ch1_buf, g_task_core1_slave_buf,
    g_task_core1_slave2_buf, g_task_core1_ch4_buf, g_task_core0_net_buf,
    g_telnet_task_buf;
extern StackType_t stackCore1Ch1[Config::Task::STACK_SIZE_CORE1],
    stackCore1Slave[Config::Task::STACK_SIZE_SLAVE],
    stackCore1Slave2[Config::Task::STACK_SIZE_SLAVE],
    stackCore1Ch4[Config::Task::STACK_SIZE_CH4],
    stackCore0Net[Config::Task::STACK_SIZE_CORE0],
    telnetTaskStack[Config::Task::STACK_SIZE_TELNET];
extern EventGroupHandle_t g_wifi_event_group;
extern QueueSetHandle_t g_ch1_queue_set;
extern QueueHandle_t g_uart0_event_queue, g_uart1_event_queue,
    g_uart2_event_queue;
extern QueueHandle_t g_ch4_passthrough_queue;
extern SemaphoreHandle_t g_mgmt_mutex;
extern std::atomic<bool> g_initial_caching_complete;
extern std::atomic<bool> g_probe_convergence_reset;
extern PacketStatistics g_pkt_stats;
extern uint32_t g_boot_start_ms;
struct WifiFallbackGuard {
  std::atomic<bool> testing{false};
  uint32_t start_ms{0};
  char prev_ssid[64]{0};
  char prev_pass[64]{0};
};
extern WifiFallbackGuard g_wifi_guard;
extern SoftwareSerial g_doorphone_serial;
extern SemaphoreHandle_t g_tracer_sem;

extern SemaphoreHandle_t g_uart0_mutex, g_uart1_mutex, g_uart2_mutex;

// ── FreeRTOS Task Functions ──
void Task_Ch1(void *pvParameters);
void Task_Ch2Ch3(void *pvParameters);
void Task_Ch4(void *pvParameters);
void Task_Network(void *pvParameters);
void Task_Telnet(void *pvParameters);

// ── Hub Socket Bridge Functions ──
void Hub_LoadConfig();
void Hub_SaveConfig();
bool Hub_SetSlot(uint8_t slot_idx, bool enabled, const char *ip, uint16_t port,
                 const char *name = nullptr);
bool Hub_SendPacket(uint8_t slot_idx, const StaticPacket &pkt);
