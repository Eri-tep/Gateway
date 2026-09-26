local gateway_client = require "gateway_client"
local telemetry_handler = require "telemetry_handler"
local capabilities = require "st.capabilities"
local log = require "log"

local CommandHandlers = {}


local function get_connection_info(device)
  local ip = device.preferences.gatewayIp or "172.30.1.3"
  local port = device.preferences.gatewayPort or 8900
  return ip, port
end

local function get_gateway_ip_port(driver)
  local ip = "172.30.1.3"
  local port = 8900
  for _, d in ipairs(driver:get_devices()) do
    if d.device_network_id == "esp32_wallpad_gateway_ctrl" then
      ip = d.preferences.gatewayIp or ip
      port = tonumber(d.preferences.gatewayPort) or port
      break
    end
  end
  return ip, port
end

local function refresh_telemetry(driver, device)
  local main_gw = nil
  for _, d in ipairs(driver:get_devices()) do
    if d.device_network_id == "esp32_wallpad_gateway_ctrl" then
      main_gw = d
      break
    end
  end

  local target_device = main_gw or device
  local ip, port = get_connection_info(target_device)
  local data, err = gateway_client.get_telemetry(ip, port)
  if data and main_gw then
    telemetry_handler.handle_telemetry(driver, main_gw, data)
  elseif not data then
    log.error("Failed to query gateway telemetry: " .. tostring(err))
  end
end

function CommandHandlers.handle_refresh(driver, device, command)
  log.info(string.format("🔄 [CMD] Refresh requested on: %s", tostring(device.label)))
  local p_key = device.parent_assigned_child_key or ""

  -- 자식 기기 새로고침 처리
  if p_key:match("^dev_") then
    -- 난방인 경우 지원 모드 (난방, 외출, 꺼짐) 갱신
    if device:supports_capability_by_id(capabilities.thermostatMode.ID) then
      local ev = capabilities.thermostatMode.supportedThermostatModes({ "heat", "away", "off" })
      ev.state_change = true
      device:emit_event(ev)
    end

    -- 콘센트인 경우 과거 오염된 누적 전력량 클리어 (비정상 수치 리셋)
    if device:supports_capability_by_id(capabilities.energyMeter.ID) then
      local m_kwh = device:get_field("monthly_energy_kwh") or 0.0
      if m_kwh > 2.0 then
        device:set_field("monthly_energy_kwh", 0.0, { persist = true })
        device:emit_event(capabilities.energyMeter.energy({ value = 0.0, unit = "kWh" }))
        log.info(string.format("🧹 [OUTLET] Cleared contaminated monthly energy (%.3f -> 0.0 kWh)", m_kwh))
      end
    end

    -- 엘리베이터인 경우 대기/상태 복원
    local cap_hist = capabilities["digituniverse06711.history"]
    if cap_hist and p_key:match("^dev_34_") then
      local ev_active = device:get_field("ev_active")
      if not ev_active then
        local ev = cap_hist.history({ value = "대기" })
        ev.state_change = true
        device:emit_event(ev)
      end
    end

    -- 게이트웨이에서 최신 실제 기기 상태 조회하여 즉시 동기화
    local ip, port = get_gateway_ip_port(driver)
    device.thread:call_with_delay(0.1, function()
      local res, _ = gateway_client.get_locked_devices(ip, port)
      if res and res.devices then
        for _, ldev in ipairs(res.devices) do
          local d_id = tonumber(ldev.dev_id) or 0
          local s1 = tonumber(ldev.sub1) or 0
          local s2 = tonumber(ldev.sub2) or 0
          local c_key = string.format("dev_%02X_%d_%d", d_id, s1, s2)
          if c_key == p_key then
            telemetry_handler.handle_device_state_event(driver, ldev)
            break
          end
        end
      end
    end)
    return
  end

  refresh_telemetry(driver, device)
end

function CommandHandlers.handle_switch_on(driver, device, command)
  local p_key = device.parent_assigned_child_key or ""
  if p_key:match("^dev_") then
    return CommandHandlers.handle_child_switch_on(driver, device, command)
  end

  local comp_id = command.component or "diagnostics"
  local comp = device.profile.components[comp_id]
  local ip, port = get_connection_info(device)

  log.info(string.format("🔘 [CMD] Switch ON received on component: %s (Device: %s)", comp_id, tostring(device.label)))
  if comp then
    device:emit_component_event(comp, capabilities.switch.switch.on())
  end

  -- 자식 기기(도어폰)의 문열림 스위치인 경우
  if p_key == "doorphone" or p_key == "doorphone_front" or p_key == "doorphone_lobby" then
    local action = (p_key == "doorphone_lobby" or comp_id == "lobby") and "open_lobby" or "open_front"
    log.info(string.format("🚪 [DOORPHONE] Dispatching 3-step sequence RPC: %s", action))

    -- 부모 게이트웨이 기기 IP/Port 찾기
    local ip = "172.30.1.3"
    local port = 8900
    for _, d in ipairs(driver:get_devices()) do
      if d.device_network_id == "esp32_wallpad_gateway_ctrl" then
        ip = d.preferences.gatewayIp or ip
        port = d.preferences.gatewayPort or port
        break
      end
    end

    local cosock = require "cosock"
    cosock.spawn(function()
      local res, err = gateway_client.doorphone_action(ip, port, action)
      if not res then
        log.error("❌ [DOORPHONE] Doorphone action failed: " .. tostring(err))
      else
        log.info("✅ [DOORPHONE] Doorphone sequence successfully triggered via RPC!")
      end
    end, "doorphone_action_task")

    -- 1.5초 후 스위치 OFF 자동 원복
    device.thread:call_with_delay(1.5, function()
      if comp then
        device:emit_component_event(comp, capabilities.switch.switch.off())
      end
    end)
    return
  end

  local cosock = require "cosock"
  if comp_id == "wallpad" then
    log.info("🔄 [CMD] Auto-Probing Reset triggered from Wallpad switch!")
    cosock.spawn(function()
      gateway_client.cache_purge_rescan(ip, port)
      CommandHandlers.refresh_telemetry(driver, device)
    end, "wallpad_reset_task")
  elseif comp_id == "diagnostics" then
    log.warn("⚠️  [CMD] System Remote Reboot triggered from Diagnostics switch!")
    cosock.spawn(function()
      gateway_client.system_reboot(ip, port, "ST Diagnostics Switch")
    end, "system_reboot_task")
  elseif comp_id == "logs" then
    log.info("🧹 [CMD] Clear Logs triggered from Logs switch!")
    local cap_hist = capabilities["digituniverse06711.history"]
    if cap_hist then
      device:emit_component_event(comp, cap_hist.history({ value = "Empty Log" }))
      telemetry_handler.register_ticker(device, comp, cap_hist, "history", "history", { "Empty Log", "Empty Crash" }, true)
    end
    cosock.spawn(function()
      gateway_client.clear_reboot_logs(ip, port)
      gateway_client.clear_coredump(ip, port)
    end, "clear_logs_task")
  elseif comp_id == "network" then
    log.info("📶 [CMD] Wi-Fi Scan triggered from Network switch!")
    local cap_scan = capabilities["digituniverse06711.scan"]
    if cap_scan then
      device:emit_component_event(comp, cap_scan.scanResult({ value = "Scanning..." }))
    end

    -- 스캔 중에는 마스터 틱 레지스트리에서 scan 항목 임시 제거
    local reg = device:get_field("ticker_registry") or {}
    reg["scan"] = nil
    device:set_field("ticker_registry", reg)

    cosock.spawn(function()
      local res, err = gateway_client.wifi_scan(ip, port)
      local valid_aps = {}

      if res and res.aps and #res.aps > 0 then
        for _, item in ipairs(res.aps) do
          local raw_s = item.ssid or ""
          local s = raw_s:match("^%s*(.-)%s*$")
          if s and s ~= "" then
            local pct = tonumber(item.pct) or 70
            table.insert(valid_aps, { ssid = s, pct = pct })
          end
        end
      end

      if #valid_aps > 0 then
        local scan_items = {}
        for _, ap in ipairs(valid_aps) do
          table.insert(scan_items, string.format("%s (%d%%)", ap.ssid, ap.pct))
        end
        device:set_field("last_scan_result", scan_items[1])
        device:set_field("scan_items", scan_items)
        telemetry_handler.register_ticker(device, comp, cap_scan, "scanResult", "scan", scan_items, true)
      else
        device:set_field("scan_items", nil)
        local fail_text = "No Networks"
        if err then
          if err == "Connection closed" or err == "Gateway connection not ready" then
            log.warn("⚠️ [CMD] Wi-Fi Scan cancelled or interrupted by gateway reconnect: " .. tostring(err))
            fail_text = "Ready"
          else
            log.error("❌ [CMD] Wi-Fi Scan RPC error: " .. tostring(err))
            fail_text = "Scan Timeout"
          end
        elseif res and res.msg then
          fail_text = res.msg
        end
        if cap_scan then
          device:emit_component_event(comp, cap_scan.scanResult({ value = fail_text }))
          device:set_field("last_scan_result", fail_text)
        end
      end
      CommandHandlers.refresh_telemetry(driver, device)
    end, "wifi_scan_task")
  elseif comp_id == "ota" then
    log.info("🚀 [CMD] Cloud OTA Update triggered from OTA switch!")
    CommandHandlers.handle_start_ota(driver, device, command)
  end

  -- 1.5초 후 스위치 OFF 자동 원복 (원터치 펄스 스위치)
  device.thread:call_with_delay(1.5, function()
    if comp then
      device:emit_component_event(comp, capabilities.switch.switch.off())
    end
  end)
end

function CommandHandlers.handle_switch_off(driver, device, command)
  local p_key = device.parent_assigned_child_key or ""
  if p_key:match("^dev_") then
    return CommandHandlers.handle_child_switch_off(driver, device, command)
  end

  local comp_id = command.component or "diagnostics"
  local comp = device.profile.components[comp_id]
  if comp then
    device:emit_component_event(comp, capabilities.switch.switch.off())
  end
end

-- ============================================================================
-- Cloud OTA 핸들러 (GitHub Raw 바이너리 다운로드 및 무중단 적용)
-- ============================================================================

function CommandHandlers.handle_start_ota(driver, device, command)
  local ip, port = get_connection_info(device)
  local repo = device.preferences.githubRepo or "Eri-tep/Gateway"
  local channel = (device.preferences.otaChannel or device:get_field("ota_channel") or "beta"):lower()
  local branch = (channel == "main") and "main" or "beta"

  -- [핵심] 설정의 githubRepo 및 otaChannel을 참조하여 실제 유효한 GitHub Raw 바이너리 전체 HTTPS URL 생성 및 전송
  local ota_url = string.format("https://raw.githubusercontent.com/%s/%s/bin/firmware.bin", repo, branch)

  -- OTA 시작 UI 즉시 업데이트 (Downloading...)
  local comp_ota = device.profile.components["ota"]
  local cap_ostate = capabilities["digituniverse06711.build"]
  if comp_ota and cap_ostate then
    device:emit_component_event(comp_ota, cap_ostate.build({ value = "Updating... (Downloading)" }))
  end

  -- [핵심] ESP32 힙 메모리 고갈(low heap) 방지를 위해 기존 주기적 폴링 타이머 일시 취소
  if device:get_field("poll_timer") then
    device.thread:cancel_timer(device:get_field("poll_timer"))
    device:set_field("poll_timer", nil)
    log.info("⏸️ [OTA] Paused periodic polling timer to preserve ESP32 Heap memory")
  end

  log.info(string.format("🚀 [OTA] Triggering Cloud OTA -> Target URL: %s (Repo: %s, Channel: %s)", ota_url, repo, branch))
  gateway_client.start_ota(ip, port, ota_url)

  -- ESP32가 TLS 연결 및 펌웨어 다운로드/플래시/재부팅을 무사히 마칠 때까지 일체 폴링하지 않음
  -- 25초 후 1회 상태 확인 및 정규 주기적 폴링 타이머 복구
  device.thread:call_with_delay(25, function()
    log.info("▶️ [OTA] OTA window finished, refreshing status and resuming periodic polling")
    refresh_telemetry(driver, device)

    -- 주기적 폴링 타이머 재등록
    local interval = device.preferences.pollingInterval or 30
    if interval < 5 then interval = 5 end
    local timer = device.thread:call_on_schedule(interval, function()
      refresh_telemetry(driver, device)
    end, "gateway_poll_timer")
    device:set_field("poll_timer", timer)
  end, "ota_resume_timer")
end

-- ============================================================================
-- Child Device Manager 드롭다운 액션 핸들러 (Add / Remove / Idle)
-- ============================================================================

local CHILD_FRONT_KEY = "doorphone_front"
local CHILD_LOBBY_KEY = "doorphone_lobby"

function CommandHandlers.handle_child_device_action(driver, device, command)
  local action = (command.args and (command.args.action or command.args[1])) or "idle"
  local comp_main = device.profile.components["main"]
  local cap_mgr = capabilities["digituniverse06711.deviceManager"]

  log.info(string.format("🎛️ [CMD] Device Manager Action requested: %s", tostring(action)))

  if cap_mgr and comp_main then
    device:emit_component_event(comp_main, cap_mgr.action({ value = action }))
  end

  if action == "add" then
    local front_exists = false
    local lobby_exists = false

    for _, dev in ipairs(driver:get_devices()) do
      local p_key = dev.parent_assigned_child_key
      if p_key == CHILD_FRONT_KEY or dev.label == "세대 도어 (현관)" then
        front_exists = true
      elseif p_key == CHILD_LOBBY_KEY or dev.label == "로비 도어 (공동현관)" then
        lobby_exists = true
      end
    end

    if not front_exists then
      log.info("🚪 [CHILD] Creating '세대 도어' Child Device...")
      local success, err = driver:try_create_device({
        type = "EDGE_CHILD",
        label = "세대 도어",
        profile = "single-door-device",
        parent_device_id = device.id,
        parent_assigned_child_key = CHILD_FRONT_KEY
      })
      if not success then
        log.error("❌ [CHILD] Failed to create front door child: " .. tostring(err))
      end
    else
      log.info("ℹ️ [CHILD] Front door child device already exists")
    end

    if not lobby_exists then
      log.info("🚪 [CHILD] Creating '로비 도어' Child Device...")
      local success, err = driver:try_create_device({
        type = "EDGE_CHILD",
        label = "로비 도어",
        profile = "single-door-device",
        parent_device_id = device.id,
        parent_assigned_child_key = CHILD_LOBBY_KEY
      })
      if not success then
        log.error("❌ [CHILD] Failed to create lobby door child: " .. tostring(err))
      end
    else
      log.info("ℹ️ [CHILD] Lobby door child device already exists")
    end

    -- ★ 게이트웨이에서 학습 및 LOCK된 기기 목록 동적 조회 및 자동 생성
    local ip = device.preferences.gatewayIp or "172.30.1.3"
    local port = tonumber(device.preferences.gatewayPort) or 8900
    log.info(string.format("🔍 [CHILD] Fetching LOCKED devices from Gateway %s:%d...", ip, port))
    local res, err = gateway_client.get_locked_devices(ip, port)

    if res and res.devices and #res.devices > 0 then
      log.info(string.format("📦 [CHILD] Found %d LOCKED devices on Gateway! Syncing...", #res.devices))
      for _, ldev in ipairs(res.devices) do
        local d_id = tonumber(ldev.dev_id) or 0
        local s1 = tonumber(ldev.sub1) or 0
        local s2 = tonumber(ldev.sub2) or 0
        local d_cls = ldev.class or "switch"
        local d_name = ldev.name or string.format("Device %02X-%d-%d", d_id, s1, s2)
        local child_key = string.format("dev_%02X_%d_%d", d_id, s1, s2)

        local exists = false
        for _, ex_dev in ipairs(driver:get_devices()) do
          if ex_dev.parent_assigned_child_key == child_key then
            exists = true
            break
          end
        end

        if not exists then
          local prof = "child-switch"
          if d_cls == "outlet" then
            prof = "child-outlet"
          elseif d_cls == "thermostat" then
            prof = "child-thermostat"
          elseif d_cls == "vent" then
            prof = "child-vent"
          elseif d_cls == "gas" then
            prof = "child-gas"
          elseif d_cls == "momentary" then
            prof = "child-momentary"
          end

          log.info(string.format("✨ [CHILD] Creating Device '%s' (%s) with key '%s' [Profile: %s]...",
                                 d_name, d_cls, child_key, prof))
          local success, c_err = driver:try_create_device({
            type = "EDGE_CHILD",
            label = d_name,
            profile = prof,
            parent_device_id = device.id,
            parent_assigned_child_key = child_key
          })
          if not success then
            log.error(string.format("❌ [CHILD] Failed to create child device '%s': %s", d_name, tostring(c_err)))
          end
        else
          log.info(string.format("ℹ️ [CHILD] Device '%s' (%s) already exists", child_key, d_name))
        end
      end

      -- 자식 기기 생성 및 등록 후 게이트웨이의 최신 실제 상태를 즉시 동기화
      device.thread:call_with_delay(0.8, function()
        log.info("🔄 [CHILD] Applying initial states for all child devices from Gateway...")
        for _, ldev in ipairs(res.devices) do
          telemetry_handler.handle_device_state_event(driver, ldev)
        end
      end)
    elseif err then
      log.error("❌ [CHILD] Failed to fetch locked devices: " .. tostring(err))
    else
      log.info("ℹ️ [CHILD] No LOCKED devices found on Gateway yet.")
    end

    -- 2.0초 후 자동으로 Idle 복귀
    device.thread:call_with_delay(2.0, function()
      if cap_mgr and comp_main then
        device:emit_component_event(comp_main, cap_mgr.action({ value = "idle" }))
      end
    end)
  elseif action == "remove" then
    log.info("🗑️ [CHILD] Removing All Child Devices...")
    local targets = {}
    for _, dev in ipairs(driver:get_devices()) do
      local p_key = dev.parent_assigned_child_key or ""
      if p_key == "doorphone" or p_key == CHILD_FRONT_KEY or p_key == CHILD_LOBBY_KEY or
         p_key:match("^dev_") or
         dev.label == "도어폰" or dev.label == "세대 도어" or dev.label == "로비 도어" then
        table.insert(targets, { id = dev.id, label = dev.label, key = p_key })
      end
    end

    log.info(string.format("🗑️ [CHILD] Found %d child devices to delete", #targets))
    for i, t in ipairs(targets) do
      local delay = (i - 1) * 0.15
      device.thread:call_with_delay(delay, function()
        log.info(string.format("🗑️ [CHILD] Deleting device (%d/%d): %s (ID: %s, Key: %s)", i, #targets, tostring(t.label), tostring(t.id), tostring(t.key)))
        local ok, del_err = pcall(function()
          driver:try_delete_device(t.id)
        end)
        if not ok then
          log.error("❌ [CHILD] Failed to delete device " .. tostring(t.id) .. ": " .. tostring(del_err))
        end
      end)
    end

    -- 모든 기기 삭제 완료 후 자동으로 Idle 복귀
    local total_wait = math.max(2.0, (#targets * 0.15) + 1.0)
    device.thread:call_with_delay(total_wait, function()
      if cap_mgr and comp_main then
        device:emit_component_event(comp_main, cap_mgr.action({ value = "idle" }))
      end
    end)
  end
end

-- ============================================================================
-- Doorphone Momentary Push 핸들러 (자식 기기 문열림 제어)
-- ============================================================================

function CommandHandlers.handle_momentary_push(driver, device, command)
  local p_key = device.parent_assigned_child_key or ""
  if p_key:match("^dev_") then
    return CommandHandlers.handle_child_momentary_push(driver, device, command)
  end

  local comp_id = command.component or "main"
  local comp = device.profile.components[comp_id]
  log.info(string.format("🚪 [DOORPHONE] Door Open requested on component: %s", comp_id))

  -- 부모(게이트웨이) 기기 찾기 (driver 내 LAN 메인 기기 검색)
  local ip = "172.30.1.3"
  local port = 8900

  for _, d in ipairs(driver:get_devices()) do
    if d.device_network_id == "esp32_wallpad_gateway_ctrl" then
      ip = d.preferences.gatewayIp or ip
      port = d.preferences.gatewayPort or port
      break
    end
  end

  local action = (comp_id == "lobby") and "open_lobby" or "open_front"
  log.info(string.format("🚪 [DOORPHONE] Dispatching 3-step sequence RPC: %s to %s:%d", action, ip, port))
  local res, err = gateway_client.doorphone_action(ip, port, action)
  if not res then
    log.error("❌ [DOORPHONE] Doorphone action failed: " .. tostring(err))
  else
    log.info("✅ [DOORPHONE] Doorphone sequence successfully triggered via RPC!")
  end
end

-- ============================================================================
-- 동적 자식 기기(Child Devices) 제어 핸들러 (스위치/난방/환기/가스/엘리베이터)
-- ============================================================================

local function parse_child_key(key)
  if not key then return nil end
  local hex_id, s1, s2 = key:match("^dev_([0-9A-Fa-f]+)_(%d+)_(%d+)$")
  if hex_id and s1 and s2 then
    return tonumber(hex_id, 16), tonumber(s1), tonumber(s2)
  end
  return nil
end

local function handle_momentary_switch_on(device, ip, port, d_id, s1, s2)
  device:set_field("ev_active", true)
  local cap_hist = capabilities["digituniverse06711.history"]
  if cap_hist then
    local h_evt = cap_hist.history({ value = "호출 중" })
    h_evt.state_change = true
    device:emit_event(h_evt)
  end
  gateway_client.device_control(ip, port, d_id, s1, s2, "momentary", 1)
end

local function handle_momentary_switch_off(device)
  device:set_field("ev_active", false)
  local cur_timer = device:get_field("ev_arrival_timer")
  if cur_timer then
    device.thread:cancel_timer(cur_timer)
    device:set_field("ev_arrival_timer", nil)
  end
  local cap_hist = capabilities["digituniverse06711.history"]
  if cap_hist then
    local h_evt = cap_hist.history({ value = "대기 중" })
    h_evt.state_change = true
    device:emit_event(h_evt)
  end
end

function CommandHandlers.handle_child_switch_on(driver, device, command)
  local d_id, s1, s2 = parse_child_key(device.parent_assigned_child_key)
  if not d_id then return end
  local ip, port = get_gateway_ip_port(driver)
  log.info(string.format("💡 [CHILD CMD] %s ON -> DevID 0x%02X (%d-%d)", device.label, d_id, s1, s2))

  device:emit_event(capabilities.switch.switch.on())

  local p_key = device.parent_assigned_child_key or ""
  if p_key:match("^dev_34_") or device:supports_capability_by_id(capabilities.momentary.ID) then
    handle_momentary_switch_on(device, ip, port, d_id, s1, s2)
  else
    local cap_vent = capabilities["digituniverse06711.ventmode"]
    local is_vent = cap_vent and device:supports_capability_by_id(cap_vent.ID)
    if is_vent then
      pcall(function()
        device:emit_event(cap_vent.ventMode({ value = "normal" }))
      end)
    end
    local cap_speed = capabilities["digituniverse06711.ventspeed"]
    if cap_speed and device:supports_capability_by_id(cap_speed.ID) then
      device:emit_event(cap_speed.ventSpeed("low"))
    end
    if device:supports_capability_by_id(capabilities.fanSpeed.ID) then
      device:emit_event(capabilities.fanSpeed.fanSpeed(1))
    end
    gateway_client.device_control(ip, port, d_id, s1, s2, "power", 1)
  end
end

function CommandHandlers.handle_child_switch_off(driver, device, command)
  local d_id, s1, s2 = parse_child_key(device.parent_assigned_child_key)
  if not d_id then return end
  local ip, port = get_gateway_ip_port(driver)
  log.info(string.format("💡 [CHILD CMD] %s OFF -> DevID 0x%02X (%d-%d)", device.label, d_id, s1, s2))

  device:emit_event(capabilities.switch.switch.off())

  local p_key = device.parent_assigned_child_key or ""
  if p_key:match("^dev_34_") or device:supports_capability_by_id(capabilities.momentary.ID) then
    handle_momentary_switch_off(device)
  else
    if device:supports_capability_by_id(capabilities.fanSpeed.ID) then
      device:emit_event(capabilities.fanSpeed.fanSpeed(0))
    end
    gateway_client.device_control(ip, port, d_id, s1, s2, "power", 0)
  end
end

function CommandHandlers.handle_child_set_heating_setpoint(driver, device, command)
  local d_id, s1, s2 = parse_child_key(device.parent_assigned_child_key)
  if not d_id then return end
  -- SmartThings 표준 capability는 command.args.setpoint 또는 command.args.heatingSetpoint 로 전달됨
  local raw_temp = (command.args and (command.args.setpoint or command.args.heatingSetpoint))
  if not raw_temp and command.positional_args and #command.positional_args > 0 then
    raw_temp = command.positional_args[1]
  end
  local temp = math.floor((tonumber(raw_temp) or 22) + 0.5)
  local ip, port = get_gateway_ip_port(driver)
  log.info(string.format("🔥 [CHILD CMD] %s SetTemp -> %dC (DevID 0x%02X %d-%d)", device.label, temp, d_id, s1, s2))

  local sp_evt = capabilities.thermostatHeatingSetpoint.heatingSetpoint({ value = temp, unit = "C" })
  sp_evt.state_change = true
  device:emit_event(sp_evt)

  device:set_field("last_thermo_temp", temp, { persist = true })
  gateway_client.device_control(ip, port, d_id, s1, s2, "set_temp", temp)
end

function CommandHandlers.handle_child_set_thermostat_mode(driver, device, command)
  local d_id, s1, s2 = parse_child_key(device.parent_assigned_child_key)
  if not d_id then return end
  local mode = (command.args and command.args.mode) or "off"
  local pwr = 0
  if mode == "heat" then
    pwr = 1
  elseif mode == "away" or mode == "eco" then
    pwr = 2
  end
  local ip, port = get_gateway_ip_port(driver)
  log.info(string.format("🔥 [CHILD CMD] %s SetMode -> %s (pwr=%d, DevID 0x%02X %d-%d)", device.label, mode, pwr, d_id, s1, s2))
  device:emit_event(capabilities.thermostatMode.thermostatMode(mode))

  -- 난방(heat) 모드로 켤 때, 이전에 저장된 희망온도를 즉시 UI에 복원 방출
  if mode == "heat" and capabilities.thermostatHeatingSetpoint then
    local saved_temp = device:get_field("last_thermo_temp") or 22
    local sp_evt = capabilities.thermostatHeatingSetpoint.heatingSetpoint({ value = saved_temp, unit = "C" })
    sp_evt.state_change = true
    device:emit_event(sp_evt)
  end

  gateway_client.device_control(ip, port, d_id, s1, s2, "power", pwr)
end

local VENT_MODE_MAP = {
  ["normal"] = 1,
  ["general"] = 1,
  ["1"] = 1,
  ["bypass"] = 2,
  ["natural"] = 2,
  ["2"] = 2,
  ["auto"] = 3,
  ["automatic"] = 3,
  ["3"] = 3,
  ["clean"] = 4,
  ["airclean"] = 4,
  ["4"] = 4
}

local VENT_MODE_NAMES = {
  [1] = "normal",
  [2] = "bypass",
  [3] = "auto",
  [4] = "clean"
}

function CommandHandlers.handle_child_set_vent_mode(driver, device, command)
  local d_id, s1, s2 = parse_child_key(device.parent_assigned_child_key)
  if not d_id then return end

  local raw_mode = nil
  if command.args then
    raw_mode = command.args.mode or (type(command.args) == "table" and command.args[1])
  end
  if not raw_mode and command.positional_args and #command.positional_args > 0 then
    raw_mode = command.positional_args[1]
  end
  raw_mode = raw_mode or "normal"

  local mode_val = VENT_MODE_MAP[string.lower(tostring(raw_mode))] or 1
  local mode_str = VENT_MODE_NAMES[mode_val] or "normal"

  local ip, port = get_gateway_ip_port(driver)
  log.info(string.format("🌀 [CHILD CMD] %s SetVentMode -> %s (val=%d) (DevID 0x%02X %d-%d)", device.label, mode_str, mode_val, d_id, s1, s2))

  local cap_vent = capabilities["digituniverse06711.ventmode"]
  if cap_vent then
    pcall(function()
      device:emit_event(cap_vent.ventMode({ value = mode_str }))
    end)
  end

  local sw_on = capabilities.switch.switch.on()
  sw_on.state_change = true
  device:emit_event(sw_on)

  gateway_client.device_control(ip, port, d_id, s1, s2, "vent_mode", mode_val)
end

function CommandHandlers.handle_child_set_vent_speed(driver, device, command)
  local d_id, s1, s2 = parse_child_key(device.parent_assigned_child_key)
  if not d_id then return end

  local raw_speed = nil
  if command.args then
    raw_speed = command.args.speed or (type(command.args) == "table" and command.args[1])
  end
  if not raw_speed and command.positional_args and #command.positional_args > 0 then
    raw_speed = command.positional_args[1]
  end
  raw_speed = raw_speed or "low"

  local speed_map = { ["low"] = 1, ["medium"] = 2, ["high"] = 3 }
  local spd = speed_map[string.lower(tostring(raw_speed))] or 1
  local speed_names = { [1] = "low", [2] = "medium", [3] = "high" }
  local speed_str = speed_names[spd] or "low"

  local ip, port = get_gateway_ip_port(driver)
  log.info(string.format("🌀 [CHILD CMD] %s SetVentSpeed -> %s (val=%d) (DevID 0x%02X %d-%d)", device.label, speed_str, spd, d_id, s1, s2))

  local cap_speed = capabilities["digituniverse06711.ventspeed"]
  if cap_speed then
    device:emit_event(cap_speed.ventSpeed(speed_str))
  end

  local sw_on = capabilities.switch.switch.on()
  sw_on.state_change = true
  device:emit_event(sw_on)

  gateway_client.device_control(ip, port, d_id, s1, s2, "fan_speed", spd)
end

function CommandHandlers.handle_child_set_fan_speed(driver, device, command)
  local d_id, s1, s2 = parse_child_key(device.parent_assigned_child_key)
  if not d_id then return end
  local spd = tonumber(command.args.speed) or 1
  local ip, port = get_gateway_ip_port(driver)
  log.info(string.format("🌀 [CHILD CMD] %s SetFanSpeed -> %d (DevID 0x%02X %d-%d)", device.label, spd, d_id, s1, s2))

  if capabilities.fanSpeed then
    device:emit_event(capabilities.fanSpeed.fanSpeed(spd))
  end

  local sw_on = capabilities.switch.switch.on()
  sw_on.state_change = true
  device:emit_event(sw_on)

  gateway_client.device_control(ip, port, d_id, s1, s2, "fan_speed", spd)
end

function CommandHandlers.handle_child_valve_close(driver, device, command)
  local d_id, s1, s2 = parse_child_key(device.parent_assigned_child_key)
  if not d_id then return end
  local ip, port = get_gateway_ip_port(driver)
  log.info(string.format("🔒 [CHILD CMD] %s CLOSE -> DevID 0x%02X (%d-%d)", device.label, d_id, s1, s2))
  device:emit_event(capabilities.valve.valve.closed())
  gateway_client.device_control(ip, port, d_id, s1, s2, "valve_close", 0)
end

function CommandHandlers.handle_child_momentary_push(driver, device, command)
  local d_id, s1, s2 = parse_child_key(device.parent_assigned_child_key)
  if not d_id then return end
  local ip, port = get_gateway_ip_port(driver)
  log.info(string.format("🛗 [CHILD CMD] %s PUSH -> DevID 0x%02X (%d-%d)", device.label, d_id, s1, s2))
  device:set_field("ev_active", true)
  device:set_field("ev_call_ts", os.time())
  device:set_field("ev_arrived_until", nil)
  device:emit_event(capabilities.switch.switch.on())
  local cap_hist = capabilities["digituniverse06711.history"]
  if cap_hist then
    local h_evt = cap_hist.history({ value = "호출 중" })
    h_evt.state_change = true
    device:emit_event(h_evt)
  end
  gateway_client.device_control(ip, port, d_id, s1, s2, "momentary", 1)
end


CommandHandlers.refresh_telemetry = refresh_telemetry

return CommandHandlers
