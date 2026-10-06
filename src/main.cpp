#include "L0_Foundation/System_Buffer.h"
#include "L0_Foundation/System_Config.h"
#include "L0_Foundation/System_Platform.h"
#include "L2_Transport/RS485_CH.h"
#include "L2_Transport/TCP_CH.h"
#include "L1_HAL/Uart_Driver.h"
#include "L3_Protocol/Public/Device_Registry.h"
#include "L3_Protocol/Public/Packet_Router.h"
#include "L3_Protocol/Public/Protocol_Diagnostics.h"
#include "L4_Services/Mgmt_Service.h"
#include "L4_Services/CLI_Service.h"
#include "L4_Services/EW11_Service.h"
#include "L1_HAL/Diagnostics_Driver.h"
#include "L1_HAL/OTA_Driver.h"

#include "esp_attr.h"
#include "esp_idf_version.h"
#include "esp_ota_ops.h"
#include "esp_sntp.h"
#include "esp_task_wdt.h"
#include "esp_wifi.h"
#include "lwip/ip.h"
#include "lwip/tcp.h"
#include <ArduinoOTA.h>
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
#include <Network.h>
#endif
#include <WiFi.h>

void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info);

// ============================================================================
// FreeRTOS Task Priorities & Deployment Descriptors
// ============================================================================
namespace TaskPriority {
constexpr UBaseType_t CH1_REALTIME =
    13; // Core 1: High-priority RS-485 framing & packet handling
constexpr UBaseType_t NETWORK =
    12; // Core 0: TCP sockets, HTTP server, Cosock Edge LAN
constexpr UBaseType_t CH4_SUBWALLPAD =
    11; // Core 1: Sub-Wallpad passthrough & bridging
constexpr UBaseType_t WALLPAD_EMULATION =
    10; // Core 1: CH#2 / CH#3 Slave Wallpad packet processing
constexpr UBaseType_t TELNET_CLI =
    10; // Core 0: Interactive ANSI Telnet management
} // namespace TaskPriority

struct TaskSpawnDescriptor {
  TaskFunction_t function;
  const char *name;
  uint32_t stack_size;
  void *param;
  UBaseType_t priority;
  BaseType_t core_id;
  StackType_t *stack_buf;
  StaticTask_t *tcb_buf;
  SystemTaskId task_id;
  bool bypass_in_rescue;
};

static WallpadChannelConfig ch2_config = {
    .uart_num = UART_NUM_1,
    .event_queue_ptr = nullptr,
    .channel_id = 2,
};

static WallpadChannelConfig ch3_config = {
    .uart_num = UART_NUM_2,
    .event_queue_ptr = nullptr,
    .channel_id = 3,
};

// ============================================================================
// Stage 1: Post-Mortem Diagnostics & Crash Loop Protection
// ============================================================================
static void Boot_CheckCrashLoop() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  Diag_SetBootTimeMs(millis());
  System_DiagnoseStuck();
  System_CheckCoreDump();
  System_LogResetReason();
  g_metrics.init();

  Serial.println(F("\r\n========================================"));
  Serial.printf("  GATEWAY BRIDGE %s BOOT INITIALIZATION\r\n",
                Config::FIRMWARE_VERSION);
  Serial.println(F("========================================"));
  if (const char *reason = Diag_GetPendingRebootReason()) {
    Serial.printf("[BOOT] Last Reset Reason: %s\r\n", reason);
  }

  const esp_partition_t *next_p = esp_ota_get_next_update_partition(nullptr);
  esp_ota_img_states_t next_state = ESP_OTA_IMG_UNDEFINED;
  if (next_p && esp_ota_get_state_partition(next_p, &next_state) == ESP_OK) {
    if (next_state == ESP_OTA_IMG_INVALID ||
        next_state == ESP_OTA_IMG_ABORTED) {
      g_rollback_detected = true;
      const esp_partition_t *run_p = esp_ota_get_running_partition();
      Serial.printf("[BOOT] ★ AUTO-ROLLBACK ACTIVE: Rolled back from failed "
                    "'%s' to stable '%s'!\r\n",
                    next_p->label, run_p ? run_p->label : "app0");
    }
  }

  esp_reset_reason_t reset_reason = esp_reset_reason();
  bool is_abnormal_crash =
      (reset_reason == ESP_RST_PANIC || reset_reason == ESP_RST_TASK_WDT ||
       reset_reason == ESP_RST_INT_WDT || reset_reason == ESP_RST_WDT ||
       reset_reason == ESP_RST_BROWNOUT);

  if (rtc_rescue_magic != RTC_MAGIC_RESCUE || !is_abnormal_crash) {
    rtc_rescue_magic = RTC_MAGIC_RESCUE;
    rtc_crash_counter = 0;
  } else {
    rtc_crash_counter++;
    Serial.printf("[BOOT] Consecutive crash count: %u (Reason: %d)\r\n",
                  rtc_crash_counter, reset_reason);
  }

  pinMode(Config::GPIO::BTN_PIN, INPUT_PULLUP);
  if (digitalRead(Config::GPIO::BTN_PIN) == LOW) {
    Serial.println(F("[BOOT] Front button pressed, checking 2.5s hold..."));
    uint32_t press_start = millis();
    bool held = true;
    while (!TimeUtils::isElapsed(press_start,
                                 Config::Timing::RESCUE_BUTTON_HOLD_MS)) {
      if (digitalRead(Config::GPIO::BTN_PIN) != LOW) {
        held = false;
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (held) {
      Serial.println(F("[SAFE BOOT] ★ Front Button Held (2.5s) -> Forcing "
                       "Rescue Safe Mode!"));
      System_EnterRescueMode("Hardware Button Override");
    }
  }

  if (!g_rescue_mode.load(std::memory_order_relaxed) &&
      rtc_crash_counter >= 3) {
    const esp_partition_t *run_p = esp_ota_get_running_partition();
    const esp_partition_t *next_p_check =
        esp_ota_get_next_update_partition(nullptr);

    if (run_p && next_p_check &&
        strcmp(run_p->label, next_p_check->label) != 0) {
      Serial.printf("[RESCUE] ★ Crash Loop detected (%u crashes)! Rolling back "
                    "from '%s' to '%s'...\r\n",
                    rtc_crash_counter, run_p->label, next_p_check->label);
      rtc_crash_counter = 0;
      esp_err_t err = esp_ota_set_boot_partition(next_p_check);
      if (err == ESP_OK) {
        Serial.println(F("[RESCUE] Boot partition switched successfully. "
                         "Rebooting into previous firmware..."));
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
      } else {
        Serial.printf("[RESCUE] esp_ota_set_boot_partition failed (err=0x%x). "
                      "Fallback to Rescue Safe Mode...\r\n",
                      err);
      }
    }

    Serial.printf("[RESCUE] ★ Crash Loop detected (%u crashes)! Forcing Rescue "
                  "Safe Mode...\r\n",
                  rtc_crash_counter);
    System_EnterRescueMode("Consecutive Crash Loop (>=3)");
  }
}

// ============================================================================
// Stage 2: RTOS Synchronization Primitives & Static Queues
// ============================================================================
static void Boot_InitSyncPrimitives() {
  RS485_InitQueues();

  ch2_config.event_queue_ptr = RS485_GetUartEventQueuePtr(1);
  ch3_config.event_queue_ptr = RS485_GetUartEventQueuePtr(2);

  if (!g_wifi_event_group)
    g_wifi_event_group = xEventGroupCreate();
  if (!g_system_event_group) {
    g_system_event_group = xEventGroupCreate();
    xEventGroupSetBits(g_system_event_group, SYS_EVT_OTA_IDLE);
  }
  WiFi.onEvent(onWifiEvent);
}

// ============================================================================
// Stage 3 & 4: Configuration, State & Subsystem Registries
// ============================================================================
static void Boot_RestoreConfigAndState() {
  Config_Load();
  Serial.printf("[CONFIG] WiFi SSID: '%s', Timeout: %us, AP SSID: '%s'\r\n",
                g_config.wifi_ssid, g_config.wifi_connect_timeout_s,
                g_config.ap_ssid);
  Protocol_WarmCacheRestoreOnBoot();
  System_RegisterShutdownHook(ProtocolDiag_WarmCacheSaveToNvs);
  char dp_ns[16];
  ProtocolDiag_GetFramingNamespace(g_config.wallpad_profile, dp_ns, sizeof(dp_ns));
  Protocol_DoorphoneRestoreNvs(dp_ns);
}

static bool HandleRemoteControl(StaticPacket &req,
                                StaticPacket &out_ack) noexcept {
  return Router_DispatchControl(req, out_ack);
}

static void Boot_InitSubsystems() {
  // ── Mediator: Wire L4 Services Decoupled Event Listeners ──
  Device_RegisterStateListener(Mgmt_BroadcastDeviceResult);
  Device_RegisterDoorphoneListener(Mgmt_BroadcastDoorphoneEvent);
  Router_RegisterCh5ForwardHandler(Bridge_ForwardPacket);

  Bridge_RegisterDeviceStateListener(Mgmt_BroadcastDeviceResult);
  Bridge_RegisterElevatorListener(Mgmt_BroadcastElevatorEvent);

  Remote_RegisterControlHandler(HandleRemoteControl);

  // ── Register L3 Protocol Dispatcher SPI into L2 RS-485 Engine ──
  RS485_PacketDispatcher rs485_dispatcher{};
  Protocol_BindDispatcher(rs485_dispatcher);
  RS485_RegisterDispatcher(rs485_dispatcher);

  Protocol_DoorphoneInit();
  Remote_Init();
  Bridge_Init();
  Mgmt_Init();
  System_RegisterShutdownHook(Bridge_ShutdownSockets);
  SystemOta_RegisterPreOtaHook(Bridge_ShutdownSockets);
}

// ============================================================================
// Stage 5: Hardware UART Buses & Peripheral Serial Setup
// All UART init delegated to Uart_Driver L1 HAL (AGENTS.md Rule 17).
// ============================================================================

static void Boot_InitHardwareAndDevices() {
  Uart_InitHw(UART_NUM_0, 2, 1,
              g_config.uart_baud_rate, g_config.uart_data_bits,
              g_config.uart_parity, g_config.uart_stop_bits,
              RS485_GetUartEventQueuePtr(0));
  Uart_InitHw(UART_NUM_1, 6, 5,
              g_config.ch2_baud_rate, g_config.ch2_data_bits,
              g_config.ch2_parity, g_config.ch2_stop_bits,
              RS485_GetUartEventQueuePtr(1));
  Uart_InitHw(UART_NUM_2, 8, 7,
              g_config.ch3_baud_rate, g_config.ch3_data_bits,
              g_config.ch3_parity, g_config.ch3_stop_bits,
              RS485_GetUartEventQueuePtr(2));

  Uart_InitDoorphone(g_config.doorphone_baud_rate,
                     g_config.doorphone_data_bits,
                     g_config.doorphone_parity,
                     g_config.doorphone_stop_bits,
                     Config::GPIO::RX_GPIO, Config::GPIO::TX_GPIO);
  Device_Init();
}

// ============================================================================
// Stage 6: Network Stack, SoftAP Fallback & ArduinoOTA Lifecycle
// ============================================================================
static void Boot_InitWifiAndOta() {
  if (!g_rescue_mode.load(std::memory_order_relaxed)) {
    Serial.printf("[WIFI] Connecting to '%s' (Timeout: %us)...\r\n",
                  g_config.wifi_ssid, g_config.wifi_connect_timeout_s);
    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    esp_wifi_set_ps(WIFI_PS_NONE);

    wifi_config_t w_conf;
    memset(&w_conf, 0, sizeof(w_conf));
    strncpy(reinterpret_cast<char *>(w_conf.sta.ssid), g_config.wifi_ssid,
            sizeof(w_conf.sta.ssid) - 1);
    strncpy(reinterpret_cast<char *>(w_conf.sta.password),
            g_config.wifi_password, sizeof(w_conf.sta.password) - 1);
    w_conf.sta.scan_method = WIFI_FAST_SCAN;
    w_conf.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    w_conf.sta.pmf_cfg.capable = true;
    w_conf.sta.pmf_cfg.required = false;
    w_conf.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_set_config(WIFI_IF_STA, &w_conf);
    esp_wifi_connect();

    uint32_t t_start = millis();
    uint32_t max_wait =
        (g_config.wifi_connect_timeout_s ? g_config.wifi_connect_timeout_s
                                         : 30) *
        1000;
    bool connected = false;

    while (millis() - t_start < max_wait) {
      if ((connected = (WiFi.status() == WL_CONNECTED)))
        break;
      vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (connected) {
      WiFi.setSleep(false);
      Serial.printf("[WIFI] Connected successfully! IP: %s, RSSI: %d dBm\r\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
      configTime(0, 0, "pool.ntp.org", "asia.pool.ntp.org");
      setenv("TZ", "KST-9", 1);
      tzset();
    } else {
      WiFi.mode(WIFI_AP_STA);
      vTaskDelay(pdMS_TO_TICKS(100));

      WiFi.softAPConfig(IPAddress(172, 30, 2, 1), IPAddress(172, 30, 2, 1),
                        IPAddress(255, 255, 255, 0));
      bool ap_ok = WiFi.softAP(g_config.ap_ssid, g_config.ap_password, 1, 0, 4);

      WiFi.setSleep(false);
      esp_wifi_set_max_tx_power(78);
      Serial.printf("[WIFI] STA connect failed. Fallback SoftAP '%s' started: "
                    "%s (IP: %s)\r\n",
                    g_config.ap_ssid, ap_ok ? "SUCCESS" : "FAILED",
                    WiFi.softAPIP().toString().c_str());
    }

    ArduinoOTA.setHostname("gateway-bridge");
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.onStart([]() {
      g_ota_in_progress.store(true, std::memory_order_release);
      if (g_system_event_group) {
        xEventGroupClearBits(g_system_event_group, SYS_EVT_OTA_IDLE);
      }
      ::Serial.println(F("[ArduinoOTA] Start transfer..."));
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
      esp_task_wdt_reset();
      g_wdt_monitor.feed(4);
    });
    ArduinoOTA.onEnd([]() {
      ::Serial.println(F("[ArduinoOTA] Finished successfully!"));
      System_Restart("OTA Firmware Update");
    });
    ArduinoOTA.onError([](ota_error_t error) {
      g_ota_in_progress.store(false, std::memory_order_release);
      if (g_system_event_group) {
        xEventGroupSetBits(g_system_event_group, SYS_EVT_OTA_IDLE);
      }
      ::Serial.printf("[ArduinoOTA] Error (%u)\r\n", (unsigned)error);
    });
    ArduinoOTA.begin();
  }
}

// ============================================================================
// Stage 7: FreeRTOS Task Spawning & Watchdog Guard
// ============================================================================
static StaticTask_t s_task_core1_ch1_buf, s_task_core1_slave_buf,
    s_task_core1_slave2_buf, s_task_core1_ch4_buf, s_task_core0_net_buf,
    s_telnet_task_buf;
static StackType_t s_stackCore1Ch1[Config::Task::STACK_SIZE_CORE1],
    s_stackCore1Slave[Config::Task::STACK_SIZE_SLAVE],
    s_stackCore1Slave2[Config::Task::STACK_SIZE_SLAVE],
    s_stackCore1Ch4[Config::Task::STACK_SIZE_CH4],
    s_stackCore0Net[Config::Task::STACK_SIZE_CORE0],
    s_telnetTaskStack[Config::Task::STACK_SIZE_TELNET];

static const TaskSpawnDescriptor kTaskDescriptors[] = {
    {Task_Ch1, "CH#1_IoT", Config::Task::STACK_SIZE_CORE1, nullptr,
     TaskPriority::CH1_REALTIME, 1, s_stackCore1Ch1, &s_task_core1_ch1_buf,
     SystemTaskId::CH1, true},
    {Task_Ch2, "CH#2_WP#1", Config::Task::STACK_SIZE_SLAVE, &ch2_config,
     TaskPriority::WALLPAD_EMULATION, 1, s_stackCore1Slave,
     &s_task_core1_slave_buf, SystemTaskId::CH2, true},
    {Task_Ch3, "CH#3_WP#2", Config::Task::STACK_SIZE_SLAVE, &ch3_config,
     TaskPriority::WALLPAD_EMULATION, 1, s_stackCore1Slave2,
     &s_task_core1_slave2_buf, SystemTaskId::CH3, true},
    {Task_Ch4, "CH#4_WP#3", Config::Task::STACK_SIZE_CH4, nullptr,
     TaskPriority::CH4_SUBWALLPAD, 1, s_stackCore1Ch4, &s_task_core1_ch4_buf,
     SystemTaskId::CH4, true},
    {Transport::TcpReactor::runTask, "Network", Config::Task::STACK_SIZE_CORE0, nullptr,
     TaskPriority::NETWORK, 0, s_stackCore0Net, &s_task_core0_net_buf,
     SystemTaskId::NETWORK, false},
    {Task_Telnet, "Telnet_CLI", Config::Task::STACK_SIZE_TELNET, nullptr,
     TaskPriority::TELNET_CLI, 0, s_telnetTaskStack, &s_telnet_task_buf,
     SystemTaskId::TELNET, false},
};

static void Boot_StartTasks() {
  uint32_t now = millis();
  for (size_t i = 0; i < 6; i++) {
    rtc_last_alive_ms[i] = now;
  }

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  esp_task_wdt_config_t twdt_config = {
      .timeout_ms = 30 * 1000,
      .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
      .trigger_panic = true,
  };
  esp_task_wdt_init(&twdt_config);
#else
  esp_task_wdt_init(30, true);
#endif

  bool rescue_active = g_rescue_mode.load(std::memory_order_relaxed);

  for (const auto &desc : kTaskDescriptors) {
    if (rescue_active && desc.bypass_in_rescue) {
      Serial.printf("[RESCUE] Task '%s' bypassed in safe mode.\r\n", desc.name);
      continue;
    }

    TaskHandle_t h = xTaskCreateStaticPinnedToCore(
        desc.function, desc.name, desc.stack_size, desc.param, desc.priority,
        desc.stack_buf, desc.tcb_buf, desc.core_id);

    if (h) {
      System_RegisterTaskHandle(desc.task_id, h);
    } else {
      Serial.printf("[FATAL] Failed to create static task '%s' on core %d!\r\n",
                    desc.name, static_cast<int>(desc.core_id));
    }
  }
}

// ============================================================================
// Firmware Main Setup & Loop Handoff
// ============================================================================
void setup() {
  Boot_CheckCrashLoop();
  Boot_InitSyncPrimitives();
  Boot_RestoreConfigAndState();
  Boot_InitHardwareAndDevices();
  Boot_InitWifiAndOta();
  Boot_InitSubsystems();
  Boot_StartTasks();

  Serial.println(F("[BOOT] All FreeRTOS tasks started successfully."));
  esp_task_wdt_delete(nullptr);
  if (g_system_event_group) {
    xEventGroupSetBits(g_system_event_group, SYS_EVT_SYSTEM_RUNNING);
  }
  vTaskDelete(nullptr);
}

void loop() {}