# The IR Command Language - Clean Reference

Current-truth spec of the command protocol the remote (and breezy) sends to
the HVAC controller of a **USP 5010** kit, validated on the permanent
install (2026-08-14). Written top-down:
messages -> serialization -> wire. History lives in `ir_commands.md`; the
bus that reports state back is `PROTOCOL_BUS.md`.

## 1. Message model

There are exactly **two message types**, and both carry the same payload:

```
STATE_COMMAND (mode, fan, temp)   # "set the running unit to this"
POWER_FRAME   (mode, fan, temp)   # "toggle power; apply this on wake"
```

| field | domain |
|-------|--------|
| mode | heat, cool, fan_only |
| fan  | auto, low, med, high |
| temp | 16..30 (°C) |

Semantics:

- **STATE_COMMAND** applies mode/fan/temp to a running unit. An OFF unit
  ignores it completely.
- **POWER_FRAME** toggles power. There is no discrete on or off message -
  the remote holds no power state, and neither can you. When the toggle
  lands as power-ON, the unit wakes *directly into* the embedded
  (mode, fan, temp). When it lands as power-OFF, the payload is stored but
  the unit just turns off.
- Therefore: wake a known-off unit with ONE power frame carrying the target
  state; never blind-retry a power frame (a re-sent toggle un-does the
  first - verify against the bus first).

**Relationship between the two types**: think of POWER_FRAME as
STATE_COMMAND plus a 1-bit "and toggle power" flag - except the flag lives
in the header timing (section 3.3), not the payload. Putting the one
destructive bit in a gross timing difference (repeated at both separators)
makes it essentially impossible for noise to turn a temp change into a
power toggle. The two types also use different CODE tables for the same
(mode, fan) - see section 2.

There is no reply channel in IR. The unit beeps on accept (~always, within
~1s); actual state feedback comes from the bus (`PROTOCOL_BUS.md`).

## 2. Serialization: message -> bits

Both message types serialize to the same 30-31 bit frame body:

```
+----------------+----------------+----------+---------------+
| CODE(mode,fan) | TEMP(temp)     | 00000000 | TAIL          |
| 7-8 bits       | 8 bits         | 8 bits   | 6-7 bits      |
+----------------+----------------+----------+---------------+
```

The message TYPE is not in the bits - it's carried by the frame header
timing on the wire (section 3.3).

**TEMP** - lookup table, MSB first:

| °C | byte | | °C | byte | | °C | byte |
|----|------|-|----|------|-|----|------|
| 16 | `0C` | | 21 | `28` | | 26 | `74` |
| 17 | `18` | | 22 | `24` | | 27 | `50` |
| 18 | `14` | | 23 | `60` | | 28 | `5C` |
| 19 | `30` | | 24 | `78` | | 29 | `48` |
| 20 | `3C` | | 25 | `78` | | 30 | `44` |

**TAIL** = `0000011`, except `000011` (6 bits) when temp = 24. That tail
length is the ONLY thing distinguishing 24°C from 25°C - they share `78`.

**CODE** - for STATE_COMMAND the code is a byte (with ONE exception, below).
DRY and AUTO are documented for completeness but unsupported by breezy;
blank cells are fan speeds the remote refuses in those modes:

| fan  | heat | cool | fan_only | AUTO | DRY |
|------|------|------|----------|------|-----|
| auto | `74` | `24` | `E4` | `44` | -    |
| low  | `60` | `30` | `F0` | `50` | `C0` |
| med  | (7-bit) | `3C` | `FC` | `5C` | - |
| high | `78` | `28` | `E8` | -    | -    |

**The byte codes are COMPOSITIONAL** (confirmed 2026-08-14 by capturing
DRY/AUTO): `code = mode_base<<4 | fan_nibble | parity`. Fan nibbles:
auto=`4`, low=`0`, med=`C`, high=`8` - exact across all modes. Mode bases:
cool=`2`, AUTO=`4`, heat=`6`, DRY=`C`, fan_only=`E`. Bit `0x10` is set
when needed to make the byte's popcount EVEN. Every entry above derives
exactly (e.g. AUTO/low = `40`, odd popcount, `|0x10` -> `50`).

**The exception - heat/med is not a byte** (2026-08-14 capture, live
validated): its code is the 7-BIT `0111100` in a 30-bit frame, spelled
`SS LS LM LS LM SS SS` - heat/high truncated one bit, disambiguated by
frame LENGTH, the same trick as 24°C-vs-25°C. The historic `5C` "heat/med"
table entry was a mislabeled AUTO/med capture (`5C` = `4C|0x10` per the
composition; sending `5C` to a unit in heat was observed live to switch it
out of heat into a mode whose bus code is `00` - by elimination, AUTO).
With that relabel the vendor table contains NO anomalies.

For POWER_FRAME the code is 7-8 bits and - critically - is defined at the
SYMBOL level, not the bit level. See the wrinkle below.

## 3. Wire encoding: bits -> pulses

### 3.1 The alphabet, and the wrinkle

The wire alphabet is four pulse-distance symbols (baseband on the IR line,
idle high, mark = line low; timings in µs, remote-matched):

| symbol | mark | space | reads as bits |
|--------|------|-------|---------------|
| SS | 1024 | 894  | `0`  |
| LS | 1024 | 1854 | `1`  |
| LM | 1992 | 894  | `1`  |
| LL | 1992 | 1849 | `11` |

Three different symbols encode `1`s, so one bit string has many possible
"spellings" - and **the receiver distinguishes commands by spelling, not
bits**. Example: heat/med and cool/med power codes are both `1111100` as
bits, but heat spells it `LL LM LS LM SS SS` and cool spells it
`LM LS LL LM SS SS` - different commands.

Consequences, and they are asymmetric:

- **Encoding**: reproduce spellings exactly. Right bits in the wrong
  spelling are silently ignored - no beep, no reaction, indistinguishable
  from not transmitting at all.
- **Decoding**: collapsing symbols to bits is LOSSY AND UNSOUND. It happens
  to identify most commands uniquely, which is why a bit-level decoder can
  appear to work for a long time, but the heat/med and cool/med power codes
  genuinely collide - no bit-level decoder can tell those two apart. Decode
  to the symbol sequence and compare that.

### 3.2 Spelling rules

The TEMP/zeros/TAIL region is always spelled by the `Standard` state
machine below. The CODE region:

- STATE_COMMAND codes: spelled by a state machine over the whole frame -
  `Standard` for heat, `LsStart` for cool/fan_only.
- POWER_FRAME codes: verbatim symbol strings (the code IS the spelling):

| fan  | heat                   | fan_only               | cool                   |
|------|------------------------|------------------------|------------------------|
| auto | `LL LL SS LM SS SS`    | `SS LL SS SS LM SS SS` | `LM LS SS SS LM SS SS` |
| low  | `LL LM SS SS SS SS SS` | `SS LL LM SS SS SS SS` | `LM LS LM SS SS SS SS` |
| med  | `LL LM LS LM SS SS`    | `SS LL LL LM SS SS`    | `LM LS LL LM SS SS`    |
| high | `LL LL LM SS SS SS`    | `SS LL SS LM SS SS SS` | `LM LS SS LM SS SS SS` |

after which the rest of the frame continues on the `Standard` machine,
seeded with the state the code symbols left behind.

The `Standard` machine (states `Fresh`, `After0`, `AfterLL`, `AfterLM`,
`AfterLS`), consuming bits:

- `0` -> emit SS. Fresh->AfterLM; AfterLL/AfterLS->After0; else unchanged.
- `1` followed by `1`, from any state except AfterLM -> emit LL (consumes
  both bits).
- `1` otherwise: from Fresh -> LS; from After0/AfterLS/AfterLL -> LM.
- `1` from AfterLM -> LS, always (this is why tails end `... LS LM`).

`LsStart` is identical except LL is not allowed from `Fresh`.

Net effect: isolated `1`s alternate LS/LM through the frame; runs of `1`s
compress to LL pairs where the machine allows.

### 3.3 Framing: the blast

One transmission ("blast") is **three identical frames** - all three
required; 1- and 2-frame blasts are rejected - then a trailing mark:

```
[HDR][frame][SEP][frame][SEP][frame][TRAIL]
```

| element | mark (µs) | space (µs) |
|---------|-----------|------------|
| HDR / SEP, STATE_COMMAND | 2994 / 2953 | **~2813** |
| HDR / SEP, POWER_FRAME   | 2994 / 2953 | **~3773** |
| TRAIL | 3980 | (idle) |

The header/separator SPACE is the message-type discriminator - same body
bits with the ~3773µs spacing is a power frame.

## 4. Epistemics: what is fact and what is model

The layer cake in this document (messages -> bits -> symbols) is OUR
parser's abstraction; the wire truth is symbols, and the receiver's
internals are inferred. Notably, the "shared" temp/zeros/tail bits can
still be SPELLED differently between the two message types (the state
machine enters the temp region in whatever state the code region left it).
Evidence the receiver really decodes field-like structure rather than
matching whole memorized blasts: the encoding is compositional - we built
frames for (mode, fan, temp) combinations never present in any capture by
combining code spellings with temp bytes, and all tested combinations were
accepted first-frame. Evidence it does NOT work at the bit level: correct
bits in a wrong spelling are rejected. Best model: a per-family symbol
template matcher with a substitutable temp slot - consistent with remote
and receiver both being generated from the same authored waveform tables.

## 5. Implementer notes

- Acceptance is first-frame; beep = accepted. Verify by decoding the bus,
  wait ~2.5s, require a fresh reading before judging, back off on
  disagreement - and never blind-retry power frames (toggle parity).
- Your own blast echoes into your own receiver on the shared wire; expect
  ~0.5s of decode disruption and discard the echo.
- Transmit with hardware timing (RMT or equivalent) - bit-banged output
  glitches under multitasking and the receiver notices.
