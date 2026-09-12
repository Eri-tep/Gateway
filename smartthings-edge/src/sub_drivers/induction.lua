-- ============================================================================
-- Induction Breaker SubDriver (Dev ID: 0x1B, 1 Device)
-- ============================================================================
-- Monitors induction power status and provides remote emergency cut-off.
-- SAFETY CONSTRAINT: Remote ON (energize) is intentionally disabled.
-- ============================================================================

local capabilities = require "st.capabilities"
local log = require "log"
local PacketBuilder = require "packet_builder"
local Connection = require "connection"

local InductionSubDriver = {}

--- SubDriver selector predicate.
function InductionSubDriver.can_handle(opts, driver, device, ...)
  return device:get_field("device_type") == "induction"
end

--- Resolve parent bridge device.
local function get_parent(device)
  return device:get_parent_device()
end

--- Handle switch ON command (Safety Restriction).
local function handle_switch_on(driver, device, command)
  log.warn(string.format("Induction [%s]: Remote power ON is blocked for safety.", device.id))
  -- Re-emit current state to reflect physical status in SmartThings UI
  local current_state = device:get_latest_state("main", capabilities.switch.ID, capabilities.switch.switch.NAME)
  if current_state then
    device:emit_event(capabilities.switch.switch(current_state))
  else
    device:emit_event(capabilities.switch.switch.off())
  end
end

--- Handle switch OFF command (Cut-off power).
local function handle_switch_off(driver, device, command)
  local parent = get_parent(device)
  if not parent then
    log.error(string.format("Induction [%s]: Missing parent bridge", device.id))
    return
  end

  log.info(string.format("Induction [%s]: Sending emergency power cut-off command", device.id))
  local pkt = PacketBuilder.induction_cutoff()
  local ok, err = Connection.send_wallpad(parent, pkt)
  if ok then
    device:emit_event(capabilities.switch.switch.off())
  else
    log.error(string.format("Induction [%s] TX failed: %s", device.id, tostring(err)))
  end
end

--- Handle refresh command.
local function handle_refresh(driver, device, command)
  log.debug(string.format("Induction [%s] refresh requested", device.id))
end

--- Handle inbound RS-485 ACK frame for induction breaker (0x1B).
-- ACK format: F7 0D 01 1B 04 43 11 00 [State: 0x04=energized, 0x02/0x00=cut-off] ... [CS] EE
-- frame.data contains: { 0x00, State, ... }
function InductionSubDriver.handle_rx_frame(driver, device, frame)
  if not frame.data or #frame.data < 2 then
    return
  end

  local state_byte = frame.data[2]
  if state_byte == 0x04 then
    device:emit_event(capabilities.switch.switch.on())
  else
    device:emit_event(capabilities.switch.switch.off())
  end
end

InductionSubDriver.NAME = "InductionSubDriver"
InductionSubDriver.capability_handlers = {
  [capabilities.switch.ID] = {
    [capabilities.switch.commands.on.NAME] = handle_switch_on,
    [capabilities.switch.commands.off.NAME] = handle_switch_off,
  },
  [capabilities.refresh.ID] = {
    [capabilities.refresh.commands.refresh.NAME] = handle_refresh,
  }
}

return InductionSubDriver
