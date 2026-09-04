-- ============================================================================
-- Lifecycle Handlers (Parent Bridge & 24 Child Devices)
-- ============================================================================
-- Manages platform lifecycle events: init, added, doConfigure, infoChanged, removed,
-- and driver_lifecycle shutdown.
-- ============================================================================

local capabilities = require "st.capabilities"
local log = require "log"
local ChildManager = require "child_manager"
local Connection = require "connection"

local Lifecycle = {}

--- Helper to check if a device is the root parent bridge.
-- @param device table: Device instance
-- @return boolean: true if parent bridge
local function is_parent_bridge(device)
  return device.network_type == "LAN" or device.parent_device_id == nil
end

--- Device added callback (fired ONCE when device is newly created on SmartThings).
function Lifecycle.device_added(driver, device)
  log.info(string.format("Device added: [%s] %s (Type: %s)", device.id, device.label, device.network_type or "CHILD"))

  if is_parent_bridge(device) then
    device:set_field("is_bridge", true)
    device:emit_event(capabilities.switch.switch.on())
    -- Automatically instantiate all 24 child devices
    ChildManager.create_all_children(driver, device)
  else
    -- Child device added: bind type metadata
    local child_key = device.parent_assigned_child_key
    local def = ChildManager.get_definition(child_key)
    if def then
      device:set_field("device_type", def.type)
      device:set_field("sub2", def.sub2)
      device:set_field("sub1", def.sub1)
      device:set_field("dev_id", def.dev_id)
      log.info(string.format("Configured child [%s] as type '%s' (dev_id: 0x%02X, sub2: 0x%02X)",
        device.id, def.type, def.dev_id, def.sub2))
    end
    device:online()
  end
end

--- Device init callback (fired on driver startup, hub reboot, and after added).
function Lifecycle.device_init(driver, device)
  log.info(string.format("Device init: [%s] %s", device.id, device.label))

  if is_parent_bridge(device) then
    device:set_field("is_bridge", true)
    device:online()
    -- Start TCP connections and background RX coroutines
    Connection.init_connections(driver, device)
    -- Ensure child devices are synced
    driver:call_with_delay(2, function()
      ChildManager.create_all_children(driver, device)
    end, "sync_children_timer")
  else
    -- Child device init: ensure transient fields are set
    local child_key = device.parent_assigned_child_key
    local def = ChildManager.get_definition(child_key)
    if def then
      device:set_field("device_type", def.type)
      device:set_field("sub2", def.sub2)
      device:set_field("sub1", def.sub1)
      device:set_field("dev_id", def.dev_id)
    end
    device:online()
  end
end

--- Device doConfigure callback.
function Lifecycle.device_do_configure(driver, device)
  log.info(string.format("Device doConfigure: [%s] %s", device.id, device.label))
  device:online()
end

--- Device preferences changed callback (infoChanged).
function Lifecycle.device_info_changed(driver, device, event, args)
  log.info(string.format("Device infoChanged: [%s] %s", device.id, device.label))

  if is_parent_bridge(device) and args and args.old_st_store then
    local old_prefs = args.old_st_store.preferences or {}
    local new_prefs = device.preferences or {}

    local wp_changed = (old_prefs.wallpadIp ~= new_prefs.wallpadIp) or (old_prefs.wallpadPort ~= new_prefs.wallpadPort)
    local el_changed = (old_prefs.elevatorIp ~= new_prefs.elevatorIp) or (old_prefs.elevatorPort ~= new_prefs.elevatorPort)

    if wp_changed or el_changed then
      log.info("Gateway connection preferences changed. Restarting TCP connections...")
      Connection.close_all(device)
      Connection.init_connections(driver, device)
    end
  end
end

--- Device removed callback.
function Lifecycle.device_removed(driver, device)
  log.info(string.format("Device removed: [%s] %s", device.id, device.label))

  if is_parent_bridge(device) then
    Connection.close_all(device)
    log.info("Closed all TCP sockets for removed bridge device.")
  end
end

--- Driver-level lifecycle callback (e.g. driver shutdown / update).
function Lifecycle.driver_lifecycle(driver, event)
  log.info(string.format("Driver lifecycle event: %s", tostring(event)))
  if event == "shutdown" then
    for _, dev in ipairs(driver:get_devices()) do
      if is_parent_bridge(dev) then
        Connection.close_all(dev)
      end
    end
  end
end

return Lifecycle
