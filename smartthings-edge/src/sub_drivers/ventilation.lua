-- ============================================================================
-- Ventilation SubDriver (Dev ID: 0x2B, 1 Device)
-- ============================================================================
-- Manages Heat Recovery Ventilator (ERV/HRV), 3 fan speeds, and Bypass/Heat Exchange modes.
-- Protocol:
-- - Control: F7 0D 01 2B 02 42 11 00 [power] [speed] [mode] [CS] EE (13 bytes)
-- - ACK:     F7 0D 01 2B 04 40 11 00 [power] [speed] [mode] [CS] EE (13 bytes)
-- ============================================================================

local capabilities = require "st.capabilities"
local log = require "log"
local PacketBuilder = require "packet_builder"
local Connection = require "connection"

local VentilationSubDriver = {}

--- SubDriver selector predicate.
function VentilationSubDriver.can_handle(opts, driver, device, ...)
  return device:get_field("device_type") == "ventilation"
end

--- Resolve parent bridge device.
local function get_parent(device)
  return device:get_parent_device()
end

--- Send ventilation control command.
local function send_ventilation_control(device, is_on, speed, is_bypass)
  local parent = get_parent(device)
  if not parent then
    log.error(string.format("Ventilation [%s]: Missing parent bridge", device.id))
    return
  end

  local pkt = PacketBuilder.ventilation_cmd(is_on, speed, is_bypass)
  local ok, err = Connection.send_wallpad(parent, pkt)
  if not ok then
    log.error(string.format("Ventilation [%s] TX failed: %s", device.id, tostring(err)))
  end
end

--- Handle switch ON command.
local function handle_switch_on(driver, device, command)
  local speed = device:get_field("fan_speed") or 1
  local is_bypass = device:get_field("is_bypass") or false
  send_ventilation_control(device, true, speed, is_bypass)
  device:emit_event(capabilities.switch.switch.on())
  device:emit_event(capabilities.fanSpeed.fanSpeed(speed))
end

--- Handle switch OFF command.
local function handle_switch_off(driver, device, command)
  local speed = device:get_field("fan_speed") or 1
  local is_bypass = device:get_field("is_bypass") or false
  send_ventilation_control(device, false, speed, is_bypass)
  device:emit_event(capabilities.switch.switch.off())
  device:emit_event(capabilities.fanSpeed.fanSpeed(0))
end

--- Handle setFanSpeed command.
local function handle_set_fan_speed(driver, device, command)
  local speed = command.args.speed
  local is_bypass = device:get_field("is_bypass") or false

  if speed == 0 then
    send_ventilation_control(device, false, 1, is_bypass)
    device:emit_event(capabilities.switch.switch.off())
    device:emit_event(capabilities.fanSpeed.fanSpeed(0))
  else
    speed = math.max(1, math.min(3, speed))
    device:set_field("fan_speed", speed)
    send_ventilation_control(device, true, speed, is_bypass)
    device:emit_event(capabilities.switch.switch.on())
    device:emit_event(capabilities.fanSpeed.fanSpeed(speed))
  end
end

--- Handle setThermostatMode command (Mode selection: heat=Heat Exchange, fanonly=Bypass).
local function handle_set_thermostat_mode(driver, device, command)
  local mode_str = command.args.mode
  local speed = device:get_field("fan_speed") or 1
  local is_on = device:get_latest_state("main", capabilities.switch.ID, capabilities.switch.switch.NAME) == "on"

  if mode_str == "fanonly" then
    device:set_field("is_bypass", true)
    send_ventilation_control(device, is_on, speed, true)
    device:emit_event(capabilities.thermostatMode.thermostatMode.fanonly())
  else
    device:set_field("is_bypass", false)
    send_ventilation_control(device, is_on, speed, false)
    device:emit_event(capabilities.thermostatMode.thermostatMode.heat())
  end
end

--- Handle refresh command.
local function handle_refresh(driver, device, command)
  log.debug(string.format("Ventilation [%s] refresh requested", device.id))
end

--- Handle inbound RS-485 ACK frame for ventilation (0x2B).
-- ACK format: F7 0D 01 2B 04 40 11 00 [Power: 0x01=ON, 0x02=OFF] [Speed: 1~3] [Mode: 0xFF=Heat, 0x01=Bypass] [CS] EE
-- frame.data contains: { 0x00, power, speed, mode }
function VentilationSubDriver.handle_rx_frame(driver, device, frame)
  if not frame.data or #frame.data < 4 then
    return
  end

  local power_byte = frame.data[2]
  local speed_byte = frame.data[3]
  local mode_byte = frame.data[4]

  local is_on = (power_byte == 0x01)
  local is_bypass = (mode_byte == 0x01)

  device:set_field("fan_speed", speed_byte)
  device:set_field("is_bypass", is_bypass)

  if is_on then
    device:emit_event(capabilities.switch.switch.on())
    device:emit_event(capabilities.fanSpeed.fanSpeed(speed_byte))
  else
    device:emit_event(capabilities.switch.switch.off())
    device:emit_event(capabilities.fanSpeed.fanSpeed(0))
  end

  if is_bypass then
    device:emit_event(capabilities.thermostatMode.thermostatMode.fanonly())
  else
    device:emit_event(capabilities.thermostatMode.thermostatMode.heat())
  end
end

VentilationSubDriver.NAME = "VentilationSubDriver"
VentilationSubDriver.capability_handlers = {
  [capabilities.switch.ID] = {
    [capabilities.switch.commands.on.NAME] = handle_switch_on,
    [capabilities.switch.commands.off.NAME] = handle_switch_off,
  },
  [capabilities.fanSpeed.ID] = {
    [capabilities.fanSpeed.commands.setFanSpeed.NAME] = handle_set_fan_speed,
  },
  [capabilities.thermostatMode.ID] = {
    [capabilities.thermostatMode.commands.setThermostatMode.NAME] = handle_set_thermostat_mode,
  },
  [capabilities.refresh.ID] = {
    [capabilities.refresh.commands.refresh.NAME] = handle_refresh,
  }
}

return VentilationSubDriver
