local log = require "log"
local cosock = require "cosock"
local socket = require "cosock.socket"
local json
local ok, st_json = pcall(require, "st.json")
if ok then
  json = st_json
else
  json = require "dkjson"
end

local gateway = {}
package.loaded["gateway"] = gateway

local bridge_ui = require "bridge_ui"
local light_handler = require "handlers.light"
local thermostat_handler = require "handlers.thermostat"
local air_conditioner_handler = require "handlers.air_conditioner"
local ventilation_handler = require "handlers.ventilation"
local gas_handler = require "handlers.gas"
local outlet_handler = require "handlers.outlet"

local RECONNECT_DELAYS = { 1, 2, 4, 8, 16, 32, 60 }
local active_connection = nil
local keepalive_timer = nil

local function dispatch_event(driver, msg)
  if msg.event == "discovered" then
    log.info("[GATEWAY] [EVENT] 'discovered' event received from gateway.")
    local discovery = require "discovery"
    discovery.handle_discovered(driver, msg.devices)
  elseif msg.event == "state" then
    local dev_id = msg.id
    local suffix = ":" .. tostring(dev_id)
    local matched_any = false

    for _, device in ipairs(driver:get_devices()) do
      local dnid = device.device_network_id
      if dnid and dnid:find(suffix, 1, true) then
        matched_any = true
        local model = device.model or ""
        log.debug(string.format("[GATEWAY] [ROUTE STATE] Routing event for dev_id='%s' to ST Device '%s' (Model='%s')",
          tostring(dev_id), tostring(device.label), model))

        if dnid:find(":light") or model == "light" then
          light_handler.handle_state(device, msg)
        elseif dnid:find(":thermo") or model == "thermostat" then
          thermostat_handler.handle_state(device, msg)
        elseif dnid:find(":aircon") or model == "aircon" then
          air_conditioner_handler.handle_state(device, msg)
        elseif dnid:find(":vent") or model == "ventilation" then
          ventilation_handler.handle_state(device, msg)
        elseif dnid:find(":gas") or model == "gas" then
          gas_handler.handle_state(device, msg)
        elseif dnid:find(":outlet") or model == "outlet" then
          outlet_handler.handle_state(device, msg)
        else
          log.warn(string.format("[GATEWAY] [ROUTE STATE] Device '%s' matched ID but has unknown model/type.", tostring(device.label)))
        end
      end
    end

    if not matched_any then
      log.debug(string.format("[GATEWAY] [ROUTE STATE] No registered ST device found matching dev_id='%s'", tostring(dev_id)))
    end
  else
    log.warn(string.format("[GATEWAY] [EVENT] Unknown event type '%s'", tostring(msg.event)))
  end
end

local function connect_loop(driver, device, ip, port)
  local delay_index = 1
  while true do
    log.info(string.format("[GATEWAY] [TCP CONNECT] Attempting connection to %s:%d (Timeout: 5s)...", tostring(ip), tonumber(port)))
    local sock = socket.tcp()
    sock:settimeout(5)
    local res, err = sock:connect(ip, port)

    if res then
      log.info(string.format("[GATEWAY] [TCP CONNECT] SUCCESS: Connected to gateway at %s:%d", tostring(ip), tonumber(port)))
      delay_index = 1
      active_connection = sock
      sock:settimeout(nil)
      bridge_ui.update_status(driver, "정상 동작 중")

      -- Mark all devices online
      log.info("[GATEWAY] [ONLINE] Marking all registered devices as ONLINE in SmartThings...")
      for _, dev in ipairs(driver:get_devices()) do
        dev:online()
      end

      -- Send discovery on connect
      log.info("[GATEWAY] Requesting device discovery from gateway...")
      gateway.send({ cmd = "discover" })

      while true do
        local line, err, partial = sock:receive("*l")
        if not line then
          log.error("[GATEWAY] [TCP RX ERROR] Socket read failed or disconnected: " .. tostring(err))
          bridge_ui.update_status(driver, "오류: 게이트웨이 연결 끊김")
          break
        end

        log.info("[GATEWAY] [TCP RX] >>> " .. line)
        local msg, pos, err = json.decode(line, 1, nil)
        if msg then
          dispatch_event(driver, msg)
        else
          log.error(string.format("[GATEWAY] [JSON ERROR] Failed to decode: %s (Raw: '%s')", tostring(err), tostring(line)))
        end
      end
    else
      log.error(string.format("[GATEWAY] [TCP CONNECT ERROR] Connection to %s:%d failed: %s", tostring(ip), tonumber(port), tostring(err)))
      bridge_ui.update_status(driver, "오류: 게이트웨이 연결 끊김")
      if device then
        log.warn("[GATEWAY] [OFFLINE] Setting bridge device to OFFLINE state.")
        device:offline()
      end
    end

    if active_connection then
      active_connection:close()
      active_connection = nil
    end

    local delay = RECONNECT_DELAYS[delay_index]
    log.info(string.format("[GATEWAY] [RECONNECT] Retrying connection in %d seconds (Backoff step %d/%d)...",
      delay, delay_index, #RECONNECT_DELAYS))
    cosock.socket.sleep(delay)
    delay_index = math.min(delay_index + 1, #RECONNECT_DELAYS)
  end
end

function gateway.connect(driver, device)
  local ip = (device.preferences and device.preferences.serverIp) or "172.30.1.3"
  local port = (device.preferences and device.preferences.serverPort) or 8899

  log.info(string.format("[GATEWAY] Starting gateway connection task for %s:%d", tostring(ip), tonumber(port)))

  -- Close existing connection if any
  gateway.disconnect(device)

  cosock.spawn(function()
    connect_loop(driver, device, ip, port)
  end, "gateway_connect_loop")

  -- 60s Health Check & Keepalive timer
  if keepalive_timer then
    driver:cancel_timer(keepalive_timer)
    keepalive_timer = nil
  end
  keepalive_timer = driver:call_on_schedule(60, function()
    gateway.ping(device)
  end, "gateway_health")
end

function gateway.send(msg)
  if active_connection then
    local payload = json.encode(msg) .. "\n"
    log.info("[GATEWAY] [TCP TX] <<< " .. json.encode(msg))
    local success, err = active_connection:send(payload)
    if not success then
      log.error("[GATEWAY] [TCP TX ERROR] Failed to send JSON command: " .. tostring(err))
    end
  else
    log.error("[GATEWAY] [TCP TX ERROR] Cannot send command (Socket not connected): " .. json.encode(msg))
  end
end

function gateway.disconnect(device)
  if active_connection then
    log.info("[GATEWAY] Disconnecting active socket connection...")
    active_connection:close()
    active_connection = nil
  end
end

function gateway.ping(device)
  log.debug("[GATEWAY] Sending 60s keepalive ping...")
  gateway.send({ cmd = "ping" })
end

return gateway
