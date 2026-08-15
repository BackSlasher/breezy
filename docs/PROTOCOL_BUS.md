# The Controller <-> Panel Bus - Clean Reference

Current-truth spec of the wire protocol between the HVAC controller and the
LCD panel of a **USP 5010** kit (panel marked USP5010BE), as measured on the
permanent install (2026-08-14). No history - for how each fact was
established, see the working notes (`ir_commands.md`, and the bring-up
post-mortems where they are available). The IR command language (what the
remote and breezy transmit) is a separate document: `PROTOCOL_IR.md`.

## 1. Physical layer

```
HVAC controller ==== 6-wire cable ==== LCD panel
                     (breezy inline)
```

A clocked two-wire bus (CLK + DATA) plus 5V/GND and the IR line (see
`PROTOCOL_IR.md`). The panel PCB carries `isck`/`isda` silkscreen labels,
but it is NOT established that those pads connect to the cable's bus legs -
they may be a separate programming interface for the panel's chip.

**The DATA line is wired-AND (open-drain style).** The controller clocks
every frame; a party "writes" only by pulling bits low, and an unwritten
line reads as 1. Proven by unplugging the panel (2026-08-14): frame cadence
was unchanged, but every bit the panel used to write floated to 1. This is
why there is no such thing as a "reply" here - see section 2.

Breezy taps CLK/DATA read-only via interrupt, recording gap-delimited
bursts (>500µs idle delimits a burst).

## 2. Frame inventory - there are exactly TWO frames

| frame | bits | written by | carries |
|-------|------|-----------|---------|
| **LCD status poll** | 36 | 4-bit controller header `1001`, then 32 bits the PANEL pulls low | whatever the panel reports - believed to include keypresses (unverified) |
| **controller status** | 76 | controller only | the complete unit state + 2 checksums |

Both are clocked by the controller on a ~33ms cycle; a controller status
follows its LCD status poll after ~611µs.

The poll is a genuine poll, with an unusual twist: because the line is
wired-AND, the LCD's answer is written INTO the poll frame rather than
sent as a separate response. What the panel actually reports is NOT
established (section 2.1) - only the controller status has been decoded.

### 2.1 LCD status poll - PROVISIONAL, low priority

Not heavily researched, as I'm not reading / writing this.  
These are written by the panel, because when unplugged these are all "1"s.  
The panel's idle value (plugged in, not pressing anything) is `95 5F FF F1 5x`.

Unverified notes from a single session, don't blindly trust:

| key (claimed) | poll read (unverified) |
|-----|-----------|
| power | `95 50 F0 F5` |
| temp up | `95 52 F2 F4` |
| mode | `95 54 F4 F7` |
| fan | `95 58 F8 F3` |
| temp down | `95 5C FC F0` |
| (idle, panel absent) | `9F FF FF FF F0` |

Current hardware can't physically write these (DATA is read-only) and has nothing to do with keypresses.  
A possible redesign can support this if we come up with a reason.

## 3. Controller status - the complete state record

**Structurally complete** as of 2026-08-14: every bit's position and
encoding is known, so a frame can be generated from scratch and validated
by its own checksums. Two flag bits are located but their MEANING is not
identified (byte2 `0x04` and byte3 bit7 - see the table). Canonical length
is exactly **76 bits**; any other length is a capture defect (section 5).

`br4(x)` below = 4-bit reversal; this protocol transmits nibbles LSB-first.

| field | meaning |
|-------|---------|
| byte0-1 | header `60 00` |
| byte2 | **bitfield**: `0x08` power ON, `0x02` compressor running, `0x04` an unidentified flag that only appears alongside byte3 bit7 |
| byte3 | `00`, except bit7 - a transient that pulses for a median 1.9s (2.5% duty, 86% of it while ON, no compressor correlation): an indicator blink or command-ack, not a status bit. Suspected LCD backlight |
| byte4 | set temp: `br4(low nibble) + 16` |
| byte5 | fan: `80` auto, `84` med, `88` high, `8C` low |
| byte6 | mode: `20` heat, `80` cool, `C0` fan_only, `40` DRY, `00` AUTO. (DRY and AUTO not heavily tested) |
| byte7 low nibble + byte8 high nibble | **room temp**, one 8-bit field, LSB-first, biased by 40: `room = (br4(byte7 & 0x0F) \| (br4(byte8 >> 4) << 4)) - 40`. Matches an independent oracle exactly on 97.8% of denoised frames, within 1C on 99.99% |
| byte8 low nibble | **checksum 1** (section 3.1) |
| byte9 high nibble | **checksum 2**; byte9's low nibble is always 0 |

**The off-state mirror**: while the unit is OFF, bytes 4-6 keep reporting
the *stored* mode/set/fan (what the unit would wake into). Only byte2 tells
the truth about power. Never infer power from mode.

### 3.1 The two checksums

Read the 76 bits LSB-first as nineteen 4-bit groups; the last two are sums
of the preceding groups, mod 16. In byte terms:

```
br4(byte8 & 0x0F) == sum(br4(byte[i] & 0x0F) for i in 0..7) % 16
br4(byte9 >> 4)   == (sum(br4(byte[i] >> 4) for i in 0..7) - 2) % 16
```

The `-2` seed is constant across a 29h archive. Both hold on 99.87% of
denoised archive frames and 100% of clean live capture (91/91).

**Use them as the frame filter** - they are a true validity oracle, and
they replaced the structural guesses (byte3/mode/temp-range) that used to
reject legal frames and pass corrupt ones. One blind spot: the room field's
upper bits (byte8 high nibble) are covered by NEITHER checksum, so a bit
dropped there is invisible - keep a separate repeat vote on the room.

## 4. What the panel is

A dumb terminal. It answers the poll with its keypad state and renders the
controller status; it holds no state of its own, and the unit runs normally
with it unplugged.

## 5. Capture health - read before believing any capture

We used to have frames that are one bit short.
Turns out it's capture artifacts from keeping the ESP32 cpu busy with logging (40.5% corruption on VERBOSE logging, 0.1% on DEBUG).  
When in doubt, use the **throttled DEBUG sampler** (`sample:` log lines, <=2 bursts/s, verified 100% pure), never the VERBOSE dump.  
Alternatively, use a dedicated capture device (either another ESP32, or BusPirate).

**Bursts longer than 76 bits are two frames stuck together.** If you delimit
bursts on an idle gap (breezy uses >500µs), the ~611µs gap between a poll and
its controller status sometimes measures short and the pair arrives as one
111-114 bit burst - more often when the CPU is busy. Don't discard them: the
controller status starts at bit 37 (36-bit poll + 1) and passes both
checksums. Breezy un-glues them rather than dropping the frame.


## 6. Appendix: the splitter dialect

The AC ships an accessory "splitter" (**USP5020**) - a hub with one uplink
socket, one panel socket, and three sockets for dumb IR-eye/LED pods (an LCD
panel does not work on those). The permanent install does without it. It is not a protocol translator: downstream of it the
controller status keeps the same header and field offsets and merely gains a
padded tail (`... 55 FF FF 15`), a prefix-compatible superset that a
length-tolerant reader parses without knowing anything changed. Breezy
retains that grammar as `dialect: splitter`; the permanent install is
splitterless and uses `raw`. The pod protocol on the three remaining
sockets has never been tapped.
