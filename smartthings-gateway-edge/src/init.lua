local capabilities = require "st.capabilities"
local Driver = require "st.driver"
local log = require "log"

local gateway = require "gateway"
local discovery = require "discovery"
local bridge_ui = require "bridge_ui"

local light_handler = require "handlers.light"
local thermostat_handler = require "handlers.thermostat"
local air_conditioner_handler = require "handlers.air_conditioner"
local ventilation_handler = require "handlers.ventilation"
local gas_handler = require "handlers.gas"
local outlet_handler = require "handlers.outlet"

local function is_bridge(device)
  return device.device_network_id == "universal_wallpad_bridge" or device.model == "Eri's Gateway" or device.model == "WallpadGateway"
end

local function get_device_type(device)
  if is_bridge(device) then return "bridge" end
  local dnid = device.device_network_id or ""
  local model = device.model or ""
  if dnid:find(":light") or model == "light" then return "light" end
  if dnid:find(":thermo") or model == "thermostat" then return "thermostat" end
  if dnid:find(":aircon") or model == "aircon" then return "air-conditioner" end
  if dnid:find(":vent") or model == "ventilation" then return "ventilation" end
  if dnid:find(":gas") or model == "gas" then return "gas" end
  if dnid:find(":outlet") or model == "outlet" then return "outlet" end
  return "unknown"
end

local function device_init(driver, device)
  log.info("Device initialized: " .. device.id .. " (" .. tostring(device.label) .. ", DNI: " .. tostring(device.device_network_id) .. ")")
  local dtype = get_device_type(device)
  if dtype == "bridge" then
    -- Initialize bridge action switches to 'off'
    if device.profile and device.profile.components and device.profile.components.addDevices then
      device:emit_component_event(device.profile.components.addDevices, capabilities.switch.switch.off())
      bridge_ui.emit_info(device, "addDevices", "템플릿 조회 중...")
    elseif device.profile and device.profile.components and device.profile.components.main and device.profile.components.main.capabilities.switch then
      device:emit_component_event(device.profile.components.main, capabilities.switch.switch.off())
    end
    if device.profile and device.profile.components and device.profile.components.deleteDevices then
      device:emit_component_event(device.profile.components.deleteDevices, capabilities.switch.switch.off())
      bridge_ui.emit_info(device, "deleteDevices", "정상 동작 중")
    end
    if device.profile and device.profile.components and device.profile.components.systemManage then
      device:emit_component_event(device.profile.components.systemManage, capabilities.switch.switch.off())
    end
    device:emit_event(capabilities.healthCheck.checkInterval({ value = 120, data = { protocol = "lan", scheme = "untracked" } }))
    device:online()
    gateway.connect(driver, device)
  elseif dtype == "thermostat" then
    device:emit_event(capabilities.thermostatMode.supportedThermostatModes({ "off", "heat", "eco" }))
    device:emit_event(capabilities.thermostatHeatingSetpoint.heatingSetpointRange({ value = { minimum = 5, maximum = 40 }, unit = "C" }))
    device:online()
  elseif dtype == "air-conditioner" then
    device:emit_event(capabilities.airConditionerMode.supportedAcModes({ "cool", "dry", "fanOnly", "auto" }))
    device:emit_event(capabilities.thermostatCoolingSetpoint.coolingSetpointRange({ value = { minimum = 18, maximum = 30 }, unit = "C" }))
    device:online()
  elseif dtype == "ventilation" then
    device:emit_event(capabilities.airConditionerMode.supportedAcModes({ "auto", "cool" }))
    device:online()
  else
    device:online()
  end
end

local function device_added(driver, device)
  log.info("Device added: " .. device.id .. " (" .. tostring(device.label) .. ")")
  device_init(driver, device)
end

local function device_info_changed(driver, device, event, args)
  log.info("Device info changed: " .. device.id)
  if is_bridge(device) then
    local old_ip = args.old_st_store.preferences and args.old_st_store.preferences.serverIp
    local new_ip = device.preferences and device.preferences.serverIp
    local old_port = args.old_st_store.preferences and args.old_st_store.preferences.serverPort
    local new_port = device.preferences and device.preferences.serverPort

    if old_ip ~= new_ip or old_port ~= new_port then
      log.info(string.format("Gateway server changed: %s:%s -> %s:%s. Reconnecting...",
        tostring(old_ip), tostring(old_port), tostring(new_ip), tostring(new_port)))
      gateway.disconnect(device)
      gateway.connect(driver, device)
    end
  end
end

local function device_deleted(driver, device)
  log.info("Device deleted: " .. device.id)
  if is_bridge(device) then
    log.info("Parent bridge deleted, disconnecting...")
    gateway.disconnect(device)
  end
end

local function handle_refresh(driver, device)
  log.info(string.format("[USER CMD] [REFRESH] Device: '%s' (ID: %s)", tostring(device.label), tostring(device.id)))
  if is_bridge(device) then
    gateway.ping(device)
    gateway.send({ cmd = "discover" })
  end
end

local driver = Driver("UniversalGatewayBridge", {
  discovery = discovery.find_devices,
  lifecycle_handlers = {
    init = device_init,
    added = device_added,
    infoChanged = device_info_changed,
    deleted = device_deleted,
    removed = device_deleted
  },
  capability_handlers = {
    [capabilities.refresh.ID] = {
      [capabilities.refresh.commands.refresh.NAME] = handle_refresh
    },
    [capabilities.switch.ID] = {
      [capabilities.switch.commands.on.NAME] = function(d, dev, cmd)
        local dtype = get_device_type(dev)
        log.info(string.format("[USER CMD] [SWITCH ON] Device: '%s' (Type: %s, Component: %s)",
          tostring(dev.label), dtype, tostring(cmd.component)))

        if dtype == "bridge" then
          local comp_id = cmd.component
          if comp_id == "addDevices" or comp_id == "main" then
            -- [1. 기기 추가 스위치]
            log.info("[BRIDGE] '기기 추가' triggered -> requesting discovery from gateway...")
            local comp = dev.profile.components.addDevices or dev.profile.components.main
            dev:emit_component_event(comp, capabilities.switch.switch.on())
            gateway.send({ cmd = "discover" })
            d:call_with_delay(1, function()
              dev:emit_component_event(comp, capabilities.switch.switch.off())
            end)
          elseif comp_id == "deleteDevices" then
            -- [2. 기기 삭제 스위치]
            log.info("[BRIDGE] '기기 삭제' triggered -> deleting all child devices...")
            dev:emit_component_event(dev.profile.components.deleteDevices, capabilities.switch.switch.on())
            local count = 0
            for _, child in ipairs(d:get_devices()) do
              if not is_bridge(child) then
                log.info("Deleting child device: " .. child.label .. " (" .. child.id .. ")")
                d:try_delete_device(child.id)
                count = count + 1
              end
            end
            bridge_ui.update_status(d, string.format("알림: 하위 기기 %d대 삭제 완료", count))
            d:call_with_delay(1, function()
              dev:emit_component_event(dev.profile.components.deleteDevices, capabilities.switch.switch.off())
            end)
          elseif comp_id == "systemManage" then
            -- [3. 시스템 관리 (드라이버 재시작) 스위치]
            log.info("[BRIDGE] '시스템 관리' triggered -> Restarting Edge Driver process...")
            dev:emit_component_event(dev.profile.components.systemManage, capabilities.switch.switch.on())
            gateway.disconnect(dev)
            d:call_with_delay(0.5, function()
              log.info("[BRIDGE] Exiting process for Hub Core respawn/restart...")
              os.exit(0)
            end)
          end
        elseif dtype == "light" then
          light_handler.on(d, dev, cmd)
        elseif dtype == "outlet" then
          outlet_handler.on(d, dev, cmd)
        elseif dtype == "ventilation" then
          ventilation_handler.on(d, dev, cmd)
        elseif dtype == "air-conditioner" then
          air_conditioner_handler.on(d, dev, cmd)
        elseif dtype == "thermostat" then
          thermostat_handler.on(d, dev, cmd)
        else
          log.warn(string.format("[USER CMD] [SWITCH ON] Unhandled device type '%s' for '%s'", dtype, tostring(dev.label)))
        end
      end,
      [capabilities.switch.commands.off.NAME] = function(d, dev, cmd)
        local dtype = get_device_type(dev)
        log.info(string.format("[USER CMD] [SWITCH OFF] Device: '%s' (Type: %s, Component: %s)",
          tostring(dev.label), dtype, tostring(cmd.component)))

        if dtype == "bridge" then
          local comp = dev.profile.components[cmd.component] or dev.profile.components.addDevices or dev.profile.components.main
          if comp then
            dev:emit_component_event(comp, capabilities.switch.switch.off())
          end
        elseif dtype == "light" then
          light_handler.off(d, dev, cmd)
        elseif dtype == "outlet" then
          outlet_handler.off(d, dev, cmd)
        elseif dtype == "ventilation" then
          ventilation_handler.off(d, dev, cmd)
        elseif dtype == "air-conditioner" then
          air_conditioner_handler.off(d, dev, cmd)
        elseif dtype == "thermostat" then
          thermostat_handler.off(d, dev, cmd)
        else
          log.warn(string.format("[USER CMD] [SWITCH OFF] Unhandled device type '%s' for '%s'", dtype, tostring(dev.label)))
        end
      end,
    },
    [capabilities.thermostatMode.ID] = {
      [capabilities.thermostatMode.commands.setThermostatMode.NAME] = function(d, dev, cmd)
        log.info(string.format("[USER CMD] [THERMOSTAT MODE] Device: '%s', Target Mode: '%s'",
          tostring(dev.label), tostring(cmd.args.mode)))
        thermostat_handler.set_mode(d, dev, cmd)
      end
    },
    [capabilities.thermostatHeatingSetpoint.ID] = {
      [capabilities.thermostatHeatingSetpoint.commands.setHeatingSetpoint.NAME] = function(d, dev, cmd)
        log.info(string.format("[USER CMD] [HEATING SETPOINT] Device: '%s', Target Temp: %s C",
          tostring(dev.label), tostring(cmd.args.setpoint)))
        thermostat_handler.set_heating_setpoint(d, dev, cmd)
      end
    },
    [capabilities.airConditionerMode.ID] = {
      [capabilities.airConditionerMode.commands.setAirConditionerMode.NAME] = function(d, dev, cmd)
        local dtype = get_device_type(dev)
        log.info(string.format("[USER CMD] [AC/VENT MODE] Device: '%s' (Type: %s), Target Mode: '%s'",
          tostring(dev.label), dtype, tostring(cmd.args.mode)))
        if dtype == "air-conditioner" then
          air_conditioner_handler.set_mode(d, dev, cmd)
        elseif dtype == "ventilation" then
          ventilation_handler.set_mode(d, dev, cmd)
        end
      end
    },
    [capabilities.thermostatCoolingSetpoint.ID] = {
      [capabilities.thermostatCoolingSetpoint.commands.setCoolingSetpoint.NAME] = function(d, dev, cmd)
        log.info(string.format("[USER CMD] [COOLING SETPOINT] Device: '%s', Target Temp: %s C",
          tostring(dev.label), tostring(cmd.args.setpoint)))
        air_conditioner_handler.set_cooling_setpoint(d, dev, cmd)
      end
    },
    [capabilities.fanSpeed.ID] = {
      [capabilities.fanSpeed.commands.setFanSpeed.NAME] = function(d, dev, cmd)
        local dtype = get_device_type(dev)
        log.info(string.format("[USER CMD] [FAN SPEED] Device: '%s' (Type: %s), Speed: %s",
          tostring(dev.label), dtype, tostring(cmd.args.fanSpeed)))
        if dtype == "air-conditioner" then
          air_conditioner_handler.set_fan_speed(d, dev, cmd)
        elseif dtype == "ventilation" then
          ventilation_handler.set_fan_speed(d, dev, cmd)
        end
      end
    },
    [capabilities.valve.ID] = {
      [capabilities.valve.commands.close.NAME] = function(d, dev, cmd)
        log.info(string.format("[USER CMD] [GAS VALVE CLOSE] Device: '%s'", tostring(dev.label)))
        gas_handler.close(d, dev, cmd)
      end,
      [capabilities.valve.commands.open.NAME] = function(d, dev, cmd)
        log.info(string.format("[USER CMD] [GAS VALVE OPEN] Device: '%s'", tostring(dev.label)))
        gas_handler.open(d, dev, cmd)
      end
    }
  }
})

log.info("Starting Universal Gateway Bridge driver...")
driver:run()
