local capabilities = require "st.capabilities"
local gateway = require "gateway"
local log = require "log"

local air_conditioner_handler = {}

local function extract_id(device)
  local parts = {}
  for match in (device.device_network_id..":"):gmatch("(.-):") do
    table.insert(parts, match)
  end
  return parts[#parts]
end

function air_conditioner_handler.on(driver, device, command)
  local id = extract_id(device)
  log.info(string.format("[AIRCON] [TX] Device '%s' (ID: %s) -> Sending Power ON", tostring(device.label), tostring(id)))
  gateway.send({id = id, cmd = "set_power", value = "on"})
end

function air_conditioner_handler.off(driver, device, command)
  local id = extract_id(device)
  log.info(string.format("[AIRCON] [TX] Device '%s' (ID: %s) -> Sending Power OFF", tostring(device.label), tostring(id)))
  gateway.send({id = id, cmd = "set_power", value = "off"})
end

function air_conditioner_handler.set_mode(driver, device, command)
  local id = extract_id(device)
  local mode = command.args.mode
  log.info(string.format("[AIRCON] [TX] Device '%s' (ID: %s) -> Set Mode to '%s'", tostring(device.label), tostring(id), tostring(mode)))
  gateway.send({id = id, cmd = "set_power", value = "on"})
  gateway.send({id = id, cmd = "set_mode", value = mode})
end

function air_conditioner_handler.set_cooling_setpoint(driver, device, command)
  local id = extract_id(device)
  local temp = command.args.setpoint
  log.info(string.format("[AIRCON] [TX] Device '%s' (ID: %s) -> Set Cooling Setpoint to %s C", tostring(device.label), tostring(id), tostring(temp)))
  gateway.send({id = id, cmd = "set_temp", value = temp})
end

function air_conditioner_handler.set_fan_speed(driver, device, command)
  local id = extract_id(device)
  local speed = command.args.fanSpeed
  log.info(string.format("[AIRCON] [TX] Device '%s' (ID: %s) -> Set Fan Speed to %s", tostring(device.label), tostring(id), tostring(speed)))
  gateway.send({id = id, cmd = "set_fan", value = tonumber(speed)})
end

function air_conditioner_handler.handle_state(device, state)
  log.info(string.format("[AIRCON] [RX] Device '%s' state received: Power='%s', Mode='%s', Fan=%s, CurTemp=%s, SetTemp=%s",
    tostring(device.label), tostring(state.power), tostring(state.mode), tostring(state.fan), tostring(state.cur_temp), tostring(state.set_temp)))

  if state.power then
    if state.power == "on" then
      device:emit_event(capabilities.switch.switch.on())
    else
      device:emit_event(capabilities.switch.switch.off())
    end
  end
  
  device:emit_event(capabilities.airConditionerMode.supportedAcModes({"cool", "dry", "fanOnly", "auto"}))

  if state.mode then
    device:emit_event(capabilities.airConditionerMode.airConditionerMode({value = state.mode}))
  end
  
  if state.cur_temp then
    device:emit_event(capabilities.temperatureMeasurement.temperature({value = state.cur_temp, unit = "C"}))
  end
  
  if state.set_temp then
    device:emit_event(capabilities.thermostatCoolingSetpoint.coolingSetpoint({value = state.set_temp, unit = "C"}))
  end
  
  if state.fan then
    device:emit_event(capabilities.fanSpeed.fanSpeed({value = tonumber(state.fan)}))
  end
end

return air_conditioner_handler
