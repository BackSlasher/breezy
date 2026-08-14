# Wire Color Mapping

## White Cable (LCD) → Red Cable (Extension)

Based on LCD panel labels (I..., GN..., +5..., DA..., CL...):

| White Cable | Red Cable | Signal |
|-------------|-----------|--------|
| Blue        | Red       | IR     |
| Turquoise   | Yellow    | GND    |
| Yellow      | Turquoise | +5V    |
| Orange      | Blue      | DATA   |
| Red         | White     | CLK    |
| (empty)     | Black     | NC     |

## Bus Pirate 5 Connections (SPI monitoring)

| Red Cable | BP5 Pin | Signal |
|-----------|---------|--------|
| Red       | IO5     | IR RX (monitoring) |
| Yellow    | GND     | GND    |
| Blue      | IO0     | DATA   |
| White     | IO1     | CLK    |

BP5 ports:
- `/dev/ttyACM0` - Interactive terminal (picocom)
- `/dev/ttyACM1` - OLS/SUMP mode (sigrok-cli)

## Arduino UNO R4 WiFi Connections (IR transmission)

| Arduino Pin | Connection | Target |
|-------------|------------|--------|
| ~3 (D3)     | → 1N4148 cathode (stripe) → anode → | IR line (diode path - NOT WORKING) |
| ~6 (D6)     | direct | IR line (Red on red cable) - WORKING |
| GND         | direct | Common GND |

Arduino port: `/dev/ttyACM2`

### Working Setup: Direct Connection (Pin 6)

```
IR Line ───┬─────────────── to AC Controller
           │
           ├─── BP5 IO5 (monitoring)
           │
           └─── Arduino D6 (direct - WORKING)
```

**Critical: Pin must be INPUT (high-Z) when idle so real remote still works.**
**When transmitting: set OUTPUT, HIGH, wait 100µs, then send timings.**

The diode path (D3) did not work - possibly due to voltage drop or timing issues.

### Arduino Sketch

Located at: `rpi:~/ac_ir/ac_ir.ino` and `arduino/ac_ir.ino`

Commands via serial (115200 baud):
- `t` - Test toggle (100ms pulse)
- `b` - Blink LED 3x (visibility test)
- `d` - Switch to DIRECT mode (pin 6) - USE THIS
- `p` - Switch to DIODE mode (pin 3)
- `r` - Repeat last command continuously
- `s` - Stop repeating
- CSV timings - Send IR (e.g., `3040,2813,1026,906,...,65535`)

### Verified Working IR Commands

See `ir_commands.md` for captured and tested IR command timings.
