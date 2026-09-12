-- ============================================================================
-- Child Device Manager & Mapping Table (24 Devices)
-- ============================================================================
-- Manages device metadata, (dev_id, norm_sub1, sub2) addressing, reverse lookup,
-- and dynamic creation of EDGE_CHILD devices.
-- ============================================================================

local log = require "log"

local ChildManager = {}

--- Master definition of all 24 child devices across 4 rooms and shared facilities.
ChildManager.CHILD_DEFINITIONS = {
  -- [거실 1x]
  { key = "light_11", label = "거실 조명 1", dev_id = 0x19, sub1 = 0x40, sub2 = 0x11, profile = "light-switch.v1", type = "light" },
  { key = "light_12", label = "거실 조명 2", dev_id = 0x19, sub1 = 0x40, sub2 = 0x12, profile = "light-switch.v1", type = "light" },
  { key = "light_13", label = "거실 조명 3", dev_id = 0x19, sub1 = 0x40, sub2 = 0x13, profile = "light-switch.v1", type = "light" },
  { key = "light_14", label = "거실 조명 4", dev_id = 0x19, sub1 = 0x40, sub2 = 0x14, profile = "light-switch.v1", type = "light" },
  { key = "outlet_11", label = "거실 콘센트 1", dev_id = 0x1F, sub1 = 0x40, sub2 = 0x11, profile = "smart-outlet.v1", type = "outlet" },
  { key = "thermo_11", label = "거실 난방", dev_id = 0x18, sub1 = 0x46, sub2 = 0x11, profile = "thermostat.v1", type = "thermostat" },

  -- [침실 2x]
  { key = "light_21", label = "침실 조명 1", dev_id = 0x19, sub1 = 0x40, sub2 = 0x21, profile = "light-switch.v1", type = "light" },
  { key = "light_22", label = "침실 조명 2", dev_id = 0x19, sub1 = 0x40, sub2 = 0x22, profile = "light-switch.v1", type = "light" },
  { key = "outlet_21", label = "침실 콘센트 1", dev_id = 0x1F, sub1 = 0x40, sub2 = 0x21, profile = "smart-outlet.v1", type = "outlet" },
  { key = "outlet_22", label = "침실 콘센트 2", dev_id = 0x1F, sub1 = 0x40, sub2 = 0x22, profile = "smart-outlet.v1", type = "outlet" },
  { key = "thermo_12", label = "침실 난방", dev_id = 0x18, sub1 = 0x46, sub2 = 0x12, profile = "thermostat.v1", type = "thermostat" },

  -- [업무실 3x]
  { key = "light_31", label = "업무실 조명 1", dev_id = 0x19, sub1 = 0x40, sub2 = 0x31, profile = "light-switch.v1", type = "light" },
  { key = "light_32", label = "업무실 조명 2", dev_id = 0x19, sub1 = 0x40, sub2 = 0x32, profile = "light-switch.v1", type = "light" },
  { key = "outlet_31", label = "업무실 콘센트 1", dev_id = 0x1F, sub1 = 0x40, sub2 = 0x31, profile = "smart-outlet.v1", type = "outlet" },
  { key = "outlet_32", label = "업무실 콘센트 2", dev_id = 0x1F, sub1 = 0x40, sub2 = 0x32, profile = "smart-outlet.v1", type = "outlet" },
  { key = "thermo_13", label = "업무실 난방", dev_id = 0x18, sub1 = 0x46, sub2 = 0x13, profile = "thermostat.v1", type = "thermostat" },

  -- [휴식실 4x]
  { key = "light_41", label = "휴식실 조명 1", dev_id = 0x19, sub1 = 0x40, sub2 = 0x41, profile = "light-switch.v1", type = "light" },
  { key = "light_42", label = "휴식실 조명 2", dev_id = 0x19, sub1 = 0x40, sub2 = 0x42, profile = "light-switch.v1", type = "light" },
  { key = "outlet_41", label = "휴식실 콘센트 1", dev_id = 0x1F, sub1 = 0x40, sub2 = 0x41, profile = "smart-outlet.v1", type = "outlet" },
  { key = "outlet_42", label = "휴식실 콘센트 2", dev_id = 0x1F, sub1 = 0x40, sub2 = 0x42, profile = "smart-outlet.v1", type = "outlet" },
  { key = "thermo_14", label = "휴식실 난방", dev_id = 0x18, sub1 = 0x46, sub2 = 0x14, profile = "thermostat.v1", type = "thermostat" },

  -- [공용 설비]
  { key = "vent_11", label = "전열교환기", dev_id = 0x2B, sub1 = 0x40, sub2 = 0x11, profile = "ventilation.v1", type = "ventilation" },
  { key = "induction_11", label = "인덕션 차단기", dev_id = 0x1B, sub1 = 0x43, sub2 = 0x11, profile = "induction-breaker.v1", type = "induction" },
  { key = "elevator_10", label = "엘리베이터", dev_id = 0x34, sub1 = 0x41, sub2 = 0x10, profile = "elevator.v1", type = "elevator" },
}

-- Fast reverse lookup map: "devId_normSub1_sub2" -> child_key
local reverse_address_map = {}
local key_to_definition_map = {}

for _, def in ipairs(ChildManager.CHILD_DEFINITIONS) do
  local hash = string.format("%02x_%02x_%02x", def.dev_id, def.sub1, def.sub2)
  reverse_address_map[hash] = def.key
  key_to_definition_map[def.key] = def
end

--- Find child key by matching parsed frame addresses.
-- @param frame table: Parsed frame from FrameParser ({ dev_id, norm_sub1, sub2 })
-- @return string|nil: Child key (e.g. "light_11") or nil
function ChildManager.frame_to_child_key(frame)
  if not frame or not frame.dev_id or not frame.norm_sub1 or not frame.sub2 then
    return nil
  end
  local hash = string.format("%02x_%02x_%02x", frame.dev_id, frame.norm_sub1, frame.sub2)
  return reverse_address_map[hash]
end

--- Get child definition table by child key.
-- @param key string: Child key (e.g. "light_11")
-- @return table|nil: Definition entry
function ChildManager.get_definition(key)
  return key_to_definition_map[key]
end

--- Find child Device object for a given parsed frame.
-- @param parent_device table: Parent gateway bridge Device instance
-- @param frame table: Parsed frame from FrameParser
-- @return table|nil: Child Device object or nil
function ChildManager.find_child_by_frame(parent_device, frame)
  local child_key = ChildManager.frame_to_child_key(frame)
  if not child_key then
    return nil
  end
  return parent_device:get_child_by_parent_assigned_key(child_key)
end

--- Create all 24 child devices under the parent bridge if they do not exist.
-- @param driver table: SmartThings Edge Driver instance
-- @param parent_device table: Parent gateway bridge Device instance
function ChildManager.create_all_children(driver, parent_device)
  log.info(string.format("Checking/creating 24 child devices for parent: %s", parent_device.id))

  for _, def in ipairs(ChildManager.CHILD_DEFINITIONS) do
    local existing = parent_device:get_child_by_parent_assigned_key(def.key)
    if not existing then
      log.info(string.format("Creating child device: [%s] %s (Profile: %s)", def.key, def.label, def.profile))
      driver:try_create_device({
        type = "EDGE_CHILD",
        parent_device_id = parent_device.id,
        parent_assigned_child_key = def.key,
        label = def.label,
        profile = def.profile,
        manufacturer = "CustomHome",
        model = string.format("RS485-%s", def.type:gsub("^%l", string.upper)),
        vendor_provided_label = def.label
      })
    else
      -- Ensure device_type and sub2 fields are populated in transient store
      existing:set_field("device_type", def.type)
      existing:set_field("sub2", def.sub2)
      existing:set_field("sub1", def.sub1)
      existing:set_field("dev_id", def.dev_id)
      existing:set_field("parent_device_id", parent_device.id)
    end
  end
end

return ChildManager
