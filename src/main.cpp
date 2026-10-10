// ── L1 HAL Drivers ──
#include "L1_HAL/Diagnostics_Driver.h"
#include "L1_HAL/OTA_Driver.h"
#include "L1_HAL/Wifi_Driver.h"

// ── L2 Transport Channels ──
#include "L2_Transport/RS485_CH.h"
#include "L2_Transport/TCP_CH.h"
#include "L2_Transport/Bridge_CH.h"

// ── L3 Protocol Routing ──
#include "L3_Protocol/Public/Protocol_Facade.h"

// ── L4 Network Services ──
#include "L4_Services/Mgmt_Service.h"
#include "L4_Services/CLI_Service.h"

#include "esp_idf_version.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"

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
  uint32_t deadline_ms;
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

  esp_err_t nvs_err = nvs_flash_init();
  if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    Serial.println(F("[BOOT] NVS truncated or corrupt. Auto-healing via nvs_flash_erase..."));
    nvs_flash_erase();
    nvs_flash_init();
  }

  Diagnostics_Init();
  Diag_DiagnoseStuck();
  Diag_CheckCoreDump();
  Diag_LogResetReason();

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
      System_SetRollbackDetected();
      const esp_partition_t *run_p = esp_ota_get_running_partition();
      Serial.printf("[BOOT] ★ AUTO-ROLLBACK ACTIVE: Rolled back from failed "
                    "'%s' to stable '%s'!\r\n",
                    next_p->label, run_p ? run_p->label : "app0");
    }
  }

  esp_reset_reason_t reset_reason = esp_reset_reason();
  const uint32_t crash_count = Diag_EvaluateCrashCounter(reset_reason);

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
      if (!Config_IsFrozen()) {
        Config_Load();
        Config_Freeze();
      }
      const auto &cfg = Config_Get();
      RescueHwConfig r_cfg{
          .reason = "Hardware Button Override",
          .sta_ssid = cfg.wifi_ssid,
          .sta_password = cfg.wifi_password,
      };
      Diag_StartRescueAp(r_cfg);
    }
  }

  if (!System_IsRescueMode() &&
      crash_count >= 3) {
    const esp_partition_t *run_p = esp_ota_get_running_partition();
    const esp_partition_t *next_p_check =
        esp_ota_get_next_update_partition(nullptr);

    if (run_p && next_p_check &&
        strcmp(run_p->label, next_p_check->label) != 0) {
      Serial.printf("[RESCUE] ★ Crash Loop detected (%u crashes)! Rolling back "
                    "from '%s' to '%s'...\r\n",
                    Diag_GetCrashCounter(), run_p->label, next_p_check->label);
      Diag_ResetCrashCounter();
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
                  Diag_GetCrashCounter());
    if (!Config_IsFrozen()) {
      Config_Load();
      Config_Freeze();
    }
    const auto &cfg = Config_Get();
    RescueHwConfig r_cfg{
        .reason = "Consecutive Crash Loop (>=3)",
        .sta_ssid = cfg.wifi_ssid,
        .sta_password = cfg.wifi_password,
    };
    Diag_StartRescueAp(r_cfg);
  }
}

// ============================================================================
// Stage 2: RTOS Synchronization Primitives & Static Queues
// ============================================================================
static void Boot_InitSyncPrimitives() {
  RS485_InitQueues();

  ch2_config.event_queue_ptr = RS485_GetUartEventQueuePtr(1);
  ch3_config.event_queue_ptr = RS485_GetUartEventQueuePtr(2);

  static StaticEventGroup_t s_system_event_group_buf;
  if (!g_system_event_group) {
    g_system_event_group = xEventGroupCreateStatic(&s_system_event_group_buf);
    xEventGroupSetBits(g_system_event_group, SYS_EVT_OTA_IDLE);
  }
}

// ============================================================================
// Stage 3 & 4: Configuration, State & Subsystem Registries
// ============================================================================
static void Boot_RestoreConfigAndState() {
  if (!Config_IsFrozen()) {
    Config_Load();
    TimingConfig_Load();
    Config_Freeze();
  }
  const auto &cfg = Config_Get();
  Serial.printf("[CONFIG] WiFi SSID: '%s', Timeout: %us, AP SSID: '%s'\r\n",
                cfg.wifi_ssid, cfg.wifi_connect_timeout_s,
                cfg.ap_ssid);
  Protocol_WarmCacheRestoreOnBoot();
  System_RegisterShutdownHook(ProtocolDiag_WarmCacheSaveToNvs);
  char dp_ns[16];
  ProtocolDiag_GetFramingNamespace(Config_GetWallpadProfile(), dp_ns, sizeof(dp_ns));
  Protocol_DoorphoneRestoreNvs(dp_ns);
}

static void Boot_InitSubsystems() {
  // ── Register L3 Protocol Dispatcher SPI into L2 RS-485 Engine ──
  RS485_PacketDispatcher rs485_dispatcher{};
  Protocol_BindDispatcher(rs485_dispatcher);
  RS485_RegisterDispatcher(rs485_dispatcher);

  // ── Register L3 Protocol Dispatcher SPI into L2 EW11 Bridge ──
  Bridge_PacketDispatcher bridge_dispatcher{};
  Protocol_BindBridgeDispatcher(bridge_dispatcher);
  Bridge_RegisterDispatcher(bridge_dispatcher);

  Protocol_DoorphoneInit();
  Remote_Init();
  Bridge_Init();

  // ── Register L2 Bridge Reactor Participant into L2 TCP Reactor ──
  Transport::ReactorParticipant bridge_part{};
  bridge_part.name = "BridgeService";
  bridge_part.populateFds = Bridge_PopulateFds;
  bridge_part.processEvents = Bridge_ProcessEvents;
  bridge_part.tick = Bridge_Tick;
  Transport::TcpReactor::registerParticipant(bridge_part);

  Mgmt_Init();
  System_RegisterShutdownHook(Bridge_ShutdownSockets);
  SystemOta_RegisterPreOtaHook(Bridge_ShutdownSockets);
}

// ============================================================================
// Stage 5: Hardware UART Buses & Peripheral Serial Setup
// All UART init delegated to Uart_Driver L1 HAL (AGENTS.md Rule 17).
// ============================================================================

static void Boot_InitHardwareAndDevices() {
  const auto &cfg = Config_Get();

  auto buildUartConfig = [](uint32_t baud, uint8_t data_bits, uint8_t parity,
                            uint8_t stop_bits) noexcept -> UartHwConfig {
    auto d = toEnum<DataBits>(data_bits, DataBits::Five, DataBits::Eight)
                 .value_or(DataBits::Eight);
    auto p = toEnum<Parity>(parity, Parity::None, Parity::Odd)
                 .value_or(Parity::None);
    auto s = toEnum<StopBits>(stop_bits, StopBits::One, StopBits::Two)
                 .value_or(StopBits::One);
    return makeUartConfig(baud, d, p, s);
  };

  if (esp_err_t err = Uart_InitHw(UART_NUM_0, 2, 1,
                                  buildUartConfig(cfg.uart_baud_rate, cfg.uart_data_bits,
                                                  cfg.uart_parity, cfg.uart_stop_bits),
                                  RS485_GetUartEventQueuePtr(0)); err != ESP_OK) {
    ESP_LOGE("BOOT", "Failed to init UART0 (CH1): %s", esp_err_to_name(err));
  }
  if (esp_err_t err = Uart_InitHw(UART_NUM_1, 6, 5,
                                  buildUartConfig(cfg.ch2_baud_rate, cfg.ch2_data_bits,
                                                  cfg.ch2_parity, cfg.ch2_stop_bits),
                                  RS485_GetUartEventQueuePtr(1)); err != ESP_OK) {
    ESP_LOGE("BOOT", "Failed to init UART1 (CH2): %s", esp_err_to_name(err));
  }
  if (esp_err_t err = Uart_InitHw(UART_NUM_2, 8, 7,
                                  buildUartConfig(cfg.ch3_baud_rate, cfg.ch3_data_bits,
                                                  cfg.ch3_parity, cfg.ch3_stop_bits),
                                  RS485_GetUartEventQueuePtr(2)); err != ESP_OK) {
    ESP_LOGE("BOOT", "Failed to init UART2 (CH3): %s", esp_err_to_name(err));
  }

  if (esp_err_t err = Uart_InitSwSerial(Config::GPIO::RX_GPIO, Config::GPIO::TX_GPIO,
                                        buildUartConfig(cfg.doorphone_baud_rate,
                                                        cfg.doorphone_data_bits,
                                                        cfg.doorphone_parity,
                                                        cfg.doorphone_stop_bits)); err != ESP_OK) {
    ESP_LOGE("BOOT", "Failed to init SW Serial (CH4): %s", esp_err_to_name(err));
  }
  Device_Init();
}

static void Boot_PreheatHotPaths() {
  // Flash XIP instruction cache pre-heating using isolated dummy vector (dev_id = 0xFE)
  // Dynamically constructed from active profile framing to ensure 100% vendor-agnostic pre-heating
  // without polluting production registries.
  constexpr uint8_t DUMMY_DEV_ID = 0xFE;
  DeviceStateEntry dummy_snap{};
  (void)Device_FindCopy(DUMMY_DEV_ID, 0x01, 0x00, dummy_snap);
  (void)Device_Exists(DUMMY_DEV_ID, 0x01, 0x00);

  ProfileInfoSnapshot p_snap{};
  uint8_t stx = 0xF7, etx = 0xEE, q_op = 0x40;
  if (ProtocolDiag_GetProfileInfo(Config_GetWallpadProfile(), p_snap) && p_snap.stx != 0) {
    stx = p_snap.stx;
    etx = p_snap.etx;
    if (p_snap.query_op != 0) {
      q_op = p_snap.query_op;
    }
  }

  StaticPacket dummy_pkt{};
  dummy_pkt.length = 11;
  dummy_pkt.data.fill(0);
  dummy_pkt.data[0] = stx;
  dummy_pkt.data[1] = 11;
  dummy_pkt.data[2] = 0x01;
  dummy_pkt.data[3] = DUMMY_DEV_ID;
  dummy_pkt.data[4] = 0x01;
  dummy_pkt.data[5] = q_op;
  dummy_pkt.data[10] = etx;

  uint8_t d_dev = 0, d_sub1 = 0, d_sub2 = 0;
  (void)ProtocolDiag_ExtractDeviceKey(dummy_pkt.data.data(), dummy_pkt.length, d_dev, d_sub1, d_sub2);
}

// ============================================================================
// Stage 6: Network Stack, SoftAP Fallback & ArduinoOTA Lifecycle
// ============================================================================
static void Boot_InitWifiAndOta() {
  if (!System_IsRescueMode()) {
    const auto &cfg = Config_Get();
    WifiHwConfig w_cfg{
        .sta_ssid = cfg.wifi_ssid,
        .sta_password = cfg.wifi_password,
        .timeout_s = cfg.wifi_connect_timeout_s,
        .ap_ssid = cfg.ap_ssid,
        .ap_password = cfg.ap_password,
    };
    Wifi_Driver_Init(w_cfg);
    SystemOta_InitArduinoOta("gateway-bridge", OTA_PASSWORD);
  }
}

// ============================================================================
// Stage 7: FreeRTOS Task Spawning & Watchdog Guard
// ============================================================================
static StaticTask_t s_task_core1_ch1_buf, s_task_core1_slave_buf,
    s_task_core1_slave2_buf, s_task_core1_ch4_buf, s_task_core0_net_buf,
    s_telnet_task_buf;
static StackType_t s_stackCore1Ch1[Config::Task::STACK_SIZE_CORE1],
    s_stackCore1Slave[Config::Task::STACK_SIZE_CH2],
    s_stackCore1Slave2[Config::Task::STACK_SIZE_CH3],
    s_stackCore1Ch4[Config::Task::STACK_SIZE_CH4],
    s_stackCore0Net[Config::Task::STACK_SIZE_CORE0],
    s_telnetTaskStack[Config::Task::STACK_SIZE_TELNET];

static const TaskSpawnDescriptor kTaskDescriptors[] = {
    {Task_Ch1, "CH#1_IoT", Config::Task::STACK_SIZE_CORE1, nullptr,
     TaskPriority::CH1_REALTIME, 1, s_stackCore1Ch1, &s_task_core1_ch1_buf,
     SystemTaskId::CH1, true, 1000},
    {Task_Ch2, "CH#2_WP#1", Config::Task::STACK_SIZE_CH2, &ch2_config,
     TaskPriority::WALLPAD_EMULATION, 1, s_stackCore1Slave,
     &s_task_core1_slave_buf, SystemTaskId::CH2, true, 1000},
    {Task_Ch3, "CH#3_WP#2", Config::Task::STACK_SIZE_CH3, &ch3_config,
     TaskPriority::WALLPAD_EMULATION, 1, s_stackCore1Slave2,
     &s_task_core1_slave2_buf, SystemTaskId::CH3, true, 1000},
    {Task_Ch4, "CH#4_WP#3", Config::Task::STACK_SIZE_CH4, nullptr,
     TaskPriority::CH4_SUBWALLPAD, 1, s_stackCore1Ch4, &s_task_core1_ch4_buf,
     SystemTaskId::CH4, true, 1000},
    {Transport::TcpReactor::runTask, "Network", Config::Task::STACK_SIZE_CORE0, nullptr,
     TaskPriority::NETWORK, 0, s_stackCore0Net, &s_task_core0_net_buf,
     SystemTaskId::NETWORK, false, 1500},
    {Task_Telnet, "Telnet_CLI", Config::Task::STACK_SIZE_TELNET, nullptr,
     TaskPriority::TELNET_CLI, 0, s_telnetTaskStack, &s_telnet_task_buf,
     SystemTaskId::TELNET, false, 3000},
};

static void Boot_StartTasks() {
  Diag_ResetTaskWdtAlive();

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

  bool rescue_active = System_IsRescueMode();

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
      Supervisor::TaskSpec spec{
          .name = desc.name,
          .deadline_ms = desc.deadline_ms,
          .core = desc.core_id,
      };
      Supervisor::registerTask(static_cast<uint8_t>(desc.task_id), h, spec);
    } else {
      Serial.printf("[FATAL] Failed to create static task '%s' on core %d!\r\n",
                    desc.name, static_cast<int>(desc.core_id));
      System_Restart("Fatal: Task Create Failed");
    }
  }

  Supervisor::start(Supervisor::Mode::Shadow);
}

// ============================================================================
// Firmware Main Setup & Loop Handoff
// ============================================================================
void setup() {
  Boot_CheckCrashLoop();
  Boot_InitSyncPrimitives();
  Boot_RestoreConfigAndState();
  Boot_InitSubsystems();
  Boot_InitHardwareAndDevices();
  Boot_PreheatHotPaths();
  Boot_StartTasks();
  Boot_InitWifiAndOta();

  Serial.println(F("[BOOT] All FreeRTOS tasks started successfully."));
  esp_task_wdt_delete(nullptr);
  if (g_system_event_group) {
    xEventGroupSetBits(g_system_event_group, SYS_EVT_SYSTEM_RUNNING);
  }
  vTaskDelete(nullptr);
}

void loop() {}