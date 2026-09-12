-- ============================================================================
-- SmartThings Edge Driver: RS-485 Wallpad & Elevator Gateway
-- ============================================================================
-- Single-driver architecture managing 24 child devices over non-blocking TCP.
-- Standard Capabilities: Switch, PowerMeter, EnergyMeter, Thermostat, FanSpeed, Button.
-- ============================================================================

local Driver = require "st.driver"
local capabilities = require "st.capabilities"
local log = require "log"

-- Core Modules
local Discovery = require "discovery"
local Lifecycle = require "lifecycle"
local Connection = require "connection"

-- SubDrivers
local LightSubDriver = require "sub_drivers.light"
local OutletSubDriver = require "sub_drivers.outlet"
local InductionSubDriver = require "sub_drivers.induction"
local ThermostatSubDriver = require "sub_drivers.thermostat"
local VentilationSubDriver = require "sub_drivers.ventilation"
local ElevatorSubDriver = require "sub_drivers.elevator"

-- Master table of inbound RS-485 ACK frame dispatchers by device code (dev_id)
local RX_DISPATCHERS = {
  [0x19] = LightSubDriver.handle_rx_frame,       -- 조명 10대
  [0x1F] = OutletSubDriver.handle_rx_frame,      -- 콘센트 7대
  [0x1B] = InductionSubDriver.handle_rx_frame,   -- 인덕션 1대
  [0x18] = ThermostatSubDriver.handle_rx_frame,  -- 난방 4대
  [0x2B] = VentilationSubDriver.handle_rx_frame, -- 전열교환기 1대
  [0x34] = ElevatorSubDriver.handle_rx_frame,    -- 엘리베이터 1대
}

--- Handle Bridge switch commands (Driver / Socket restart).
local function handle_bridge_switch_on(driver, device, command)
  device:emit_event(capabilities.switch.switch.on())
end

local function handle_bridge_switch_off(driver, device, command)
  log.info("Bridge restart triggered by user. Reconnecting TCP sockets...")
  Connection.close_all(device)
  device:emit_event(capabilities.switch.switch.off())
  driver:call_with_delay(2, function()
    Connection.init_connections(driver, device)
    device:emit_event(capabilities.switch.switch.on())
  end, "bridge_restart_timer")
end

local function handle_bridge_refresh(driver, device, command)
  log.info("Bridge refresh requested. Checking connections...")
  device:online()
  for _, child in ipairs(device:get_child_list()) do
    child:online()
  end
end

-- Driver Template Configuration
local driver_template = {
  NAME = "RS485GatewayDriver",
  discovery = Discovery.handle_discovery,
  lifecycle_handlers = {
    init = Lifecycle.device_init,
    added = Lifecycle.device_added,
    doConfigure = Lifecycle.device_do_configure,
    infoChanged = Lifecycle.device_info_changed,
    removed = Lifecycle.device_removed,
  },
  capability_handlers = {
    [capabilities.switch.ID] = {
      [capabilities.switch.commands.on.NAME] = handle_bridge_switch_on,
      [capabilities.switch.commands.off.NAME] = handle_bridge_switch_off,
    },
    [capabilities.refresh.ID] = {
      [capabilities.refresh.commands.refresh.NAME] = handle_bridge_refresh,
    },
  },
  sub_drivers = {
    LightSubDriver,
    OutletSubDriver,
    InductionSubDriver,
    ThermostatSubDriver,
    VentilationSubDriver,
    ElevatorSubDriver,
  },
  driver_lifecycle = Lifecycle.driver_lifecycle,
}

-- Instantiate Driver
local rs485_driver = Driver("RS485GatewayDriver", driver_template)

-- Store RX dispatchers for frame routing
rs485_driver:set_field("rx_dispatchers", RX_DISPATCHERS)

-- Start non-blocking cosock event loop
rs485_driver:run()
