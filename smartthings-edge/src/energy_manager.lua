-- ============================================================================
-- Energy Manager (Power Meter & Monthly Energy Accumulation)
-- ============================================================================
-- Accumulates real-time Watt measurements into kWh with persistent storage
-- and automatic monthly reset on the 1st of each month (KST, UTC+9).
-- ============================================================================

local capabilities = require "st.capabilities"
local socket = require "cosock.socket"
local log = require "log"

local EnergyManager = {}

local KST_OFFSET_SEC = 9 * 3600 -- UTC+9 for Korea Standard Time

--- Process power update and calculate energy integration.
-- @param device table: Child Device instance for outlet
-- @param current_watts number: Real-time power consumption in Watts
function EnergyManager.update(device, current_watts)
  current_watts = math.max(0, current_watts or 0)

  local now = socket.gettime()
  local last_time = device:get_field("last_energy_time")
  local accumulated_kwh = device:get_field("accumulated_kwh") or 0.0

  if last_time and last_time > 0 then
    local delta_sec = now - last_time
    -- Only accumulate if interval is sane (under 5 minutes, e.g. not after hub reboot)
    if delta_sec > 0 and delta_sec < 300 then
      local delta_kwh = (current_watts * delta_sec) / 3600000.0
      accumulated_kwh = accumulated_kwh + delta_kwh
    end
  end

  -- Check KST month rollover for automatic reset
  local kst_time = os.date("*t", math.floor(now + KST_OFFSET_SEC))
  local current_month = kst_time.month
  local last_reset_month = device:get_field("last_reset_month")

  if last_reset_month and last_reset_month ~= current_month then
    log.info(string.format("Outlet [%s]: New month detected (KST Month %d != %d). Resetting accumulated energy.",
      device.id, current_month, last_reset_month))
    accumulated_kwh = 0.0
    device:set_field("last_reset_month", current_month, { persist = true })
  elseif not last_reset_month then
    device:set_field("last_reset_month", current_month, { persist = true })
  end

  -- Save state
  device:set_field("last_energy_time", now)
  device:set_field("accumulated_kwh", accumulated_kwh, { persist = true })

  -- Emit SmartThings Capability Events
  device:emit_event(capabilities.powerMeter.power(math.floor(current_watts + 0.5)))
  local rounded_kwh = math.floor(accumulated_kwh * 100 + 0.5) / 100.0
  device:emit_event(capabilities.energyMeter.energy({ value = rounded_kwh, unit = "kWh" }))
end

return EnergyManager
