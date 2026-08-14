# Breezy PCB v4 Specification (rev 4.1, 2026-07-05)

> Internal full spec (rev history, design math, firmware notes). The vendor-facing
> derivative is `pcb/breezy_v4_pitch/` — README (cover), CHANGES (lean work order),
> TECHNICAL_APPENDIX (rationale). Those three intentionally omit internal context;
> keep them in sync with this file on substantive changes.

## Summary

ESP32-based AC controller that sits between an air conditioner and its LCD panel.
Reads SPI signals (CLK, DATA) and sends/receives IR commands.

**Critical change from v3:** Remove the TXS0104 level shifter entirely. It is a
pass-gate device — attaching it to the lines actively drives them and corrupted the
passthrough. v4 reads the lines through high-impedance resistor dividers and injects
IR through a single open-collector transistor. Nothing else touches the lines.

> Rev 4.1 supersedes the earlier v4 draft: divider topology corrected (series resistor
> FIRST, shunt at the GPIO), values raised 10K/10K → 100K/180K (threshold margin + bus
> loading), RC filters moved out of the passthrough, MMBT2222A instead of 2N7000,
> onboard IR receiver and board 3.3V regulator removed. Rationale: `pcb/V4_DESIGN_REVIEW.md`.

## Design Principles

1. **J1 and J2 connect directly** — six bare traces, no components of any kind in the path
2. Nothing attached to the lines may be able to drive them: reads are via **high-impedance
   resistor dividers** (280K per line), IR injection via one open-collector transistor
3. IR TX and IR RX use **separate GPIOs**
4. Keep 5V isolation from v3

---

## JST Connector Pinout (unchanged from v3)

Both J1 and J2, viewing connector with latch on top, left to right:

| Pin | Signal | Notes |
|-----|--------|-------|
| 1 | IR | IR signal line |
| 2 | GND | Ground |
| 3 | 5V_PASS | Passthrough only, NOT connected to board 5V |
| 4 | DATA | SPI data |
| 5 | CLK | SPI clock |
| 6 | NC | Dead/unused, rightmost |

---

## Signal Path Architecture

### Passthrough (J1 ↔ J2)

**CRITICAL: Direct connection. No components in the path — also remove the v3 in-path
RC filter (100R + 100pF) from the CLK line.**

```
J1 Pin 1 (IR)   ────────────────── J2 Pin 1 (IR)
J1 Pin 2 (GND)  ────────────────── J2 Pin 2 (GND)
J1 Pin 3 (5V)   ────────────────── J2 Pin 3 (5V)    ← NOT connected to board 5V
J1 Pin 4 (DATA) ────────────────── J2 Pin 4 (DATA)
J1 Pin 5 (CLK)  ────────────────── J2 Pin 5 (CLK)
J1 Pin 6 (NC)      (unconnected)   J2 Pin 6 (NC)
```

Pin 6 (NC): unconnected on both connectors, attached to no board net. Originally
specced as passed-through ("board = plain cable"), but the delivered v4 (2026-07-21)
left it open and this was accepted as-built: the LCD's white cable has no conductor
in position 6 (`wires.md`), so open vs. passed-through are electrically
indistinguishable on this system, and open matches the v3-era rule.

### Signal Taps (CLK, DATA, IR_RX)

One divider per line. **Orientation matters: the 100K series resistor sits between the
line and the GPIO node; the 180K and 100pF go from the GPIO node to GND.** (Do NOT put
the shunt on the line side — that would put 5V on the GPIO and load the bus.)

```
CLK line ──[100K]──┬────────── ESP32 GPIO14 (CLK_3V3)
                   │
             [180K]│[100pF]
                   │
                  GND

DATA line ──[100K]──┬───────── ESP32 GPIO27 (DATA_3V3)
                    │
              [180K]│[100pF]
                    │
                   GND

IR line ──[100K]──┬─────────── ESP32 GPIO5 (IR_RX)
                  │
            [180K]│[100pF]
                  │
                 GND
```

Resistors 1%. Design math (for review, not for "optimization"):

- **Levels:** 5.0V line → 3.21V at pin; 4.5V → 2.89V. ESP32 VIH is 0.75 × 3.3V =
  **2.475V** (datasheet — NOT the 2.0V TTL figure), abs max 3.6V. Margin on both sides.
- **Loading:** 280K per line — invisible even to a weak driver (a 30K-pull-up IR
  receiver output sags < 0.5V; a push-pull MCU pin doesn't notice at all).
- **Bandwidth:** ~64K Thevenin × ~110pF ≈ 7µs time constant vs a 71µs half-period at
  the 7kHz SPI clock. Edges settle in ~20µs; sampling is mid-bit. The 100pF doubles as
  the glitch filter that used to sit in the passthrough.
- **Fault isolation:** any ESP32 pin state (boot glitch, firmware bug, pin driven low)
  injects at most ~50µA into the line through the 100K. An unpowered ESP32 back-powers
  at µA level. The board physically cannot disturb the AC↔LCD link.

Acceptable substitutions: shunt anywhere in 150K–220K (target 2.9–3.4V at the pin from
the measured line-high voltage).

### IR TX Driver

Open-collector NPN pulls the IR line low to inject marks. The line's own pull-up
(inside the AC/panel) provides the high level — the board never drives the line high.

```
IR line ─────┬────────────── (continues to J2)
             │
         [collector]
             │
GPIO4 ──[1K]─┤ MMBT2222A
             │[base]
       [10K] │
             │[emitter]
            GND
```

- 1K base resistor from GPIO4; **10K base-to-GND pulldown** (holds the transistor off
  during ESP32 boot/reset)
- GPIO4 HIGH → transistor ON → line LOW (mark). GPIO4 LOW → transistor off → line
  floats, passthrough undisturbed (leakage < 1µA)
- IR protocol is active-low (TSOP-style): line idles HIGH, marks are LOW. The
  transistor inverts, so firmware drives GPIO4 HIGH for marks.
- Part: MMBT2222A (SOT-23). Acceptable alternates: BSS138 or AO3400 with the 10K as a
  gate pulldown (100K also fine for a FET). **Do NOT substitute 2N7000/2N7002** — its
  gate threshold is specced up to 3.0V, marginal from a 3.3V GPIO.

---

## Power

- 220V AC input via screw terminals; fuse (F1) and varistor (RV1) unchanged
- Hi-Link IRM-10-5 AC-DC converter → +5V_BOARD
- +5V_BOARD → **SS14 Schottky diode** → ESP32 DevKit VIN (the diode prevents contention
  if USB is plugged in while the board is mains-powered)
- **Remove the AMS1117-3.3 and its caps** — with the TXS0104 gone, nothing on the board
  uses 3.3V; the DevKit regulates its own. The DevKit 3V3 header pin connects to
  NOTHING (do not tie it to any board rail).
- 5V isolation unchanged from v3: J1 pin 3 ↔ J2 pin 3 only; +5V_BOARD is a separate net;
  the two must not connect anywhere.

---

## Component Changes from v3

| Remove | Add |
|--------|-----|
| TXS0104 level shifter + its 2× 100nF bypass caps | 3× 100K 1% (series taps) |
| Onboard IR receiver (IR2) + its resistor | 3× 180K 1% (shunts) |
| AMS1117-3.3 + its caps | 3× 100pF (tap filters) |
| In-path CLK RC filter (100R + 100pF) | 1× MMBT2222A (SOT-23) |
| | 1× 1K + 1× 10K (base network) |
| | 1× SS14 (VIN feed) |

The onboard IR receiver is redundant: the LCD panel contains the system's IR receiver
and the board taps its output line (that is what GPIO5 reads). Leaving it connected to
GPIO5 would short its output into the divider.

---

## ESP32 GPIO Assignments

| GPIO | Signal | Direction | Notes |
|------|--------|-----------|-------|
| GPIO14 | CLK_3V3 | Input | no internal pull-up (see firmware notes) |
| GPIO27 | DATA_3V3 | Input | no internal pull-up |
| GPIO5 | IR_RX | Input | no internal pull-up |
| GPIO4 | IR_TX | Output | drives transistor base; HIGH = mark |

### Firmware notes (not for the PCB designer)

- Internal pull-ups/downs on GPIO14/27/5 must stay disabled — a ~45K internal pull-up
  against a 280K divider pins the reading. ESPHome's plain `INPUT` mode is correct.
- `remote_receiver`: pin GPIO5, `inverted: true` (line is active-low).
- IR send: mark → GPIO4 HIGH, idle → GPIO4 LOW. The v2 `send_ir_raw` lambda convention
  becomes correct through the inverting transistor **as-is** (its polarity was inverted
  for the v2/v3 non-inverting TXS path — likely why v3 commands weren't accepted).
- Boot-time pull-ups on GPIO14/GPIO5 are isolated from the lines by the 100K series
  resistors; no action needed.

---

## Mechanical

- Keep v3 dimensions (80 × 55mm) and mounting/connector positions if convenient —
  the printed case/tray (`case/*.scad`) fit them but can be reprinted.

## Unchanged from v3

- ESP32 DevKit socket footprint and orientation markers
- Screw terminals for 220V AC input, fuse F1, varistor RV1
- JST pinout (Pin 1 = IR, Pin 6 = NC)
- 5V_PASS isolation

---

## Deliverables Needed

- KiCad project (schematic + PCB source files)
- Gerber files for JLCPCB
- BOM and pick-and-place files for JLCPCB assembly (LCSC part numbers)

---

## Checklist for Designer

Before sending files:

- [ ] J1 and J2 pins 1–5 connect directly — no components in any passthrough trace
      (the old CLK RC filter is gone too); pin 6 unconnected
- [ ] 5V_PASS net connects J1-pin3 to J2-pin3 and nothing else
- [ ] Divider orientation: 100K from line to GPIO node; 180K and 100pF from GPIO node
      to GND (never the shunt on the line side)
- [ ] Transistor: collector to IR line, emitter to GND, 1K from GPIO4 to base, 10K base
      to GND
- [ ] TXS0104, onboard IR receiver, AMS1117 all removed; DevKit 3V3 pin unconnected;
      VIN fed via SS14 from +5V_BOARD
- [ ] GPIO map: 14 = CLK, 27 = DATA, 5 = IR_RX, 4 = IR_TX
- [ ] Print 1:1 and verify JST connector fits cable correctly
