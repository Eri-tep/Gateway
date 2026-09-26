#pragma once

// ============================================================================
// PACKET STRUCTURES & DEVICE REPOSITORY (Extracted from Common.h SECTION 5)
// ============================================================================

#include "Metrics.h"
#include "Buffers.h"

constexpr uint8_t PKT_STX = 0xF7;
constexpr uint8_t PKT_ETX = 0xEE;

struct StaticPacket {
  uint8_t channel_id;
  uint8_t length;
  std::array<uint8_t, 64> data;
};

struct TimestampedPacket {
  uint32_t due_ms;
  StaticPacket pkt;
};

template <size_t Capacity = 8> class TimestampedPacketQueue {
private:
  TimestampedPacket _elements[Capacity];
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

// ============================================================================
// 1ST TIER WARM-START CACHE (RTC FAST SRAM & NVS SNAPSHOT)
// ============================================================================

struct RtcWarmCacheEntry {
  uint8_t dev_id;
  uint8_t sub1;
  uint8_t sub2;
  uint8_t source_channels;
  uint8_t raw_len;
  uint8_t raw_query[32];
};

struct RtcWarmCache {
  uint32_t magic; // 0x57415243 ('WARC')
  uint8_t count;
  uint8_t reserved[3];
  RtcWarmCacheEntry entries[64];
  uint32_t crc32;
};

constexpr uint32_t RTC_MAGIC_WARM_CACHE = 0x57415243; // 'WARC'

extern SemaphoreHandle_t g_ctrl_queue_mutex;

[[nodiscard]] inline bool
Queue_EnqueueDropHead(QueueHandle_t queue,
                      const StaticPacket &packet) noexcept {
  if (UNLIKELY(!queue))
    return false;
  MutexLocker lock(g_ctrl_queue_mutex);
  if (xQueueSend(queue, &packet, 0) == pdTRUE)
    return true;
  StaticPacket dummy;
  xQueueReceive(queue, &dummy, 0);
  return (xQueueSend(queue, &packet, 0) == pdTRUE);
}

class TokenBucket {
private:
  const uint32_t _capacity;
  const uint32_t _refill_ms;
  std::atomic<uint32_t> _tokens;
  std::atomic<uint32_t> _last_refill_ms;

public:
  explicit TokenBucket(uint32_t capacity, uint32_t refill_ms)
      : _capacity(capacity), _refill_ms(refill_ms), _tokens(capacity),
        _last_refill_ms(0) {}

  [[nodiscard]] bool consume(uint32_t count = 1) noexcept {
    refill();
    uint32_t current = _tokens.load(std::memory_order_relaxed);
    while (current >= count) {
      if (_tokens.compare_exchange_weak(current, current - count,
                                        std::memory_order_acquire,
                                        std::memory_order_relaxed))
        return true;
    }
    return false;
  }
  void restore(uint32_t count = 1) noexcept {
    uint32_t current = _tokens.load(std::memory_order_relaxed);
    uint32_t target;
    do {
      target = std::min(current + count, _capacity);
    } while (!_tokens.compare_exchange_weak(
        current, target, std::memory_order_release, std::memory_order_relaxed));
  }
  void refill() noexcept {
    const uint32_t now = millis();
    uint32_t last = _last_refill_ms.load(std::memory_order_relaxed);
    if (now - last >= _refill_ms) {
      const uint32_t elapsed = now - last;
      const uint32_t new_tokens = elapsed / _refill_ms;
      if (new_tokens > 0) {
        const uint32_t next_last = now - (elapsed % _refill_ms);
        if (_last_refill_ms.compare_exchange_strong(
                last, next_last, std::memory_order_release,
                std::memory_order_relaxed)) {
          uint32_t current = _tokens.load(std::memory_order_relaxed);
          uint32_t target;
          do {
            target = std::min(current + new_tokens, _capacity);
          } while (!_tokens.compare_exchange_weak(current, target,
                                                  std::memory_order_release,
                                                  std::memory_order_relaxed));
        }
      }
    }
  }
};

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

struct WallpadChannelConfig {
  uart_port_t uart_num;
  QueueHandle_t *event_queue_ptr;
  uint8_t channel_id;
};

class DeviceRepository {
private:
  static constexpr size_t MAX_DEVICES = 64;
  DeviceStateEntry cache[MAX_DEVICES];
  int8_t dev_lookup_map[256];
  size_t device_count = 0;
  SemaphoreHandle_t _cache_mutex = nullptr;

public:
  DeviceStateEntry *findMutable(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                                bool auto_create = false) noexcept;
  void initDevices();
  void clear();
  [[nodiscard]] const DeviceStateEntry *find(uint8_t dev_id, uint8_t sub1,
                                             uint8_t sub2) const noexcept;
  [[nodiscard]] const DeviceStateEntry *getAt(size_t index) const noexcept;
  [[nodiscard]] bool getSnapshot(size_t index,
                                 DeviceStateEntry &out_copy) noexcept;
  [[nodiscard]] size_t count() const noexcept;
  [[nodiscard]] size_t getOnlineCount() const noexcept;
  void setLastStalePollMs(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                          uint32_t ms) noexcept;
  void setLastStalePollMsByIndex(size_t index, uint32_t ms) noexcept;
  void updateFromBus(StaticPacket &ack);
  void handlePollingTimeout(const DeviceStateEntry *dev);
  void handlePollingTimeout(uint8_t dev_id, uint8_t sub1, uint8_t sub2);
  [[nodiscard]] bool copyVirtualAck(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                                    StaticPacket &out) noexcept;
};

namespace PacketCodec {
uint8_t calculateChecksum(const uint8_t *data, size_t len) noexcept;
} // namespace PacketCodec

class ControlDispatcher {
public:
  bool dispatch(StaticPacket &req, StaticPacket &virtual_ack_out);
};

struct HwSnapshot {
  uint8_t cpu0_cur, cpu0_15m_avg, cpu0_15m_peak, cpu0_24h_avg, cpu0_24h_peak;
  uint8_t cpu1_cur, cpu1_15m_avg, cpu1_15m_peak, cpu1_24h_avg, cpu1_24h_peak;
  uint16_t ram_cur, ram_15m_avg, ram_15m_peak, ram_24h_avg, ram_24h_peak;
  int8_t temp_cur, temp_15m_avg, temp_15m_peak, temp_24h_avg, temp_24h_peak;
};

struct StackSnapshot {
  uint16_t ch1_stack, ch2_stack, ch3_stack, ch4_stack, net_stack, telnet_stack;
};

struct LogEntry {
  uint32_t timestamp;
  char reason[32];
  SysSnapshot stats_snapshot;
  HwSnapshot hw_snapshot;
  StackSnapshot stack_snapshot;
  PktSnapshot packet_stats_snapshot;
};

class LogManager {
public:
  static constexpr size_t MAX_LOG_ENTRIES = 20;
  static void writeRebootLog(const char *reason);
  static size_t getLogCount();
  static bool getLogEntry(size_t index, LogEntry &out_entry);
  static void readRebootLog(char *out_buf, size_t max_len, size_t index = 0);
  static void clearRebootLog();
};

struct TracePacketEntry {
  struct timeval tv;
  uint8_t channel;
  bool is_tx;
  TraceType type;
  uint8_t len;
  std::array<uint8_t, 64> data;
};

class TelnetManager;
class TelnetTracer;

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

extern Ch1StateMetrics g_ch1_state_metrics;
extern DeviceRepository g_device_repo;
extern ControlDispatcher g_control_dispatcher;
extern QueueHandle_t g_ch1_control_queue, g_ch1_vip_queue;
extern StaticQueue_t g_ch1_ctrl_queue_buf, g_ch4_pass_queue_buf,
    g_ch1_vip_queue_buf;
extern uint8_t
    g_ch1_ctrl_storage[Config::Queue::POOL_SIZE_CONTROL * sizeof(StaticPacket)];
extern uint8_t
    g_ch4_pass_storage[Config::Queue::POOL_SIZE_CONTROL * sizeof(StaticPacket)];
extern uint8_t
    g_ch1_vip_storage[Config::Queue::POOL_SIZE_CONTROL * sizeof(StaticPacket)];

extern StaticTask_t g_task_core1_ch1_buf, g_task_core1_slave_buf,
    g_task_core1_slave2_buf, g_task_core1_ch4_buf, g_task_core0_net_buf,
    g_telnet_task_buf;
extern StackType_t stackCore1Ch1[Config::Task::STACK_SIZE_CORE1],
    stackCore1Slave[Config::Task::STACK_SIZE_CORE1],
    stackCore1Slave2[Config::Task::STACK_SIZE_CORE1],
    stackCore1Ch4[Config::Task::STACK_SIZE_CH4],
    stackCore0Net[Config::Task::STACK_SIZE_CORE0],
    telnetTaskStack[Config::Task::STACK_SIZE_TELNET];
extern EventGroupHandle_t g_wifi_event_group;
extern QueueSetHandle_t g_ch1_queue_set;
extern QueueHandle_t g_uart0_event_queue, g_uart1_event_queue,
    g_uart2_event_queue;
extern QueueHandle_t g_ch4_passthrough_queue;
extern SemaphoreHandle_t g_ch5_mutex;
extern SemaphoreHandle_t g_mgmt_mutex;
extern RuntimeConfig g_config;
extern portMUX_TYPE g_config_mux;
extern std::atomic<uint32_t> g_ch1_bus_ms;
extern std::atomic<bool> g_config_dirty;
extern std::atomic<bool> g_ota_in_progress;
extern std::atomic<bool> g_initial_caching_complete;
// ★ wallpad reset 시 수렴 상태를 재초기화하여 재학습·재락을 허용하는 신호 플래그
extern std::atomic<bool> g_probe_convergence_reset;
extern EventGroupHandle_t g_system_event_group;
constexpr EventBits_t SYS_EVT_OTA_IDLE = (1 << 0);
constexpr EventBits_t SYS_EVT_CACHE_READY = (1 << 1);
constexpr EventBits_t SYS_EVT_SYSTEM_RUNNING = (1 << 2);
extern PacketStatistics g_pkt_stats;
extern uint32_t g_boot_start_ms;
struct WifiFallbackGuard {
  std::atomic<bool> testing{false};
  uint32_t start_ms{0};
  char prev_ssid[64]{0};
  char prev_pass[64]{0};
};
extern WifiFallbackGuard g_wifi_guard;
extern SystemMetricsTracker g_metrics;

extern SoftwareSerial g_doorphone_serial;

void Task_Ch1(void *pvParameters);
void Task_Ch2Ch3(void *pvParameters);
void Task_Ch4(void *pvParameters);
void Task_Network(void *pvParameters);
void Task_Telnet(void *pvParameters);

extern TelnetManager g_telnet_manager;
extern SemaphoreHandle_t g_tracer_sem;
void System_TakeSnapshot(SysSnapshot &sys_snapshot, HwSnapshot &hw_snapshot,
                         StackSnapshot &stack_snapshot,
                         PktSnapshot &pkt_snapshot);

extern uint32_t rtc_magic;
extern uint32_t rtc_last_alive_ms[6];
constexpr uint32_t RTC_MAGIC_WDT = 0x57445431;
extern uint32_t rtc_rescue_magic;
constexpr uint32_t RTC_MAGIC_RESCUE = 0x52455343;
extern uint32_t rtc_clean_restart_magic;
constexpr uint32_t RTC_MAGIC_CLEAN_RESTART = 0x5AA55AA5;
extern RtcWarmCache rtc_warm_cache;
extern uint32_t rtc_crash_counter;

struct TaskWdtMetrics {
  std::atomic<uint32_t> last_feed_ms{0};
  std::atomic<uint32_t> max_interval_ms{0};
  std::atomic<uint32_t> feed_count{0};
};

class TaskWdtMonitor {
public:
  static constexpr size_t TASK_COUNT = 6;
  TaskWdtMetrics tasks[TASK_COUNT];

  inline void feed(size_t index) noexcept {
    if (index >= TASK_COUNT)
      return;
    uint32_t now = millis();
    rtc_last_alive_ms[index] = now;
    uint32_t prev =
        tasks[index].last_feed_ms.exchange(now, std::memory_order_relaxed);
    if (prev > 0) {
      uint32_t gap = (now >= prev) ? (now - prev) : 0;
      uint32_t cur_max =
          tasks[index].max_interval_ms.load(std::memory_order_relaxed);
      while (gap > cur_max &&
             !tasks[index].max_interval_ms.compare_exchange_weak(
                 cur_max, gap, std::memory_order_relaxed,
                 std::memory_order_relaxed)) {
      }
    }
    tasks[index].feed_count.fetch_add(1, std::memory_order_relaxed);
    esp_task_wdt_reset();
  }
};

extern TaskWdtMonitor g_wdt_monitor;

namespace Fmt {
void FormatHwMetrics(AppendBuf &out, const HwSnapshot &hw);
void FormatNetworkStats(AppendBuf &out, const PktSnapshot &pkt);
void FormatRs485Stats(AppendBuf &out, const PktSnapshot &pkt);
void FormatTaskStacks(AppendBuf &out, const StackSnapshot &st,
                      const TaskWdtMonitor &wdt);
} // namespace Fmt

extern TaskHandle_t g_telnet_task_handle, g_ch1_task_handle, g_ch2_task_handle,
    g_ch3_task_handle, g_ch4_task_handle, g_network_task_handle;

void Config_Load();
void Config_Save();
void Config_ResetDefaults();
void System_Restart(const char *reason);
void System_Sha256ToHex(const char *input, char *output);
void System_ReadCpuPct(uint8_t &cpu0_out, uint8_t &cpu1_out);
int8_t System_ReadTempC();
void System_EnterRescueMode(const char *reason);
void System_CheckOtaHealth();
void System_LogResetReason();
void System_DiagnoseStuck();
void System_CheckCoreDump();
extern const char *s_pending_reboot_reason;
[[nodiscard]] bool System_IsOtaPendingVerify();

void Hub_LoadConfig();
void Hub_SaveConfig();
bool Hub_SetSlot(uint8_t slot_idx, bool enabled, const char *ip, uint16_t port,
                 const char *name = nullptr);
bool Hub_SendPacket(uint8_t slot_idx, const StaticPacket &pkt);

struct RouteEndpoint {
  uint8_t channel_id{1}; // 기본 채널: CH1 (메인 물리 RS-485)
  int8_t slot_idx{-1};   // CH5인 경우 슬롯 인덱스 (0~4), 그 외 -1
  uint32_t last_seen_ms{0};
};

struct DeviceRouteEntry {
  uint8_t dev_id{0};
  uint8_t sub1{0};
  uint8_t sub2{0};
  RouteEndpoint endpoint{};
};

class DeviceRouteRegistry {
public:
  static constexpr size_t MAX_ROUTES = 64;

private:
  DeviceRouteEntry _entries[MAX_ROUTES]{};
  size_t _count{0};
  mutable portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;

public:
  void recordRoute(uint8_t channel_id, int8_t slot_idx, uint8_t dev_id,
                   uint8_t sub1, uint8_t sub2);
  bool lookupRoute(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                   RouteEndpoint &out_ep) const;
  size_t getRoutes(DeviceRouteEntry *out_buf, size_t max_count) const;
  void clear();
};

extern DeviceRouteRegistry g_route_registry;

extern std::atomic<bool> g_rescue_mode;
extern bool g_rollback_detected;
extern SemaphoreHandle_t g_uart0_mutex, g_uart1_mutex, g_uart2_mutex;
extern Config::Doorphone::FramingTracker g_doorphone_tracker;

// 1st-Tier Warm Cache externs
extern bool g_warm_cache_loaded;
extern uint8_t g_warm_cache_source; // 0: Cold, 1: RTC SRAM, 2: NVS Flash
extern uint8_t g_warm_cache_restored_count;
extern std::atomic<bool> g_warm_cache_dirty;
extern std::atomic<uint32_t> g_warm_cache_dirty_ms;

void Cache_SaveToRtc();
void Cache_SaveToNvs();
void Cache_RestoreOnBoot();
void Cache_CheckNvsDebounce();
