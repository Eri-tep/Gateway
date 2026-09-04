local log = require "log"
local cosock = require "cosock"
local gateway = require "gateway"
local schema_cache = require "schema_cache"
local bridge_ui = require "bridge_ui"

local discovery = {}

function discovery.find_devices(driver, _, should_continue)
  local DEVICE_NET_ID = "universal_wallpad_bridge"
  local existing_devices = driver:get_devices()
  log.info(string.format("[DISCOVERY] Scan started. Currently registered devices count: %d", #existing_devices))

  local bridge_exists = false
  for idx, device in ipairs(existing_devices) do
    log.debug(string.format("[DISCOVERY] Registered device #%d: Label='%s', DNI='%s', ID='%s'",
      idx, tostring(device.label), tostring(device.device_network_id), tostring(device.id)))
    if device.device_network_id == DEVICE_NET_ID then
      log.info("[DISCOVERY] Parent bridge device ('" .. DEVICE_NET_ID .. "') already exists.")
      bridge_exists = true
      break
    end
  end

  if not bridge_exists then
    log.info("[DISCOVERY] [PARENT BRIDGE] Requesting Hub Core to create Virtual Bridge device...")
    local bridge_metadata = {
      type = "LAN",
      device_network_id = DEVICE_NET_ID,
      label = "Wallpad Gateway",
      profile = "gateway-bridge",
      manufacturer = "Universal",
      model = "Eri's Gateway",
      vendor_provided_label = "Wallpad Gateway"
    }
    log.info(string.format("[DISCOVERY] [PARENT BRIDGE] Metadata: DNI='%s', Profile='%s', Model='%s'",
      bridge_metadata.device_network_id, bridge_metadata.profile, bridge_metadata.model))

    local dev, err = driver:try_create_device(bridge_metadata)
    if err then
      log.error("[DISCOVERY] [PARENT BRIDGE] FAILED to create device: " .. tostring(err))
    else
      log.info("[DISCOVERY] [PARENT BRIDGE] SUCCESS: device create request accepted by Hub Core.")
    end
  end

  -- Keep the discovery coroutine alive while user scan is active so Hub Core IPC completes
  while should_continue and should_continue() do
    cosock.socket.sleep(1)
  end
end

function discovery.handle_discovered(driver, devices_list)
  if not devices_list or #devices_list == 0 then
    log.warn("[DISCOVERY] [CHILD DEVICES] Received empty or nil device list from gateway.")
    bridge_ui.update_template_summary(driver, devices_list)
    return
  end

  log.info(string.format("[DISCOVERY] [CHILD DEVICES] Received %d calibrated devices from gateway", #devices_list))
  bridge_ui.update_template_summary(driver, devices_list)

  local bridge_device = nil
  for _, device in ipairs(driver:get_devices()) do
    if device.device_network_id == "universal_wallpad_bridge" or device.model == "Eri's Gateway" or device.model == "WallpadGateway" then
      bridge_device = device
      break
    end
  end

  if not bridge_device then
    log.error("[DISCOVERY] [CHILD DEVICES] Parent bridge device not found in driver! Cannot attach children.")
    return
  end

  local ip = (bridge_device.preferences and bridge_device.preferences.serverIp) or "172.30.1.3"
  local port = (bridge_device.preferences and bridge_device.preferences.serverPort) or 8899
  local prefix = ip .. ":" .. port .. ":"

  for idx, dev_info in ipairs(devices_list) do
    local dev_id = dev_info.id
    local dev_type = dev_info.type
    local dev_name = dev_info.name or dev_id
    local network_id = prefix .. dev_id

    log.info(string.format("[DISCOVERY] [CHILD #%d] ID='%s', Type='%s', Name='%s', TargetDNI='%s'",
      idx, tostring(dev_id), tostring(dev_type), tostring(dev_name), network_id))

    schema_cache.store(dev_id, dev_info)

    local existing_device = driver:get_device_info(network_id)
    if not existing_device then
      local profile_name = ""
      if dev_type == "light" then
        profile_name = "light-switch"
      elseif dev_type == "thermostat" then
        profile_name = "thermostat"
      elseif dev_type == "aircon" then
        profile_name = "air-conditioner"
      elseif dev_type == "ventilation" then
        profile_name = "ventilation"
      elseif dev_type == "gas" then
        profile_name = "gas-valve"
      elseif dev_type == "outlet" then
        profile_name = "smart-outlet"
      end

      if profile_name ~= "" then
        log.info(string.format("[DISCOVERY] [CHILD CREATE] Creating '%s' with profile '%s'...", dev_name, profile_name))
        local create_device_msg = {
          type = "LAN",
          device_network_id = network_id,
          label = dev_name,
          profile = profile_name,
          manufacturer = "Universal",
          model = dev_type,
          vendor_provided_label = dev_name,
          parent_device_id = bridge_device.id
        }
        local child_dev, child_err = driver:try_create_device(create_device_msg)
        if child_err then
          log.error(string.format("[DISCOVERY] [CHILD CREATE] FAILED to create child '%s': %s", dev_name,
            tostring(child_err)))
        else
          log.info(string.format("[DISCOVERY] [CHILD CREATE] SUCCESS: Child '%s' request accepted.", dev_name))
        end
      else
        log.warn(string.format("[DISCOVERY] [CHILD CREATE] Unknown device type '%s', skipping creation.",
          tostring(dev_type)))
      end
    else
      log.info(string.format("[DISCOVERY] [CHILD] Device '%s' (%s) already exists, skipping creation.", dev_name,
        network_id))
    end
  end
end

return discovery
