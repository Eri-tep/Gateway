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

local function apply_log_level(level)
  level = level or "INFO"
  if level == "WARN" then
    log.trace = function() end
    log.debug = function() end
    log.info = function() end
    log.warn = _orig_warn
  elseif level == "ERROR" then
    log.trace = function() end
    log.debug = function() end
    log.info = function() end
    log.warn = function() end
  elseif level == "DEBUG" then
    log.trace = _orig_trace
    log.debug = _orig_debug
    log.info = _orig_info
    log.warn = _orig_warn
  else -- "INFO" (기본값: trace/debug 숨김, info/warn/error 출력)
    log.trace = function() end
    log.debug = function() end
    log.info = _orig_info
    log.warn = _orig_warn
  end
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

local function device_init(driver, device)
  log.info("Initializing Device: " .. tostring(device.label) .. " (Type: " .. tostring(device.type) .. ")")

  -- 자식 기기(도어폰 - 세대 도어 / 로비 도어 / 레거시)인 경우 초기 상태 설정
  local p_key = device.parent_assigned_child_key or ""
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

  -- ★ 동적 자식 기기 (LOCKED 조명/콘센트/난방/환기/가스/엘리베이터)인 경우 초기 상태 설정
  if p_key:match("^dev_") then
    -- 자식 기기에 혹시 남아있을 수 있는 롤링 타이머/레지스트리 완전 제거
    if device:get_field("master_roll_timer") then
      device.thread:cancel_timer(device:get_field("master_roll_timer"))
      device:set_field("master_roll_timer", nil)
    end
    device:set_field("ticker_registry", nil)

    -- 1. Outlet 초기화 (월초 리셋 검사 & 누적 전력량 복원)
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

    -- 2. Thermostat 초기화 (지원 모드 메타데이터만 등록, 기존 상태는 보존)
    if device:supports_capability_by_id(capabilities.thermostatMode.ID) then
      device:emit_event(capabilities.thermostatMode.supportedThermostatModes({ "heat", "away", "off" }))
    end

    -- 3. Elevator 초기화 (상태 이력: 대기 중)
    local cap_hist = capabilities["digituniverse06711.history"]
    if cap_hist and p_key:match("^dev_34_") then
      local ev = cap_hist.history({ value = "대기 중" })
      ev.state_change = true
      device:emit_event(ev)
    end

    return
  end

  device:set_field("__state_cache", nil, { persist = true })
  device:try_update_metadata({ profile = "gateway-ultra" })
  if device.preferences and device.preferences.logLevel then
    apply_log_level(device.preferences.logLevel)
  end
  if not device:get_field("ota_channel") then
    device:set_field("ota_channel", device.preferences.otaChannel or "main")
  end

  -- deviceManager 초기 상태 idle 설정
  local comp_main = device.profile.components["main"]
  local cap_mgr = capabilities["digituniverse06711.deviceManager"]
  if cap_mgr and comp_main then
    device:emit_component_event(comp_main, cap_mgr.action({ value = "idle" }))
  end

  schedule_polling_timer(driver, device)
  command_handlers.refresh_telemetry(driver, device)

  -- CH6 실시간 푸시 이벤트 리스너 실행 (단 1회)
  if not device:get_field("listener_started") then
    device:set_field("listener_started", true)
    local ip = device.preferences.gatewayIp or "172.30.1.3"
    local port = tonumber(device.preferences.gatewayPort) or 8900
    gateway_client.start_event_listener(driver, ip, port, function(d, event_data)
      if event_data.event == "doorphone" then
        telemetry_handler.handle_doorphone_event(d, event_data)
      elseif event_data.event == "device_state" then
        telemetry_handler.handle_device_state_event(d, event_data)
      elseif event_data.event == "devices_updated" then
        telemetry_handler.handle_devices_updated_event(d, event_data)
      end
    end)
  end
end

local function device_added(driver, device)
  log.info("ESP32 Gateway Device added to SmartThings")
  device:set_field("ota_channel", device.preferences.otaChannel or "main")
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
    device:set_field("ota_channel", new_prefs.otaChannel)
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
  log.info("Device removed, canceling timers")
  if device:get_field("poll_timer") then
    device.thread:cancel_timer(device:get_field("poll_timer"))
    device:set_field("poll_timer", nil)
  end
  if device:get_field("master_roll_timer") then
    device.thread:cancel_timer(device:get_field("master_roll_timer"))
    device:set_field("master_roll_timer", nil)
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
    [capabilities.momentary.ID] = {
      [capabilities.momentary.commands.push.NAME] = command_handlers.handle_momentary_push
    },
    [capabilities.thermostatHeatingSetpoint.ID] = {
      [capabilities.thermostatHeatingSetpoint.commands.setHeatingSetpoint.NAME] = command_handlers.handle_child_set_heating_setpoint
    },
    [capabilities.thermostatMode.ID] = {
      [capabilities.thermostatMode.commands.setThermostatMode.NAME] = command_handlers.handle_child_set_thermostat_mode
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
    [capabilities.valve.ID] = {
      [capabilities.valve.commands.close.NAME] = command_handlers.handle_child_valve_close
    }
  }
})

gateway_driver:run()
