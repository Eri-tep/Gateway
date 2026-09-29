local Driver = require "st.driver"
local capabilities = require "st.capabilities"
local command_handlers = require "command_handlers"
local gateway_client = require "gateway_client"
local telemetry_handler = require "telemetry_handler"
local log = require "log"

local _orig_info = log.info
local _orig_debug = log.debug
local _orig_trace = log.trace
local _orig_warn = log.warn

local NOOP = function() end

local LOG_LEVEL_CONFIGS = {
  WARN = {
    trace = NOOP,
    debug = NOOP,
    info  = NOOP,
    warn  = _orig_warn,
  },
  ERROR = {
    trace = NOOP,
    debug = NOOP,
    info  = NOOP,
    warn  = NOOP,
  },
  DEBUG = {
    trace = _orig_trace,
    debug = _orig_debug,
    info  = _orig_info,
    warn  = _orig_warn,
  },
  INFO = {
    trace = NOOP,
    debug = NOOP,
    info  = _orig_info,
    warn  = _orig_warn,
  },
}

local function apply_log_level(level)
  local cfg = LOG_LEVEL_CONFIGS[level] or LOG_LEVEL_CONFIGS.INFO
  log.trace = cfg.trace
  log.debug = cfg.debug
  log.info  = cfg.info
  log.warn  = cfg.warn
end

local function schedule_polling_timer(driver, device)
  local interval = device.preferences.pollingInterval or 30
  if interval < 5 then interval = 5 end

  if device:get_field("poll_timer") then
    device.thread:cancel_timer(device:get_field("poll_timer"))
    device:set_field("poll_timer", nil)
  end

  local timer = device.thread:call_on_schedule(interval, function()
    command_handlers.refresh_telemetry(driver, device)
  end, "gateway_poll_timer")

  device:set_field("poll_timer", timer)
  log.info(string.format("Scheduled gateway telemetry polling timer every %d seconds", interval))
end

local EVENT_HANDLERS = {
  doorphone = telemetry_handler.handle_doorphone_event,
  device_state = telemetry_handler.handle_device_state_event,
  devices_updated = telemetry_handler.handle_devices_updated_event,
}

local function device_init(driver, device)
  log.info("Initializing Device: " .. tostring(device.label) .. " (Type: " .. tostring(device.type) .. ")")

  -- ★ 자식 기기 전체 분기 가드: p_key가 존재하면 절대 부모 영역으로 fallthrough되지 않음
  local p_key = device.parent_assigned_child_key or ""
  if p_key ~= "" then
    -- 1) 도어폰 초기화
    if p_key == "doorphone_front" or p_key == "doorphone_lobby" or p_key == "doorphone" then
      local cap_motion = capabilities.motionSensor
      local comp_main = device.profile.components["main"]
      if comp_main then
        device:emit_component_event(comp_main, capabilities.switch.switch.off())
        if cap_motion then
          device:emit_component_event(comp_main, cap_motion.motion.inactive())
        end
      end
      local comp_lobby = device.profile.components["lobby"]
      if comp_lobby then
        device:emit_component_event(comp_lobby, capabilities.switch.switch.off())
        if cap_motion then
          device:emit_component_event(comp_lobby, cap_motion.motion.inactive())
        end
      end
      return
    end

    -- 2) 동적 자식 기기 (조명/콘센트/난방/환기/가스/엘리베이터 등)
    if device:get_field("master_roll_timer") then
      device.thread:cancel_timer(device:get_field("master_roll_timer"))
      device:set_field("master_roll_timer", nil)
    end
    device:set_field("ticker_registry", nil)

    -- Outlet 초기화
    if device:supports_capability_by_id(capabilities.energyMeter.ID) then
      local cur_month = os.date("%Y-%m")
      local last_month = device:get_field("last_energy_month")
      local monthly_kwh = device:get_field("monthly_energy_kwh") or 0.0

      if last_month ~= cur_month then
        monthly_kwh = 0.0
        device:set_field("monthly_energy_kwh", 0.0, { persist = true })
        device:set_field("last_energy_month", cur_month, { persist = true })
      end

      local kwh_val = math.floor(monthly_kwh * 1000 + 0.5) / 1000
      device:emit_event(capabilities.energyMeter.energy({ value = kwh_val, unit = "kWh" }))
    end

    -- Thermostat 초기화 (신규 switch + heatingAway 및 기존 모드 호환)
    if p_key:match("^dev_28_") or device:supports_capability_by_id(capabilities.thermostatHeatingSetpoint.ID) then
      if device:supports_capability_by_id(capabilities.switch.ID) then
        local sw_ev = capabilities.switch.switch.off()
        sw_ev.state_change = true
        device:emit_event(sw_ev)
      end
      local cap_away = capabilities["digituniverse06711.heatingAway"]
      if cap_away and device:supports_capability_by_id(cap_away.ID) then
        local away_ev = cap_away.away("off")
        away_ev.state_change = true
        device:emit_event(away_ev)
      end
      if device:supports_capability_by_id(capabilities.thermostatMode.ID) then
        device:emit_event(capabilities.thermostatMode.supportedThermostatModes({ "heat", "away", "off" }))
      end
    end

    -- Air Conditioner 초기화
    if device:supports_capability_by_id(capabilities.airConditionerMode.ID) then
      device:emit_event(capabilities.airConditionerMode.supportedAcModes({ "cool", "dry", "wind", "auto", "heat" }))
    end

    -- FCU 자식 기기 초기화 (p_key = "dev_2c_<slot>_0" 패턴)
    if p_key:match("^dev_2c_") then
      local cap_sp = capabilities["digituniverse06711.fcuSetpoint"]
      if cap_sp and device:supports_capability_by_id("digituniverse06711.fcuSetpoint") then
        local saved_sp = device:get_field("last_fcu_setpoint") or 24
        local ev = cap_sp.setpoint({ value = saved_sp, unit = "°C" })
        ev.state_change = true
        device:emit_event(ev)
      end

      local cap_mode = capabilities["digituniverse06711.fcuMode"]
      if cap_mode and device:supports_capability_by_id("digituniverse06711.fcuMode") then
        local saved_mode = device:get_field("last_fcu_mode") or "cool"
        local ev = cap_mode.mode(saved_mode)
        ev.state_change = true
        device:emit_event(ev)
      end

      local cap_fan = capabilities["digituniverse06711.fcuFanSpeed"]
      if cap_fan and device:supports_capability_by_id("digituniverse06711.fcuFanSpeed") then
        local saved_fan = device:get_field("last_fcu_fan") or "auto"
        local ev = cap_fan.fanSpeed(saved_fan)
        ev.state_change = true
        device:emit_event(ev)
      end

      local cap_osc = capabilities["digituniverse06711.fcuOscillation"]
      if cap_osc and device:supports_capability_by_id("digituniverse06711.fcuOscillation") then
        local saved_osc = device:get_field("last_fcu_osc") or "fixed"
        local ev = cap_osc.oscillation(saved_osc)
        ev.state_change = true
        device:emit_event(ev)
      end

      local cap_info = capabilities["digituniverse06711.fcuInfo"]
      if cap_info and device:supports_capability_by_id("digituniverse06711.fcuInfo") then
        local saved_info = device:get_field("last_fcu_info") or "정상"
        local ev = cap_info.info(saved_info)
        ev.state_change = true
        device:emit_event(ev)
      end
    end

    -- Elevator 초기화
    local cap_hist = capabilities["digituniverse06711.history"]
    if cap_hist and p_key:match("^dev_34_") then
      local ev = cap_hist.history({ value = "대기 중" })
      ev.state_change = true
      device:emit_event(ev)
    end

    log.info(string.format("Child Device initialized successfully: %s", p_key))
    return
  end

  device:set_field("__state_cache", nil, { persist = true })
  device:try_update_metadata({ profile = "gateway-ultra" })
  if device.preferences and device.preferences.logLevel then
    apply_log_level(device.preferences.logLevel)
  end
  local init_ch = tostring(device.preferences and device.preferences.otaChannel or device:get_field("ota_channel") or "main"):lower()
  device:set_field("ota_channel", (init_ch == "beta") and "beta" or "main")

  -- deviceManager 초기 상태 idle 설정
  local comp_main = device.profile.components["main"]
  local cap_mgr = capabilities["digituniverse06711.deviceManager"]
  if cap_mgr and comp_main then
    device:emit_component_event(comp_main, cap_mgr.action({ value = "idle" }))
  end

  schedule_polling_timer(driver, device)
  -- 선행 refresh_telemetry 제거: 소켓이 연결되기 전에 호출하여 발생하는 "Gateway connection not ready" 에러 원천 차단

  -- CH6 실시간 푸시 이벤트 리스너 실행 (단 1회)
  if not device:get_field("listener_started") then
    device:set_field("listener_started", true)
    local ip = device.preferences.gatewayIp or "172.30.1.3"
    local port = tonumber(device.preferences.gatewayPort) or 8900
    gateway_client.start_event_listener(
      driver, ip, port,
      function(d, event_data)
        local handler = event_data and EVENT_HANDLERS[event_data.event]
        if handler then
          handler(d, event_data)
        end
      end,
      function(d)
        -- ★ TCP 소켓 연결 성공 즉시 1회 자동 텔레메트리 즉각 조회 (30초 공백 지연 0초로 단축)
        log.info("🚀 [AUTO-SYNC] Connected to gateway! Querying telemetry immediately...")
        command_handlers.refresh_telemetry(d, device)
      end
    )
  end
end

local function device_added(driver, device)
  local p_key = device.parent_assigned_child_key or ""
  if p_key ~= "" then
    log.info(string.format("Child Device added to SmartThings: %s", p_key))
    return
  end
  log.info("ESP32 Gateway Device added to SmartThings")
  local prefs = device.preferences or {}
  device:set_field("ota_channel", prefs.otaChannel or "main")
end

local function device_info_changed(driver, device, event, args)
  log.info("Device preferences updated")
  local p_key = device.parent_assigned_child_key or ""
  if p_key ~= "" then
    -- 자식 기기는 게이트웨이 환경설정(RPC/텔레메트리/UART) 대상이 아니므로 즉시 반환
    return
  end

  local old_prefs = (args and args.old_st_store and args.old_st_store.preferences) or {}
  local new_prefs = device.preferences or {}

  if old_prefs.pollingInterval ~= new_prefs.pollingInterval then
    schedule_polling_timer(driver, device)
  end

  local ip = new_prefs.gatewayIp or "172.30.1.3"
  local port = new_prefs.gatewayPort or 8900

  -- -1. Log Level change (Hide INFO/DEBUG)
  if new_prefs.logLevel then
    apply_log_level(new_prefs.logLevel)
  end

  -- 0. OTA Release Channel change
  if new_prefs.otaChannel then
    local ch = tostring(new_prefs.otaChannel):lower()
    local branch = (ch == "beta") and "beta" or "main"
    device:set_field("ota_channel", branch)
    log.info(string.format("⚙️ [Config] Synced OTA Release Channel: %s", branch))
  end

  -- 1. Wallpad Profile Slot change
  if old_prefs.wallpadProfileSlot ~= new_prefs.wallpadProfileSlot and new_prefs.wallpadProfileSlot then
    local slot = tonumber(new_prefs.wallpadProfileSlot) or 1
    log.info(string.format("🎛️  [PREF] Switching Wallpad Profile -> Slot %d", slot))
    gateway_client.set_profile(ip, port, slot)
  end

  -- 2. Wi-Fi Mode change
  if old_prefs.wifiOperationMode ~= new_prefs.wifiOperationMode and new_prefs.wifiOperationMode then
    local mode = new_prefs.wifiOperationMode
    log.info(string.format("📶 [PREF] Switching Wi-Fi Mode -> %s", mode))
    gateway_client.set_wifi_mode(ip, port, mode)
  end

  -- 3. Timing parameters
  local ch1_poll_intvl = tonumber(new_prefs.ch1PollInterval)
  local ch2_delay = tonumber(new_prefs.ch2AckDelay)
  local ch3_delay = tonumber(new_prefs.ch3AckDelay)

  if (old_prefs.ch1PollInterval ~= new_prefs.ch1PollInterval) or
     (old_prefs.ch2AckDelay ~= new_prefs.ch2AckDelay) or
     (old_prefs.ch3AckDelay ~= new_prefs.ch3AckDelay) then
    log.info(string.format("⏱️ [TIMING] Applying new RS-485 timings: CH1_Poll=%sms, CH2_Delay=%sms, CH3_Delay=%sms",
                           tostring(ch1_poll_intvl), tostring(ch2_delay), tostring(ch3_delay)))
    gateway_client.set_timing(ip, port, ch1_poll_intvl, ch2_delay, ch3_delay)
  end

  -- 4. Wi-Fi Credentials (2-Step Safe Commit: Requires explicit apply toggle)
  if new_prefs.applyWifiConfig and not old_prefs.applyWifiConfig then
    local target_ssid = new_prefs.newWifiSsid
    local target_pass = new_prefs.newWifiPassword or ""

    if target_ssid and target_ssid ~= "" then
      log.info(string.format("📶 [WIFI] Explicit Apply Triggered! Setting Wi-Fi -> SSID: '%s'", target_ssid))
      gateway_client.set_wifi(ip, port, target_ssid, target_pass)
    else
      log.warn("⚠️ [WIFI] Apply toggle turned ON, but Target SSID is empty! Aborting.")
    end
  end

  -- 5. RS-485 Serial UART Settings (CH1 ~ CH4)
  for i = 1, 4 do
    local baud_key = string.format("ch%dBaudrate", i)
    local frame_key = string.format("ch%dFraming", i)

    local new_baud = tonumber(new_prefs[baud_key])
    local new_frame = new_prefs[frame_key]
    local old_baud = tonumber(old_prefs[baud_key])
    local old_frame = old_prefs[frame_key]

    if (new_baud and old_baud ~= new_baud) or (new_frame and old_frame ~= new_frame) then
      local default_baud = (i == 4) and 3860 or 9600
      local default_frame = (i == 4) and "8E1" or "8N1"
      local baud_val = new_baud or default_baud
      local frame_val = new_frame or default_frame
      log.info(string.format("🔌 [UART] Applying CH%d Serial Config -> Baud: %d, Framing: %s", i, baud_val, frame_val))
      gateway_client.set_uart_config(ip, port, i, baud_val, frame_val)
    end
  end

  command_handlers.refresh_telemetry(driver, device)
end

local function device_removed(driver, device)
  local p_key = device.parent_assigned_child_key or ""
  if p_key ~= "" then
    log.info(string.format("Child Device removed: %s", p_key))
    if device:get_field("master_roll_timer") then
      device.thread:cancel_timer(device:get_field("master_roll_timer"))
      device:set_field("master_roll_timer", nil)
    end
    return
  end

  log.info("Parent Gateway Device removed, canceling timers and closing socket")
  if device:get_field("poll_timer") then
    device.thread:cancel_timer(device:get_field("poll_timer"))
    device:set_field("poll_timer", nil)
  end
  if device:get_field("master_roll_timer") then
    device.thread:cancel_timer(device:get_field("master_roll_timer"))
    device:set_field("master_roll_timer", nil)
  end

  local sock = device:get_field("tcp_client")
  if sock then
    pcall(function() sock:close() end)
    device:set_field("tcp_client", nil)
  end
end

local function discovery_handler(driver, _, should_continue)
  local DEVICE_NET_ID = "esp32_wallpad_gateway_ctrl"
  for _, device in ipairs(driver:get_devices()) do
    if device.device_network_id == DEVICE_NET_ID then
      return
    end
  end

  log.info("ESP32 월패드 게이트웨이 기기 생성 (LAN Discovery)")
  driver:try_create_device({
    type = "LAN",
    device_network_id = DEVICE_NET_ID,
    label = "Gateway",
    profile = "gateway-ultra",
    manufacturer = "DIY",
    model = "ESP32-S3 Wallpad Gateway"
  })
end

local gateway_driver = Driver("esp32-wallpad-gateway", {
  discovery = discovery_handler,
  lifecycle_handlers = {
    init = device_init,
    added = device_added,
    infoChanged = device_info_changed,
    removed = device_removed
  },
  capability_handlers = {
    [capabilities.refresh.ID] = {
      [capabilities.refresh.commands.refresh.NAME] = command_handlers.handle_refresh
    },
    [capabilities.switch.ID] = {
      [capabilities.switch.commands.on.NAME] = command_handlers.handle_switch_on,
      [capabilities.switch.commands.off.NAME] = command_handlers.handle_switch_off
    },
    ["digituniverse06711.deviceManager"] = {
      ["setAction"] = command_handlers.handle_child_device_action
    },
    ["digituniverse06711.heatingAway"] = {
      ["setAway"] = command_handlers.handle_child_set_heating_away,
      ["toggle"] = command_handlers.handle_child_set_heating_away,
      ["on"] = command_handlers.handle_child_set_heating_away,
      ["off"] = command_handlers.handle_child_set_heating_away
    },
    [capabilities.momentary.ID] = {
      [capabilities.momentary.commands.push.NAME] = command_handlers.handle_momentary_push
    },
    [capabilities.thermostatHeatingSetpoint.ID] = {
      [capabilities.thermostatHeatingSetpoint.commands.setHeatingSetpoint.NAME] = command_handlers.handle_child_set_heating_setpoint
    },
    [capabilities.thermostatCoolingSetpoint.ID] = {
      [capabilities.thermostatCoolingSetpoint.commands.setCoolingSetpoint.NAME] = command_handlers.handle_child_set_cooling_setpoint
    },
    [capabilities.thermostatMode.ID] = {
      [capabilities.thermostatMode.commands.setThermostatMode.NAME] = command_handlers.handle_child_set_thermostat_mode
    },
    [capabilities.airConditionerMode.ID] = {
      [capabilities.airConditionerMode.commands.setAirConditionerMode.NAME] = command_handlers.handle_child_set_aircon_mode
    },
    [capabilities.fanSpeed.ID] = {
      [capabilities.fanSpeed.commands.setFanSpeed.NAME] = command_handlers.handle_child_set_fan_speed
    },
    [capabilities.airConditionerFanMode.ID] = {
      [capabilities.airConditionerFanMode.commands.setFanMode.NAME] = command_handlers.handle_child_set_ac_fan_mode
    },
    ["digituniverse06711.ventmode"] = {
      ["setVentMode"] = command_handlers.handle_child_set_vent_mode
    },
    ["digituniverse06711.ventspeed"] = {
      ["setVentSpeed"] = command_handlers.handle_child_set_vent_speed
    },
    ["digituniverse06711.fcuSetpoint"] = {
      ["setSetpoint"] = command_handlers.handle_fcu_set_setpoint
    },
    ["digituniverse06711.fcuMode"] = {
      ["setMode"] = command_handlers.handle_fcu_set_mode
    },
    ["digituniverse06711.fcuFanSpeed"] = {
      ["setFanSpeed"] = command_handlers.handle_fcu_set_fan_speed
    },
    ["digituniverse06711.fcuOscillation"] = {
      ["setOscillation"] = command_handlers.handle_fcu_set_oscillation
    },
    [capabilities.valve.ID] = {
      [capabilities.valve.commands.close.NAME] = command_handlers.handle_child_valve_close
    },
    [capabilities.fanOscillationMode.ID] = {
      [capabilities.fanOscillationMode.commands.setFanOscillationMode.NAME] = command_handlers.handle_child_set_oscillation_mode
    }
  }
})

gateway_driver:run()
