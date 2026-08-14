# IR Protocol Documentation

> **Looking for the clean spec?** This file is the chronological lab
> notebook (discoveries, retractions, splitter-era views). The current-truth
> references are `PROTOCOL_IR.md` (the IR command language) and
> `PROTOCOL_BUS.md` (the controller<->panel bus).

## THE RAW BUS DIALECT (2026-08-04..06) - read this first

Everything below this section was reverse-engineered downstream of the AC's
"splitter" - an active protocol translator (NOT passive), which engages ONLY
when its input is fed through a MIRRORED cable (the AC's special cable is
mirror-wired; fed straight it passes the raw bus through untouched). The
permanent install removes it; the raw dialect below is what breezy decodes
there. The IR protocol (this file's main subject) is unchanged.

- The raw bus runs a **33ms poll/response cycle**: 113-bit master frames
  headed `95 5F` (controller->panel, the DISPLAY commands), short ~36-bit
  polls answered after a ~611µs turnaround by **~76-bit replies headed
  `60 00`** (panel->controller).
- **Replies carry the old dialect's fields at the old offsets**: byte4 set
  temp (bit-reversed nibble + 16), byte5 fan (`80/84/88/8C`), byte6 mode
  (`20/80/C0`).
- **Reply byte2 is a BITFIELD: bit `0x08` = POWER, bit `0x02` = compressor**
  (2026-08-07, full-bus OFF/ON diff, unanimous over ~550 replies: off = `00`,
  on-idle = `08`, on-cooling = `0A`). The old "`0x0A` = compressor" reading
  was both bits at once - the only combination ever correlated. Mode/set/fan
  mirror the live store even while off; byte2 does NOT.
- **Reply byte3 bit7 flaps only while ON** (unmapped flag - fan spinning?).
  Filtering on `byte3 == 0` silently discarded a third of on-state replies;
  the slip filter now masks it and the vote skips byte3.
- **Master byte[10] was NEVER power** (retracted 2026-08-07): the same diff
  showed it pinned at `0x44` in BOTH states (flickering `44<->88` because the
  controller alternates a 112-bit master form with one fewer mid-frame bit).
  The 2026-08-06 `0x01/0x02`-off readings were a context coincidence, still
  unexplained. Masters DO carry power redundantly at byte7 bit `0x40`
  (off `00`, on `40/44`) - unused, kept as a spare witness.
- **byte7 is NOT room temp** (retracted 2026-08-06: sat at 0x00 through LCD
  room 25->24 and setpoint churn; earlier drift tracked something else,
  suspect compressor activity). The real room field is unlocated - suspect
  byte8, which also refuses to derive as a checksum. A passive collector on
  a spare host is correlating raw bytes against a second tap's oracle
  reading. (Both halves of this were later overturned: byte7's low nibble IS
  half the room field, and byte8 does derive - see PROTOCOL_BUS.md.)
- **IR acceptance SOLVED (2026-08-07): there was never an acceptance
  problem.** The HVAC beeps AND applies FIRST frames within ~1s, always
  (dual-witness proof). All historical "misses" were SELF-JAMMING: our own
  transmission poisons our own decode for ~3-11s; impatient verification
  read stale state and retried, each retry re-poisoning the decode (13-send
  storms). The remote's ~15% re-press rate is optical reception loss. Fixes:
  freshness-gated verification + direct-status comparison + backoff. Beep =
  ack = acceptance. CLOSED 2026-08-07 night: the post-TX decode-poison was
  vote starvation from since-fixed decode bugs (byte3 filter, master-vote
  power); measured post-fix, replies resume 0.52s after a full blast.
- **POWER frames are toggle-with-embedded-state** (2026-08-07, remote
  captures + 4/4 live validation): a distinct header/separator space
  (~3773µs vs ~2813) marks a power frame; its payload is
  `[mode/fan code][TEMP_BYTES[temp]][8 zeros][tail 0000011]` and a press
  TOGGLES power, waking the unit DIRECTLY into the embedded state (single
  blast, one beep - no on-then-command two-step). The remote holds no on/off
  state; "restores the pre-off snapshot" was a misread of misfiled mode
  codes (the old `POWER_TOGGLE_BYTES 0xF4` was really the heat/auto code -
  hence every "restore" landed heat). Toggle retries are PARITY-DESTRUCTIVE:
  never re-send a power frame against a power reading older than the send.
- **THE SPELLING IS THE CODE** (2026-08-07 full capture session, all 12
  mode/fan combos): heat/med and cool/med share the bit string `1111100`
  and differ ONLY in symbol spelling (`LL LM LS LM` vs `LM LS LL LM`) - the
  receiver reads symbols, not bits. Code-region spellings by family (temp/
  zeros/tail then follow the Standard state machine seeded by the code):
  | fan  | heat (`LL...`)       | fan_only (`SS LL...`)   | cool (`LM LS...`)       |
  |------|----------------------|-------------------------|-------------------------|
  | auto | LL LL SS LM SS SS    | SS LL SS SS LM SS SS    | LM LS SS SS LM SS SS    |
  | low  | LL LM SS SS SS SS SS | SS LL LM SS SS SS SS    | LM LS LM SS SS SS SS    |
  | med  | LL LM LS LM SS SS    | SS LL LL LM SS SS       | LM LS LL LM SS SS       |
  | high | LL LL LM SS SS SS    | SS LL SS LM SS SS SS    | LM LS SS LM SS SS SS    |

  All 12 verified: 9 capture-exact, 3 grammar-derived and live-fire-proven
  (2026-08-07 self-verified run, single frame each, no retries). Cool
  bit-codes = fan_only bit-codes minus the leading 0.

Decoder: `esphome/components/breezy_spi`, `dialect: raw|splitter` (one burst
recorder ISR, two grammars). Captures: 13+ labeled logs, 2026-08-04..06.

## Protocol Overview (splitter-era dialect below)

- **Encoding**: Pulse-distance modulation
- **Carrier**: None (direct digital, not modulated 38kHz)
- **Idle state**: HIGH
- **Mark (active)**: LOW
- **Header**: ~3000µs mark, ~2800µs space
- **0-bit**: ~1000µs mark, ~900µs space
- **1-bit**: ~1000µs mark, ~1900µs space (OR ~2000µs mark, ~900µs space)
- **Frame structure**: 3 identical frames per command, ~3000µs gap between frames.
  **All three are REQUIRED** - verified 2026-08-02 by replaying a captured
  command truncated to 1 and 2 frames (both rejected) against a 3-frame control
  (accepted). The repeat is not optional redundancy.
- **Trailing**: ~4000µs final gap before idle

## Timing Format

Timings are CSV of microseconds: `mark,space,mark,space,...,65535`
- Alternating mark/space starting with mark
- 65535 signals end of transmission

## Power Toggle Command

The power toggle command uses a different format (bytes: 87 07 00 7C, 175
timings). SUPERSEDED 2026-08-07: this capture was a cool/24 power-state frame
read through the splitter-era byte lens - see "POWER frames are
toggle-with-embedded-state" in the raw-dialect section at the top; the
firmware now synthesizes power frames for any mapped mode/fan/temp.

```
2945,3879,1919,1932,1986,1866,1986,906,1013,906,1013,919,1013,906,1026,906,1013,1866,1999,893,959,1932,1999,893,1013,906,946,959,1013,906,1013,906,1013,919,1013,906,1013,906,1026,906,1013,906,1026,906,1013,906,1013,906,1026,906,1013,906,1013,919,1013,906,2919,3879,1933,1919,1999,1866,1986,906,1013,906,1013,919,1013,906,1026,906,1013,1866,1999,893,946,1945,1986,893,1013,906,1013,906,1013,906,1013,906,1013,919,1013,906,1013,906,1013,919,1013,906,1013,906,1013,906,1013,919,1013,906,1013,906,1026,906,1013,906,2945,3839,1933,1919,1999,1866,1986,906,1013,906,1013,919,1013,906,1013,919,1013,1853,1999,906,946,1946,1986,893,1026,906,1013,906,1013,906,1013,919,1013,906,1026,893,1026,906,1013,906,1026,893,1026,906,1013,906,1013,919,1013,906,1013,906,1026,893,1013,906,4012,65535
```

This toggles power on/off. When AC is on, it turns off. When off, it turns on.

## Known IR States (All Verified)

| Bytes | Mode | Temp | Fan | Source | Gen |
|-------|------|------|-----|--------|-----|
| 0C 06 00 60 | cool | 23°C | low | state.txt | ✓ |
| 14 06 00 60 | cool | 23°C | low | state.txt | |
| 24 06 00 60 | cool | 23°C | high | state.txt | |
| 1E 05 00 B0 | cool | 23°C | high | state.txt | |
| 1C 05 00 30 | cool | 25°C | med | state.txt | |
| 0C 16 00 30 | cool | 26°C | low | SPI status | |
| 0C 0A 00 60 | cool | 27°C | low | tested ✓ | ✓ |
| 1C 07 00 98 | cool | 27°C | med | state.txt | |
| 0C 1A 00 30 | cool | 28°C | low | tested ✓ | |
| 1C 0D 00 98 | cool | 28°C | med | tested ✓ | |
| 1C 09 00 30 | cool | 30°C | med | state.txt | |
| 06 1E 00 B0 | heat | 24°C | auto | state.txt | |
| 0E 0F 00 D8 | heat | 24°C | med | state.txt | |
| 1E 0F 00 D8 | heat | 24°C | med | tested ✓ | ✓ |
| 16 0F 00 D8 | heat | 24°C | high | state.txt | ✓ |
| 1E 03 00 B0 | heat | 25°C | med | state.txt | |
| 1E 07 00 D8 | heat | 27°C | med | state.txt | |
| 1E 0D 00 D8 | heat | 28°C | med | tested ✓ | ✓ |
| 0E 18 00 30 | heat | 16°C | high | 2026-01-01 ✓ | |
| 0E 0B 00 18 | heat | 26°C | high | 2026-01-01 ✓ | |
| 0E 11 00 30 | heat | 30°C | high | 2026-01-01 ✓ | |

**Gen column**: ✓ = can be sent via bytes using v9 algorithm (POST /remote)

## All IR Packets (Decoded but not all tested)

| Packet | Bytes (hex) | First 32 bits |
|--------|-------------|---------------|
| 1766863462 | 1E 0D 00 D8 | 01111000101100000000000000011011 |
| 1766863556 | 1E 0F 00 D8 | 01111000111100000000000000011011 |
| 1766863621 | 1C 05 00 30 | 00111000101000000000000000001100 |
| 1766863642 | 1C 09 00 30 | 00111000100100000000000000001100 |
| 1766863649 | 1C 0D 00 98 | 00111000101100000000000000011001 |
| 1766863669 | 1C 07 00 98 | 00111000111000000000000000011001 |
| 1766863695 | 24 06 00 60 | 00100100011000000000000000000110 |
| 1766863703 | 0C 06 00 60 | 00110000011000000000000000000110 |
| 1766863732 | 14 06 00 60 | 00101000011000000000000000000110 |
| 1766863744 | 1E 05 00 B0 | 01111000101000000000000000001101 |
| 1766863757 | 1E 07 00 D8 | 01111000111000000000000000011011 |
| 1766863799 | 1E 03 00 B0 | 01111000110000000000000000001101 |
| 1766863807 | 1E 0F 00 D8 | 01111000111100000000000000011011 |
| 1766865563 | 16 0F 00 D8 | 01101000111100000000000000011011 |
| 1766865570 | 06 1E 00 B0 | 01100000011110000000000000001101 |
| 1766865592 | 06 1E 00 B0 | 01100000011110000000000000001101 |
| 1766865599 | 1E 0F 00 D8 | 01111000111100000000000000011011 |
| 1766865605 | 0E 0F 00 D8 | 01110000111100000000000000011011 |
| 1766865612 | 16 0F 00 D8 | 01101000111100000000000000011011 |
| 1767106461 | 87 07 00 7C | 11100001111000000000000000111110 |
| 1767110231 | 0C 0A 00 60 | 00110000010100000000000000000110 |
| 1767110740 | 0C 16 00 30 | 00110000011010000000000000001100 |
| 1767110841 | 0C 0A 00 60 | 00110000010100000000000000000110 |
| 1767111194 | 0C 1A 00 30 | 00110000010110000000000000001100 |

## Protocol Analysis

### Byte 3 - Mode Indicator
Byte 3 primarily determines the operating mode:

| Byte 3 | Mode |
|--------|------|
| 0x30   | cool (only) |
| 0x60   | cool (only) |
| 0x98   | cool (only) |
| 0xB0   | cool OR heat (ambiguous) |
| 0xD8   | heat (only) |

When byte 3 = 0xB0, mode is determined by byte 0.

### Byte 0 - Fan Speed
Fan encoding differs by mode:

**Cool mode:**
| Byte 0 | Fan |
|--------|-----|
| 0x0C, 0x14 | low |
| 0x1C | med |
| 0x1E, 0x24 | high |

**Heat mode:**
| Byte 0 | Fan |
|--------|-----|
| 0x06 | auto |
| 0x0E, 0x1E | med |
| 0x16 | high |

### Symbol-level encoding (captured 2026-08-02 via the v4 board's RX tap)

Cool mode, fan auto (byte0 = `SS SS LS SS SS LM SS SS`), captured from the real
remote and correlated against the SPI decode for ground truth:

| Temp | byte1 symbols | symbols/frame |
|------|---------------|---------------|
| 21°C | `SS LS LM SS SS SS SS SS`... (byte1 0x28) | 31 |
| 22°C | byte1 0x24 | 31 |
| 23°C | `SS LS LM SS SS SS SS SS` (0x60) | 31 |
| **24°C** | **`SS LS LM LS LM SS SS SS`** | **30** |
| **25°C** | **`SS LS LL LM SS SS SS SS`** | **30** |

**This is why 24°C and 25°C collide in the byte tables.** They are NOT
distinguished by their byte value - they are distinguished by which *symbols*
encode the run of 1s. 25°C compresses the run using the `LL` symbol; 24°C spells
it out as `LS LM LS LM` and never uses `LL`. Anything that reasons only about
byte values will conflate them. 24°C also uses a 30-symbol frame (6 trailing
bits rather than 7), which is what `generate_24c_frame_()` in `breezy_climate`
exists to produce.

Verified working end-to-end: commanding 24°C through the climate entity lands on
24°C, and 25°C still lands on 25°C.

### Fan-only mode has a DIFFERENT frame alignment (captured 2026-08-03)

Captured from the real remote in **fan_only mode, fan medium**, correlated
against the display. byte0 was constant throughout:
`LS LL LL LM SS SS SS LS`.

| Temp | byte1 symbols | symbols/frame |
|------|---------------|---------------|
| 23°C | `LM SS SS SS SS SS SS SS` | 29 |
| **24°C** | **`LM LS LM SS SS SS SS SS`** | **28** |
| 25°C | `LL LM SS SS SS SS SS SS` | 28 |
| 26°C | `LL SS LM SS SS SS SS SS` | 28 |

**Fan-only frames are 28-29 symbols; cool is 30-31 and heat 29-30.** Fan-only's
byte0 carries two `LL` symbols, which shifts the bit alignment relative to the
other modes, so the byte1 slice above is not directly comparable to the cool
table below.

**Resolved 2026-08-04.** The 24°C frame structure is the same in every mode:
mode-normal byte0, then byte1 0x78 spelled no-LL (`SS LS LM LS LM SS SS SS`),
byte2 zeros, 6-bit tail (`SS SS SS SS LS LM`). The fan-only no-op was caused by
`generate_24c_frame_()` hardcoding **Standard** encoding for byte0: for cool's
0x24 Standard coincides with LsStart, but it spells fan-only's 0xFC as
`LL LL LL` where the remote sends `LS LL LL LM` - and the HVAC pattern-matches
the waveform, so the wrong byte0 *spelling* invalidates the whole frame. Fix:
byte0 now uses the mode's normal encoding. Validated by (a) offline
symbol-exact reproduction of all captured frames, (b) synthesizing the frame
over `send_ir_raw` and watching the SPI decode accept it, (c) 6/6 on the
fan_only/cool x 22/24/25 matrix through the climate entity.

Two failed hypotheses worth recording so nobody retries them:
- **Bits-only model**: treating LL as two 1-bits, 24°C differs from 25°C only
  by total length (30 vs 31 bits). True as arithmetic, but sending those 30
  bits with LL spelling is rejected outright - the receiver reads symbols
  (waveform), not decoded bits.
- **Cool-shaped hybrid frame in fan-only**: right byte1 spelling, wrong byte0
  spelling - also rejected. Both features must be right; a frame is accepted
  whole or not at all.

### Byte 1 - Temperature
Temperature encoding is complex - same value can represent different temps:

**Cool mode:**
| Byte 1 | Temp |
|--------|------|
| 0x05, 0x06 | 23°C |
| 0x05 | 25°C |
| 0x16 | 26°C |
| 0x07, 0x0A | 27°C |
| 0x0D, 0x1A | 28°C |
| 0x09 | 30°C |

**Heat mode:**
| Byte 1 | Temp |
|--------|------|
| 0x0F, 0x1E | 24°C |
| 0x03 | 25°C |
| 0x07 | 27°C |
| 0x0D | 28°C |

### Byte 2
Always 0x00 in all captured packets.

## Raw Timings for Verified States

### Cool 27°C Low Fan (1767110841)
```
3040,2813,1026,906,1026,893,1026,1866,2000,893,1026,893,1026,906,1026,893,1026,906,1013,906,1026,1866,1026,893,2000,893,1026,893,1026,906,1026,893,1026,906,1026,893,1026,906,1026,893,1026,906,1026,893,1026,906,1013,906,1026,893,1026,906,1026,893,1026,906,1013,906,1026,893,1026,1866,2000,906,2986,2826,1026,906,1013,906,1026,1866,1986,893,1026,906,1013,906,1013,906,1026,906,946,973,1026,1853,960,973,1920,960,1026,906,946,973,946,986,946,973,1013,920,946,973,946,986,946,973,946,973,946,986,946,973,946,986,946,973,946,986,946,973,946,973,960,973,946,1933,1920,986,2920,2906,946,973,946,986,946,1933,1920,973,946,986,946,973,946,986,946,973,946,973,946,1946,946,973,1920,973,946,986,946,973,946,973,946,986,946,973,946,986,946,973,946,973,960,973,946,973,946,986,946,973,946,986,946,973,946,973,946,986,946,973,946,1946,1920,973,3946,65535
```

### Cool 28°C Low Fan (1767111194)
```
3040,2813,1040,893,1040,893,1026,1853,2000,893,1026,906,1080,840,1040,880,1080,853,1066,853,1040,1853,1040,880,2013,1840,2013,880,1040,893,1026,893,1040,893,1026,893,1040,893,1026,893,1040,893,1026,893,1040,893,1026,893,1040,893,1026,893,1040,893,1026,893,1040,880,1040,1853,2000,906,3026,2800,1026,893,1040,893,1026,1853,2013,880,1040,893,1026,893,1040,893,1026,893,1026,906,1026,1853,1040,893,2000,1840,2013,880,1040,893,1000,920,1040,893,1000,920,1026,893,1040,892,1026,892,1040,893,1026,893,1013,920,1026,893,1040,880,1040,893,1026,893,1013,920,1026,1866,1973,920,2973,2853,1013,920,1026,893,1013,1880,1973,906,1013,920,1000,920,1026,906,1000,920,1040,893,1026,1853,1013,920,1973,1866,2013,880,1013,920,1026,893,1013,920,1000,920,1013,906,1040,893,1000,920,1013,920,1026,893,1013,920,1000,920,1013,920,1000,920,1000,920,1013,920,1000,1880,1986,920,4000,65535
```

### Heat 28°C Med Fan (1766863462)
```
3026,2840,1026,893,1026,1866,1986,906,1013,1866,2000,893,1026,893,1026,906,1026,893,1026,1866,1026,893,2000,1853,2000,893,1026,893,1026,906,1026,893,1026,906,1013,906,1026,906,1013,906,1026,906,1013,906,1026,906,1013,906,1040,880,1026,906,1026,893,1026,906,1013,1866,2000,906,2986,2840,1013,906,1026,1866,1986,893,1026,1866,2000,893,1013,906,1026,906,1013,906,1026,1866,1013,906,2000,1853,2000,893,1013,906,1026,893,1026,906,1026,893,1040,893,1026,893,1026,906,1013,906,1026,906,1013,906,1026,893,1026,906,1026,893,1026,906,1013,906,1026,1853,2000,906,2986,2840,1013,906,1026,1853,2000,893,1026,1866,1986,893,1026,906,1013,906,1026,893,1026,1866,1013,906,1986,1866,1986,893,1026,906,1013,906,1026,893,1026,906,1013,906,1026,906,1013,906,1026,893,1026,906,1013,906,1013,920,1013,906,946,973,1026,906,946,973,946,1933,1920,986,4013,65535
```

### Cool 25°C Med Fan (1766863621)
```
2973,2880,973,986,946,946,973,1906,1946,1906,1946,933,986,946,973,946,986,946,973,1906,986,946,1946,933,973,946,986,946,973,946,973,960,973,946,973,946,973,946,986,946,973,946,986,946,973,946,973,946,986,946,973,946,973,960,973,946,973,986,933,1906,1960,946,2946,2880,973,946,973,986,933,1906,1960,1893,1946,946,973,946,986,946,973,946,973,1906,986,946,1946,933,986,946,973,946,973,960,973,986,933,946,973,946,986,946,973,946,973,946,986,933,986,946,973,946,973,946,973,946,986,946,973,946,973,946,986,1906,1960,933,2946,2880,973,946,973,946,986,1906,1946,1893,1960,933,973,946,986,946,973,946,973,1906,986,946,1946,933,973,946,986,946,973,946,973,946,986,946,973,946,973,946,986,946,973,946,973,946,973,959,973,946,973,946,973,946,986,946,973,946,973,960,973,1906,1946,946,3986,65535
```

### Heat 24°C High Fan (1766865563)
```
2973,2880,986,946,973,1906,1946,1906,973,1000,1906,933,986,946,973,946,973,946,986,1906,1946,933,986,1906,1960,920,986,986,933,946,973,946,973,946,973,960,973,946,973,986,946,946,973,946,986,946,973,946,986,946,973,946,973,946,986,946,973,1906,1960,946,2946,2880,973,946,986,1906,1946,1893,986,946,1946,946,960,960,973,946,986,946,973,1906,1946,946,973,1906,1960,933,973,960,973,946,973,946,986,946,973,946,986,946,973,946,973,946,986,946,973,946,973,960,973,946,973,946,986,946,973,946,973,1920,1946,946,2946,2866,986,946,973,1906,1946,1906,973,946,1946,946,973,946,986,946,973,946,973,1906,1946,946,973,1906,1960,933,973,946,986,946,973,946,973,946,986,946,973,946,986,946,973,946,973,946,986,946,973,946,973,946,986,946,973,946,973,946,986,1906,1946,946,3986,65535
```

## Byte-Based Generation (v9 Algorithm)

Commands can be generated from 4 bytes using the v9 algorithm. This is implemented in:
- `test_gen.py` (Python, for testing)
- `breezy.ino` (Arduino, `generateIRTimings()`)

### Sending via Breezy HTTP API

```bash
# Send by bytes (uses v9 generation)
curl -X POST "http://breezy/remote?b0=1E&b1=0D&b2=00&b3=D8"

# Send raw timings (bypasses generation)
curl -X POST http://breezy/remote/raw -d "3026,2840,1026,893,..."
```

### Protocol Details

**Byte 3 determines frame structure:**
| Byte 3 | Mode | Bits/Frame | LL Encoding |
|--------|------|------------|-------------|
| 0xD8   | heat | 29         | isolated-1-0-1 pattern |
| 0x60   | cool | 31         | middle of 3+ consecutive 1s |
| 0x30   | cool | 30         | middle of 3+ consecutive 1s |
| 0x98   | cool | 30         | middle of 3+ consecutive 1s |

**Encoding types:**
- SS: 1026µs mark, 893µs space (0-bit)
- LS: 1026µs mark, 1866µs space (1-bit, first in alternation)
- LM: 2000µs mark, 893µs space (1-bit, second in alternation)
- LL: 2000µs mark, 1853µs space (1-bit, special pattern)

**Working commands (byte3=0xD8 heat, byte3=0x60 cool):**
- Heat mode: All 0xD8 commands work
- Cool mode: Only 0x60 commands verified working
- 0x30/0x98 commands: Encoding matches captures but don't work (unknown reason)

## Sending Commands

### Python Example (on RPi)

```python
import serial
import time

# Example: Cool 27°C Low Fan
TIMINGS_27C_COOL_LOW = "3040,2813,1026,906,..."  # Full timing string

ser = serial.Serial("/dev/ttyACM2", 115200, timeout=2)
time.sleep(2)  # Wait for Arduino reset
ser.readline()  # Discard ready message

# Switch to direct mode (REQUIRED)
ser.write(b"d\n")
time.sleep(0.2)
ser.readline()

# Send command
ser.write((TIMINGS_27C_COOL_LOW + "\n").encode())
time.sleep(1.5)
print(ser.readline().decode().strip())  # "Sent N timings"

ser.close()
```

## Capturing New Commands

Use `ir_capture.sh` on RPi:

```bash
~/packet_capture/ir_capture.sh
```

This will:
1. Read AC status via SPI (before)
2. Capture IR + SPI for ~1.75s
3. Read AC status via SPI (after)
4. Fail if no valid SPI frames or no IR activity

Extract timings from capture:

```bash
sigrok-cli -i recording.sr -C 5 -O csv | python3 decode_ir.py
```

## Panel button codes on the raw bus (mapped 2026-08-07, unit off)

Buttons appear in TWO layers simultaneously:

1. The 36-bit polls swap their idle signature `95 5F FF F1 5x` for a
   per-button code nibble (echoed in the next byte, check-ish byte after):
   power=`95 50 F0 F5`, temp_up=`95 52 F2 F4`, mode=`95 54 F4 F7`,
   fan=`95 58 F8 F3`, temp_down=`95 5C FC F0`.
2. The 60 00 replies flag the press class in byte2 (+= 0x01 temp, 0x04 mode,
   0x0C power) with byte3 bit7 set - the forgery target for bus-injected
   keypresses (v5: needs a DATA-line driver transistor; v4 taps are
   read-only by design).

BONUS: mode cycling exposed reply byte6 = 0x40 - per the user this is the
WATERDROP icon = DRY/dehumidify. **CONFIRMED 2026-08-14, with a caveat**:
most archived `0x40` frames are shifted HEAT frames (`0x20<<1 = 0x40`, a
VERBOSE-logging capture defect), but 62 checksum-authenticated frames from
this very session are genuine, and 61 of them carry fan=low - which matches
the remote refusing any other fan speed in DRY. So `0x40` = DRY holds, and
AUTO = `0x00` (live-probed). See PROTOCOL_BUS.md. The panel also has a triangle-of-arrows
icon = AUTO mode (byte6 code unobserved). Both exist on the unit and are
DELIBERATELY UNSUPPORTED (user: "I don't find them interesting"); the
decoder reports mode "unknown" if the panel is ever put there. Our IR
tables cover heat/cool/fan_only only.
