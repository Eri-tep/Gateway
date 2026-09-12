-- ============================================================================
-- Smart Outlet SubDriver (Dev ID: 0x1F, 7 Devices)
-- ============================================================================
-- Manages smart plug circuits, real-time power (W), and monthly energy (kWh).
-- ============================================================================

local capabilities = require "st.capabilities"
local log = require "log"
local PacketBuilder = require "packet_builder"
local Connection = require "connection"
local EnergyManager = require "energy_manager"

local OutletSubDriver = {}

--- SubDriver selector predicate.
function OutletSubDriver.can_handle(opts, driver, device, ...)
  return device:get_field("device_type") == "outlet"
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
    log.error(string.format("Outlet [%s]: Missing parent or sub2", device.id))
    return
  end

  local pkt = PacketBuilder.outlet_cmd(sub2, true)
  local ok, err = Connection.send_wallpad(parent, pkt)
  if ok then
    device:emit_event(capabilities.switch.switch.on())
  else
    log.error(string.format("Outlet [%s] TX failed: %s", device.id, tostring(err)))
  end
end

--- Handle switch OFF command.
local function handle_switch_off(driver, device, command)
  local sub2 = device:get_field("sub2")
  local parent = get_parent(device)
  if not parent or not sub2 then
    log.error(string.format("Outlet [%s]: Missing parent or sub2", device.id))
    return
  end

  local pkt = PacketBuilder.outlet_cmd(sub2, false)
  local ok, err = Connection.send_wallpad(parent, pkt)
  if ok then
    device:emit_event(capabilities.switch.switch.off())
  else
    log.error(string.format("Outlet [%s] TX failed: %s", device.id, tostring(err)))
  end
end

--- Handle refresh command.
local function handle_refresh(driver, device, command)
  log.debug(string.format("Outlet [%s] refresh requested", device.id))
end

--- Handle inbound RS-485 ACK frame for outlets (0x1F).
-- ACK format: F7 [LEN] 01 1F 04 40 [sub2] 00 [State: 0x01=ON, 0x02=OFF] [W_High] [W_Low] ... [CS] EE
-- frame.data contains: { 0x00, State, W_High, W_Low, ... }
function OutletSubDriver.handle_rx_frame(driver, device, frame)
  if not frame.data or #frame.data < 2 then
    return
  end

  local state_byte = frame.data[2]
  if state_byte == 0x01 then
    device:emit_event(capabilities.switch.switch.on())
  elseif state_byte == 0x02 then
    device:emit_event(capabilities.switch.switch.off())
  end

  -- Power measurement bytes (W_High, W_Low)
  if #frame.data >= 4 then
    local w_high = frame.data[3] or 0
    local w_low = frame.data[4] or 0
    local watts = (w_high * 256) + w_low
    EnergyManager.update(device, watts)
  end
end

OutletSubDriver.NAME = "OutletSubDriver"
OutletSubDriver.capability_handlers = {
  [capabilities.switch.ID] = {
    [capabilities.switch.commands.on.NAME] = handle_switch_on,
    [capabilities.switch.commands.off.NAME] = handle_switch_off,
  },
  [capabilities.refresh.ID] = {
    [capabilities.refresh.commands.refresh.NAME] = handle_refresh,
  }
}

return OutletSubDriver
