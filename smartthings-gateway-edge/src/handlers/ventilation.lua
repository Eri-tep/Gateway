local capabilities = require "st.capabilities"
local gateway = require "gateway"
local log = require "log"

local ventilation_handler = {}

local function extract_id(device)
  local parts = {}
  for match in (device.device_network_id..":"):gmatch("(.-):") do
    table.insert(parts, match)
  end
  return parts[#parts]
end

function ventilation_handler.on(driver, device, command)
  local id = extract_id(device)
  log.info(string.format("[VENTILATION] [TX] Device '%s' (ID: %s) -> Sending Power ON", tostring(device.label), tostring(id)))
  gateway.send({id = id, cmd = "set_power", value = "on"})
end

function ventilation_handler.off(driver, device, command)
  local id = extract_id(device)
  log.info(string.format("[VENTILATION] [TX] Device '%s' (ID: %s) -> Sending Power OFF", tostring(device.label), tostring(id)))
  gateway.send({id = id, cmd = "set_power", value = "off"})
end

function ventilation_handler.set_mode(driver, device, command)
  local id = extract_id(device)
  local mode = command.args.mode
  local vent_mode = "auto"
  if mode == "cool" then vent_mode = "bypass" end
  log.info(string.format("[VENTILATION] [TX] Device '%s' (ID: %s) -> Set Mode to '%s' (VentMode: %s)",
    tostring(device.label), tostring(id), tostring(mode), vent_mode))
  gateway.send({id = id, cmd = "set_mode", value = vent_mode})
end

function ventilation_handler.set_fan_speed(driver, device, command)
  local id = extract_id(device)
  local speed = tonumber(command.args.fanSpeed)
  log.info(string.format("[VENTILATION] [TX] Device '%s' (ID: %s) -> Set Speed to %s", tostring(device.label), tostring(id), tostring(speed)))
  gateway.send({id = id, cmd = "set_speed", value = speed})
end

function ventilation_handler.handle_state(device, state)
  log.info(string.format("[VENTILATION] [RX] Device '%s' state received: Power='%s', Mode='%s', Speed=%s",
    tostring(device.label), tostring(state.power), tostring(state.mode), tostring(state.speed)))

  if state.power then
    if state.power == "on" then
      device:emit_event(capabilities.switch.switch.on())
    else
      device:emit_event(capabilities.switch.switch.off())
    end
  end
  
  device:emit_event(capabilities.airConditionerMode.supportedAcModes({"auto", "cool"}))

  if state.mode then
    local mode = "auto"
    if state.mode == "bypass" then mode = "cool" end
    device:emit_event(capabilities.airConditionerMode.airConditionerMode({value = mode}))
  end
  
  if state.speed then
    device:emit_event(capabilities.fanSpeed.fanSpeed({value = tonumber(state.speed)}))
  end
end

return ventilation_handler
