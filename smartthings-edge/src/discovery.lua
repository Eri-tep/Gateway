-- ============================================================================
-- Discovery Handler
-- ============================================================================
-- Creates the root LAN Parent Gateway Bridge device during device discovery.
-- ============================================================================

local log = require "log"

local Discovery = {}

--- Discovery callback invoked when user taps "Scan nearby devices".
-- @param driver table: SmartThings Edge Driver instance
-- @param opts table: Discovery options
-- @param on_should_continue function: Returns true while scan is active
function Discovery.handle_discovery(driver, opts, on_should_continue)
  log.info("Device discovery started. Checking for existing Gateway Bridge...")

  local existing_devices = driver:get_devices()
  local bridge_exists = false

  for _, dev in ipairs(existing_devices) do
    if dev.network_type == "LAN" or dev.device_network_id == "RS485_GATEWAY_BRIDGE" then
      bridge_exists = true
      log.info(string.format("Gateway Bridge already exists (ID: %s).", dev.id))
      break
    end
  end

  if not bridge_exists and on_should_continue() then
    log.info("Creating new Gateway Bridge parent device...")
    driver:try_create_device({
      type = "LAN",
      device_network_id = "RS485_GATEWAY_BRIDGE",
      label = "월패드 게이트웨이",
      profile = "gateway-bridge.v1",
      manufacturer = "CustomHome",
      model = "RS485-Bridge",
      vendor_provided_label = "월패드 게이트웨이"
    })
  end

  log.info("Device discovery completed.")
end

return Discovery
