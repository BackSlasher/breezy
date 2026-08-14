# AC LCD Protocol Reverse Engineering - Data Dump

## Hardware Setup

- **Bus Pirate 5** connected via:
  - `/dev/ttyACM0` - Interactive terminal (picocom, INFRARED mode, etc.)
  - `/dev/ttyACM1` - OLS/SUMP mode for logic capture (sigrok-cli)
- **Target**: AC controller LCD panel (USP5010BE) connected to hub board (USP5020)
- **Capture tool**: `sigrok-cli` with OLS driver

### Wiring (Bus Pirate → AC)
| Bus Pirate | Signal | Function |
|------------|--------|----------|
| IO0 | Data | Bidirectional SPI data (TDM: controller sends display, panel sends buttons) |
| IO1 | Clock | ~7.1kHz clock (controller generates) |
| IO2 | ??? | Purpose unknown - stays high, brief low at end of captures |
| IO4 | IR TX | For IR injection via diode (IRTX in INFRARED mode) |
| IO5 | IR RX | Raw demodulated IR signal from receiver (monitoring) |
| GND | GND | Ground |
| +5V | VCC | Power (not captured) |

**Wire summary (active signals):**
- **Data (IO0)**: Single bidirectional data line, time-division multiplexed
- **Clock (IO1)**: SPI clock from controller, ~7.1kHz, asymmetric duty cycle
- **IR RX (IO5)**: Panel's IR receiver output, raw demodulated signal to controller
- **IR TX (IO4)**: BP5 IRTX output → diode → IR line (for injection)

## Protocol Characteristics

- **NOT I2C** (despite initial assumption) - proven by analyzing START/STOP conditions
- **SPI-like** clocked serial protocol
- Clock on CH1, Data on CH0
- Data sampled on clock rising edge
- Mode: CPOL=0, CPHA=1 works for decoding

### Clock Timing (Critical for Capture)
- **Clock frequency**: ~7.1 kHz (period ~140µs)
- **Duty cycle**: Highly asymmetric (~80% low, ~20% high)
- **High pulse width**: ~25-30µs (very short!)
- **Minimum sample rate**: 75kHz required for reliable capture
  - At 75kHz: 2-3 samples during high pulse → reliable edge detection
  - At 50kHz: only 1 sample during high pulse → unreliable, causes decode errors
  - Below 50kHz: often misses high pulse entirely → garbage data
- **Buffer limit**: Bus Pirate OLS mode holds 131072 samples max
- **Max capture duration**: 131072 / 75000 = **1.75 seconds** at 75kHz

### Frame Structure
- **112 bits (14 bytes) per frame**
- **~6 Hz refresh rate** (~170ms between frames)
- Frames repeat identically when display is stable
- Partial frames at capture boundaries are normal (ignore minority patterns)

## Packet Format (14 bytes)

```
Byte:  0  1  2  3  4  5  6  7  8  9  10 11 12 13
       XX 00 XX XX XX XX 20 XX XX XX XX FF FF XX
       └──────────── Controller ──────────────┘
                                       └─Panel─┘
```

**Bidirectional protocol on single data line (time-division multiplexed):**
- Bytes 0-10, 13: Controller → Panel (display state)
- Bytes 11-12: Panel → Controller (button input)
- Data line is likely open-drain with pull-up (idle = `FF FF` = all 1s)
- Panel gets 2-byte window per frame (~6Hz) for button codes

### Constant Bytes
| Byte | Value | Notes |
|------|-------|-------|
| 1 | 0x00 | Always zero |
| 11 | 0xFF | Always 0xFF (panel→controller, idle) |
| 12 | 0xFF | Always 0xFF (panel→controller, idle) |

### Confirmed Mappings (HIGH CONFIDENCE)

**Byte 2 - Power & Compressor:**
| Value | Meaning |
|-------|---------|
| 0x00 | AC off |
| 0x08 | AC on, compressor off |
| 0x0A | AC on, compressor on (heating) |

**Byte 4 - Set Temperature (16-30°C):**

Encoding: `byte4 = bit_reverse_4bit(temp - 16)`

| Temp | Offset | Binary | Reversed | Hex |
|------|--------|--------|----------|-----|
| 16°C | 0 | 0000 | 0000 | 0x00 |
| 20°C | 4 | 0100 | 0010 | 0x02 |
| 24°C | 8 | 1000 | 0001 | 0x01 |
| 25°C | 9 | 1001 | 1001 | 0x09 |
| 30°C | 14 | 1110 | 0111 | 0x07 |

**Byte 5 - Fan Speed:**
| Value | Fan |
|-------|-----|
| 0x80 | auto |
| 0x84 | medium |
| 0x88 | high |
| 0x8C | low |

**Byte 6 - Mode:**
| Value | Mode |
|-------|------|
| 0x20 | heat |
| 0x80 | cool |
| 0xC0 | fan-only |

**Panel button press (bytes 11-12):**
| State | Bytes 11-12 |
|-------|-------------|
| Idle | 0xFF 0xFF |
| Button pressed | 0x87 0x0F |

### Unconfirmed / Unknown

| Byte | Observations |
|------|--------------|
| 0 | Frame type? (0x60/0xE0 varies) |
| 3 | Unknown (0x00/0x80) |
| 7 | Room temperature integer (see encoding below) |
| 8 | Room temperature decimal / display state? (varies) |
| 9 | Display state? (0x99/0x59/0xD9) |
| 10 | Variable - display mux? |
| 13 | Variable - checksum? |

**Byte 7 - Room Temperature Encoding (CONFIRMED):**

Uses 2-bit reversed encoding with range flag in bits[1:0]:

| bits[1:0] | Range | Formula |
|-----------|-------|---------|
| 0x3 (11) | 20-23°C | `20 + bit_reverse_2bit(byte7 >> 2)` |
| 0x0 (00) | 24-27°C | `24 + bit_reverse_2bit(byte7 >> 2)` |

Verified mappings:
| Room Temp | Byte 7 | Binary |
|-----------|--------|--------|
| 20°C | 0x03 | 0000 0011 |
| 21°C | 0x0B | 0000 1011 |
| 22°C | 0x07 | 0000 0111 |
| 23°C | 0x0F | 0000 1111 |
| 24°C | 0x00 | 0000 0000 |
| 25°C | 0x08 | 0000 1000 |
| 26°C | 0x04 | 0000 0100 |
| 27°C | 0x0C | 0000 1100 |

Note: Byte 8 varies even for same room temp - may encode decimal part or display multiplex state.

**IR remote vs panel buttons:**
- Panel button press: `87 0F` code visible in SPI bytes 11-12
- IR remote press: raw IR signal on CH5 (IO5), not in SPI frames

## IR Protocol (CH5 / IO5)

The IR receiver on the panel outputs raw demodulated signal on IO5. Controller decodes directly.

### IR Signal Characteristics
- **Modulation**: Pulse-distance encoding
- **Leader**: ~3ms LOW + ~3ms HIGH
- **Bit encoding**:
  - Mark: ~1ms LOW (constant)
  - Space: ~950µs HIGH = bit 0, ~1900µs HIGH = bit 1
- **Frame**: 24 bits, repeated 3x with ~3ms gaps

### Known IR Commands

| IR Code | Observed Result |
|---------|-----------------|
| 63 00 01 | heat mode, temp commands |
| 62 00 00 | heat mode, temp up? |
| 62 00 01 | heat mode, temp down? |
| 22 00 00 | cool mode, temp up? |
| 22 00 01 | cool mode, temp down? |
| 43 00 01 | fan speed (heat) - encoding unclear |
| 40 C0 00 | fan speed (heat) - encoding unclear |

### Byte 1 Pattern (partial)
- High nibble `6` = heat mode
- High nibble `4` = heat mode (fan?)
- High nibble `2` = cool mode
- Low nibble `2` = temp down?
- Low nibble `3` = temp up?
- Low nibble `0` = fan?

### Open Issues
- **Fan encoding unclear**: Same IR code `43 00 01` observed for different fan state transitions (med→high AND high→auto). Remote displays current fan state, suggesting full state encoding, but codes don't match.
- **75kHz sample rate may be insufficient** for accurate IR decoding
- Need more systematic captures to decode full protocol

### IR Capture TODO
See `ir_capture_todo.md` for remaining captures needed.

### IR Injection (Sending Commands)

**Problem:** Directly connecting to the IR line loads it down and blocks the real IR receiver. The panel's IR receiver output is too weak to drive additional loads.

**Solution:** Use a diode to allow injection without interference:

```
IR Line ───┬─────────────── to AC Controller
           │
           ├─── IO5 (direct - monitoring)
           │
           └──►|──── IO4 (IRTX output)
           anode  cathode (stripe on diode)
```

**How it works:**
- When IO4 goes LOW (transmitting mark): diode forward-biased, pulls IR line LOW ✓
- When IO4 is HIGH (idle/space): diode reverse-biased, IO4 disconnected, no interference ✓
- Real IR receiver continues working normally ✓

**Components needed:**
- 1N4148 signal diode (or similar: 1N914, etc.)

**BP5 INFRARED mode for injection:**
```bash
# Enter INFRARED mode: m → 11 → y
# Send IR command (aIR format):
irtx $0:3000,3000,1000,950,1000,1900,...,1000,65535,;
#     └─freq  └─leader  └─bit marks/spaces      └─end
```

**aIR format:** `$<freq_khz>:<mark1>,<space1>,<mark2>,<space2>,...;`
- Use `$0` for no modulation (injecting to demodulated line)
- Use `$38` for 38kHz modulation (if using IR LED)
- Times in microseconds


## Recorded Samples

All recordings in `~/bla/recordings/` with naming: `room{X}_mode-{Y}_set{Z}_fan-{W}_comp-{V}_{timestamp}.sr`

### Captured Data

| Room | Mode | Set | Fan | Comp | Bytes (hex) |
|------|------|-----|-----|------|-------------|
| 20 | heat | 25 | high | off | 60 00 08 80 09 88 20 03 CE 59 55 FF FF 15 |
| 20 | heat | 25 | low | off | 60 00 08 80 09 8C 20 03 C9 59 55 FF FF 15 |
| 20 | heat | 25 | med | off | 60 00 08 80 09 84 20 03 C1 59 55 FF FF 15 |
| 20 | heat | 25 | med | on | E0 00 0A 00 09 84 20 03 C3 99 8A FF FF 8F |
| 20 | heat | 26 | high | off | 60 00 08 80 05 88 20 03 C1 59 55 FF FF 15 |
| 20 | heat | 26 | low | off | 60 00 08 00 05 8C 20 03 C5 99 55 FF FF 15 |
| 20 | heat | 26 | med | off | 60 00 08 80 05 84 20 03 C9 59 55 FF FF 15 |

## Capture Settings (IMPORTANT)

The following settings are required for clean captures:

```bash
sigrok-cli -d ols:conn=/dev/ttyACM1 \
    --config samplerate=75000 \
    --samples 37500 \
    --channels 0,1,2,5 \
    -o output.sr
```

| Setting | Value | Notes |
|---------|-------|-------|
| Sample rate | **75kHz** | Minimum for reliable capture (see Clock Timing above) |
| Samples | 37500 | ~0.5s capture, yields 8-10 frames |
| Max samples | 131072 | OLS buffer limit → max 1.75s at 75kHz |
| Channels | 0,1,2,5 | Data, Clock, Sync, Sync |

**Note**: On RPi, user `nitz` has Bus Pirate access (dialout group).

## Decoding Commands

```bash
# Capture new sample (from local machine)
./capture_recording.sh [output.sr]

# On RPi directly (as bob)
./record_ac.sh <room_temp> <mode> <set_temp> <fan> <compressor>
./record_ac.sh 25 heat 22 high on
./record_ac.sh 25 off

# Decode .sr file to hex bytes
sigrok-cli -i FILE.sr -P spi:clk=1:mosi=0:cpol=0:cpha=1 -A spi=mosi-data

# Group into 14-byte frames
sigrok-cli -i FILE.sr -P spi:clk=1:mosi=0:cpol=0:cpha=1 -A spi=mosi-data | \
    grep -oP '[0-9A-F]{2}' | paste -d' ' - - - - - - - - - - - - - -

# View raw waveforms
sigrok-cli -i FILE.sr -O ascii
```

## Remaining Work

1. **Room temperature encoding** - Bytes 7-8, encoding unclear (values vary even for same temp - display mux?)
2. **Byte 0, 3, 9, 10, 13** - Purpose unknown

**Note:** Backlight is likely controlled locally by the panel (timer-based after button/IR press), not transmitted in the protocol.

## Open Questions

1. What do bytes 7-8 encode for room temperature?
2. Is byte 13 a checksum?
3. What is byte 0 (0x60/0xE0)?
4. What is byte 3 (0x00/0x80)?

## Standalone Operation (Future)

For WiFi-enabled standalone control without laptop/Bus Pirate:

### Hardware Options
- **Arduino UNO R4 WiFi** - 5V logic (matches panel), has ESP32-S3 for WiFi
- **ESP32 DevKit** - 3.3V logic, needs level shifter

### Components for ESP32 (3.3V) build
| Component | Purpose |
|-----------|---------|
| 1N4148 diode | IR injection (same as BP5 setup) |
| Bidirectional level shifter | 3.3V ↔ 5V for SPI signals |
| 5V→3.3V regulator (e.g., AMS1117-3.3) | Power from panel (optional) |

### Components for Arduino R4 WiFi (5V) build
| Component | Purpose |
|-----------|---------|
| 1N4148 diode | IR injection |
| (no level shifter needed) | R4 is 5V logic |

### Power Considerations
- Panel's 5V rail capacity is unknown
- ESP32 WiFi can spike to 300-500mA
- Safer to use separate USB power initially
- Measure panel 5V capacity before powering from it

## Files

- `rpi/read_status.sh` - Read current AC state via SPI
- `rpi/capture_packet.sh` - Periodic SPI capture
- `rpi/ir_capture.sh` - IR signal capture with photos
- `packets/` - Captured recordings
