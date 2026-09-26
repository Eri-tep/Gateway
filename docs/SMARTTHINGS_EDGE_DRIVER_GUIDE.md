# SmartThings Edge Driver Developer Guide & Architecture Reference

> **Purpose**: Standardized technical reference defining the architecture, 6-card multi-component UI structure, master ticker design pattern, lifecycle management, coding conventions, and SmartThings CLI deployment workflow for the ESP32 RS-485 LAN Gateway Edge driver.

---

## 1. Architecture & Runtime Principles

The SmartThings Edge driver runs directly on the local SmartThings Hub using Lua 5.3 and monitors/controls local network devices without cloud latency or dependency.

### 1.1 Cooperative Multitasking (`cosock`)
* **Single-Threaded Non-Preemptive Coroutines**: The Hub's Lua 5.3 runtime is built on `cosock`.
* **Non-Blocking Rules**:
  * ❌ Never execute blocking calls or CPU-hogging loops (`os.execute("sleep")`, `while true do end`), as they freeze all drivers across the hub.
  * ✅ Always set an explicit timeout when using `cosock.socket` (`sock:settimeout(3)`).
  * ✅ For scheduled delays, use `device.thread:call_with_delay(sec, func)` or `socket.sleep(sec)`.

### 1.2 Single Source of Truth
* Do not store device state in module-level global variables (e.g. `local is_device_created = false`).
* Always resolve device instances via `driver:get_devices()` and the unique network identifier (`device.device_network_id`).

### 1.3 State Deduplication Bypass (`state_change = true`)
* If cyclical telemetry emission produces identical string values, SmartThings' default deduplication filter may drop the event, leaving the mobile UI unrefreshed.
* Always mark periodic rolling telemetry events with `cap_event.state_change = true`.

---

## 2. Gateway Ultra 6-Card UI Component Architecture

The `gateway-ultra` profile defines 6 independent components, each rendered as a distinct card in the SmartThings mobile app.

```
┌─────────────────────────────────────────────────────────────┐
│ 1. main (Devices)        : Active devices count             │
│                            [Action: refresh]                │
├─────────────────────────────────────────────────────────────┤
│ 2. wallpad (Wallpad)     : Wallpad protocol specs & params  │
│                            [Switch: Reset Auto-Probing]     │
├─────────────────────────────────────────────────────────────┤
│ 3. diagnostics (Gateway) : System uptime, resources, traffic│
│                            [Switch: Remote system reboot]   │
├─────────────────────────────────────────────────────────────┤
│ 4. logs (Logs)           : NVS reboot logs & Panic coredump │
│                            [Switch: Clear logs & coredump]  │
├─────────────────────────────────────────────────────────────┤
│ 5. network (Network)     : Wi-Fi scan results, IP / mode    │
│                            [Switch: Trigger Wi-Fi AP scan]  │
├─────────────────────────────────────────────────────────────┤
│ 6. ota (Firmware)        : Firmware version & build status  │
│                            [Switch: Run GitHub Cloud OTA]   │
└─────────────────────────────────────────────────────────────┘
```

### 2.1 Component & Capability Mapping

| Card ID | Label | Built-in Switch Action | Associated Capability | Description |
|---|---|---|---|---|
| `main` | Devices | Manual refresh (`refresh`) | `digituniverse06711.activeDevice`, `refresh` | Online device status (e.g., `23 / 23 Online`) |
| `wallpad` | Wallpad | Reset auto-probing | `switch`, `profile`, `frame`, `checksum`, `opcodes` | RS-485 frame format (`F7 [LN] [SA] [DT] [OC] [CD] [ID] [PL] [CS] EE`), protocol params |
| `diagnostics` | Gateway | Remote reboot gateway | `switch`, `uptime`, `resource`, `channel` | System uptime, CPU/RAM/Flash metrics, CH1~4 traffic counters |
| `logs` | Logs | Clear logs & core dump | `switch`, `history` | Last reboot reason & crash core dump telemetry |
| `network` | Network | Re-scan Wi-Fi APs | `switch`, `scan`, `wifi` | Detected SSID scan list, Wi-Fi SSID/RSSI & IP/Mode |
| `ota` | Firmware | Cloud OTA update | `switch`, `version`, `build` | Firmware versions (`v1.1.5 ➔ v1.1.6`), stability status (`Stable (Idle)`) |

### 2.2 Momentary Action Switch Pattern
* Switches in cards 2–6 execute commands immediately upon tap and **automatically return to the OFF state after 1.5 seconds**.
* Implementation:
  ```lua
  device.thread:call_with_delay(1.5, function()
    if comp then
      device:emit_component_event(comp, capabilities.switch.switch.off())
    end
  end)
  ```

---

## 3. Master Ticker Design Pattern

### 3.1 Motivation
* When each card manages its own `call_with_delay` recursive timer:
  1. Rolling timings desynchronize across cards, causing erratic UI updates.
  2. Spawning new anonymous closures on every tick creates garbage collection pressure in Lua.
  3. Dynamic items (e.g., Wi-Fi scan results) can collide with existing timers or cause leaks.

### 3.2 Canonical Master Ticker Structure
* **Single Periodic Schedule (`call_on_schedule`)**:
  ```lua
  local TICKER_INTERVAL = 10 -- Unified 10-second interval

  function TelemetryHandler.ensure_master_ticker(device)
    if device:get_field("master_roll_timer") then return end

    local timer = device.thread:call_on_schedule(TICKER_INTERVAL, function()
      local registry = device:get_field("ticker_registry") or {}
      for _, entry in pairs(registry) do
        if entry.items and #entry.items > 1 and entry.cap and entry.comp then
          entry.idx = (entry.idx % #entry.items) + 1
          emit_event(device, entry.comp, entry.cap[entry.attr_name]({ value = entry.items[entry.idx] }))
        end
      end
    end, "master_roll_timer")
    device:set_field("master_roll_timer", timer)
  end
  ```
* **Benefits**:
  1. Minimizes resource footprint with only one timer thread on the hub.
  2. All multi-line metric cards rotate synchronously every 10 seconds.
  3. Dynamically updated lists (e.g., scanned SSIDs) register into the ticker loop seamlessly via `register_ticker`.

---

## 4. Driver Lifecycle Best Practices

```
[Discovery / Join] ──> added ──(Synthetic event)──> init ──> [Normal Operation]
                                                      │
[Driver Restart / Hub Reboot] ────────────────────────┘
                                                      │
[Preferences Updated] ───────────────────────> infoChanged
                                                      │
[Device Deleted] ────────────────────────────> removed
```

### 4.1 Lifecycle Responsibilities
1. **`init`**:
   - **Primary entry point** executed on hub reboots, driver updates, and after pairing.
   - Verify profile metadata (`try_update_metadata`), register the polling timer (`schedule_polling_timer`), and trigger initial telemetry query.
2. **`added`**:
   - Invoked **only once** when a device is newly paired.
   - The SmartThings Edge framework automatically queues a synthetic `init` call after `added`, so avoid registering duplicate timers in `added`.
3. **`infoChanged`**:
   - Called when user preferences change in the mobile app.
   - Compare `args.old_st_store.preferences` against `device.preferences` to apply only the diffs.
4. **`removed`**:
   - Explicitly cancel all timers (`cancel_timer`) when the device is deleted.

---

## 5. Code Style & Quality Standards

1. **Indentation**: 2 spaces (no tabs).
2. **Module Imports**: `require "module"` with double quotes.
3. **Naming**: `snake_case` for variables/functions, `UPPER_SNAKE_CASE` for constants.
4. **Returns**: Return a single table at the bottom of each module (`return ModuleName`).
5. **Zero Dead Code Policy**:
   - Remove unused handlers or orphaned mapping tables immediately.
6. **Defect Prevention**:
   - Never leave deprecated capability IDs in code; synchronize directly with capability schemas.
7. **Table-Driven Dispatch & FSM Invariants**:
   - ❌ **No `if-elseif` Chains for Classes/States**: Never chain `if d_cls == "..."` in telemetry/command handlers. Use dispatch tables (`HANDLERS[cls](dev, data)`).
   - ❌ **No Conditional String Formatting**: Map directions/states via lookup tables (`MAP[val]`).
   - ✅ **Encapsulated FSM Timers**: State transitions with delayed actions must cancel existing timers before scheduling to prevent race conditions.

---

## 6. SmartThings CLI Deployment Workflow

> [!CAUTION]
> **NO UNMODIFIED PACKAGE MANDATE**: 엣지드라이버 폴더(`Gateway-edge-driver/`) 내 파일 수정이 발생하지 않은 작업에서는 패키징 명령(`edge:drivers:package`) 실행이 절대 금지됩니다.

### 6.1 One-Liner Package & Deploy (Recommended)
Package, assign channel, and install to hub in a single step:
```bash
smartthings edge:drivers:package "/Users/eri/Library/CloudStorage/OneDrive-개인/Home/Gateway/Gateway-edge-driver" \
  --channel="5c5ac2ac-84fb-4783-82df-da58cc675f41" \
  --hub="b65b1792-8510-423f-b12d-00d7ff78b700"
```

### 6.2 Live Logcat Streaming
```bash
smartthings edge:drivers:logcat bcfbdfbd-891b-4cf3-a969-df9b237e1f7b \
  --hub=b65b1792-8510-423f-b12d-00d7ff78b700
```

### 6.3 Deployment Metadata Summary
* **Hub ID**: `b65b1792-8510-423f-b12d-00d7ff78b700`
* **Channel ID**: `5c5ac2ac-84fb-4783-82df-da58cc675f41` (Eri)
* **Driver ID**: `bcfbdfbd-891b-4cf3-a969-df9b237e1f7b`
* **Package Key**: `com.gateway.smartthings.controller`
* **Device Profile**: `gateway-ultra`
