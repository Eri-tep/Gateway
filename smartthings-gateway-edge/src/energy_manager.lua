local log = require "log"

local energy_manager = {}

-- Helper to get current KST month and day
local function get_kst_time()
  -- UTC to KST is +9 hours (32400 seconds)
  local kst_time = os.time() + 32400
  local date_table = os.date("!*t", kst_time)
  return date_table
end

function energy_manager.update(device, power_w)
  local current_time = os.time()
  local last_time = device:get_field("last_power_time")

  if last_time then
    local delta_t = current_time - last_time
    if delta_t > 0 then
      local delta_kwh = (power_w * delta_t) / 3600000.0
      local current_kwh = device:get_field("cumulative_kwh") or 0.0
      current_kwh = current_kwh + delta_kwh
      device:set_field("cumulative_kwh", current_kwh, { persist = true })
      log.debug(string.format("[ENERGY] Device '%s' update: %s W * %ds = +%.5f kWh (Total: %.3f kWh)",
        tostring(device.label), tostring(power_w), delta_t, delta_kwh, current_kwh))
    end
  end

  device:set_field("last_power_time", current_time, { persist = false })
end

function energy_manager.check_monthly_reset(device)
  local kst_date = get_kst_time()
  local current_month = kst_date.month
  local last_reset_month = device:get_field("last_reset_month")

  if last_reset_month == nil then
    -- First time we've ever checked for this device: nothing to
    -- reset yet, just remember the current month.
    device:set_field("last_reset_month", current_month, { persist = true })
    log.info(string.format("[ENERGY] Initialized last_reset_month=%d for '%s'", current_month, tostring(device.label)))
    return
  end

  -- Reset whenever the month has changed, regardless of which day
  -- the state event happens to arrive on. Relying on "day == 1"
  -- meant a missed event on the 1st (offline gateway, hub reboot,
  -- device powered off, etc.) would silently skip the reset for
  -- the entire month.
  if last_reset_month ~= current_month then
    local current_kwh = device:get_field("cumulative_kwh") or 0.0

    device:set_field("prev_month_kwh", current_kwh, { persist = true })
    device:set_field("cumulative_kwh", 0.0, { persist = true })
    device:set_field("last_reset_month", current_month, { persist = true })

    log.info(string.format("[ENERGY] [MONTHLY RESET] Device '%s' (Month %d -> %d): Saved Prev Month = %.3f kWh, Reset Current = 0.0 kWh",
      tostring(device.label), last_reset_month, current_month, current_kwh))
  end
end

function energy_manager.get_stats(device)
  local current_kwh = device:get_field("cumulative_kwh") or 0.0
  local prev_month_kwh = device:get_field("prev_month_kwh") or 0.0
  local last_reset_month = device:get_field("last_reset_month") or 0

  return {
    current_kwh = current_kwh,
    prev_month_kwh = prev_month_kwh,
    last_reset_month = last_reset_month
  }
end

return energy_manager
