-- ============================================================================
-- Bridge UI Manager
-- ============================================================================
-- Manages custom info capabilities and subcomponent events for Gateway Bridge.
-- ============================================================================

local capabilities = require "st.capabilities"
local log = require "log"

local BridgeUI = {}

--- Find parent bridge device in driver.
-- @param driver table: Driver instance
-- @return table|nil: Bridge Device object
function BridgeUI.get_bridge_device(driver)
  for _, device in ipairs(driver:get_devices()) do
    if device.device_network_id == "universal_wallpad_bridge" or
       device.device_network_id == "RS485_GATEWAY_BRIDGE" or
       device.model == "Eri's Gateway" or
       device.model == "WallpadGateway" or
       device.model == "RS485-Bridge" then
      return device
    end
  end
  return nil
end

--- Emit text info to a specific bridge subcomponent (circlecircle06391.info).
-- @param device table: Bridge Device instance
-- @param component_id string: "addDevices" or "deleteDevices"
-- @param text string: Text to display in the UI tile
function BridgeUI.emit_info(device, component_id, text)
  if not device or not device.profile or not device.profile.components then return end
  local comp = device.profile.components[component_id]
  if not comp then return end

  local info_cap = capabilities["circlecircle06391.info"]
  if info_cap and info_cap.info then
    pcall(function()
      device:emit_component_event(comp, info_cap.info({ value = text }))
    end)
  else
    pcall(function()
      device:emit_component_event(comp, {
        capability = "circlecircle06391.info",
        attribute = "info",
        value = { value = text }
      })
    end)
  end
  log.info(string.format("[BRIDGE UI] %s -> '%s'", component_id, text))
end

--- Update the 24-device template summary on the [기기 추가] info window.
-- @param driver table: Driver instance
function BridgeUI.update_template_summary(driver)
  local bridge = BridgeUI.get_bridge_device(driver)
  if not bridge then return end

  -- Clean 1-line summary format
  local summary_str = "총 24대: 조명 10, 콘센트 7, 난방 4, 전열 1, 인덕션 1, 엘리베이터 1"
  BridgeUI.emit_info(bridge, "addDevices", summary_str)
end

--- Update status text on the [기기 삭제] info window.
-- @param driver table: Driver instance
-- @param status_text string: Status or alert message
function BridgeUI.update_status(driver, status_text)
  local bridge = BridgeUI.get_bridge_device(driver)
  if not bridge then return end
  BridgeUI.emit_info(bridge, "deleteDevices", status_text)
end

--- Report error with room location prefix.
-- @param driver table: Driver instance
-- @param device table: Child device that experienced error
-- @param error_msg string: Error description
function BridgeUI.report_device_error(driver, device, error_msg)
  local name = device.label or device.vendor_provided_label or "기기"
  local text = string.format("오류: %s %s", name, error_msg or "응답 없음")
  BridgeUI.update_status(driver, text)
end

return BridgeUI
