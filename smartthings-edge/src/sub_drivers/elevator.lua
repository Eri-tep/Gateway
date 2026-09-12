-- ============================================================================
-- Elevator SubDriver (Dev ID: 0x34, 1 Device)
-- ============================================================================
-- Handles One-touch elevator calling and arrival notification to user floor (e.g. 15F).
-- - Switch ON: Sends elevator call packet
-- - Button pushed event: Triggered on arrival at user's floor (automations/routines)
-- Protocol:
-- - Call: F7 0D 01 34 02 41 10 00 02 00 00 9E EE (13 bytes)
-- - Status: F7 0D 01 34 01 41 10 00 [State/Dir] [Floor] [CarID] [CS] EE (13 bytes)
--   State/Dir: 0xA6=Up, 0xB6=Down, 0x01=Arrived, 0x00=Idle
-- ============================================================================

local capabilities = require "st.capabilities"
local log = require "log"
local PacketBuilder = require "packet_builder"
local Connection = require "connection"

local ElevatorSubDriver = {}

--- SubDriver selector predicate.
function ElevatorSubDriver.can_handle(opts, driver, device, ...)
  return device:get_field("device_type") == "elevator"
end

--- Resolve parent bridge device.
local function get_parent(device)
  return device:get_parent_device()
end

--- Handle switch ON command (Call Elevator).
local function handle_switch_on(driver, device, command)
  local parent = get_parent(device)
  if not parent then
    log.error(string.format("Elevator [%s]: Missing parent bridge", device.id))
    return
  end

  log.info(string.format("Elevator [%s]: Calling elevator...", device.id))
  local pkt = PacketBuilder.elevator_call()
  local ok, err = Connection.send_elevator(parent, pkt)
  if ok then
    device:emit_event(capabilities.switch.switch.on())
  else
    log.error(string.format("Elevator [%s] Call TX failed: %s", device.id, tostring(err)))
  end
end

--- Handle switch OFF command (Reset button state).
local function handle_switch_off(driver, device, command)
  device:emit_event(capabilities.switch.switch.off())
end

--- Handle refresh command.
local function handle_refresh(driver, device, command)
  log.debug(string.format("Elevator [%s] refresh requested", device.id))
end

--- Handle inbound RS-485 ACK/Status frame for elevator (0x34).
-- Status format: F7 0D 01 34 01 41 10 00 [State/Dir] [Floor] [CarID] [CS] EE
-- frame.data contains: { 0x00, state_dir, floor, car_id }
function ElevatorSubDriver.handle_rx_frame(driver, device, frame)
  if not frame.data or #frame.data < 4 then
    return
  end

  local state_dir = frame.data[2]
  local floor = frame.data[3]
  local car_id = frame.data[4]

  local parent = get_parent(device)
  local my_floor = (parent and parent.preferences and parent.preferences.myFloor) or 15

  log.debug(string.format("Elevator Status: Car #%d at Floor %d (State/Dir: 0x%02X, MyFloor: %d)",
    car_id, floor, state_dir, my_floor))

  -- Check arrival at user's floor (0x01 = Arrived)
  if (state_dir == 0x01 and floor == my_floor) or (floor == my_floor and state_dir == 0x01) then
    log.info(string.format("Elevator Arrived! Car #%d reached floor %d.", car_id, my_floor))
    -- Trigger SmartThings automation routines via button.pushed event
    device:emit_event(capabilities.button.button.pushed({ state_change = true }))
    -- Reset calling switch back to OFF
    device:emit_event(capabilities.switch.switch.off())
  end
end

ElevatorSubDriver.NAME = "ElevatorSubDriver"
ElevatorSubDriver.capability_handlers = {
  [capabilities.switch.ID] = {
    [capabilities.switch.commands.on.NAME] = handle_switch_on,
    [capabilities.switch.commands.off.NAME] = handle_switch_off,
  },
  [capabilities.refresh.ID] = {
    [capabilities.refresh.commands.refresh.NAME] = handle_refresh,
  }
}

return ElevatorSubDriver
