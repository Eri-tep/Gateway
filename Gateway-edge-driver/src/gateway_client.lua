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
  if not persistent_tcp then
    log.warn("⚠️ [RPC SEND] persistent_tcp not connected yet")
    return nil, "Gateway connection not ready"
  end

  timeout = timeout or DEFAULT_TIMEOUT_SEC

  -- Unique request ID assignment for O(1) table dispatch
  next_req_id = (next_req_id % 1000000) + 1
  local cur_req_id = next_req_id
  cmd_table.id = cur_req_id

  local tx, rx = cosock.channel.new()
  pending_requests[cur_req_id] = tx

  local payload = json.encode(cmd_table) .. "\n"
  log.debug(string.format("🌐 [RPC SEND] (Single Session) [ID:%d] -> %s:%d | %s", cur_req_id, ip, port, payload:sub(1, -2)))

  local sent, send_err = persistent_tcp:send(payload)
  if not sent then
    log.warn(string.format("⚠️ [RPC SEND] Send failed [ID:%d]: %s", cur_req_id, tostring(send_err)))
    pending_requests[cur_req_id] = nil
    return nil, "Send failed: " .. tostring(send_err)
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
  if not persistent_tcp then
    log.debug("ℹ️ [FAST PATH] persistent_tcp not connected yet")
    return false, "Gateway connection not ready"
  end
  local payload = json.encode(cmd_table) .. "\n"
  local sent, err = persistent_tcp:send(payload)
  if not sent then
    log.warn(string.format("⚠️ [FAST PATH] Send failed: %s", tostring(err)))
    return false, err
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

function GatewayClient.get_locked_devices(ip, port)
  return GatewayClient.send_rpc(ip, port, {
    cmd = "get_locked_devices"
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

  -- 1순위: 초저지연 Fast-Path (Non-blocking 즉시 전송)
  local sent = GatewayClient.send_fast_path(payload)
  if sent then return true end

  -- Fallback: 연결 준비 중이거나 복구 중일 때 기존 send_rpc로 대기 전송
  return GatewayClient.send_rpc(ip, port, payload)
end


--- CH6 (8900) 실시간 푸시 이벤트 리스너 (백그라운드 지속 소켓 + 클라이언트 Heartbeat Ping + RPC 공유)
function GatewayClient.start_event_listener(driver, ip, port, on_event_cb)
  if not ip or not port then return end

  local cosock = require "cosock"
  cosock.spawn(function()
    log.info(string.format("📡 [CH6 PUSH] Starting single persistent session on %s:%d", ip, port))
    while true do
      local tcp, err = socket.tcp()
      if tcp then
        tcp:settimeout(nil) -- 블로킹 대기
        local ok, conn_err = tcp:connect(ip, port)
        if ok then
          pcall(function() tcp:setoption("tcp-nodelay", true) end)
          log.info(string.format("✅ [CH6 PUSH] Connected to Gateway %s:%d (TCP_NODELAY Active)", ip, port))
          persistent_tcp = tcp
          local is_alive = true

          -- [Heartbeat Ping Worker] 15초마다 게이트웨이로 Ping 전송하여 세션 활성화 유지 및 끊김 감지 (단축 포맷)
          cosock.spawn(function()
            while is_alive do
              cosock.socket.sleep(15)
              if not is_alive or not persistent_tcp then break end
              local ping_sent, ping_err = persistent_tcp:send("{\"c\":\"ping\"}\n")
              if not ping_sent then
                log.warn(string.format("⚠️ [CH6 PING] Ping failed: %s (closing socket)", tostring(ping_err)))
                is_alive = false
                pcall(function() persistent_tcp:close() end)
                persistent_tcp = nil
                break
              end
            end
          end, "ch6_heartbeat_ping")

          -- [Receive Loop] 단일 세션에서 Push 이벤트와 RPC 응답을 모두 처리
          while is_alive do
            local line, recv_err = tcp:receive("*l")
            if not line then
              log.warn(string.format("⚠️ [CH6 PUSH] Connection lost (%s), reconnecting...", tostring(recv_err)))
              break
            end
            if #line > 0 then
              local s_ok, data = pcall(json.decode, line)
              if s_ok and type(data) == "table" then
                local req_id = data.id
                if req_id and pending_requests[req_id] then
                  local ch = pending_requests[req_id]
                  pending_requests[req_id] = nil
                  ch:send(data)
                elseif data.event and on_event_cb then
                  on_event_cb(driver, data)
                elseif data.pong then
                  -- Heartbeat Pong 소비
                end
              end
            end
          end

          is_alive = false
          persistent_tcp = nil
          pcall(function() tcp:close() end)
          -- 남아있는 대기열 Fail-safe 언락
          for req_id, ch in pairs(pending_requests) do
            pending_requests[req_id] = nil
            ch:send({ error = "Connection closed" })
          end
        else
          tcp:close()
          log.warn(string.format("⚠️ [CH6 PUSH] Connect failed: %s, retrying in 5s...", tostring(conn_err)))
        end
      end
      cosock.socket.sleep(5)
    end
  end, "ch6_push_listener")
end

return GatewayClient
