local capabilities = require "st.capabilities"
local log = require "log"

local TelemetryHandler = {}

-- Helper to parse semantic version numbers
local function parse_version(v_str)
  local clean = (v_str or ""):gsub("^v", "")
  local major, minor, patch = clean:match("^(%d+)%.(%d+)%.?(%d*)")
  return tonumber(major) or 0, tonumber(minor) or 0, tonumber(patch) or 0
end

-- Helper to check if new_v is strictly newer than cur_v
local function is_newer_version(new_v, cur_v)
  local n_maj, n_min, n_pat = parse_version(new_v)
  local c_maj, c_min, c_pat = parse_version(cur_v)
  if n_maj ~= c_maj then return n_maj > c_maj end
  if n_min ~= c_min then return n_min > c_min end
  return n_pat > c_pat
end

-- Helper to emit component event safely
local function emit_event(device, comp, cap_event)
  if comp and cap_event then
    cap_event.state_change = true
    local ev, err = device:emit_component_event(comp, cap_event)
    if err then
      log.error(string.format("Failed to emit event: %s", tostring(err)))
    end
  end
end

-- Unified rolling ticker interval (seconds)
local TICKER_INTERVAL = 10

-- Ensure the single unified master rolling ticker is running
function TelemetryHandler.ensure_master_ticker(device)
  if device:get_field("master_roll_timer") then return end

  local timer = device.thread:call_on_schedule(TICKER_INTERVAL, function()
    local registry = device:get_field("ticker_registry") or {}
    for _, entry in pairs(registry) do
      if entry.items and #entry.items > 1 and entry.cap and entry.comp then
        entry.idx = (entry.idx % #entry.items) + 1
        emit_event(device, entry.comp, entry.cap[entry.attr_name]({ value = entry.items[entry.idx] }))
      end
    end
  end, "master_roll_timer")
  device:set_field("master_roll_timer", timer)
end

-- Unified ticker registration: registers items to the master tick cycle
function TelemetryHandler.register_ticker(device, comp, cap, attr_name, ticker_id, items, reset_idx)
  if not cap or not items or #items == 0 then return end

  local registry = device:get_field("ticker_registry") or {}
  local current = registry[ticker_id] or { idx = 1 }
  current.comp = comp
  current.cap = cap
  current.attr_name = attr_name
  current.items = items

  if reset_idx or current.idx > #items then
    current.idx = 1
  end

  registry[ticker_id] = current
  device:set_field("ticker_registry", registry)

  -- Emit initial/current value immediately
  emit_event(device, comp, cap[attr_name]({ value = items[current.idx] }))

  -- Ensure master ticker is active
  TelemetryHandler.ensure_master_ticker(device)
end

local register_ticker = TelemetryHandler.register_ticker

function TelemetryHandler.handle_telemetry(driver, device, data)
  if not data or data.res ~= "ok" then
    log.warn("Invalid telemetry payload received from gateway")
    return
  end

  -- 자식 기기 오염 방지: 게이트웨이 기기가 아닌 경우 즉시 리턴
  if device.device_network_id ~= "esp32_wallpad_gateway_ctrl" then
    log.warn("Attempted to run gateway telemetry report on non-gateway device: " .. tostring(device.label))
    return
  end

  local comp_main = device.profile.components["main"]
  local comp_wallpad = device.profile.components["wallpad"] or comp_main
  local comp_diag = device.profile.components["diagnostics"] or comp_main
  local comp_logs = device.profile.components["logs"] or comp_main
  local comp_net = device.profile.components["network"] or comp_main
  local comp_ota = device.profile.components["ota"] or comp_main

  if not comp_main then
    log.error("Main component not found in profile")
    return
  end

  log.info("📊 ══════════════ [ESP32 GATEWAY 6-CARD TELEMETRY REPORT] ══════════════")

  -- ═══════════════════════════════════════════════════════════════════════════
  -- CARD 1: Devices (최상단 단독 카드 - 23/23)
  -- ═══════════════════════════════════════════════════════════════════════════
  if data.cache then
    local total = data.cache.total_devices or 0
    local active = data.cache.online_devices or 0
    local health_str = string.format("%d/%d", active, total)
    local cap_health = capabilities["digituniverse06711.device"]
    if cap_health then
      emit_event(device, comp_main, cap_health.deviceHealth({ value = health_str }))
    end
  end

  local cap_mgr = capabilities["digituniverse06711.deviceManager"]
  if cap_mgr then
    emit_event(device, comp_main, cap_mgr.action({ value = "idle" }))
  end

  -- ═══════════════════════════════════════════════════════════════════════════
  -- CARD 2: Wallpad (월패드 프로토콜 & Auto-Probing 리셋)
  -- ═══════════════════════════════════════════════════════════════════════════

  -- 2-0. Switch (Auto-Probing Reset 스위치) 🟦 대표 파란 헤더
  emit_event(device, comp_wallpad, capabilities.switch.switch.off())

  -- 2-1. Protocol Profile Name (The_Astin)
  local slot = (data.profile and data.profile.active_slot) or 1
  local raw_key = data.profile and data.profile.active_key
  local label_key = ""
  if slot == 0 then
    label_key = "Auto Detect"
  elseif slot == 1 then
    label_key = (raw_key and raw_key ~= "" and not raw_key:match("^Custom")) and raw_key or "The_Astin"
  else
    label_key = (raw_key and raw_key ~= "" and not raw_key:match("^Custom")) and raw_key or "Custom " .. tostring(slot)
  end
  -- 2-1. Protocol Profile Name (Auto Detect)
  local final_profile_str = label_key

  local cap_pname = capabilities["digituniverse06711.profile"]
  if cap_pname then
    emit_event(device, comp_wallpad, cap_pname.profileName({ value = final_profile_str }))
  end

  local prof = data.profile or {}

  -- 2-2. Catalog Match (Hyundai HT)
  local cat_match = prof.catalog_match or "None"
  cat_match = cat_match:gsub("%s*%b()", "") -- "(6 Devices)" 제거
  local cap_match = capabilities["digituniverse06711.catalogMatch"]
  if cap_match then
    emit_event(device, comp_wallpad, cap_match.match({ value = cat_match }))
  end

  -- 2-3. Blueprints (6 Groups)
  local bp_status = prof.blueprints or "0 Groups"
  bp_status = bp_status:gsub("%s*%b()", "") -- "(Locked)", "(Learning)" 제거
  local cap_bp = capabilities["digituniverse06711.blueprints"]
  if cap_bp then
    emit_event(device, comp_wallpad, cap_bp.status({ value = bp_status }))
  end

  -- ═══════════════════════════════════════════════════════════════════════════
  -- CARD 3: Gateway (게이트웨이 시스템 & 리소스)
  -- ═══════════════════════════════════════════════════════════════════════════

  -- 3-0. Switch (시스템 재부팅 스위치) 🟦 대표 파란 헤더
  emit_event(device, comp_diag, capabilities.switch.switch.off())

  -- 3-1. Uptime: 1d 1h 1m 형식
  local up_s = (data.system and data.system.uptime_s) or 0
  local up_days = math.floor(up_s / 86400)
  local up_hours = math.floor((up_s % 86400) / 3600)
  local up_mins = math.floor((up_s % 3600) / 60)
  local up_str = string.format("%dd %dh %dm", up_days, up_hours, up_mins)
  local cap_uptime = capabilities["digituniverse06711.uptime"]
  if cap_uptime then
    emit_event(device, comp_diag, cap_uptime.uptime({ value = up_str }))
  end

  -- 3-2. Unified Resource (CPU -> RAM -> Flash 4초 순회)
  local c0 = (data.system and data.system.cpu0_load) or 0
  local c1 = (data.system and data.system.cpu1_load) or 0
  local cpu_str = string.format("CPU: Core0(%d%%), Core1(%d%%)", c0, c1)

  local free_heap = (data.system and data.system.free_heap_kb) or 77
  local used_heap = math.max(0, 320 - free_heap)
  local ram_pct = math.floor(math.max(0, math.min(100, used_heap * 100 / 320)))
  local ram_str = string.format("RAM: %d%% (%d / 320 KB)", ram_pct, used_heap)

  local sys = data.system or {}
  local f_total = tonumber(sys.flash_total_mb or sys.flash_size_mb or sys.flash_total or sys.flash_mb)
  local f_used = tonumber(sys.flash_used_mb or sys.flash_used)
  local rom_pct = tonumber(sys.flash_pct or sys.flash_usage_pct)

  if f_total and f_used then
    rom_pct = rom_pct or math.floor(f_used * 100 / f_total)
  elseif f_total and not f_used and rom_pct then
    f_used = (f_total * rom_pct) / 100
  elseif not f_total then
    f_total = 8.19
    f_used = f_used or 3.98
    rom_pct = rom_pct or 49
  end
  local flash_str = string.format("Flash: %d%% (%.2f / %.2f MB)", rom_pct or 49, f_used or 3.98, f_total or 8.19)

  register_ticker(device, comp_diag, capabilities["digituniverse06711.resource"], "resource", "resource",
                  { cpu_str, ram_str, flash_str })

  -- 3-3. Traffic Packet Stats (CH1 ~ CH4 10초 순회)
  local function fmt_num(n)
    n = tonumber(n) or 0
    if n >= 1000000 then
      return string.format("%.1fM", n / 1000000)
    elseif n >= 1000 then
      return string.format("%.1fk", n / 1000)
    else
      return tostring(n)
    end
  end

  local channels = data.channels or {}
  local ch1 = channels.ch1 or {}
  local ch2 = channels.ch2 or {}
  local ch3 = channels.ch3 or {}
  local ch4 = channels.ch4 or {}

  -- CH1: T <tx> / R <rx> (CRC <err>) -> 0 초과 시에만 (CRC N) 표기
  local ch1_str = string.format("CH1: T %s / R %s", fmt_num(ch1.tx), fmt_num(ch1.rx))
  local ch1_crc = tonumber(ch1.crc_err or ch1.crc_errors) or 0
  if ch1_crc > 0 then
    ch1_str = string.format("%s (CRC %d)", ch1_str, ch1_crc)
  end

  -- CH2: T <tx> / R <rx> (UnC <uncached>) -> 0 초과 시에만 (UnC N) 표기
  local ch2_str = string.format("CH2: T %s / R %s", fmt_num(ch2.tx), fmt_num(ch2.rx))
  local ch2_unc = tonumber(ch2.uncached or ch2.uncached_pkts) or 0
  if ch2_unc > 0 then
    ch2_str = string.format("%s (UnC %d)", ch2_str, ch2_unc)
  end

  -- CH3: T <tx> / R <rx> (UnC <uncached>) -> 0 초과 시에만 (UnC N) 표기
  local ch3_str = string.format("CH3: T %s / R %s", fmt_num(ch3.tx), fmt_num(ch3.rx))
  local ch3_unc = tonumber(ch3.uncached or ch3.uncached_pkts) or 0
  if ch3_unc > 0 then
    ch3_str = string.format("%s (UnC %d)", ch3_str, ch3_unc)
  end

  -- CH4: T <tx> / R <rx> (InV <invalid>) -> 0 초과 시에만 (InV N) 표기
  local ch4_str = string.format("CH4: T %s / R %s", fmt_num(ch4.tx), fmt_num(ch4.rx))
  local ch4_inv = tonumber(ch4.inv or ch4.invalid or ch4.invalid_frames or ch4.err) or 0
  if ch4_inv > 0 then
    ch4_str = string.format("%s (InV %d)", ch4_str, ch4_inv)
  end

  local ch_items = { ch1_str, ch2_str, ch3_str, ch4_str }
  local ch5 = channels.ch5
  if ch5 then
    local ch5_str = string.format("CH5: T %s / R %s", fmt_num(ch5.tx), fmt_num(ch5.rx))
    local ch5_drp = tonumber(ch5.dropped or ch5.dropped_pkts) or 0
    if ch5_drp > 0 then
      ch5_str = string.format("%s (Drp %d)", ch5_str, ch5_drp)
    end
    table.insert(ch_items, ch5_str)
  end

  register_ticker(device, comp_diag, capabilities["digituniverse06711.channel"], "channel", "channel", ch_items)

  -- ═══════════════════════════════════════════════════════════════════════════
  -- CARD 4: Logs (진단 로그 & 코어 덤프 순회)
  -- ═══════════════════════════════════════════════════════════════════════════

  -- 4-0. Switch (진단 로그 초기화 스위치) 🟦 대표 파란 헤더
  emit_event(device, comp_logs, capabilities.switch.switch.off())

  -- 4-1. History (Log & Crash Dump 10초 주기 롤링 순회 - 시간 부분 제외, Empty일 때 Empty Log / Empty Crash)
  local log_display = "Empty Log"
  if data.diagnostics and data.diagnostics.reboot_logs and #data.diagnostics.reboot_logs > 0 then
    local top = data.diagnostics.reboot_logs[1]
    local reason_short = top.reason or "Unknown"
    if reason_short:match("Low Heap") then
      reason_short = "Low Heap"
    elseif reason_short:match("Software Restart") then
      reason_short = "Software Restart"
    elseif reason_short:match("Remote Reboot") then
      reason_short = "Remote Reboot"
    elseif reason_short:match("Power") then
      reason_short = "Power On"
    elseif reason_short:match("CPU Panic") then
      reason_short = "CPU Panic"
    end
    log_display = reason_short
  end

  local crash_display = "Empty Crash"
  if data.diagnostics and data.diagnostics.coredump and data.diagnostics.coredump.valid then
    local cd = data.diagnostics.coredump
    crash_display = string.format("Panic Task %s", cd.task or "main")
  end

  register_ticker(device, comp_logs, capabilities["digituniverse06711.history"], "history", "history",
                  { log_display, crash_display })

  -- ═══════════════════════════════════════════════════════════════════════════
  -- CARD 5: Network (네트워크 정보 & WiFi 스캔)
  -- ═══════════════════════════════════════════════════════════════════════════

  -- 5-0. Switch (WiFi 스캔 스위치) 🟦 대표 파란 헤더
  emit_event(device, comp_net, capabilities.switch.switch.off())

  -- 5-1. Scan (Ready / N APs Found or Active Rolling Scan)
  local cap_scan = capabilities["digituniverse06711.scan"]
  local scan_items = device:get_field("scan_items")
  if scan_items and #scan_items > 0 then
    register_ticker(device, comp_net, cap_scan, "scanResult", "scan", scan_items)
  else
    local last_scan = device:get_field("last_scan_result") or "Ready"
    if cap_scan then
      emit_event(device, comp_net, cap_scan.scanResult({ value = last_scan }))
    end
  end

  -- 5-2. WiFi 정보 & Mode 순회 (Wi-Fi:, Mode: 접두사 없음)
  local wifi_data = data.wifi or {}
  local ssid = (wifi_data.ssid and wifi_data.ssid ~= "") and wifi_data.ssid or "Disconnected"
  local rssi = wifi_data.rssi or (sys and sys.wifi_rssi)
  local wifi_display = "Disconnected (0%)"
  if ssid == "Disconnected" then
    wifi_display = "Disconnected (0%)"
  elseif rssi then
    local lqi = math.floor(math.min(100, math.max(0, 2 * (rssi + 100))))
    wifi_display = string.format("%s (%d%%)", ssid, lqi)
  else
    wifi_display = string.format("%s (70%%)", ssid)
  end

  local ip_addr = (data.wifi and data.wifi.ip and data.wifi.ip ~= "") and data.wifi.ip or "172.30.1.3"
  local ap_ip_str = "172.30.2.1"
  local raw_wmode = (data.wifi and data.wifi.mode) or "STA"
  local mode_str = ""
  if raw_wmode:match("AP") and raw_wmode:match("STA") then
    mode_str = string.format("STA + AP (%s)", ap_ip_str)
  elseif raw_wmode == "AP" then
    mode_str = string.format("AP (%s)", ap_ip_str)
  else
    mode_str = string.format("STA (%s)", ip_addr)
  end

  register_ticker(device, comp_net, capabilities["digituniverse06711.wifi"], "wifiInfo", "wifi",
                  { wifi_display, mode_str })

  -- ═══════════════════════════════════════════════════════════════════════════
  -- CARD 6: Firmware & OTA (펌웨어 관리 & OTA)
  -- ═══════════════════════════════════════════════════════════════════════════

  -- 6-0. Switch (OTA 업데이트 시작 스위치) 🟦 대표 파란 헤더
  emit_event(device, comp_ota, capabilities.switch.switch.off())

  -- 6-1. Version (OTA 진행 시 Updating % 표시)
  local cur_fw = data.system and data.system.firmware
  if cur_fw and cur_fw ~= "" then
    if not cur_fw:match("^v") then cur_fw = "v" .. cur_fw end
  else
    cur_fw = "Unknown"
  end

  local latest_fw = (data.system and data.system.latest_firmware) or (data.ota and data.ota.latest_firmware)
  local fw_display = cur_fw
  if latest_fw and latest_fw ~= "" then
    if not latest_fw:match("^v") then latest_fw = "v" .. latest_fw end
    if is_newer_version(latest_fw, cur_fw) then
      fw_display = string.format("%s ➔ %s", cur_fw, latest_fw)
    end
  end

  -- OTA 진행 중이거나 에러 발생 시 Version 항목에 상태 출력
  if data.ota then
    if data.ota.in_progress then
      fw_display = string.format("Updating %d%%", data.ota.progress_pct or 0)
    elseif data.ota.last_error and data.ota.last_error ~= "" then
      fw_display = string.format("OTA Error: %s", data.ota.last_error)
    end
  end

  local cap_fw = capabilities["digituniverse06711.version"]
  if cap_fw then
    emit_event(device, comp_ota, cap_fw.version({ value = fw_display }))
  end

  -- 6-2. Build (순회 제거, Build: 접두사 제거, 단독 고정 표시)
  local stab_str = "Stable"
  if up_s < 120 then
    stab_str = "Pending"
  elseif data.diagnostics and data.diagnostics.coredump and data.diagnostics.coredump.valid then
    stab_str = "Crash 1"
  end
  local build_str = string.format("%s (Idle)", stab_str)

  local cap_ostate = capabilities["digituniverse06711.build"]
  if cap_ostate then
    emit_event(device, comp_ota, cap_ostate.build({ value = build_str }))
  end

  -- ═══════════════════════════════════════════════════════════════════════════
  -- DOORPHONE CHILD DEVICE: 호출 감지 상태 업데이트 (세대 도어 & 로비 도어)
  -- ═══════════════════════════════════════════════════════════════════════════
  if data.doorphone then
    TelemetryHandler.handle_doorphone_event(driver, data.doorphone)
  end

  log.info("📊 ═══════════════════════════════════════════════════════════════════════")
end

-- ═══════════════════════════════════════════════════════════════════════════
-- 실시간 도어폰 이벤트 핸들러 (CH6 Server Push 즉시 처리)
-- ═══════════════════════════════════════════════════════════════════════════
function TelemetryHandler.handle_doorphone_event(driver, event_data)
  if not event_data then return end
  local front_bell = event_data.front_bell
  local lobby_bell = event_data.lobby_bell
  local cap_motion = capabilities.motionSensor

  for _, dev in ipairs(driver:get_devices()) do
    local p_key = dev.parent_assigned_child_key
    local c_main = dev.profile.components["main"]

    -- 1. 신규 분리형 세대 도어 (doorphone_front)
    if p_key == "doorphone_front" and cap_motion and c_main then
      local motion_evt = front_bell and cap_motion.motion.active({ state_change = true }) or cap_motion.motion.inactive({ state_change = true })
      log.info(string.format("🚪 [DOORPHONE REALTIME] Front Door Motion -> %s", front_bell and "ACTIVE (호출 중)" or "INACTIVE (대기)"))
      dev:emit_component_event(c_main, motion_evt)
    end

    -- 2. 신규 분리형 로비 도어 (doorphone_lobby)
    if p_key == "doorphone_lobby" and cap_motion and c_main then
      local motion_evt = lobby_bell and cap_motion.motion.active({ state_change = true }) or cap_motion.motion.inactive({ state_change = true })
      log.info(string.format("🚪 [DOORPHONE REALTIME] Lobby Door Motion -> %s", lobby_bell and "ACTIVE (호출 중)" or "INACTIVE (대기)"))
      dev:emit_component_event(c_main, motion_evt)
    end

    -- 3. 레거시 통합 도어폰 (하위 호환)
    if p_key == "doorphone" and cap_motion then
      local c_lobby = dev.profile.components["lobby"]
      if c_main then
        local motion_evt = front_bell and cap_motion.motion.active({ state_change = true }) or cap_motion.motion.inactive({ state_change = true })
        dev:emit_component_event(c_main, motion_evt)
      end
      if c_lobby then
        local motion_evt = lobby_bell and cap_motion.motion.active({ state_change = true }) or cap_motion.motion.inactive({ state_change = true })
        dev:emit_component_event(c_lobby, motion_evt)
      end
    end
  end
end

-- ═══════════════════════════════════════════════════════════════════════════
-- 장치 클래스별 텔레메트리 디스패치 테이블 및 핸들러 (Table-Driven Pattern)
-- ═══════════════════════════════════════════════════════════════════════════

local ELEVATOR_DIR_FORMAT = {
  [1] = "%d호 상승 (%dF)",
  [2] = "%d호 하강 (%dF)",
  [3] = "%d호 도착 (%dF)",
}

local function schedule_elevator_idle(dev, delay_sec)
  local timer_key = "ev_arrival_timer"
  local existing = dev:get_field(timer_key)
  if existing then
    dev.thread:cancel_timer(existing)
    dev:set_field(timer_key, nil)
  end

  local function apply_idle()
    dev:set_field(timer_key, nil)
    local off_sw = capabilities.switch.switch.off()
    off_sw.state_change = true
    dev:emit_event(off_sw)
    local cap_hist = capabilities["digituniverse06711.history"]
    if cap_hist then
      local off_evt = cap_hist.history({ value = "대기 중" })
      off_evt.state_change = true
      dev:emit_event(off_evt)
    end
  end

  if delay_sec <= 0 then
    apply_idle()
  else
    local t = dev.thread:call_with_delay(delay_sec, apply_idle, timer_key)
    dev:set_field(timer_key, t)
  end
end

local DEVICE_TELEMETRY_HANDLERS = {
  ["switch"] = function(dev, event_data)
    if event_data.power ~= nil and capabilities.switch then
      local sw_evt = (event_data.power == 1) and capabilities.switch.switch.on() or capabilities.switch.switch.off()
      sw_evt.state_change = true
      dev:emit_event(sw_evt)
    end
  end,

  ["momentary"] = function(dev, event_data)
    local pwr = tonumber(event_data.power) or 0
    local cap_hist = capabilities["digituniverse06711.history"]

    if pwr == 1 then
      -- 호출 중: 스위치 ON 및 상태 이력 "호출 중"
      if capabilities.switch then
        local on_evt = capabilities.switch.switch.on()
        on_evt.state_change = true
        dev:emit_event(on_evt)
      end
      if cap_hist then
        local h_evt = cap_hist.history({ value = "호출 중" })
        h_evt.state_change = true
        dev:emit_event(h_evt)
      end
    else
      -- 도착 또는 대기 복귀: 스위치 OFF 및 상태 이력 "대기 중"
      schedule_elevator_idle(dev, 0)
    end
  end,

  ["outlet"] = function(dev, event_data)
    if event_data.power ~= nil and capabilities.switch then
      local sw_evt = (event_data.power == 1) and capabilities.switch.switch.on() or capabilities.switch.switch.off()
      sw_evt.state_change = true
      dev:emit_event(sw_evt)
    end

    local cur_month = os.date("%Y-%m")
    local last_month = dev:get_field("last_energy_month")
    local monthly_kwh = dev:get_field("monthly_energy_kwh") or 0.0

    if last_month ~= cur_month then
      monthly_kwh = 0.0
      dev:set_field("monthly_energy_kwh", 0.0, { persist = true })
      dev:set_field("last_energy_month", cur_month, { persist = true })
      log.info(string.format("📅 [OUTLET] New month (%s) detected! Reset monthly energy to 0.0 kWh", cur_month))
    end

    if event_data.power_w ~= nil then
      local p_w = tonumber(event_data.power_w) or 0.0
      local now_ts = os.time()
      local last_ts = dev:get_field("last_power_ts") or now_ts
      local dt = math.max(0, now_ts - last_ts)

      -- 30초 이상 연결 단절 후 첫 수신이거나, 전원이 꺼져있거나, 대기전력 미만이면 누적하지 않고 타임스탬프만 동기화
      local is_power_on = (event_data.power == 1) or (event_data.power == nil and p_w > 0.5)
      if dt > 0 and dt <= 30 and p_w > 0.5 and is_power_on then
        monthly_kwh = monthly_kwh + (p_w * dt / 3600000.0)
        dev:set_field("monthly_energy_kwh", monthly_kwh, { persist = true })
      end
      dev:set_field("last_power_ts", now_ts, { persist = true })

      if capabilities.powerMeter then
        dev:emit_event(capabilities.powerMeter.power({ value = p_w, unit = "W" }))
      end
      if capabilities.energyMeter then
        local kwh_val = math.floor(monthly_kwh * 1000 + 0.5) / 1000
        dev:emit_event(capabilities.energyMeter.energy({ value = kwh_val, unit = "kWh" }))
      end
    end
  end,

  ["thermostat"] = function(dev, event_data)
    local is_away = (event_data.power == 2)
    if event_data.power ~= nil and capabilities.thermostatMode then
      local mode_str = (event_data.power == 1) and "heat" or (is_away and "away" or "off")
      dev:emit_event(capabilities.thermostatMode.thermostatMode(mode_str))
    end

    local saved_temp = dev:get_field("last_thermo_temp") or 22
    local target_temp = saved_temp
    if event_data.target_temp and event_data.target_temp >= 10 and event_data.target_temp <= 35 then
      target_temp = event_data.target_temp
      if not is_away then
        dev:set_field("last_thermo_temp", target_temp, { persist = true })
      end
    end

    local current_temp = (event_data.current_temp and event_data.current_temp > 0) and event_data.current_temp or target_temp

    if capabilities.thermostatHeatingSetpoint then
      local sp_evt = capabilities.thermostatHeatingSetpoint.heatingSetpoint({ value = target_temp, unit = "C" })
      sp_evt.state_change = true
      dev:emit_event(sp_evt)
    end
    if capabilities.temperatureMeasurement then
      local cur_evt = capabilities.temperatureMeasurement.temperature({ value = current_temp, unit = "C" })
      cur_evt.state_change = true
      dev:emit_event(cur_evt)
    end
  end,

  ["vent"] = function(dev, event_data)
    local has_fan_speed = capabilities.fanSpeed and dev:supports_capability_by_id(capabilities.fanSpeed.ID)
    local cap_vent = capabilities["digituniverse06711.ventmode"]
    local cap_speed = capabilities["digituniverse06711.ventspeed"]

    if event_data.power == 0 then
      if capabilities.switch then
        local sw_off = capabilities.switch.switch.off()
        sw_off.state_change = true
        dev:emit_event(sw_off)
      end
      if has_fan_speed then
        dev:emit_event(capabilities.fanSpeed.fanSpeed(0))
      end
    else
      if capabilities.switch then
        local sw_on = capabilities.switch.switch.on()
        sw_on.state_change = true
        dev:emit_event(sw_on)
      end

      -- 1. 풍량 상태 (ventspeed: low, medium, high / fanSpeed: 1~3)
      local spd = tonumber(event_data.fan_speed) or 1
      if spd < 1 or spd > 3 then spd = 1 end
      local spd_map = { [1] = "low", [2] = "medium", [3] = "high" }
      local spd_str = spd_map[spd] or "low"
      if cap_speed and dev:supports_capability_by_id(cap_speed.ID) then
        dev:emit_event(cap_speed.ventSpeed(spd_str))
      end
      if has_fan_speed then
        dev:emit_event(capabilities.fanSpeed.fanSpeed(spd))
      end

      -- 2. 운전 모드 상태 (1: normal, 2: bypass, 3: auto, 4: clean)
      local mode_num = tonumber(event_data.vent_mode) or 1
      local mode_map = { [1] = "normal", [2] = "bypass", [3] = "auto", [4] = "clean" }
      local mode_str = mode_map[mode_num] or "normal"
      if cap_vent and dev:supports_capability_by_id(cap_vent.ID) then
        pcall(function()
          dev:emit_event(cap_vent.ventMode({ value = mode_str }))
        end)
      end
    end
  end,

  ["gas"] = function(dev, event_data)
    if capabilities.valve then
      local v_evt = (event_data.valve == "closed") and capabilities.valve.valve.closed() or capabilities.valve.valve.open()
      dev:emit_event(v_evt)
    end
  end
}

-- ═══════════════════════════════════════════════════════════════════════════
-- 실시간 기기 상태 이벤트 핸들러 (CH6 Server Push)
-- ═══════════════════════════════════════════════════════════════════════════
function TelemetryHandler.handle_device_state_event(driver, event_data)
  if not event_data then return end
  local d_id = tonumber(event_data.dev_id)
  local s1 = tonumber(event_data.sub1)
  local s2 = tonumber(event_data.sub2)
  if not d_id or not s1 or not s2 then return end

  local target_key = string.format("dev_%02X_%d_%d", d_id, s1, s2)
  local d_cls = event_data.class or "switch"

  for _, dev in ipairs(driver:get_devices()) do
    if dev.parent_assigned_child_key == target_key then
      log.info(string.format("📡 [DEVICE STATE] %s (%s) State Update: Power=%s, Class=%s",
                             dev.label, target_key, tostring(event_data.power), d_cls))

      local handler = DEVICE_TELEMETRY_HANDLERS[d_cls] or DEVICE_TELEMETRY_HANDLERS["switch"]
      handler(dev, event_data)
      break
    end
  end
end

-- ═══════════════════════════════════════════════════════════════════════════
-- 기기 락 변경 이벤트 핸들러 (CH6 Server Push)
-- ═══════════════════════════════════════════════════════════════════════════
function TelemetryHandler.handle_devices_updated_event(driver, event_data)
  log.info("🔔 [DEVICES UPDATED] Gateway reports new device LOCKED! Click 'Add' on Child Device Manager to sync.")
end

return TelemetryHandler


