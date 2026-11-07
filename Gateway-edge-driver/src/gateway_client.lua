local socket = require "cosock.socket"
local cosock = require "cosock"
local json = require "st.json"
local log = require "log"

local GatewayClient = {}

local persistent_tcp = nil
local pending_requests = {} -- [req_id] = cosock.channel
local next_req_id = 1
local DEFAULT_TIMEOUT_SEC = 5

function GatewayClient.send_rpc(ip, port, cmd_table, timeout)
  local sock = persistent_tcp
  if not sock then
    log.warn("⚠️ [RPC SEND] persistent_tcp not connected yet")
    return nil, "Gateway connection not ready"
  end

  timeout = timeout or DEFAULT_TIMEOUT_SEC

  -- Monotonically increasing request ID with safe 32-bit wrap
  next_req_id = (next_req_id >= 2147483640) and 1 or (next_req_id + 1)
  local cur_req_id = next_req_id
  cmd_table.id = cur_req_id

  local tx, rx = cosock.channel.new()
  pending_requests[cur_req_id] = tx

  local ok_enc, encoded = pcall(json.encode, cmd_table)
  if not ok_enc then
    log.warn(string.format("⚠️ [RPC SEND] json.encode failed [ID:%d]: %s", cur_req_id, tostring(encoded)))
    pending_requests[cur_req_id] = nil
    return nil, "json encode error: " .. tostring(encoded)
  end
  local payload = encoded .. "\n"
  log.debug(string.format("🌐 [RPC SEND] (Single Session) [ID:%d] -> %s:%d | %s", cur_req_id, ip, port, payload:sub(1, -2)))

  local ok_send, sent, send_err = pcall(function() return sock:send(payload) end)
  if not ok_send or not sent then
    local err_msg = tostring(send_err or sent)
    log.warn(string.format("⚠️ [RPC SEND] Send failed [ID:%d]: %s", cur_req_id, err_msg))
    pending_requests[cur_req_id] = nil
    return nil, "Send failed: " .. err_msg
  end

  -- Event-driven zero-delay wait via cosock channel
  local resp, recv_err = rx:receive(timeout)
  pending_requests[cur_req_id] = nil

  if not resp then
    return nil, recv_err or "RPC response timeout"
  end

  if type(resp) == "table" then
    if resp.error then
      return nil, resp.error
    end
    return resp, nil
  end

  return nil, "Invalid response"
end

--- Checks if the persistent management TCP connection is alive
function GatewayClient.is_connected()
  return persistent_tcp ~= nil
end

--- Non-blocking Fast-Path: Fire-and-Forget send (Zero RTT latency for UI actions)
function GatewayClient.send_fast_path(cmd_table)
  local sock = persistent_tcp
  if not sock then
    log.debug("ℹ️ [FAST PATH] persistent_tcp not connected yet")
    return false, "Gateway connection not ready"
  end
  local ok_enc, encoded = pcall(json.encode, cmd_table)
  if not ok_enc then
    log.warn(string.format("⚠️ [FAST PATH] json.encode failed: %s", tostring(encoded)))
    return false, "json encode error: " .. tostring(encoded)
  end
  local payload = encoded .. "\n"
  local ok_send, sent, err = pcall(function() return sock:send(payload) end)
  if not ok_send or not sent then
    local err_msg = tostring(err or sent)
    log.warn(string.format("⚠️ [FAST PATH] Send failed: %s", err_msg))
    return false, err_msg
  end
  return true
end

function GatewayClient.get_telemetry(ip, port)
  return GatewayClient.send_rpc(ip, port, { cmd = "get_telemetry" })
end

function GatewayClient.cache_purge_rescan(ip, port)
  return GatewayClient.send_rpc(ip, port, { cmd = "cache_purge_rescan" })
end


function GatewayClient.set_profile(ip, port, slot)
  return GatewayClient.send_rpc(ip, port, { cmd = "set_profile", slot = slot })
end

function GatewayClient.set_wifi_mode(ip, port, mode)
  return GatewayClient.send_rpc(ip, port, { cmd = "set_wifi_mode", mode = mode })
end

function GatewayClient.set_wifi(ip, port, ssid, password)
  return GatewayClient.send_rpc(ip, port, { cmd = "set_wifi", ssid = ssid, password = password })
end

function GatewayClient.set_timing(ip, port, ch1_poll_intvl, ch2_delay, ch3_delay)
  local params = { cmd = "set_timing" }
  if ch1_poll_intvl then params.ch1_poll_intvl = ch1_poll_intvl end
  if ch2_delay then params.ch2_delay = ch2_delay end
  if ch3_delay then params.ch3_delay = ch3_delay end
  return GatewayClient.send_rpc(ip, port, params)
end

function GatewayClient.clear_coredump(ip, port)
  return GatewayClient.send_rpc(ip, port, { cmd = "clear_coredump" })
end

function GatewayClient.clear_reboot_logs(ip, port)
  return GatewayClient.send_rpc(ip, port, { cmd = "clear_reboot_logs" })
end

function GatewayClient.start_ota(ip, port, url)
  return GatewayClient.send_rpc(ip, port, {
    cmd = "start_ota",
    url = url
  })
end

function GatewayClient.wifi_scan(ip, port)
  return GatewayClient.send_rpc(ip, port, {
    cmd = "wifi_scan"
  }, 20)
end

function GatewayClient.system_reboot(ip, port, reason)
  return GatewayClient.send_rpc(ip, port, {
    cmd = "system_reboot",
    reason = reason or "ST Switch Remote Reboot"
  })
end

function GatewayClient.set_uart_config(ip, port, ch, baud, format)
  return GatewayClient.send_rpc(ip, port, {
    cmd = "set_uart",
    ch = ch,
    baud = baud,
    format = format
  })
end

function GatewayClient.doorphone_action(ip, port, action)
  return GatewayClient.send_rpc(ip, port, {
    cmd = "doorphone_action",
    action = action
  })
end

function GatewayClient.get_devices(ip, port)
  return GatewayClient.send_rpc(ip, port, {
    cmd = "get_devices"
  })
end

function GatewayClient.device_control(ip, port, dev_id, sub1, sub2, action, value)
  local payload = {
    c = "ctl",
    d = dev_id,
    s1 = sub1,
    s2 = sub2,
    a = action,
    v = value
  }

  if persistent_tcp then
    return GatewayClient.send_fast_path(payload)
  else
    return GatewayClient.send_rpc(ip, port, payload)
  end
end

function GatewayClient.device_control_custom(ip, port, payload)
  if persistent_tcp then
    return GatewayClient.send_fast_path(payload)
  else
    return GatewayClient.send_rpc(ip, port, payload)
  end
end


--- CH6 (8900) 실시간 푸시 이벤트 리스너 (백그라운드 지속 소켓 + 클라이언트 Heartbeat Ping + RPC 공유)
function GatewayClient.start_event_listener(driver, ip, port, on_event_cb, on_connected_cb)
  if not ip or not port then return end

  listener_running = true

  cosock.spawn(function()
    log.info(string.format("📡 [CH6 PUSH] Starting single persistent session on %s:%d", ip, port))
    local backoff_sec = 1
    while listener_running do
      local tcp = socket.tcp()
      if tcp then
        tcp:settimeout(5) -- [H-7] 연결 시도 타임아웃 5초
        local ok, conn_err = tcp:connect(ip, port)
        if ok then
          backoff_sec = 1 -- 연결 성공 시 백오프 리셋
          pcall(function() tcp:setoption("tcp-nodelay", true) end)
          tcp:settimeout(35) -- [H-7] 수신 타임아웃 35초 (15초 핑 주기 고려)
          log.info(string.format("✅ [CH6 PUSH] Connected to Gateway %s:%d (TCP_NODELAY Active)", ip, port))
          persistent_tcp = tcp
          local is_alive = true

          -- [On-Connect Immediate Refresh] 연결 수립 즉시 텔레메트리 1회 즉시 동기화 트리거
          if on_connected_cb then
            cosock.spawn(function()
              pcall(on_connected_cb, driver)
            end, "on_connected_sync")
          end

          -- [Heartbeat Ping Worker] 15초마다 게이트웨이로 Ping 전송하여 세션 활성화 유지 및 끊김 감지
          cosock.spawn(function()
            local ping_fail_count = 0
            while is_alive and listener_running do
              cosock.socket.sleep(15)
              if not is_alive or not listener_running or not persistent_tcp then break end
              local ok_send, ping_sent, ping_err = pcall(function()
                return persistent_tcp:send("{\"c\":\"ping\"}\n")
              end)
              if not ok_send or not ping_sent then
                ping_fail_count = ping_fail_count + 1
                log.warn(string.format("⚠️ [CH6 PUSH] Ping failed (%d/3): %s", ping_fail_count, tostring(ping_err or ping_sent)))
                if ping_fail_count >= 3 then
                  log.warn("[CH6 PING] 3 consecutive ping failures, closing socket.")
                  is_alive = false
                  pcall(function() persistent_tcp:close() end)
                  persistent_tcp = nil
                  break
                end
              else
                ping_fail_count = 0 -- 성공 시 카운터 리셋
              end
            end
          end, "ch6_heartbeat_ping")

          -- [Receive Loop] 단일 세션에서 Push 이벤트와 RPC 응답을 모두 처리
          while is_alive and listener_running do
            local line, recv_err = tcp:receive("*l")
            if not line then
              if recv_err == "timeout" then
                -- 타임아웃 시 연결 여부만 점검하고 루프 계속
                if not is_alive or not listener_running then break end
              else
                log.warn(string.format("⚠️ [CH6 PUSH] Connection lost (%s), reconnecting...", tostring(recv_err)))
                break
              end
            else
              if #line > 0 then
                local s_ok, data = pcall(json.decode, line)
                if s_ok and type(data) == "table" then
                  local req_id = data.id
                  if req_id and pending_requests[req_id] then
                    local ch = pending_requests[req_id]
                    pending_requests[req_id] = nil
                    pcall(function() ch:send(data) end)
                  elseif data.event and on_event_cb then
                    local cb_ok, cb_err = pcall(on_event_cb, driver, data)
                    if not cb_ok then
                      log.error(string.format("❌ [CH6 PUSH] Error in event callback (%s): %s", tostring(data.event), tostring(cb_err)))
                    end
                  elseif data.pong then
                    log.debug("📡 [CH6 PUSH] Heartbeat pong received")
                  end
                end
              end
            end
          end

          is_alive = false
          if persistent_tcp == tcp then
            persistent_tcp = nil
          end
          pcall(function() tcp:close() end)
          -- 남아있는 대기열 Fail-safe 언락
          for req_id, ch in pairs(pending_requests) do
            pending_requests[req_id] = nil
            pcall(function() ch:send({ error = "Connection closed" }) end)
          end
        else
          pcall(function() tcp:close() end)
          log.warn(string.format("⚠️ [CH6 PUSH] Connect failed: %s (Backoff: %ds)", tostring(conn_err), backoff_sec))
        end
      end
      if listener_running then
        local jitter = (math.random() * 500) / 1000
        cosock.socket.sleep(backoff_sec + jitter)
        backoff_sec = math.min(backoff_sec * 2, 30)
      end
    end
    log.info("📡 [CH6 PUSH] Listener loop stopped.")
  end, "ch6_push_listener")
end

--- CH6 실시간 리스너 정지 및 소켓 안전 정리
function GatewayClient.stop_event_listener()
  listener_running = false
  if persistent_tcp then
    pcall(function() persistent_tcp:close() end)
    persistent_tcp = nil
  end
  for req_id, ch in pairs(pending_requests) do
    pending_requests[req_id] = nil
    pcall(function() ch:send({ error = "Listener stopped" }) end)
  end
  log.info("🛑 [CH6 PUSH] Event listener stop requested.")
end

return GatewayClient
