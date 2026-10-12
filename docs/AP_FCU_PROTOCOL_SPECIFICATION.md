# AP FCU (Fan Coil Unit) RS-485 Modbus Protocol Specification

Defines the RS-485 Modbus-RTU communication specification and control interface for the **AP 2-Pipe Changeover Ceiling Cassette Fan Coil Unit (FCU)** controller board. Fully validated against physical hardware measurements (impedance, bias voltage, register captures, and IR remote coexistence).

---

## 1. Physical Layer & Hardware Wiring

### 1.1 Port Location & Pinout
- **Port**: `485-IN` (Red 3-pin connector on upper-right board section).
  > [!IMPORTANT]
  > Do **not** connect to `485-OUT` (Red 2-pin connector); it is disabled by factory firmware in single-household configurations.
- **Pinout (3-Pin Measured)**:

| Pin | Position / Wire | Signal | Measurement / Characteristics |
|:---:|:---|:---:|:---|
| **1** | Left (Capacitor side) / Black | **RS-485 B (-)** | 52.7 kΩ to Pin 3 |
| **2** | Center / Shield | **GND** | Internal shield / DC 0 Ω to Pin 1 |
| **3** | Right / Red | **RS-485 A (+)** | +300 mV DC bias relative to Pin 1 |

### 1.2 Serial Configuration (EW11 Settings)
- **Baud Rate**: `9600 bps` | **Data Bits**: `8` | **Stop Bits**: `1` | **Parity**: `None` | **Flow Control**: `None`
- **EW11 UART Protocol**: `NONE` (Transparent transmission mode mandatory)

---

## 2. Protocol Specification

- **Protocol**: Standard Modbus-RTU
- **Default Slave ID**: `0x01`
- **Supported Function Codes**:
  - `0x03` : **Read Holding Registers** (Status & ambient temperature readout)
  - `0x06` : **Write Single Register** (Individual parameter control)
  - `0x10` : **Write Multiple Registers** (Batch control)
  > [!NOTE]
  > Function codes `0x01`, `0x02`, `0x04`, `0x05` are unsupported. All parameters reside in Holding Registers `0x0000`–`0x0006`. Addresses $\ge$ `0x0007` return Exception `0x83 0x02` (*Illegal Data Address*).

---

## 3. Holding Register Map (`0x0000`–`0x0006`)

| Address (Hex) | PLC Addr | Name | Access | Valid Values | Description |
|:---:|:---:|:---|:---:|:---:|:---|
| `0x0000` | `40001` | **Central Lock** | R/W | `0`<br>`1` | **`0`: Local mode (IR remote enabled; MUST MAINTAIN 0)**<br>`1`: Central lock (IR remote locked/ignored) |
| `0x0001` | `40002` | **Operation Mode** | R/W | `1`<br>`2`<br>`3` | `1`: Cool<br>`2`: Heat (Active during winter hot-water changeover)<br>`3`: Fan Only |
| `0x0002` | `40003` | **Fan Speed** | R/W | `0`<br>`1`<br>`2`<br>`3`<br>`4` | `0`: Off (Triggers 30–40 s purge delay)<br>`1`: Low<br>`2`: Medium<br>`3`: High<br>`4`: Auto |
| `0x0003` | `40004` | **Vane Swing** | R/W | `0`<br>`2` | `0`: Swing Off (Vane fixed)<br>`2`: Swing On (Continuous oscillation) |
| `0x0004` | `40005` | **Alarm Code** | Read | `0`<br>`1–5` | `0`: Normal (No fault)<br>`1–5`: Hardware fault (sensor, drain pan, etc.) |
| `0x0005` | `40006` | **Target Temp** | R/W | `18–30` | Target setpoint in °C (synchronizes with IR remote) |
| `0x0006` | `40007` | **Room Temp** | Read | Measured | Current ambient temperature in °C |

---

## 4. Packet Reference (Hex & CRC-16)

### 4.1 Status Query (Registers 0–6 Batch)
- **TX**: `01 03 00 00 00 07 04 08`
- **RX**: `01 03 0E [Reg0] [Reg1] [Reg2] [Reg3] [Reg4] [Reg5] [Reg6] [CRC_L] [CRC_H]`
  - *Example RX*: `01 03 0E 00 00 00 01 00 04 00 02 00 00 00 1A 00 1A 5A E5`
  - *Decoded*: Lock: `0`, Mode: `Cool`, Fan: `Auto (4)`, Swing: `On (2)`, Alarm: `0`, Target: `26°C`, Room: `26°C`

### 4.2 Power Control (Remote-Lock Safe)
- **Turn On (Cool / High / 18°C)**: `01 10 00 01 00 03 06 00 01 00 03 00 12 A3 2A`
  *(Writes Regs 1–3 without altering Reg 0, preserving IR remote operation)*
- **Turn On (Fan Only / High)**: `01 10 00 01 00 02 04 00 03 00 03 F2 48`
- **Turn Off (Fan Speed 0)**: `01 06 00 02 00 00 28 0A`
  *(Fan runs for ~30–40 s post-command to dissipate residual heat/drain condensate before full shutdown)*

### 4.3 Fan Speed Control
- **Low (1)**: `01 06 00 02 00 01 E9 CB` | **Medium (2)**: `01 06 00 02 00 02 A9 CA`
- **High (3)**: `01 06 00 02 00 03 68 0B` | **Auto (4)**: `01 06 00 02 00 04 29 C8`

### 4.4 Vane Swing Control
- **Swing On**: `01 06 00 03 00 02 F8 0B` | **Swing Off**: `01 06 00 03 00 00 79 CB`

### 4.5 Target Setpoint Control
- **18°C**: `01 06 00 05 00 12 18 0E` | **20°C**: `01 06 00 05 00 14 98 0F` | **22°C**: `01 06 00 05 00 16 19 CF`
- **24°C**: `01 06 00 05 00 18 99 CA` | **26°C**: `01 06 00 05 00 1A 19 C8` | **28°C**: `01 06 00 05 00 1C 98 09`

### 4.6 Mode Select
- **Cool (1)**: `01 06 00 01 00 01 19 CA` | **Heat (2)**: `01 06 00 01 00 02 59 CB` | **Fan Only (3)**: `01 06 00 01 00 03 98 0B`

---

## 5. Hardware & Operational Characteristics

1. **IR Remote Coexistence**:
   - `0x0000` must remain `0`. Writing `1` engages central control lock, disabling the physical IR remote receiver.
   - When updating `0x0001`–`0x0005`, RS-485 commands and the IR remote coexist seamlessly with bi-directional state synchronization.
2. **Delayed Shutdown (Delay-Off)**:
   - Power-off commands (Fan = `0`) do not immediately halt the blower. The motor continues spinning for 30–40 s to purge condensation and prevent mold.
3. **2-Pipe Hydronic Changeover**:
   - Pipe temperature sensors regulate operational viability based on water supply temperature (chilled vs. hot).

---

## 6. High-Speed Modbus CRC-16 Architecture (Slice-by-4 SWAR)

The gateway engine uses a **Slice-by-4 SWAR (SIMD Within A Register)** algorithm for Modbus-RTU frames (poly: `0xA001`, init: `0xFFFF`, Little-Endian).

### 6.1 Architecture & Memory Footprint
- **Word Processing**: Processes 4 bytes (32-bit word) per iteration against four precomputed lookup tables ($T_0, T_1, T_2, T_3$).
- **Storage**: $4 \times (256 \times 2\text{ bytes}) = 2{,}048\text{ bytes}$ located in Flash `.rodata` (`PROGMEM`). **0 bytes SRAM overhead**.
- **Formula**:
  $$CRC_{new} = T_3[(CRC \oplus B_0) \text{ \& } 0\text{xFF}] \oplus T_2[((CRC \gg 8) \oplus B_1) \text{ \& } 0\text{xFF}] \oplus T_1[B_2] \oplus T_0[B_3]$$
- **Tail Bytes**: Remaining 1–3 bytes are processed sequentially using table $T_0$.

### 6.2 Performance Comparison
| Metric | Byte-by-Byte LUT | Slice-by-4 SWAR | Improvement |
|---|:---:|:---:|:---:|
| **4-Byte Processing Cycles** | ~140 cycles | **~35 cycles** | **4x speedup** |
| **D-Cache / SRAM Overhead** | 0 Bytes | **0 Bytes** (Flash `.rodata`) | Zero heap / Zero RAM |
| **Compile-Time Safety** | Runtime manual check | **`static_assert` golden vectors** | `01 06 00 00 00 00` $\rightarrow$ `0x0804` |
