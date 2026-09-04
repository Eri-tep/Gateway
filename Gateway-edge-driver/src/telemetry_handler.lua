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

function TelemetryHandler.handle_telemetry(driver, device, data)
  if not data or data.res ~= "ok" then
    log.warn("Invalid telemetry payload received from gateway")
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
  -- CARD 1: Devices (최상단 단독 카드 - 23 / 23 Online)
  -- ═══════════════════════════════════════════════════════════════════════════
  if data.cache then
    local total = data.cache.total_devices or 0
    local active = data.cache.online_devices or 0
    local health_str = string.format("%d / %d Online", active, total)
    local cap_health = capabilities["digituniverse06711.activeDevice"]
    if cap_health then
      emit_event(device, comp_main, cap_health.deviceHealth({ value = health_str }))
    end
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
  local cap_pname = capabilities["digituniverse06711.profile"]
  if cap_pname then
    emit_event(device, comp_wallpad, cap_pname.profileName({ value = label_key }))
  end

  -- 2-2. Frame: F7...XOR EE (11B)
  local prof = data.profile or {}
  local stx_raw = prof.stx or prof.header or prof.start_byte or prof.stx_hex or (data.packet and data.packet.stx)
  local stx = stx_raw and tostring(stx_raw):gsub("^0x", ""):upper() or "F7"
  local etx_raw = prof.etx or prof.footer or prof.end_byte or prof.etx_hex or (data.packet and data.packet.etx)
  local etx = etx_raw and tostring(etx_raw):gsub("^0x", ""):upper() or "EE"
  local plen = tonumber(prof.min_len or prof.len or prof.packet_len or (data.packet and data.packet.len)) or 11

  local cs_algo = prof.cs_algo or prof.checksum or prof.checksum_type or prof.crc or (data.packet and data.packet.checksum) or "XOR"
  local cs_name = tostring(cs_algo):upper():gsub("%s*%(.*%)", ""):gsub("%s*%[.*%]", "")
  if cs_name == "" or cs_name == "N/A" then cs_name = "XOR" end

  local sig_str = string.format("%s...%s %s (%dB)", stx, cs_name, etx, plen)
  local cap_frame = capabilities["digituniverse06711.frame"]
  if cap_frame then
    emit_event(device, comp_wallpad, cap_frame.frame({ value = sig_str }))
  end

  -- 2-3. Checksum (<Type> (<Range>))
  local cs_type = prof.checksum or prof.checksum_type or prof.crc or (data.packet and data.packet.checksum)
  local cs_str = "XOR (0 ... [n-3])"
  if cs_type and cs_type ~= "" then
    if tostring(cs_type):match("%(") then
      cs_str = tostring(cs_type)
    else
      cs_str = string.format("%s (0 ... [n-3])", tostring(cs_type):upper())
    end
  end
  local cap_cs = capabilities["digituniverse06711.checksum"]
  if cap_cs then
    emit_event(device, comp_wallpad, cap_cs.checksum({ value = cs_str }))
  end

  -- 2-4. Opcodes (Qry(01), Cmd(02), Ack(04))
  local q_op = "01"
  local c_op = "02"
  local a_op = "04"
  if prof.opcodes then
    q_op = tostring(prof.opcodes.query or prof.opcodes.qry or "01"):gsub("^0x", ""):upper()
    c_op = tostring(prof.opcodes.control or prof.opcodes.cmd or "02"):gsub("^0x", ""):upper()
    a_op = tostring(prof.opcodes.ack or "04"):gsub("^0x", ""):upper()
  end
  local opcodes_str = string.format("Qry(%s), Cmd(%s), Ack(%s)", q_op, c_op, a_op)
  local cap_opcodes = capabilities["digituniverse06711.opcodes"]
  if cap_opcodes then
    emit_event(device, comp_wallpad, cap_opcodes.opcodes({ value = opcodes_str }))
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

  -- 2-2. Cpu (Core0, 1 (5%, 4%))
  local c0 = (data.system and data.system.cpu0_load) or 0
  local c1 = (data.system and data.system.cpu1_load) or 0
  local cpu_str = string.format("Core0, 1 (%d%%, %d%%)", c0, c1)
  local cap_cpu = capabilities["digituniverse06711.cpu"]
  if cap_cpu then
    emit_event(device, comp_diag, cap_cpu.cpuLoad({ value = cpu_str }))
  end

  -- 2-3. Ram (76% (243 / 320 KB))
  local free_heap = (data.system and data.system.free_heap_kb) or 77
  local used_heap = math.max(0, 320 - free_heap)
  local ram_pct = math.floor(math.max(0, math.min(100, used_heap * 100 / 320)))
  local ram_str = string.format("%d%% (%d / 320 KB)", ram_pct, used_heap)
  local cap_res = capabilities["digituniverse06711.ram"]
  if cap_res then
    emit_event(device, comp_diag, cap_res.ram({ value = ram_str }))
  end

  -- 2-4. Flash (<pct>% (<used> / <total> MB))
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

  local flash_str = string.format("%d%% (%.2f / %.2f MB)", rom_pct or 49, f_used or 3.98, f_total or 8.19)
  local cap_flash = capabilities["digituniverse06711.flash"]
  if cap_flash then
    emit_event(device, comp_diag, cap_flash.flash({ value = flash_str }))
  end

  -- ═══════════════════════════════════════════════════════════════════════════
  -- CARD 3: Logs (진단 로그 & 코어 덤프 - 네트워크 바로 위)
  -- ═══════════════════════════════════════════════════════════════════════════

  -- 3-0. Switch (진단 로그 초기화 스위치) 🟦 대표 파란 헤더
  emit_event(device, comp_logs, capabilities.switch.switch.off())

  -- 3-1. Log History (20:12, Low Heap 또는 Empty)
  local log_str = "Empty"
  if data.diagnostics and data.diagnostics.reboot_logs and #data.diagnostics.reboot_logs > 0 then
    local top = data.diagnostics.reboot_logs[1]
    local t = top.time or ""
    local time_hm = t:match("%s(%d%d:%d%d)") or t:match("(%d%d:%d%d)") or t
    local reason_short = top.reason or "Unknown"
    if reason_short:match("Low Heap") then
      reason_short = "Low Heap"
    elseif reason_short:match("Software Restart") then
      reason_short = "Software Restart"
    elseif reason_short:match("Remote Reboot") then
      reason_short = "Remote Reboot"
    elseif reason_short:match("Power") then
      reason_short = "Power On"
    end
    if time_hm ~= "" then
      log_str = string.format("%s, %s", time_hm, reason_short)
    else
      log_str = reason_short
    end
  end
  local cap_rlog = capabilities["digituniverse06711.logHistory"]
  if cap_rlog then
    emit_event(device, comp_logs, cap_rlog.history({ value = log_str }))
  end

  -- 3-2. Crash Dump (Empty 또는 Panic Task ...)
  local cd_str = "Empty"
  if data.diagnostics and data.diagnostics.coredump and data.diagnostics.coredump.valid then
    local cd = data.diagnostics.coredump
    cd_str = string.format("Panic Task %s", cd.task or "main")
  end
  local cap_cd = capabilities["digituniverse06711.crashDump"]
  if cap_cd then
    emit_event(device, comp_logs, cap_cd.details({ value = cd_str }))
  end

  -- ═══════════════════════════════════════════════════════════════════════════
  -- CARD 4: Network (네트워크 정보 & WiFi 스캔)
  -- ═══════════════════════════════════════════════════════════════════════════

  -- 4-0. Switch (WiFi 스캔 스위치) 🟦 대표 파란 헤더
  emit_event(device, comp_net, capabilities.switch.switch.off())

  -- 4-1. Scan (Ready / N APs Found)
  local last_scan = device:get_field("last_scan_result") or "Ready"
  local cap_scan = capabilities["digituniverse06711.scan"]
  if cap_scan then
    emit_event(device, comp_net, cap_scan.scanResult({ value = last_scan }))
  end

  -- 4-2. WiFi (<SSID> (<Quality>%))
  local wifi_data = data.wifi or {}
  local ssid = (wifi_data.ssid and wifi_data.ssid ~= "") and wifi_data.ssid or "Connected"
  local rssi = wifi_data.rssi or (sys and sys.wifi_rssi)
  local wifi_display = ssid
  if rssi then
    local lqi = math.floor(math.min(100, math.max(0, 2 * (rssi + 100))))
    wifi_display = string.format("%s (%d%%)", ssid, lqi)
  else
    wifi_display = string.format("%s (70%%)", ssid)
  end
  local cap_winfo = capabilities["digituniverse06711.wifi"]
  if cap_winfo then
    emit_event(device, comp_net, cap_winfo.wifiInfo({ value = wifi_display }))
  end

  -- 4-3. Mode (STA (172.30.1.3) 또는 STA + AP (172.30.2.1))
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
  local cap_wmode = capabilities["digituniverse06711.mode"]
  if cap_wmode then
    emit_event(device, comp_net, cap_wmode.wifiMode({ value = mode_str }))
  end

  -- ═══════════════════════════════════════════════════════════════════════════
  -- CARD 5: Firmware & OTA (펌웨어 관리 & OTA)
  -- ═══════════════════════════════════════════════════════════════════════════

  -- 5-0. Switch (OTA 업데이트 시작 스위치) 🟦 대표 파란 헤더
  emit_event(device, comp_ota, capabilities.switch.switch.off())

  -- 5-1. Version
  local cur_fw = data.system and data.system.firmware
  if cur_fw and cur_fw ~= "" then
    if not cur_fw:match("^v") then cur_fw = "v" .. cur_fw end
  else
    cur_fw = "Unknown"
  end

  local latest_fw = (data.system and data.system.latest_firmware) or (data.ota and data.ota.latest_firmware)
  local fw_display = string.format("%s (Latest)", cur_fw)
  if latest_fw and latest_fw ~= "" then
    if not latest_fw:match("^v") then latest_fw = "v" .. latest_fw end
    if is_newer_version(latest_fw, cur_fw) then
      fw_display = string.format("%s ➔ New %s", cur_fw, latest_fw)
    end
  end

  local cap_fw = capabilities["digituniverse06711.version"]
  if cap_fw then
    emit_event(device, comp_ota, cap_fw.version({ value = fw_display }))
  end

  -- 5-2. Build (Stable (Idle) / Updating 45% 등)
  local stab_str = "Stable"
  if up_s < 120 then
    stab_str = "Pending"
  elseif data.diagnostics and data.diagnostics.coredump and data.diagnostics.coredump.valid then
    stab_str = "Crash 1"
  end

  local ota_str = string.format("%s (Idle)", stab_str)
  if data.ota then
    if data.ota.in_progress then
      ota_str = string.format("Updating %d%%", data.ota.progress_pct or 0)
    elseif data.ota.last_error and data.ota.last_error ~= "" then
      ota_str = string.format("Failed, %s", data.ota.last_error)
    end
  end
  local cap_ostate = capabilities["digituniverse06711.build"]
  if cap_ostate then
    emit_event(device, comp_ota, cap_ostate.build({ value = ota_str }))
  end

  -- 6-3. Updated: YYYY-MM-DD HH:MM 형식
  local cur_time = os.time()
  local boot_epoch = cur_time - up_s
  local date_str = os.date("%Y-%m-%d %H:%M", boot_epoch)
  local cap_udate = capabilities["digituniverse06711.updated"]
  if cap_udate then
    emit_event(device, comp_ota, cap_udate.updated({ value = date_str }))
  end

  log.info("📊 ═══════════════════════════════════════════════════════════════════════")
end

return TelemetryHandler
