local log = require "log"
local cache = {}

local schema_cache = {}

function schema_cache.store(device_id, device_info)
  log.debug(string.format("[SCHEMA CACHE] Storing schema for device_id='%s' (Type: %s)",
    tostring(device_id), tostring(device_info and device_info.type)))
  cache[device_id] = device_info
end

function schema_cache.get(device_id)
  return cache[device_id]
end

function schema_cache.get_all()
  return cache
end

return schema_cache
