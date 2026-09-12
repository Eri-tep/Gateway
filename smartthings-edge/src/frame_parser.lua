-- ============================================================================
-- RS-485 Frame Parser with Ring/Accumulation Buffer
-- ============================================================================
-- Handles F7..EE packet framing, length validation, XOR checksum verification,
-- and NormSub1 address normalization.
-- ============================================================================

local log = require "log"

local STX = 0xF7
local ETX = 0xEE
local MIN_PACKET_LEN = 5
local MAX_PACKET_LEN = 64

local FrameParser = {}
FrameParser.__index = FrameParser

--- Create a new FrameParser instance with an empty accumulator buffer.
-- @return table: FrameParser instance
function FrameParser.new()
  local self = setmetatable({}, FrameParser)
  self.buffer = ""
  return self
end

--- Append incoming raw TCP binary chunk to the internal buffer.
-- @param data string: Raw binary string received from TCP socket
function FrameParser:append(data)
  if data and #data > 0 then
    self.buffer = self.buffer .. data
    -- Prevent buffer from growing unbounded if corrupted stream is received
    if #self.buffer > 4096 then
      log.warn("FrameParser buffer exceeded 4KB, trimming leading corrupted bytes")
      local f7_pos = self.buffer:find(string.char(STX))
      if f7_pos then
        self.buffer = self.buffer:sub(f7_pos)
      else
        self.buffer = ""
      end
    end
  end
end

--- Calculate XOR checksum over a sequence of byte values.
-- @param bytes table: Array of integers (0-255)
-- @param start_idx integer: 1-based start index (default 1)
-- @param end_idx integer: 1-based end index (default #bytes)
-- @return integer: XOR checksum (0-255)
local function calculate_xor_checksum(bytes, start_idx, end_idx)
  local cs = 0
  start_idx = start_idx or 1
  end_idx = end_idx or #bytes
  for i = start_idx, end_idx do
    cs = cs ~ bytes[i]
  end
  return cs & 0xFF
end

--- Normalize sub1 address for legacy or asymmetric vendor packets.
-- Thermostat (0x18): 0x45 -> 0x46 (safety net)
-- Ventilator (0x2B): 0x42 -> 0x40 (safety net)
-- @param dev_id integer: Device type code (e.g. 0x18, 0x2B)
-- @param sub1 integer: Sub1 address byte
-- @return integer: Normalized sub1 address byte
local function norm_sub1(dev_id, sub1)
  if dev_id == 0x18 and sub1 == 0x45 then
    return 0x46
  end
  if dev_id == 0x2B and sub1 == 0x42 then
    return 0x40
  end
  return sub1
end

--- Extract next valid RS-485 frame from the accumulator buffer.
-- @return table|nil: Parsed frame table or nil if no complete frame is available
function FrameParser:next_frame()
  while #self.buffer >= MIN_PACKET_LEN do
    -- 1. Find STX (0xF7)
    local stx_pos = self.buffer:find(string.char(STX))
    if not stx_pos then
      -- No STX in buffer, clear everything
      self.buffer = ""
      return nil
    end

    -- Discard leading garbage before STX
    if stx_pos > 1 then
      self.buffer = self.buffer:sub(stx_pos)
    end

    if #self.buffer < MIN_PACKET_LEN then
      return nil
    end

    -- 2. Read declared length (byte 2)
    local len = self.buffer:byte(2)
    if len < MIN_PACKET_LEN or len > MAX_PACKET_LEN then
      -- Corrupted length byte, skip this 0xF7 and search next
      self.buffer = self.buffer:sub(2)
    else
      -- 3. Check if entire packet is in buffer
      if #self.buffer < len then
        return nil -- Wait for more data from TCP stream
      end

      -- 4. Verify ETX (0xEE)
      local etx_byte = self.buffer:byte(len)
      if etx_byte ~= ETX then
        -- Invalid ETX, skip this STX and search next
        self.buffer = self.buffer:sub(2)
      else
        -- Extract packet bytes
        local raw_pkt = self.buffer:sub(1, len)
        local bytes = { raw_pkt:byte(1, len) }

        -- 5. Verify XOR Checksum (bytes[1] .. bytes[len-2] vs bytes[len-1])
        local expected_cs = calculate_xor_checksum(bytes, 1, len - 2)
        local actual_cs = bytes[len - 1]

        if expected_cs ~= actual_cs then
          log.warn(string.format("Checksum mismatch: expected 0x%02X, got 0x%02X (raw: %s)",
            expected_cs, actual_cs, raw_pkt))
          self.buffer = self.buffer:sub(2)
        else
          -- Checksum OK! Consume packet from buffer
          self.buffer = self.buffer:sub(len + 1)

          -- Standard packet layout:
          -- [1]=STX(F7), [2]=LEN, [3]=TYPE(01), [4]=DEV_ID, [5]=OPCODE, [6]=SUB1, [7]=SUB2, [8..N-2]=DATA, [N-1]=CS, [N]=ETX
          local dev_id = bytes[4]
          local opcode = bytes[5]
          local sub1 = bytes[6]
          local sub2 = bytes[7]

          local payload = {}
          for i = 8, len - 2 do
            table.insert(payload, bytes[i])
          end

          local normalized_sub1 = norm_sub1(dev_id, sub1)

          return {
            length = len,
            dev_id = dev_id,
            opcode = opcode,
            sub1 = sub1,
            norm_sub1 = normalized_sub1,
            sub2 = sub2,
            data = payload,
            raw_bytes = bytes,
            raw_string = raw_pkt
          }
        end
      end
    end
  end

  return nil
end

--- Reset/clear internal buffer.
function FrameParser:clear()
  self.buffer = ""
end

return FrameParser
