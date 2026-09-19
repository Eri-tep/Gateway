#include "Common.h"
#include <esp_attr.h>

RTC_NOINIT_ATTR uint32_t rtc_magic;
RTC_NOINIT_ATTR uint32_t rtc_last_alive_ms[6];
RTC_NOINIT_ATTR uint32_t rtc_rescue_magic;
RTC_NOINIT_ATTR uint32_t rtc_crash_counter;
RTC_NOINIT_ATTR uint32_t rtc_clean_restart_magic;
RTC_NOINIT_ATTR RtcWarmCache rtc_warm_cache;

std::atomic<bool> g_rescue_mode{false};
bool g_rollback_detected = false;

// 1st-Tier Warm Cache
bool g_warm_cache_loaded = false;
uint8_t g_warm_cache_source = 0; // 0: None/Cold, 1: RTC SRAM, 2: NVS Flash
uint8_t g_warm_cache_restored_count = 0;
std::atomic<bool> g_warm_cache_dirty{false};
std::atomic<uint32_t> g_warm_cache_dirty_ms{0};

DeviceRepository g_device_repo;
ControlDispatcher g_control_dispatcher;
PacketStatistics g_pkt_stats;
SystemMetricsTracker g_metrics;
TaskWdtMonitor g_wdt_monitor;

StaticQueue_t g_ch1_ctrl_queue_buf, g_ch4_pass_queue_buf, g_ch1_vip_queue_buf;
uint8_t g_ch1_ctrl_storage[Config::Queue::POOL_SIZE_CONTROL * sizeof(StaticPacket)];
uint8_t g_ch4_pass_storage[Config::Queue::POOL_SIZE_CONTROL * sizeof(StaticPacket)];
uint8_t g_ch1_vip_storage[Config::Queue::POOL_SIZE_CONTROL * sizeof(StaticPacket)];

QueueHandle_t g_ch1_control_queue = nullptr, g_ch1_vip_queue = nullptr;
QueueSetHandle_t g_ch1_queue_set = nullptr;
QueueHandle_t g_uart0_event_queue = nullptr, g_uart1_event_queue = nullptr,
              g_uart2_event_queue = nullptr;
QueueHandle_t g_ch4_passthrough_queue = nullptr;

EventGroupHandle_t g_system_event_group = nullptr;

SoftwareSerial g_doorphone_serial;
RuntimeConfig g_config{};

HubClientSlot g_hub_slots[Config::TCP::MAX_EW11_SLOTS];
SemaphoreHandle_t g_ch5_mutex = nullptr;
SemaphoreHandle_t g_ctrl_queue_mutex = nullptr;

StaticTask_t g_task_core1_ch1_buf, g_task_core1_slave_buf,
    g_task_core1_slave2_buf, g_task_core1_ch4_buf, g_task_core0_net_buf,
    g_telnet_task_buf;
StackType_t stackCore1Ch1[Config::Task::STACK_SIZE_CORE1],
    stackCore1Slave[Config::Task::STACK_SIZE_CORE1],
    stackCore1Slave2[Config::Task::STACK_SIZE_CORE1],
    stackCore1Ch4[Config::Task::STACK_SIZE_CH4],
    stackCore0Net[Config::Task::STACK_SIZE_CORE0],
    telnetTaskStack[Config::Task::STACK_SIZE_TELNET];
TaskHandle_t g_telnet_task_handle = nullptr, g_ch1_task_handle = nullptr,
             g_ch2_task_handle = nullptr, g_ch3_task_handle = nullptr,
             g_ch4_task_handle = nullptr, g_network_task_handle = nullptr;

uint32_t g_boot_start_ms = 0;
std::atomic<uint32_t> g_ch1_bus_ms{0};
SemaphoreHandle_t g_uart0_mutex = nullptr, g_uart1_mutex = nullptr,
                  g_uart2_mutex = nullptr, g_tracer_sem = nullptr;
Ch1StateMetrics g_ch1_state_metrics;
portMUX_TYPE g_config_mux = portMUX_INITIALIZER_UNLOCKED;
std::atomic<bool> g_config_dirty{false}, g_ota_in_progress{false},
    g_initial_caching_complete{false}, g_probe_convergence_reset{false};
WifiFallbackGuard g_wifi_guard;
Config::Doorphone::DoorphoneState g_doorphone_state{};
