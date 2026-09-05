local Driver = require "st.driver"
local capabilities = require "st.capabilities"
local command_handlers = require "command_handlers"
local gateway_client = require "gateway_client"
local log = require "log"

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
  log.info("Initializing ESP32 Gateway Device: " .. tostring(device.label))
  device:set_field("__state_cache", nil, { persist = true })
  device:try_update_metadata({ profile = "gateway-ultra" })
  if not device:get_field("ota_channel") then
    device:set_field("ota_channel", device.preferences.otaChannel or "main")
  end
  schedule_polling_timer(driver, device)
  command_handlers.refresh_telemetry(driver, device)
end

local function device_added(driver, device)
  log.info("ESP32 Gateway Device added to SmartThings")
  device:set_field("ota_channel", device.preferences.otaChannel or "main")
  command_handlers.refresh_telemetry(driver, device)
end

local function device_info_changed(driver, device, event, args)
  log.info("Device preferences updated")
  local old_prefs = (args and args.old_st_store and args.old_st_store.preferences) or {}
  local new_prefs = device.preferences or {}

  if old_prefs.pollingInterval ~= new_prefs.pollingInterval then
    schedule_polling_timer(driver, device)
  end

  local ip = new_prefs.gatewayIp or "172.30.1.3"
  local port = new_prefs.gatewayPort or 8900

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
  local ch1 = tonumber(new_prefs.ch1PollInterval)
  local ch2 = tonumber(new_prefs.ch2AckDelay)
  local ch3 = tonumber(new_prefs.ch3AckDelay)

  if (old_prefs.ch1PollInterval ~= new_prefs.ch1PollInterval) or
     (old_prefs.ch2AckDelay ~= new_prefs.ch2AckDelay) or
     (old_prefs.ch3AckDelay ~= new_prefs.ch3AckDelay) then
    log.info(string.format("⏱️ [TIMING] Applying new RS-485 timings: CH1=%sms, CH2=%sms, CH3=%sms",
                           tostring(ch1), tostring(ch2), tostring(ch3)))
    gateway_client.set_timing(ip, port, ch1, ch2, ch3)
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
end

local is_device_created = false
local function discovery_handler(driver, _, should_continue)
  if is_device_created then return end
  local DEVICE_NET_ID = "esp32_wallpad_gateway_ctrl"
  for _, device in ipairs(driver:get_devices()) do
    if device.device_network_id == DEVICE_NET_ID then
      is_device_created = true
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
  is_device_created = true
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
    }
  }
})

gateway_driver:run()
