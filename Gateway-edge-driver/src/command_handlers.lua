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

local function refresh_telemetry(driver, device)
  local ip, port = get_connection_info(device)
  local data, err = gateway_client.get_telemetry(ip, port)
  if data then
    telemetry_handler.handle_telemetry(driver, device, data)
  else
    log.error("Failed to query gateway telemetry: " .. tostring(err))
  end
end

function CommandHandlers.handle_refresh(driver, device, command)
  log.info("🔄 [CMD] Safe Telemetry Refresh requested")
  refresh_telemetry(driver, device)
end

function CommandHandlers.handle_switch_on(driver, device, command)
  local comp_id = command.component or "diagnostics"
  local comp = device.profile.components[comp_id]
  local ip, port = get_connection_info(device)

  log.info(string.format("🔘 [CMD] Switch ON received on component: %s", comp_id))
  if comp then
    device:emit_component_event(comp, capabilities.switch.switch.on())
  end

  if comp_id == "wallpad" then
    log.info("🔄 [CMD] Auto-Probing Reset triggered from Wallpad switch!")
    gateway_client.cache_purge_rescan(ip, port)
    CommandHandlers.refresh_telemetry(driver, device)
  elseif comp_id == "diagnostics" then
    log.warn("⚠️  [CMD] System Remote Reboot triggered from Diagnostics switch!")
    gateway_client.system_reboot(ip, port, "ST Diagnostics Switch")
  elseif comp_id == "logs" then
    log.info("🧹 [CMD] Clear Logs triggered from Logs switch!")
    gateway_client.clear_reboot_logs(ip, port)
    gateway_client.clear_coredump(ip, port)
    local cap_hist = capabilities["digituniverse06711.history"]
    if cap_hist then
      device:emit_component_event(comp, cap_hist.history({ value = "Empty Log" }))
      telemetry_handler.register_ticker(device, comp, cap_hist, "history", "history", { "Empty Log", "Empty Crash" }, true)
    end
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
        log.error("❌ [CMD] Wi-Fi Scan RPC error: " .. tostring(err))
        fail_text = "Scan Timeout"
      elseif res and res.msg then
        fail_text = res.msg
      end
      if cap_scan then
        device:emit_component_event(comp, cap_scan.scanResult({ value = fail_text }))
        device:set_field("last_scan_result", fail_text)
      end
    end
    CommandHandlers.refresh_telemetry(driver, device)
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

CommandHandlers.refresh_telemetry = refresh_telemetry

return CommandHandlers
