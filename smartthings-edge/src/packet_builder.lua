-- ============================================================================
-- RS-485 Packet Builder
-- ============================================================================
-- Builds standard RS-485 control binary frames with automatic XOR checksum.
-- Frame structure: [STX=F7] [LEN] [TYPE=01] [DEV_ID] [OP=02] [SUB1] [SUB2] [DATA...] [CS] [ETX=EE]
-- ============================================================================

local STX = 0xF7
local ETX = 0xEE
local TYPE_BYTE = 0x01
local CMD_CONTROL = 0x02

local PacketBuilder = {}

--- Calculate XOR checksum over array of byte values.
-- @param bytes table: Array of integers (0-255)
-- @return integer: XOR checksum byte (0-255)
function PacketBuilder.checksum(bytes)
  local cs = 0
  for i = 1, #bytes do
    cs = cs ~ bytes[i]
  end
  return cs & 0xFF
end

--- Build a raw binary RS-485 control packet.
-- @param dev_id integer: Device type code (e.g. 0x19, 0x1F, 0x18)
-- @param opcode integer: Operation code (default 0x02 for control)
-- @param sub1 integer: Sub1 address byte
-- @param sub2 integer: Sub2 address byte
-- @param data_bytes table: Array of data payload bytes (e.g. { 0x00, 0x01 })
-- @return string: Raw binary string ready to send over TCP socket
function PacketBuilder.build(dev_id, opcode, sub1, sub2, data_bytes)
  opcode = opcode or CMD_CONTROL
  data_bytes = data_bytes or {}

  -- Total length = STX(1) + LEN(1) + TYPE(1) + DEV_ID(1) + OP(1) + SUB1(1) + SUB2(1) + #DATA + CS(1) + ETX(1)
  local total_len = 7 + #data_bytes + 2

  local packet_bytes = {
    STX,
    total_len,
    TYPE_BYTE,
    dev_id,
    opcode,
    sub1,
    sub2
  }

  for _, b in ipairs(data_bytes) do
    table.insert(packet_bytes, b)
  end

  local cs = PacketBuilder.checksum(packet_bytes)
  table.insert(packet_bytes, cs)
  table.insert(packet_bytes, ETX)

  return string.char(table.unpack(packet_bytes))
end

--- Light control packet (11 bytes).
-- F7 0B 01 19 02 40 [sub2] 00 [0x01(ON) / 0x02(OFF)] [CS] EE
-- @param sub2 integer: Light sub2 address (e.g. 0x11, 0x12, 0x21...)
-- @param is_on boolean: true for ON, false for OFF
-- @return string: Binary packet
function PacketBuilder.light_cmd(sub2, is_on)
  local state_byte = is_on and 0x01 or 0x02
  return PacketBuilder.build(0x19, CMD_CONTROL, 0x40, sub2, { 0x00, state_byte })
end

--- Smart outlet control packet (11 bytes).
-- F7 0B 01 1F 02 40 [sub2] 00 [0x01(ON) / 0x02(OFF)] [CS] EE
-- @param sub2 integer: Outlet sub2 address (e.g. 0x11, 0x21, 0x31...)
-- @param is_on boolean: true for ON, false for OFF
-- @return string: Binary packet
function PacketBuilder.outlet_cmd(sub2, is_on)
  local state_byte = is_on and 0x01 or 0x02
  return PacketBuilder.build(0x1F, CMD_CONTROL, 0x40, sub2, { 0x00, state_byte })
end

--- Thermostat control packet (11 bytes).
-- F7 0B 01 18 02 46 [sub2] [mode] [set_temp] [CS] EE
-- Verified by live bus captures: mode 0x04=Heat, 0x07=Away
-- @param sub2 integer: Thermostat sub2 address (0x11=Living, 0x12=Bed, 0x13=Office, 0x14=Relax)
-- @param mode integer: Mode byte (0x04=Heat, 0x07=Away)
-- @param set_temp integer: Target temperature in Celsius (e.g. 22)
-- @return string: Binary packet
function PacketBuilder.thermostat_cmd(sub2, mode, set_temp)
  mode = mode or 0x04
  set_temp = set_temp or 0x00
  return PacketBuilder.build(0x18, CMD_CONTROL, 0x46, sub2, { mode, set_temp })
end

--- Ventilation control packet (13 bytes).
-- F7 0D 01 2B 02 42 11 00 [power] [speed] [mode] [CS] EE
-- power: 0x01=ON, 0x02=OFF
-- speed: 0x01=Low, 0x02=Med, 0x03=High
-- mode: 0xFF=Heat Exchange(전열), 0x01=Bypass(바이패스)
-- @param is_on boolean: Power ON/OFF
-- @param speed integer: Fan speed (1~3)
-- @param is_bypass boolean: true for bypass (0x01), false for heat recovery (0xFF)
-- @return string: Binary packet
function PacketBuilder.ventilation_cmd(is_on, speed, is_bypass)
  local power_byte = is_on and 0x01 or 0x02
  speed = math.max(1, math.min(3, speed or 1))
  local mode_byte = is_bypass and 0x01 or 0xFF
  return PacketBuilder.build(0x2B, CMD_CONTROL, 0x42, 0x11, { 0x00, power_byte, speed, mode_byte })
end

--- Induction breaker cutoff control packet (11 bytes).
-- F7 0B 01 1B 02 43 11 00 00 [CS] EE
-- Power OFF/cut-off only (Safety restriction: App cannot remotely energize).
-- @return string: Binary packet
function PacketBuilder.induction_cutoff()
  return PacketBuilder.build(0x1B, CMD_CONTROL, 0x43, 0x11, { 0x00, 0x00 })
end

--- Elevator call packet (13 bytes).
-- F7 0D 01 34 02 41 10 00 02 00 00 [CS] EE
-- Fixed checksum: 0x9E
-- @return string: Binary packet
function PacketBuilder.elevator_call()
  return PacketBuilder.build(0x34, CMD_CONTROL, 0x41, 0x10, { 0x00, 0x02, 0x00, 0x00 })
end

return PacketBuilder
