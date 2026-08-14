# v5 Backlog (if a v5 ever happens)

Accumulated from v4 arrival/bring-up. None of these justify a respin alone.

- **Silkscreen pin labels on J1/J2** (IR/GND/5V/DATA/CLK/NC) — missed in the v4
  spec; only the pinout table existed, silk marking was never required.
- **L/N markings at the screw terminal (J5)** — also missing. Electrically
  irrelevant on this board (Hi-Link AC input is polarity-insensitive, varistor
  symmetric, no AC-side fuse), but electricians expect them. For the record on
  v4: N = outer terminal (nearest left board edge), L = inner (J5 pad 1 = L at
  15.8mm from edge, pad 2 = N at 8.2mm).
- **Silk polarity marks** for C6 (+/−) — the footprint carries only a corner
  notch; assembly is CPL-driven so it built fine, but hand-rework has no guide.
- **Specify mounting hole size explicitly** — v4 vendor chose 2.1mm (v3 was
  3.2/M3); nobody specced it, discovered at case-update time. M2 hardware now.
- Consider specifying board name/version on silk ("BREEZY v4.x") near the logo.

## v5 flagship candidate: DATA-line driver for bus-injected keypresses
One open-collector transistor on DATA (like Q1 on IR) + a spare GPIO, driven
CLK-synchronously, lets breezy forge panel button presses (codes mapped
2026-08-07, see ir_commands.md). Native control path: no IR, no decode
self-poisoning, instant verification. Also: DRY mode (reply byte6=0x40)
exists and is unreachable from our IR tables - capture the remote's dry
button or reach it via forged mode presses.
