local socket = require "cosock.socket"
local json = require "st.json"
local log = require "log"

local GatewayClient = {}

local DEFAULT_TIMEOUT_SEC = 3

function GatewayClient.send_rpc(ip, port, cmd_table, timeout)
  if not ip or not port then
    return nil, "Missing IP or Port"
  end

  timeout = timeout or DEFAULT_TIMEOUT_SEC

  local tcp, err = socket.tcp()
  if not tcp then
    return nil, "Failed to create TCP socket: " .. tostring(err)
  end

  tcp:settimeout(timeout)
  local ok, conn_err = tcp:connect(ip, port)
  if not ok then
    tcp:close()
    return nil, "Failed to connect to " .. tostring(ip) .. ":" .. tostring(port) .. " (" .. tostring(conn_err) .. ")"
  end

  local payload = json.encode(cmd_table) .. "\n"
  log.debug(string.format("🌐 [RPC SEND] -> %s:%d | %s", ip, port, json.encode(cmd_table)))
  local sent, send_err = tcp:send(payload)
  if not sent then
    tcp:close()
    log.error(string.format("❌ [RPC ERROR] Failed to send to %s:%d: %s", ip, port, tostring(send_err)))
    return nil, "Failed to send RPC payload: " .. tostring(send_err)
  end

  -- Receive complete line using "*l"
  local line, recv_err, partial = tcp:receive("*l")
  tcp:close()

  local final_data = line or partial
  if not final_data or #final_data == 0 then
    log.error(string.format("❌ [RPC ERROR] Empty response from %s:%d: %s", ip, port, tostring(recv_err)))
    return nil, "Failed to receive RPC response: " .. tostring(recv_err)
  end

  local ok_dec, resp_table = pcall(json.decode, final_data)
  if not ok_dec or not resp_table then
    log.error(string.format("❌ [RPC ERROR] JSON parse fail from %s:%d | Length: %d | Raw: %s", ip, port, #final_data, tostring(final_data)))
    return nil, "JSON decode error (Raw: " .. tostring(final_data) .. ")"
  end

  log.debug(string.format("📥 [RPC RECV] <- %s:%d | Length: %d", ip, port, #final_data))
  return resp_table, nil
end

function GatewayClient.get_telemetry(ip, port)
  return GatewayClient.send_rpc(ip, port, { cmd = "get_telemetry" })
end

function GatewayClient.cache_purge_rescan(ip, port)
  return GatewayClient.send_rpc(ip, port, { cmd = "cache_purge_rescan" })
end

function GatewayClient.wallpad_reset(ip, port)
  return GatewayClient.send_rpc(ip, port, { cmd = "wallpad_reset" })
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

function GatewayClient.set_timing(ip, port, ch1, ch2, ch3)
  local params = { cmd = "set_timing" }
  if ch1 then params.ch1 = ch1 end
  if ch2 then params.ch2 = ch2 end
  if ch3 then params.ch3 = ch3 end
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
  })
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

return GatewayClient
