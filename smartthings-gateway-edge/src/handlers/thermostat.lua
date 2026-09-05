local capabilities = require "st.capabilities"
local gateway = require "gateway"
local log = require "log"

local thermostat_handler = {}

local function extract_id(device)
  local parts = {}
  for match in (device.device_network_id..":"):gmatch("(.-):") do
    table.insert(parts, match)
  end
  return parts[#parts]
end

function thermostat_handler.on(driver, device, command)
  local id = extract_id(device)
  log.info(string.format("[THERMOSTAT] [TX] Device '%s' (ID: %s) -> Sending Power ON", tostring(device.label), tostring(id)))
  gateway.send({id = id, cmd = "set_power", value = "on"})
end

function thermostat_handler.off(driver, device, command)
  local id = extract_id(device)
  log.info(string.format("[THERMOSTAT] [TX] Device '%s' (ID: %s) -> Sending Power OFF", tostring(device.label), tostring(id)))
  gateway.send({id = id, cmd = "set_power", value = "off"})
end

function thermostat_handler.set_mode(driver, device, command)
  local mode = command.args.mode
  local id = extract_id(device)
  log.info(string.format("[THERMOSTAT] [TX] Device '%s' (ID: %s) -> Set Mode to '%s'", tostring(device.label), tostring(id), tostring(mode)))
  if mode == "off" then
    gateway.send({id = id, cmd = "set_power", value = "off"})
  else
    gateway.send({id = id, cmd = "set_power", value = "on"})
    gateway.send({id = id, cmd = "set_mode", value = mode}) -- 'heat' or 'eco'
  end
end

function thermostat_handler.set_heating_setpoint(driver, device, command)
  local id = extract_id(device)
  local temp = command.args.setpoint
  log.info(string.format("[THERMOSTAT] [TX] Device '%s' (ID: %s) -> Set Heating Setpoint to %s C", tostring(device.label), tostring(id), tostring(temp)))
  gateway.send({id = id, cmd = "set_temp", value = temp})
end

function thermostat_handler.handle_state(device, state)
  log.info(string.format("[THERMOSTAT] [RX] Device '%s' state received: Power='%s', Mode='%s', CurTemp=%s, SetTemp=%s",
    tostring(device.label), tostring(state.power), tostring(state.mode), tostring(state.cur_temp), tostring(state.set_temp)))

  if state.power then
    if state.power == "on" then
      device:emit_event(capabilities.switch.switch.on())
    else
      device:emit_event(capabilities.switch.switch.off())
      device:emit_event(capabilities.thermostatMode.thermostatMode.off())
    end
  end
  
  -- Emit supported modes to restrict UI to valid modes
  device:emit_event(capabilities.thermostatMode.supportedThermostatModes({"off", "heat", "eco"}))

  if state.mode and state.power == "on" then
    if state.mode == "heat" then
      device:emit_event(capabilities.thermostatMode.thermostatMode.heat())
    elseif state.mode == "eco" then
      device:emit_event(capabilities.thermostatMode.thermostatMode.eco())
    end
  end
  
  if state.cur_temp then
    device:emit_event(capabilities.temperatureMeasurement.temperature({value = state.cur_temp, unit = "C"}))
  end
  
  if state.set_temp then
    device:emit_event(capabilities.thermostatHeatingSetpoint.heatingSetpoint({value = state.set_temp, unit = "C"}))
  end
end

return thermostat_handler
