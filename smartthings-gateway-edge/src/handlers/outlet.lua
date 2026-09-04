local capabilities = require "st.capabilities"
local gateway = require "gateway"
local energy_manager = require "energy_manager"
local log = require "log"

local outlet_handler = {}

local function extract_id(device)
  local parts = {}
  for match in (device.device_network_id..":"):gmatch("(.-):") do
    table.insert(parts, match)
  end
  return parts[#parts]
end

function outlet_handler.on(driver, device, command)
  local id = extract_id(device)
  log.info(string.format("[OUTLET] [TX] Device '%s' (ID: %s) -> Sending Outlet ON", tostring(device.label), tostring(id)))
  gateway.send({id = id, cmd = "set_state", value = "on"})
end

function outlet_handler.off(driver, device, command)
  local id = extract_id(device)
  log.info(string.format("[OUTLET] [TX] Device '%s' (ID: %s) -> Sending Outlet OFF", tostring(device.label), tostring(id)))
  gateway.send({id = id, cmd = "set_state", value = "off"})
end

function outlet_handler.handle_state(device, state)
  log.info(string.format("[OUTLET] [RX] Device '%s' state received: State='%s', Power=%s W",
    tostring(device.label), tostring(state.state), tostring(state.power_w)))

  if state.state == "on" then
    device:emit_event(capabilities.switch.switch.on())
  elseif state.state == "off" then
    device:emit_event(capabilities.switch.switch.off())
  end
  
  if state.power_w then
    device:emit_event(capabilities.powerMeter.power({value = state.power_w, unit = "W"}))
    energy_manager.update(device, state.power_w)
    energy_manager.check_monthly_reset(device)
    
    local stats = energy_manager.get_stats(device)
    device:emit_event(capabilities.energyMeter.energy({value = stats.current_kwh, unit = "kWh"}))
  end
end

return outlet_handler
