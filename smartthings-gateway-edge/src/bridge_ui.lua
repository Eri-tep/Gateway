local capabilities = require "st.capabilities"
local log = require "log"

local bridge_ui = {}

function bridge_ui.get_bridge_device(driver)
  for _, device in ipairs(driver:get_devices()) do
    if device.device_network_id == "universal_wallpad_bridge" or device.model == "Eri's Gateway" or device.model == "WallpadGateway" then
      return device
    end
  end
  return nil
end

function bridge_ui.emit_info(device, component_id, text)
  if not device or not device.profile or not device.profile.components then return end
  local comp = device.profile.components[component_id]
  if not comp then return end

  local info_cap = capabilities["circlecircle06391.info"]
  if info_cap and info_cap.info then
    device:emit_component_event(comp, info_cap.info({ value = text }))
  else
    device:emit_component_event(comp, {
      capability = "circlecircle06391.info",
      attribute = "info",
      value = { value = text }
    })
  end
  log.info(string.format("[BRIDGE UI] %s -> '%s'", component_id, text))
end

function bridge_ui.update_template_summary(driver, devices_list)
  local bridge = bridge_ui.get_bridge_device(driver)
  if not bridge then return end

  if not devices_list or #devices_list == 0 then
    bridge_ui.emit_info(bridge, "addDevices", "등록된 기기 없음")
    return
  end

  local type_counts = { light = 0, thermostat = 0, aircon = 0, ventilation = 0, gas = 0, outlet = 0 }
  for _, dev_info in ipairs(devices_list) do
    local t = dev_info.type or "other"
    type_counts[t] = (type_counts[t] or 0) + 1
  end

  local parts = {}
  if type_counts.light and type_counts.light > 0 then table.insert(parts, "조명 " .. type_counts.light) end
  if type_counts.thermostat and type_counts.thermostat > 0 then table.insert(parts, "난방 " .. type_counts.thermostat) end
  if type_counts.aircon and type_counts.aircon > 0 then table.insert(parts, "에어컨 " .. type_counts.aircon) end
  if type_counts.ventilation and type_counts.ventilation > 0 then table.insert(parts, "환기 " .. type_counts.ventilation) end
  if type_counts.gas and type_counts.gas > 0 then table.insert(parts, "가스 " .. type_counts.gas) end
  if type_counts.outlet and type_counts.outlet > 0 then table.insert(parts, "콘센트 " .. type_counts.outlet) end

  local summary_str = string.format("총 %d대: %s", #devices_list, table.concat(parts, ", "))
  bridge_ui.emit_info(bridge, "addDevices", summary_str)
end

function bridge_ui.update_status(driver, status_text)
  local bridge = bridge_ui.get_bridge_device(driver)
  if not bridge then return end
  bridge_ui.emit_info(bridge, "deleteDevices", status_text)
end

function bridge_ui.report_device_error(driver, device, error_msg)
  local name = device.label or device.vendor_provided_label or "기기"
  local text = string.format("오류: %s %s", name, error_msg or "응답 없음")
  bridge_ui.update_status(driver, text)
end

return bridge_ui
