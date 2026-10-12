# Hyundai Wallpad RS-485 Communication Protocol Specification

> **Specification Version**: 2.0 (Universal Template Architecture)  
> **Target Vendor / Series**: Hyundai HT (HDHN series, e.g., HDHN-2000) & P2P Bus Residential Complexes

Standard baseline for Hyundai HT RS-485 wallpad communication frames, slot offsets, and device interaction invariants.

---

## 1. Physical Layer & Serial Specifications

### 1.1 Main Control Bus (CH1–CH3, CH5)
- **Target Devices**: Lights, Thermostats, Ventilation, Gas Valve, Outlets, HVAC/FCU, Elevator, HEMS.
- **Protocol / Medium**: RS-485 Half-Duplex
- **Serial Parameters**: `9600 bps`, `8 Data bits`, `1 Stop bit`, `No Parity (8N1)`

### 1.2 Sub Video Phone Bus (CH4 Kitchen TV / Sub Phone)
- **Target Devices**: Front Doorphone, Lobby Intercom, Door Strike Release.
- **Protocol / Medium**: UART (SoftwareSerial Half-Duplex)
- **Serial Parameters**: `3840 bps` (Auto-tunes to 3880–3890 bps depending on line capacitance), `8N1`

### 1.3 Architecture Comparison: New P2P vs. Legacy 34B

| Feature | Legacy System (< 2015) | New System (P2P Standard) |
|---|---|---|
| **Polling Topology** | Full-apartment broadcast (all rooms in one frame) | Point-to-Point (P2P) individual room query/response |
| **Frame Length** | Fixed 34-byte (`0x22`) monolithic frame | 11B Query $\leftrightarrow$ 11B/13B/14B/15B/18B Response |
| **Addressing** | Fixed array byte offsets | Split nibble address (`Byte #6` = `Room | Dev`) |
| **Control Mechanism** | Modifies room bits inside 34B broadcast frame | Dedicated 11-byte command frame (`F7 0B 01 ...`) |
| **Control ACK** | Waits for next global broadcast cycle | Immediate Echo ACK (`01 01` for ON, `02 02` for OFF) |
| **Command Categories** | Monolithic control byte | Category codes: Power (`0x40`), Mode (`0x41`), Fan (`0x42`), Temp (`0x45`) |

---

## 2. Packet Framing & Checksum Algorithms

### 2.1 Main Control Bus Framing (`0xF7` .. `0xEE`)

| Byte Index | Field | Value | Description |
|:---:|:---|:---:|:---|
| **0** | **Prefix (STX)** | `0xF7` | Frame start delimiter |
| **1** | **Length ($N$)** | `N` | Total frame length in bytes (`0x0B`: 11B, `0x0D`: 13B, `0x12`: 18B) |
| **2** | **Sender ID** | `0x01` | Master transmitter ID (Wallpad Master) |
| **3** | **Device Type** | `??h` | Target device identifier (`0x18`, `0x19`, `0x1B`, `0x1C`, `0x1F`, `0x2B`, `0x34`) |
| **4** | **Packet Type** | `??h` | `0x01`: Query (QRY) / Broadcast (BC), `0x02`: Control (CTL), `0x04`: Response (ACK) |
| **5 .. $N-3$** | **Payload** | Var | Category, Room/Device sub-address, state/control parameters ($M = N - 7$ bytes) |
| **$N-2$** | **Checksum** | `??h` | XOR sum of bytes 0 through $N-3$ |
| **$N-1$** | **Suffix (ETX)** | `0xEE` | Frame end delimiter |

#### Checksum Equation (XOR Sum)
$$\text{Checksum} = \bigoplus_{i=0}^{N-3} \text{Packet}[i]$$

### 2.2 Doorphone Framing (`0x7F` .. `0xEE`, 5 Bytes Fixed)

| Byte Index | Field | Value | Description |
|:---:|:---|:---:|:---|
| **0** | **STX** | `0x7F` | Doorphone start delimiter |
| **1** | **Opcode** | `??h` | Ring call event or 3-step door release command |
| **2** | **Arg1** | `0x00` | Padding byte |
| **3** | **Arg2** | `0x00` | Padding byte |
| **4** | **ETX** | `0xEE` | Doorphone end delimiter |

### 2.3 Packet Type Code Registry

| Code | Type | Direction | Description |
|:---:|:---:|:---:|:---|
| `0x01` | **QRY** | Master $\rightarrow$ Device | Periodic polling query |
| `0x02` | **CTL** | Master $\rightarrow$ Device | Discrete 11B state change command |
| `0x04` | **ACK** | Device $\rightarrow$ Master | Status telemetry or immediate control echo |
| `0x01` | **BC** | Device $\rightarrow$ Bus | Spontaneous broadcast event (elevator arrival, away switch) |

---

## 3. Device & Category Registry

### 3.1 Device Type Master Table

| DevID | Name | QRY Len | ACK Len | CTL ACK Len | Supported Functions |
|:---:|:---|:---:|:---:|:---:|:---|
| `0x19` | Light | 11B | 11B | 11B | Discrete power, all-lights-off per room |
| `0x1A` | Dimming Light | — | — | 11B | Brightness level (0–100%) |
| `0x15` | Color Temp Light | — | — | 11B | Color temperature steps |
| `0x18` | Thermostat | 11B | **18B** | **13B** | Power, target temperature, away mode |
| `0x1C` | HVAC / FCU | 11B | **14B/15B/18B** | 11B | Power, mode, target temp, fan speed, vane swing |
| `0x1F` | Smart Outlet | 11B | **18B** | 11B | Relay power, standby-power cutoff mode, 1W power metering |
| `0x2B` | Ventilation (ERV) | 11B | **13B** | **13B** | Power, fan speed, operation mode |
| `0x1B` | Gas Valve | 11B | **13B** | **13B** | Close command only (Remote open prohibited by law) |
| `0x34` | Elevator | — | — | 11B | Call trigger (CTL), arrival broadcast (13B BC) |
| `0x41`/`0x36` | Away / Security Switch | — | — | — | Spontaneous broadcast (BC) for home occupancy sync |
| `0x30`/`0x32` | HEMS Remote Metering | — | **18B** | — | Real-time metering (electric, water, gas, hot water) |
| **CH4** | Doorphone (Front/Lobby) | — | **5B** | **5B $\times$ 3** | Ring event detection and 3-step door release FSM |

### 3.2 Category (Cat) Code Registry

| Cat Code | Primary Function | Target Devices | Description / Variants |
|:---:|:---|:---|:---|
| `0x40` | Power Control / Status Query | Universal | General ON/OFF state query and control |
| `0x41` | Operation Mode / Elevator | `0x1C`, `0x34` | HVAC cool/dry/fan/auto/heat (Variants: `0x43`, `0x5C`) |
| `0x42` | Fan Speed | `0x1C`, `0x2B` | HVAC and ERV fan speed levels (Variant: `0x5D`) |
| `0x43` | Aux Mode / Gas Valve | `0x2B`, `0x1F`, `0x1B` | ERV modes, outlet standby mode, gas valve query/close |
| `0x44` | Vane Swing | `0x1C` | HVAC vertical oscillation start/stop |
| `0x45` | Target Temperature | `0x18`, `0x1C` | Thermostat & HVAC target setpoint (0.5°C uses `+0x80` MSB) |
| `0x46` | Thermostat Status / Power | `0x18` | Thermostat 1:1 QRY and Power/Away control |
| `0x5C` | Mode Control (Variant) | `0x1C` | Site-specific external HVAC gateway mode command |
| `0x5D` | Fan Speed (Variant) | `0x1C` | Site-specific external HVAC gateway fan speed command |

---

## 4. Master Packet Reference Table

| DevID | Device | Packet Type | Len | Frame Structure & Payload | Slot Details |
|:---:|:---|:---|:---:|:---|:---|
| **0x19** | **Light** | **Query (QRY)** | 11B | `F7 0B 01 19 01 40 [Room\|Dev] 00 00 [CS] EE`<br>Room poll: `F7 0B 01 19 01 40 [Room\|0] 00 00 [CS] EE` | `Room\|Dev`: Room & Light ID (e.g., Room 1 Light 1 = `0x11`)<br>`Dev=0`: Batch queries all lights in room |
| | | **Query ACK** | 11B | `F7 0B 01 19 04 40 [Room\|Dev] 00 [State] [CS] EE` | **Byte #8 = State** (`0x01`: ON, `0x02`: OFF) |
| | | **Control (CTL)** | 11B | `F7 0B 01 19 02 40 [Room\|Dev] [Cmd] 00 [CS] EE`<br>Room off: `F7 0B 01 19 02 40 [Room\|0] 02 00 [CS] EE` | **Byte #7 = Cmd** (`0x01`: ON, `0x02`: OFF)<br>`Dev=0` + `Cmd=0x02` turns off all room lights |
| | | **Control ACK** | 11B | `F7 0B 01 19 04 40 [Room\|Dev] [Cmd] [State] [CS] EE` | **#7 Echo, #8 State** $\rightarrow$ ON: `01 01`, OFF: `02 02` |
| **0x1A** | **Dimming** | **Control (CTL)** | 11B | `F7 0B 01 1A 02 40 [Room\|Dev] [Level] 00 [CS] EE` | **Byte #7 = Level** (`0x00`–`0x64`, 0–100%) |
| | | **Control ACK** | 11B | `F7 0B 01 1A 04 40 [Room\|Dev] [Echo] [Level] [CS] EE` | **#7 Echo, #8 Applied Level** |
| **0x15** | **Color Temp** | **Control (CTL)** | 11B | `F7 0B 01 15 02 40 [Room\|Dev] [Tone] 00 [CS] EE` | **Byte #7 = Tone** step value |
| | | **Control ACK** | 11B | `F7 0B 01 15 04 40 [Room\|Dev] [Echo] [Tone] [CS] EE` | **#7 Echo, #8 Applied Tone** |
| **0x18** | **Thermostat** | **Query (QRY)** | 11B | `F7 0B 01 18 01 46 [1\|Room] 00 00 [CS] EE` | `1\|Room`: Target room (`0x11`–`0x14`) |
| | | **Query ACK** | **18B** | `F7 12 01 18 04 46 [1\|Room] 00 [State] [Amb] [Tgt] 00 00 00 00 00 [CS] EE` | **#8**: State (`0x01`: ON, `0x04`: OFF, `0x07`: Away)<br>**#9**: Ambient temp (`Amb`), **#10**: Target temp (`Tgt`)<br>*Away (`0x07`) maintains ON with auto Tgt=10°C (`0x0A`)* |
| | | **Control (CTL)** | 11B | Power: `F7 0B 01 18 02 46 [1\|Room] [Cmd] 00 [CS] EE`<br>Temp: `F7 0B 01 18 02 45 [1\|Room] [Tgt] 00 [CS] EE` | Power `Cmd`: `0x01` (ON), `0x04` (OFF), `0x07` (Away)<br>Temp: Cat `0x45`, `Tgt` = Integer °C (e.g. 24°C $\rightarrow$ `0x18`) |
| | | **Control ACK** | **13B** | `F7 0D 01 18 04 [Cat] [1\|Room] [Echo] [State] [Amb] [Tgt] [CS] EE` | **#7**: Echo, **#8**: State, **#9**: Amb, **#10**: Tgt, **#11**: CS, **#12**: EE |
| **0x1C** | **HVAC / FCU** | **Query (QRY)** | 11B | `F7 0B 01 1C 01 40 [Room\|Dev] 00 00 [CS] EE` | `Room\|Dev`: e.g., Living room `0x11`, Master bedroom `0x21` |
| | | **Query ACK** | **14B/15B/18B** | **15B**: `F7 0F 01 1C 04 40 [Room\|Dev] 00 [State] [Mode] [Speed] [Amb] [Tgt] [CS] EE`<br>**14B**: `F7 0E 01 1C 04 40 [Room\|Dev] [State] [Mode] [Speed] [Amb] [Tgt] [CS] EE`<br>**18B**: `F7 12 01 1C 04 40 [Room\|Dev] 00 [State] [Mode] [Speed] [Amb] [Tgt] 00 00 00 [CS] EE` | **#8 (or 14B #7)**: State (`0x01`: ON, `0x02`: OFF, `0x80` MSB: Fault)<br>**Mode**: `0x01` Cool, `0x02` Dry, `0x03` Fan, `0x04` Auto, `0x05` Heat<br>**Speed**: `0x01` Low, `0x02` Mid, `0x03` High, `0x00`/`0x04` Auto<br>**Amb / Tgt**: Hex °C (0.5°C flag: `+0x80`) |
| | | **Control (CTL)** | 11B | Power: `F7 0B 01 1C 02 40 [Room\|Dev] [Cmd] 00 [CS] EE`<br>Temp: `F7 0B 01 1C 02 45 [Room\|Dev] [Tgt] 00 [CS] EE`<br>Speed: `F7 0B 01 1C 02 42 [Room\|Dev] [Spd] 00 [CS] EE`<br>Mode: `F7 0B 01 1C 02 41 [Room\|Dev] [Mode] 00 [CS] EE`<br>Swing: `F7 0B 01 1C 02 44 [Room\|Dev] [Swing] 00 [CS] EE` | `Cmd`: `0x01` ON, `0x02` OFF<br>`Tgt`: 18–30°C (`0x12`–`0x1E`, 0.5°C: `+0x80`)<br>`Spd`: `0x01` Low, `0x02` Mid, `0x03` High (Variant Cat: `0x5D`)<br>`Mode`: `0x01` Cool, `0x02` Dry, `0x03` Fan, `0x05` Heat (Var Cat: `0x43`, `0x5C`)<br>`Swing`: `0x01` Off, `0x02` Oscillate |
| | | **Control ACK** | **11B** | `F7 0B 01 1C 04 [Cat] [Room\|Dev] [Echo] [State/Echo] [CS] EE` | Immediate Echo ACK: ON $\rightarrow$ `01 01`, OFF $\rightarrow$ `02 02` |
| **0x1F** | **Smart Outlet** | **Query (QRY)** | 11B | `F7 0B 01 1F 01 40 [Room\|Idx] 00 00 [CS] EE` | `Room\|Idx`: Room & outlet index (e.g. `0x31`) |
| | | **Query ACK** | **18B** | `F7 12 01 1F 04 40 [Room\|Idx] 00 [State] [W_H] [W_L] 00 00 00 00 [Mode] [CS] EE` | **#8**: State (`0x01` ON, `0x02` OFF)<br>**#9–10**: Power consumption in **1W integer Big-Endian**<br>**#15**: Mode (`0x01`: Always-on, `0x02`: Standby cutoff) |
| | | **Power CTL** | 11B | `F7 0B 01 1F 02 40 [Room\|Idx] [Cmd] 00 [CS] EE` | **#7 Cmd**: `0x01` ON, `0x02` OFF |
| | | **Mode CTL** | 11B | `F7 0B 01 1F 02 43 [Room\|Idx] [ModeCmd] 00 [CS] EE` | **#7 ModeCmd**: `0x01` Always-on, `0x02` Auto-cutoff |
| | | **Control ACK** | **11B** | `F7 0B 01 1F 04 [Cat] [Room\|Idx] [Cmd] [State] [CS] EE` | **#7 Echo, #8 State** $\rightarrow$ ON: `01 01`, OFF: `02 02` |
| **0x2B** | **Ventilation** | **Query (QRY)** | 11B | `F7 0B 01 2B 01 40 11 00 00 [CS] EE` | Primary power/speed query |
| | | **Query ACK** | **13B** | `F7 0D 01 2B 04 40 11 00 [State] [Mode\|Spd] FF [CS] EE` | **#8**: State (`0x01` ON, `0x02`/`0x00` OFF)<br>**#9**: `0x11` Low, `0x13` Mid, `0x17` High, `0x10` Auto |
| | | **Mode QRY** | 11B | `F7 0B 01 2B 01 43 11 00 00 84 EE` | Category `0x43` operation mode query (Gateway-injected) |
| | | **Mode ACK** | **13B** | `F7 0D 01 2B 04 43 11 00 [Mode] [Speed] FF [CS] EE` | **#8 Mode**: `0x01` Normal, `0x02` Bypass, `0x03` Auto, `0x04` Clean, `0x81` Reject<br>**#9 Speed**: `0x11` Low, `0x13` Mid, `0x17` High, `0x10` Auto |
| | | **Control (CTL)** | 11B | Power: `F7 0B 01 2B 02 40 11 [Cmd] 00 [CS] EE`<br>Speed: `F7 0B 01 2B 02 42 11 [Spd] 00 [CS] EE`<br>Mode: `F7 0B 01 2B 02 43 11 [Mode] 00 [CS] EE` | `Cmd`: `0x01` ON, `0x02` OFF<br>`Spd`: `0x01` Low, `0x03` Mid, `0x07` High<br>`Mode`: `0x01` Normal, `0x02` Bypass, `0x03` Auto, `0x04` Clean |
| | | **Control ACK** | **13B** | `F7 0D 01 2B 04 [Cat] 11 00 [Echo/Mode] [Speed] FF [CS] EE` | Echoes status; wall remote interlock may return `0x81` on mode override |
| **0x1B** | **Gas Valve** | **Query (QRY)** | 11B | `F7 0B 01 1B 01 43 11 00 00 [CS] EE` | Status query via Category `0x43` |
| | | **Query ACK** | **13B** | `F7 0D 01 1B 04 43 11 00 [State] 00 00 [CS] EE` | **Byte #8 = State** (`0x01`: Open, `0x04`: Closed) |
| | | **Control (CTL)** | 11B | `F7 0B 01 1B 02 43 11 02 00 [CS] EE` | **Close command only (`0x02`)**; Remote open is physically locked |
| | | **Control ACK** | **13B** | `F7 0D 01 1B 04 43 11 00 04 00 00 [CS] EE` | **Byte #8 = 0x04** confirms closed status |
| **0x34** | **Elevator** | **Call (CTL)** | 11B | `F7 0B 01 34 02 41 10 06 00 9C EE` | **Byte #7 = 0x06** triggers call sequence |
| | | **Call ACK** | 11B | `F7 0B 01 34 04 41 10 00 06 9A EE` | **Byte #8 = 0x06** indicates active call / transit |
| | | **Arrival (BC)** | **13B** | `F7 0D 01 34 01 41 10 00 01 [Floor] [Car] [CS] EE` | **#8**: `0x01` Arrival event, **#9**: Resident floor, **#10**: Car number |
| | | **Reset (RST)** | 11B | `F7 0B 01 34 04 41 10 00 00 9C EE` | **Byte #8 = 0x00** idle state restored |
| **CH4** | **Front Door** | **Ring (RX)** | **5B** | `7F B5 00 00 EE` | Front door chime received (`bell_front = 0xB5`) |
| *(3840 bps)* | | **End (RX)** | **5B** | `7F B8 00 00 EE` | Call missed / call ended (`end_front`) |
| | | **Release FSM** | **5B $\times$ 3** | ① Call: `7F B9 00 00 EE`<br>② Open: `7F B4 00 00 EE`<br>③ End: `7F B8 00 00 EE` | **Front door release sequence**:<br>• 50 ms line silence guard time<br>• Send ① `0xB9`, wait 350 ms<br>• Send ② `0xB4`, wait 750 ms<br>• Send ③ `0xB8` to release lock and reset |
| **CH4** | **Lobby Door** | **Ring (RX)** | **5B** | `7F 5A 00 00 EE` | Lobby intercom chime received (`bell_lobby = 0x5A`) |
| *(3840 bps)* | | **End (RX)** | **5B** | `7F 60 00 00 EE` | Lobby call ended (`end_lobby = 0x60`) |
| | | **Release FSM** | **5B $\times$ 3** | ① Call: `7F 5F 00 00 EE`<br>② Open: `7F 61 00 00 EE`<br>③ End: `7F 60 00 00 EE` | **Lobby door release sequence**:<br>• 50 ms line silence guard time<br>• Send ① `0x5F`, wait 350 ms<br>• Send ② `0x61`, wait 750 ms<br>• Send ③ `0x60` to complete release |
| **0x41/0x36** | **Away Switch** | **Away Event (BC)**| 11B | `F7 0B 01 [Dev] 01 40 10 [State] 00 [CS] EE` | `Byte #7`: `0x01` Home/Disarm, `0x02` Away/Arm |
| **0x30/0x32** | **HEMS Meter** | **Data ACK** | **18B** | `F7 12 01 [Dev] 04 40 [Media] 00 [Data_4B] 00 00 00 00 [CS] EE` | `Media`: `0x01` Electric, `0x02` Water, `0x03` Gas, `0x04` Hot water<br>`Data_4B`: 4-byte cumulative meter counter |

---

## 5. Architectural Invariants & Protocol Rules

### 5.1 Universal State Slot = Byte #8
The primary operational state is universally encoded in **`Byte #8`**:
- Light (`0x19`): `0x01` ON, `0x02` OFF
- Thermostat (`0x18`): `0x01` ON, `0x04` OFF, `0x07` Away
- HVAC (`0x1C`): `0x01` ON, `0x02` OFF *(Except 14B packet where padding is omitted $\rightarrow$ Byte #7)*
- Outlet (`0x1F`): `0x01` ON, `0x02` OFF
- ERV (`0x2B`): `0x01` ON, `0x02`/`0x00` OFF
- Gas (`0x1B`): `0x01` Open, `0x04` Closed
- Elevator (`0x34`): `0x06` In transit, `0x00` Idle

### 5.2 Universal Control Slot = Byte #7
- 11-byte control commands place the action parameter in **`Byte #7`**.
- Control ACK packets echo `Byte #7` as verification, creating duplicate consecutive bytes:
  - ON Success: `0x01` + `0x01` $\rightarrow$ **`01 01`**
  - OFF Success: `0x02` + `0x02` $\rightarrow$ **`02 02`**

### 5.3 HVAC Multi-Command Serialization & 120 ms TX Guard
- When transmitting compound commands (Power ON + Cool + 24°C + High), each category frame (`0x40`, `0x41`, `0x45`, `0x42`) must be serialized via a FIFO TX queue.
- **Command Precedence**: HVAC gateways (e.g. LG PI485) drop mode/temperature packets received while powered off. Therefore, **Power ON (`0x40`) must precede all other commands**:
  $$\mathbf{Power(0x40)} \xrightarrow{120\text{ ms}} \mathbf{Mode(0x41)} \xrightarrow{120\text{ ms}} \mathbf{Target(0x45)} \xrightarrow{120\text{ ms}} \mathbf{Speed(0x42)}$$
- **Mandatory Guard Delay**: Enforce a **100–150 ms (nominal 120 ms)** interval between consecutive TX frames to prevent RS-485 line collisions and buffer overflows.

### 5.4 Doorphone 3-Step FSM Sequence
Hyundai doorphone hardware ignores raw door-release signals unless an active audio session is established. The release sequence must execute strictly in 3 steps:
$$\mathbf{Call(Op1)} \xrightarrow{350\text{ ms}} \mathbf{Open(Op2)} \xrightarrow{750\text{ ms}} \mathbf{End(Op3)}$$
A **50 ms silent line guard time** is required prior to initiating Step 1.

### 5.5 Temperature Resolution & 0.5°C Encoding
- Standard temperatures use integer Hex (°C).
- For systems supporting 0.5°C increments, the **MSB (`0x80`) serves as the 0.5°C flag** (e.g. 24.5°C = `0x18 | 0x80 = 0x98`).
- Integer extraction: `TempByte & 0x7F`; add 0.5°C if `(TempByte & 0x80) != 0`.

### 5.6 HVAC Error Mask (`0x80`) & 0x81 NAK Defense
- **Error Flag**: The MSB of the state byte (`State & 0x80`) indicates outdoor unit / condensate drain fault.
- **NAK Defense**: Aircon controllers return `0x81` (NAK / Command Reject) during mode conflicts (e.g., requesting heat while another zone cools). Gateways must drop `0x81` packets to avoid corrupting valid cached states.

### 5.7 Dynamic HVAC Packet Length Offset
Depending on indoor unit firmware, HVAC ACK frames arrive with length 13B, 14B, 15B, or 18B:
- **14B Frame**: `state_idx = 7` (padding byte omitted)
- **15B/18B Frame**: `state_idx = 8`
- Dynamic selection: `state_idx = (ack.length == 14) ? 7 : 8`.

---

## 6. Corrected Open-Source Fallacies vs. Empirical Facts

| Component | Open-Source Fallacy (GitHub / Forums) | Verified Hardware Reality | Severity / Impact |
|:---|:---|:---|:---|
| **Thermostat (`0x18`)** | Assumed 34B broadcast | Per-room (`0x11`–`0x14`) **18B Query ACK** & **13B Control ACK** | **Critical**: 34B parser drops 100% of modern traffic |
| **Ventilation (`0x2B`)** | Stated as 12B without operation modes | Fixed **13B** frame; Cat `0x43` queries operation modes | **Verified**: Mode queries require direct gateway injection |
| **Gas Valve (`0x1B`)** | Bi-directional open/close assumed | **Close Only (`0x02`)**; Remote open is physically locked | **Safety**: Open commands prohibited by fire/gas safety codes |
| **Outlet (`0x1F`)** | Fractional power scaling assumed | **1W integer Big-Endian (Bytes 9–10)**; 11B immediate ACK | **Accurate**: Direct 1W integer reading without 0.1x scaling |
| **HVAC (`0x1C`)** | Fixed 14B assumed; ignored wall remote | **13B/14B/15B/18B variable lengths**; Wall remotes operate as slaves | **Critical**: Requires dynamic offset and `0x81` NAK filtering |
| **Doorphone (CH4)** | Single-step immediate command | **3-Step FSM with 50 ms line silence guard** | **Critical**: Door fails to open without 3-step sequence |

---

## 7. Appendix: Parser Reference Implementations

### 7.1 Packet Integrity & Checksum Verification
```text
function VerifyPacket(packet, length):
    if length < 5: return false
    if packet[0] != 0xF7: return false
    if packet[length - 1] != 0xEE: return false
    if packet[1] != length: return false

    calculated_cs = 0
    for i from 0 to length - 3:
        calculated_cs = calculated_cs XOR packet[i]

    return (calculated_cs == packet[length - 2])
```

### 7.2 Dynamic HVAC Parsing
```text
function ParseAirconDynamic(packet, length):
    room_dev = packet[6]
    state_idx = (length == 14) ? 7 : 8

    state = packet[state_idx]
    mode  = packet[state_idx + 1]
    speed = packet[state_idx + 2]
    amb   = packet[state_idx + 3]
    tgt   = packet[state_idx + 4]

    has_fault = (state & 0x80) != 0
    is_on     = (state & 0x7F) == 0x01
    target_c  = (tgt & 0x7F) + (((tgt & 0x80) != 0) ? 0.5 : 0.0)
```
