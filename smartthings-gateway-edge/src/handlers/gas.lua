local capabilities = require "st.capabilities"
local gateway = require "gateway"
local log = require "log"

local gas_handler = {}

local function extract_id(device)
  local parts = {}
  for match in (device.device_network_id .. ":"):gmatch("(.-):") do
    table.insert(parts, match)
  end
  return parts[#parts]
end

function gas_handler.close(driver, device, command)
  local id = extract_id(device)
  log.info(string.format("[GAS VALVE] [TX] Device '%s' (ID: %s) -> Sending Valve CLOSE command", tostring(device.label), tostring(id)))
  gateway.send({ id = id, cmd = "close" })
end

-- Gas valves are intentionally not remotely re-openable for safety.
-- We register a handler (rather than leaving "open" unhandled) so the
-- command doesn't get silently dropped, which can otherwise leave the
-- app's toggle UI stuck waiting for a state update that never comes.
-- No command is sent to the gateway; we just bounce the UI back to
-- "closed" and log what happened.
function gas_handler.open(driver, device, command)
  log.warn("[GAS VALVE] [BLOCKED] Remote 'open' blocked for safety on device " ..
    device.id .. " (" .. tostring(device.label) .. "). Gas valve can only " ..
    "be closed remotely; re-opening requires physical action at the valve.")
  device:emit_event(capabilities.valve.valve.closed())
end

function gas_handler.handle_state(device, state)
  log.info(string.format("[GAS VALVE] [RX] Device '%s' state received: '%s'", tostring(device.label), tostring(state.state)))
  if state.state == "open" then
    device:emit_event(capabilities.valve.valve.open())
  elseif state.state == "closed" then
    device:emit_event(capabilities.valve.valve.closed())
  end
end

return gas_handler
