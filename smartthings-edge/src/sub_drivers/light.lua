-- ============================================================================
-- Light SubDriver (Dev ID: 0x19, 10 Devices)
-- ============================================================================
-- Manages lighting circuits for living room (4), bedroom (2), office (2), relax (2).
-- ============================================================================

local capabilities = require "st.capabilities"
local log = require "log"
local PacketBuilder = require "packet_builder"
local Connection = require "connection"

local LightSubDriver = {}

--- SubDriver selector predicate.
function LightSubDriver.can_handle(opts, driver, device, ...)
  return device:get_field("device_type") == "light"
end

--- Resolve parent bridge device.
local function get_parent(device)
  return device:get_parent_device()
end

--- Handle switch ON command.
local function handle_switch_on(driver, device, command)
  local sub2 = device:get_field("sub2")
  local parent = get_parent(device)
  if not parent or not sub2 then
    log.error(string.format("Light [%s]: Missing parent or sub2", device.id))
    return
  end

  local pkt = PacketBuilder.light_cmd(sub2, true)
  local ok, err = Connection.send_wallpad(parent, pkt)
  if ok then
    device:emit_event(capabilities.switch.switch.on())
  else
    log.error(string.format("Light [%s] TX failed: %s", device.id, tostring(err)))
  end
end

--- Handle switch OFF command.
local function handle_switch_off(driver, device, command)
  local sub2 = device:get_field("sub2")
  local parent = get_parent(device)
  if not parent or not sub2 then
    log.error(string.format("Light [%s]: Missing parent or sub2", device.id))
    return
  end

  local pkt = PacketBuilder.light_cmd(sub2, false)
  local ok, err = Connection.send_wallpad(parent, pkt)
  if ok then
    device:emit_event(capabilities.switch.switch.off())
  else
    log.error(string.format("Light [%s] TX failed: %s", device.id, tostring(err)))
  end
end

--- Handle refresh command.
local function handle_refresh(driver, device, command)
  log.debug(string.format("Light [%s] refresh requested", device.id))
end

--- Handle inbound RS-485 ACK frame for lights (0x19).
-- ACK format: F7 0B 01 19 04 40 [sub2] 00 [State: 0x01=ON, 0x02=OFF] [CS] EE
-- frame.data contains: { 0x00, State }
function LightSubDriver.handle_rx_frame(driver, device, frame)
  if not frame.data or #frame.data < 2 then
    return
  end

  local state_byte = frame.data[2]
  if state_byte == 0x01 then
    device:emit_event(capabilities.switch.switch.on())
  elseif state_byte == 0x02 then
    device:emit_event(capabilities.switch.switch.off())
  end
end

LightSubDriver.NAME = "LightSubDriver"
LightSubDriver.capability_handlers = {
  [capabilities.switch.ID] = {
    [capabilities.switch.commands.on.NAME] = handle_switch_on,
    [capabilities.switch.commands.off.NAME] = handle_switch_off,
  },
  [capabilities.refresh.ID] = {
    [capabilities.refresh.commands.refresh.NAME] = handle_refresh,
  }
}

return LightSubDriver
