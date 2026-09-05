local capabilities = require "st.capabilities"
local gateway = require "gateway"
local log = require "log"

local light_handler = {}

local function extract_id(device)
  -- network id format: ip:port:device_id
  local parts = {}
  for match in (device.device_network_id..":"):gmatch("(.-):") do
    table.insert(parts, match)
  end
  return parts[#parts]
end

function light_handler.on(driver, device, command)
  local id = extract_id(device)
  log.info(string.format("[LIGHT] [TX] Device '%s' (ID: %s) -> Sending Power ON", tostring(device.label), tostring(id)))
  gateway.send({ id = id, cmd = "set_state", value = "on" })
end

function light_handler.off(driver, device, command)
  local id = extract_id(device)
  log.info(string.format("[LIGHT] [TX] Device '%s' (ID: %s) -> Sending Power OFF", tostring(device.label), tostring(id)))
  gateway.send({ id = id, cmd = "set_state", value = "off" })
end

function light_handler.handle_state(device, state)
  log.info(string.format("[LIGHT] [RX] Device '%s' state received: '%s'", tostring(device.label), tostring(state.state)))
  if state.state == "on" then
    device:emit_event(capabilities.switch.switch.on())
  elseif state.state == "off" then
    device:emit_event(capabilities.switch.switch.off())
  end
end

return light_handler
