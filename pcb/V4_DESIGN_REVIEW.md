# v4 Spec Design Review (2026-07-05)

Review of `BREEZY_V4_SPEC.md` (pre-revision), the v3 as-built schematic (nets extracted
from the v3 EasyEDA project), v3 testing notes, and the firmware on both sides of
the IR path. Findings below are incorporated into the revised spec (v4.1). Confidence
ratings are honest: "certain" = internal contradiction or datasheet fact; "high" =
arithmetic with wide margins; "medium" = plausible but unverified on hardware.

## Findings

### F1. Divider diagram drawn wrong (critical — certain)
The tap drawings in the original v4 spec placed the shunt resistor on the passthrough
line and the series resistor between junction and GPIO. Built literally: a high-Z input
draws no current, so the GPIO sees the **full 5V** (ESP32 abs max is 3.6V — not 5V
tolerant), and the bus gets a hard 10K-to-GND load. The text ("gives ~2.5V") contradicted
the drawing. Fixed: series resistor first, shunt at the GPIO node.

### F2. 10K/10K ratio sits on the input threshold (critical — certain on the datasheet, high on practical failure)
The spec claimed "ESP32 reads HIGH > 2.0V". The datasheet says VIH = 0.75 × VDD3P3 =
**2.475V** at 3.3V. A 2.5V tap has 25mV margin — erased by resistor tolerance or the AC's
5V rail sagging (a rail already known to be weak). Fixed: 100K/180K → ~3.2V at the pin.

### F3. 20K bus loading is risky on weakly driven lines (high — high)
CLK/DATA driver strength is unknown; the IR line is almost certainly weak (TSOP-style
receiver outputs typically have ~30K internal pull-ups — a 20K tap would drag the idle
level to ~2V and break the real remote *through* the board). Fixed: 280K total tap
impedance, invisible even to a 30K source. Bonus: any ESP32 pin fault (boot glitch,
firmware bug, output-low) can inject at most ~50µA into the bus through the 100K series
resistor, and an unpowered ESP32 back-powers at ~µA level — both harmless.

### F4. 2N7000 is a gate-drive lottery at 3.3V (medium — certain on the spec)
2N7000 VGS(th) is specced up to 3.0V; driving it from a 3.3V GPIO is worst-case-part
roulette. Fixed: MMBT2222A with 1K base resistor (deterministic at 3.3V), 10K base
pulldown to hold it off during boot. BSS138/AO3400 acceptable alternates; 2N7000 rejected.

### F5. Onboard IR receiver collides with GPIO5 (high — certain from v3 netlist)
The v3 board has an onboard IR receiver (IR2, net `IR_RECEIVE`) wired to GPIO5. The v4
spec reassigned GPIO5 to the IR-line tap without mentioning the receiver — an
edit-in-place designer would short the receiver output into the divider. It is redundant
(the LCD panel is the IR eye; we tap its line). Fixed: explicit removal.
### F6. Dual-LDO conflict carried over from v2/v3 (low-medium — high)
v2/v3 fed both the DevKit's VIN (5V) and its 3V3 pin (from the board AMS1117) — two LDO
outputs tied together. With the TXS gone, nothing on the board needs 3.3V. Fixed: remove
the board AMS1117; feed VIN only; 3V3 header pin left unconnected.

### F7. RC filters don't belong in the shared path (low — high)
The noise being filtered corrupts the *reader*, not the LCD — which ran for years on the
raw signal. Filtering moved to the tap side (100pF at the GPIO node); passthrough is six
bare traces; v3's in-path 100R/100pF on CLK removed. Also fixes a math nit: 100R×100pF is
a ~16MHz corner, not the ~1.6MHz claimed in the v3 requirements.

### F8. v3 IR TX failure was probably firmware polarity, not hardware (medium-high; the mismatch itself is certain)
The proven-working Arduino sketch (`arduino/ac_ir.ino`) idles the line HIGH and sends
marks as LOW. The ESPHome `send_ir_raw` lambda in `esphome/breezy_v2.yaml` does the exact
inverse (idle LOW, mark HIGH) through a non-inverting shifter — consistent with "backlight
wakes but commands not accepted". v4's transistor stage inverts, so the existing lambda
convention (mark → GPIO HIGH → line LOW) becomes correct unchanged. Testable for free on
the v3 board (firmware-only reflash) any time before v4 arrives.

### F9. USB-while-mains hazard on VIN (low — certain)
The DevKit has no diode between USB 5V and VIN; plugging USB while the board is
mains-powered ties the two 5V sources. Existed in v2/v3 too. Fixed: SS14 Schottky in the
+5V_BOARD → VIN feed.

## Why v3 actually failed (mechanism, refined)

The v3 netlist shows single `CLK`/`DATA` nets: J1↔J2 were already directly connected, and
the TXS0104 was *attached* as a tap — the testing-notes theory ("signals go through the
shifter") was topologically wrong, but the conclusion was right, because the TXS0104 is a
pass-gate device: each B pin couples to its A pin through a pass transistor, with ~10K
internal pull-ups on both ports and one-shot edge accelerators that actively drive the
lines. Attaching it to a bus is never passive. It wired the ESP32's pin states onto the
5V bus (hence corruption only with the ESP32 socketed: boot-time pulls, pin modes, and a
second paralleled LDO on VCCA all arrive with the DevKit), and its internal pull-ups
quietly re-coupled the AC's signal lines to the board's 5V rail — bypassing the pin-3
isolation v3 got right. The v4 rule is therefore not "nothing between J1 and J2" but
"nothing attached to the lines that can drive them"; resistor dividers satisfy it.

## Residual risks after v4.1 (and why they're acceptable)

1. **Unknown bus driver topology.** Mitigated by 280K taps; worst case is a misread, not
   damage, and shows up immediately on first power-up with the LCD as the witness.
2. **IR protocol subtleties** (timing drift, frame format). The hardware bakes in no
   protocol assumptions — taps plus an open-collector pull-down — so every failure of
   this class is firmware-iterable using the board's own RX tap (capture the real remote
   through the actual panel path, replay, compare).
3. **No bench validation before fab** (no parts on hand, no soldering). Accepted: values
   were chosen so that being wrong is cheap, and the two free v3 firmware tests (polarity
   reflash; EN-held-low LCD observation) remain available while boards are in transit.

## Recommendations

- Convert to KiCad at this revision (major rev anyway — the central IC is removed and a
  dozen parts added). Text-format sources allow machine verification of the delivered
  design: netlist assertions against the designer checklist plus `kicad-cli` ERC/DRC,
  before JLCPCB sees gerbers.
- Verify the delivered files rather than buying a design review. The changed circuitry is
  six resistors, three caps and one transistor with wide margins; the historically
  expensive errors (v2 reversed pinout, v2 tied rails, v3 active tap) were all
  execution-level, which file verification catches. If extra human eyes are wanted, a
  one-page schematic gets real scrutiny for free on EEVblog or r/AskElectronics.
