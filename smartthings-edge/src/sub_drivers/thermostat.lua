-- ============================================================================
-- Thermostat SubDriver (Dev ID: 0x18, 4 Devices)
-- ============================================================================
-- Manages floor heating, temperature setpoints, and heating/away modes for
-- Living Room, Bedroom, Office, and Relax Room.
-- Protocol verified via live packet capture:
-- - Control: F7 0B 01 18 02 46 [sub2] [mode] [setTemp] [CS] EE (11 bytes)
-- - ACK:     F7 0D 01 18 04 46 [sub2] [mode] [setTemp] [curTemp] [??] [CS] EE (13 bytes)
-- Modes: 0x04 = Heat, 0x07 = Away
-- ============================================================================

local capabilities = require "st.capabilities"
local log = require "log"
local PacketBuilder = require "packet_builder"
local Connection = require "connection"

local ThermostatSubDriver = {}

local MODE_HEAT = 0x04
local MODE_AWAY = 0x07

--- SubDriver selector predicate.
function ThermostatSubDriver.can_handle(opts, driver, device, ...)
  return device:get_field("device_type") == "thermostat"
end

--- Resolve parent bridge device.
local function get_parent(device)
  return device:get_parent_device()
end

--- Send thermostat control command.
local function send_thermostat_control(device, mode, target_temp)
  local sub2 = device:get_field("sub2")
  local parent = get_parent(device)
  if not parent or not sub2 then
    log.error(string.format("Thermostat [%s]: Missing parent or sub2", device.id))
    return
  end

  local pkt = PacketBuilder.thermostat_cmd(sub2, mode, target_temp)
  local ok, err = Connection.send_wallpad(parent, pkt)
  if not ok then
    log.error(string.format("Thermostat [%s] TX failed: %s", device.id, tostring(err)))
  end
end

--- Handle switch ON command (Turns on Heating mode).
local function handle_switch_on(driver, device, command)
  local target_temp = device:get_field("target_temp") or 22
  send_thermostat_control(device, MODE_HEAT, target_temp)
  device:emit_event(capabilities.switch.switch.on())
  device:emit_event(capabilities.thermostatMode.thermostatMode.heat())
end

--- Handle switch OFF command (Turns into Away or Off mode).
local function handle_switch_off(driver, device, command)
  send_thermostat_control(device, MODE_AWAY, 0x00)
  device:emit_event(capabilities.switch.switch.off())
  device:emit_event(capabilities.thermostatMode.thermostatMode.away())
end

--- Handle setHeatingSetpoint command.
local function handle_set_heating_setpoint(driver, device, command)
  local setpoint = command.args.setpoint
  if not setpoint then return end

  local target_temp = math.floor(setpoint + 0.5)
  device:set_field("target_temp", target_temp)

  local current_mode = device:get_field("current_mode") or MODE_HEAT
  send_thermostat_control(device, current_mode, target_temp)
  device:emit_event(capabilities.thermostatHeatingSetpoint.heatingSetpoint({ value = target_temp, unit = "C" }))
end

--- Handle setThermostatMode command.
local function handle_set_thermostat_mode(driver, device, command)
  local mode_str = command.args.mode
  local target_temp = device:get_field("target_temp") or 22

  if mode_str == "away" then
    send_thermostat_control(device, MODE_AWAY, 0x00)
    device:emit_event(capabilities.thermostatMode.thermostatMode.away())
  elseif mode_str == "heat" then
    send_thermostat_control(device, MODE_HEAT, target_temp)
    device:emit_event(capabilities.switch.switch.on())
    device:emit_event(capabilities.thermostatMode.thermostatMode.heat())
  elseif mode_str == "off" then
    send_thermostat_control(device, MODE_AWAY, 0x00)
    device:emit_event(capabilities.switch.switch.off())
    device:emit_event(capabilities.thermostatMode.thermostatMode.away())
  end
end

--- Handle refresh command.
local function handle_refresh(driver, device, command)
  log.debug(string.format("Thermostat [%s] refresh requested", device.id))
end

--- Handle inbound RS-485 ACK frame for thermostats (0x18).
-- ACK format: F7 0D 01 18 04 46 [sub2] [mode] [setTemp] [curTemp] [??] [CS] EE
-- frame.data contains: { mode, setTemp, curTemp, ... }
function ThermostatSubDriver.handle_rx_frame(driver, device, frame)
  if not frame.data or #frame.data < 3 then
    return
  end

  local mode_byte = frame.data[1]
  local set_temp = frame.data[2]
  local cur_temp = frame.data[3]

  device:set_field("current_mode", mode_byte)
  device:set_field("target_temp", set_temp)

  -- Emit mode and switch state
  if mode_byte == MODE_HEAT then
    device:emit_event(capabilities.switch.switch.on())
    device:emit_event(capabilities.thermostatMode.thermostatMode.heat())
  elseif mode_byte == MODE_AWAY then
    device:emit_event(capabilities.switch.switch.off())
    device:emit_event(capabilities.thermostatMode.thermostatMode.away())
  else
    device:emit_event(capabilities.switch.switch.off())
  end

  -- Emit temperatures
  if set_temp and set_temp > 0 and set_temp < 50 then
    device:emit_event(capabilities.thermostatHeatingSetpoint.heatingSetpoint({ value = set_temp, unit = "C" }))
  end

  if cur_temp and cur_temp > 0 and cur_temp < 60 then
    device:emit_event(capabilities.temperatureMeasurement.temperature({ value = cur_temp, unit = "C" }))
  end
end

ThermostatSubDriver.NAME = "ThermostatSubDriver"
ThermostatSubDriver.capability_handlers = {
  [capabilities.switch.ID] = {
    [capabilities.switch.commands.on.NAME] = handle_switch_on,
    [capabilities.switch.commands.off.NAME] = handle_switch_off,
  },
  [capabilities.thermostatHeatingSetpoint.ID] = {
    [capabilities.thermostatHeatingSetpoint.commands.setHeatingSetpoint.NAME] = handle_set_heating_setpoint,
  },
  [capabilities.thermostatMode.ID] = {
    [capabilities.thermostatMode.commands.setThermostatMode.NAME] = handle_set_thermostat_mode,
  },
  [capabilities.refresh.ID] = {
    [capabilities.refresh.commands.refresh.NAME] = handle_refresh,
  }
}

return ThermostatSubDriver
