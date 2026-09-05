-- ============================================================================
-- TCP Connection Manager & cosock Coroutine RX Loops
-- ============================================================================
-- Manages non-blocking TCP socket lifecycles for:
-- 1. Wallpad ESP32 Gateway (CH6 port 8899)
-- 2. Elevator EW11 Module (dedicated TCP server)
-- Handles non-blocking cosock RX loops, buffer framing, dispatching, and auto-reconnect.
-- ============================================================================

local cosock = require "cosock"
local socket = require "cosock.socket"
local log = require "log"
local FrameParser = require "frame_parser"
local ChildManager = require "child_manager"

local Connection = {}

local SOCKET_FIELD_WALLPAD = "wallpad_socket"
local SOCKET_FIELD_ELEVATOR = "elevator_socket"
local PARSER_FIELD_WALLPAD = "wallpad_parser"
local PARSER_FIELD_ELEVATOR = "elevator_parser"

--- Connect a TCP socket with a given timeout.
-- @param ip string: Target IPv4 address
-- @param port integer: Target TCP port
-- @param timeout number: Connection timeout in seconds
-- @return table|nil, string: Socket instance or nil, error message
function Connection.connect_tcp(ip, port, timeout)
  if not ip or ip == "" or not port or port <= 0 then
    return nil, "Invalid IP or port"
  end

  local sock = socket.tcp()
  sock:settimeout(timeout or 5)
  local ok, err = sock:connect(ip, port)
  if not ok then
    sock:close()
    return nil, err
  end

  -- Set short timeout for cooperative cosock receive yielding
  sock:settimeout(0.5)
  return sock, nil
end

--- Dispatch a single parsed RS-485 frame to the appropriate child device.
-- @param driver table: SmartThings Driver instance
-- @param bridge_device table: Parent gateway bridge Device instance
-- @param frame table: Parsed frame from FrameParser
function Connection.dispatch_frame(driver, bridge_device, frame)
  local child = ChildManager.find_child_by_frame(bridge_device, frame)
  if child then
    child:online()
    local rx_dispatchers = driver:get_field("rx_dispatchers") or {}
    local handler = rx_dispatchers[frame.dev_id]
    if handler then
      handler(driver, child, frame)
    else
      log.debug(string.format("No rx handler for dev_id 0x%02X on child %s", frame.dev_id, child.id))
    end
  else
    log.trace(string.format("Frame received for unmapped device: dev=0x%02X, sub1=0x%02X(norm=0x%02X), sub2=0x%02X",
      frame.dev_id, frame.sub1, frame.norm_sub1, frame.sub2))
  end
end

--- Start a persistent cosock receive loop for a given socket field.
-- @param driver table: Driver instance
-- @param bridge_device table: Parent bridge device
-- @param socket_field string: Field key in transient store (wallpad_socket or elevator_socket)
-- @param parser_field string: Field key in transient store for FrameParser
-- @param get_connection_info function: Returns (ip, port) from device preferences
-- @param loop_name string: Descriptive name for the coroutine
local function start_rx_coroutine(driver, bridge_device, socket_field, parser_field, get_connection_info, loop_name)
  cosock.spawn(function()
    log.info(string.format("Starting background RX coroutine: %s", loop_name))
    local reconnect_delay = 5

    while true do
      local sock = bridge_device:get_field(socket_field)
      local parser = bridge_device:get_field(parser_field)

      if not parser then
        parser = FrameParser.new()
        bridge_device:set_field(parser_field, parser)
      end

      if sock then
        local data, err, partial = sock:receive(1024)
        local chunk = data or partial

        if chunk and #chunk > 0 then
          parser:append(chunk)
          while true do
            local frame = parser:next_frame()
            if not frame then break end
            Connection.dispatch_frame(driver, bridge_device, frame)
          end
          reconnect_delay = 5 -- Reset backoff on valid traffic
        end

        if err == "closed" then
          log.warn(string.format("[%s] TCP connection closed by remote server. Reconnecting in %ds...", loop_name, reconnect_delay))
          sock:close()
          bridge_device:set_field(socket_field, nil)
          parser:clear()

          if socket_field == SOCKET_FIELD_WALLPAD then
            bridge_device:offline()
          end

          socket.sleep(reconnect_delay)
          reconnect_delay = math.min(reconnect_delay * 2, 60)

          local ip, port = get_connection_info(bridge_device)
          if ip and ip ~= "" and port and port > 0 then
            local new_sock, connect_err = Connection.connect_tcp(ip, port, 5)
            if new_sock then
              log.info(string.format("[%s] Reconnected successfully to %s:%d", loop_name, ip, port))
              bridge_device:set_field(socket_field, new_sock)
              if socket_field == SOCKET_FIELD_WALLPAD then
                bridge_device:online()
                -- Put all children online
                for _, child in ipairs(bridge_device:get_child_list()) do
                  child:online()
                end
              end
            else
              log.error(string.format("[%s] Reconnect failed to %s:%d: %s", loop_name, ip, port, tostring(connect_err)))
            end
          end
        elseif err == "timeout" then
          -- Normal cosock yielding: give execution time to other tasks
        end
      else
        -- Socket is not connected, attempt periodic connection
        local ip, port = get_connection_info(bridge_device)
        if ip and ip ~= "" and port and port > 0 then
          local new_sock, connect_err = Connection.connect_tcp(ip, port, 5)
          if new_sock then
            log.info(string.format("[%s] Connected to %s:%d", loop_name, ip, port))
            bridge_device:set_field(socket_field, new_sock)
            if socket_field == SOCKET_FIELD_WALLPAD then
              bridge_device:online()
              for _, child in ipairs(bridge_device:get_child_list()) do
                child:online()
              end
            end
          else
            log.debug(string.format("[%s] Connection attempt to %s:%d failed: %s", loop_name, ip, port, tostring(connect_err)))
          end
        end
        socket.sleep(reconnect_delay)
      end
    end
  end, loop_name)
end

--- Initialize all TCP connections and start RX loops for the bridge device.
-- @param driver table: SmartThings Driver instance
-- @param bridge_device table: Parent gateway bridge Device instance
function Connection.init_connections(driver, bridge_device)
  -- 1. Wallpad ESP32 Socket
  start_rx_coroutine(
    driver,
    bridge_device,
    SOCKET_FIELD_WALLPAD,
    PARSER_FIELD_WALLPAD,
    function(dev)
      local prefs = dev.preferences or {}
      return prefs.wallpadIp or "172.30.1.3", prefs.wallpadPort or 8899
    end,
    "Wallpad_ESP32_RX_Loop"
  )

  -- 2. Elevator EW11 Socket (if configured)
  start_rx_coroutine(
    driver,
    bridge_device,
    SOCKET_FIELD_ELEVATOR,
    PARSER_FIELD_ELEVATOR,
    function(dev)
      local prefs = dev.preferences or {}
      return prefs.elevatorIp or "", prefs.elevatorPort or 8899
    end,
    "Elevator_EW11_RX_Loop"
  )
end

--- Send a raw binary command over the Wallpad TCP socket.
-- @param bridge_device table: Parent gateway bridge Device instance
-- @param packet_bytes string: Binary RS-485 packet string
-- @return boolean, string: success, error
function Connection.send_wallpad(bridge_device, packet_bytes)
  local sock = bridge_device:get_field(SOCKET_FIELD_WALLPAD)
  if not sock then
    return false, "Wallpad socket not connected"
  end

  local tx_delay_ms = (bridge_device.preferences and bridge_device.preferences.txDelay) or 50
  if tx_delay_ms > 0 then
    socket.sleep(tx_delay_ms / 1000.0)
  end

  local sent, err = sock:send(packet_bytes)
  if not sent then
    log.error(string.format("Failed to send packet to Wallpad: %s", tostring(err)))
    return false, err
  end
  return true, nil
end

--- Send a raw binary command over the Elevator TCP socket (or fallback to Wallpad if EW11 not configured).
-- @param bridge_device table: Parent gateway bridge Device instance
-- @param packet_bytes string: Binary RS-485 packet string
-- @return boolean, string: success, error
function Connection.send_elevator(bridge_device, packet_bytes)
  local elev_sock = bridge_device:get_field(SOCKET_FIELD_ELEVATOR)
  if elev_sock then
    local sent, err = elev_sock:send(packet_bytes)
    if sent then return true, nil end
  end

  -- Fallback to Wallpad socket
  return Connection.send_wallpad(bridge_device, packet_bytes)
end

--- Close all sockets for a bridge device on shutdown or removal.
-- @param bridge_device table: Parent gateway bridge Device instance
function Connection.close_all(bridge_device)
  local wp_sock = bridge_device:get_field(SOCKET_FIELD_WALLPAD)
  if wp_sock then
    wp_sock:close()
    bridge_device:set_field(SOCKET_FIELD_WALLPAD, nil)
  end

  local el_sock = bridge_device:get_field(SOCKET_FIELD_ELEVATOR)
  if el_sock then
    el_sock:close()
    bridge_device:set_field(SOCKET_FIELD_ELEVATOR, nil)
  end
end

return Connection
