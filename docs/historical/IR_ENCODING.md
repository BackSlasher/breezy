# IR Encoding Protocol Documentation

## Overview

This document describes the IR encoding protocol for the AC unit, reverse-engineered from captured IR signals.

## Bit Encoding

The protocol uses 4 symbols to encode bits:

| Symbol | Mark (µs) | Space (µs) | Bits Encoded |
|--------|-----------|------------|--------------|
| SS     | ~1000     | ~920       | 0            |
| LS     | ~1000     | ~1880      | 1            |
| LM     | ~1966     | ~920       | 1            |
| LL     | ~1966     | ~1875      | 11           |

## State Machine

The choice of symbol depends on the current state and the upcoming bits. There are 5 states:

- `fresh` - Initial state before any bits
- `after_0` - After zeros from `after_LL` or `after_LS`
- `after_LL` - After encoding LL
- `after_LM` - After encoding LM
- `after_LS` - After encoding LS

### State Transitions for Zero Bits

| Current State | On Zero | Next State |
|---------------|---------|------------|
| fresh         | SS      | after_LM   |
| after_0       | SS      | after_0    |
| after_LL      | SS      | after_0    |
| after_LM      | SS      | after_LM   |
| after_LS      | SS      | after_0    |

### State Transitions for One Bits

| Current State | Next Bit | Symbol | Next State |
|---------------|----------|--------|------------|
| fresh         | 1        | LL     | after_LL   |
| fresh         | 0        | LS     | after_LS   |
| after_0       | 1        | LL     | after_LL   |
| after_0       | 0        | LM     | after_LM   |
| after_LL      | (any)    | LM     | after_LM   |
| after_LM      | (any)    | LS     | after_LS   |
| after_LS      | 1        | LL     | after_LL   |
| after_LS      | 0        | LM     | after_LM   |

Key insight: `after_0` uses LM for a single 1, while `fresh` uses LS. This distinction is critical.

### Alternative Encoding (No-LL)

Some temperature settings (notably 24°C) use a simpler encoding without LL symbols:

- **0** → SS, reset to use LS for next 1
- **1** → alternates between LS and LM, starting with LS after a 0

This produces longer signals (more symbols for the same bits) but avoids the complexity of the LL lookahead.

## Frame Structure

### Power Toggle Command

- **Header**: 3000µs mark, 3806µs space
- **Separator**: 2953µs mark, 3810µs space
- **Trail**: 3980µs
- **3 identical frames**
- **30 bits per frame** (24 data bits + 6 extra bits)

Bytes:
- byte0: 0xf8 (fixed)
- byte1: temperature (bit-reversed encoding, see below)
- byte2: 0x00
- byte3: 0x03 (lower 6 bits = 000011)

### Heat Mode Command

- **Header**: 2994µs mark, 2846µs space
- **Separator**: 2953µs mark, 2856µs space
- **Trail**: 3976µs
- **3 IDENTICAL frames** (all frames are the same)
- **31 bits per frame** (24 data bits + 7 extra bits)

Bytes:
- byte0: 0x74 (fixed for heat mode)
- byte1: temperature encoding (see table below)
- byte2: 0x00
- byte3: 0x03 (lower 7 bits = 0000011)

## Temperature Encoding

### Heat Mode Temperature Bytes

These are the actual byte1 values decoded from captured heat mode commands:

| Temp | byte1 (hex) | Notes |
|------|-------------|-------|
| 16°C | 0x0c        |       |
| 17°C | 0x18        |       |
| 18°C | 0x14        |       |
| 19°C | 0x30        |       |
| 20°C | 0x3c        |       |
| 21°C | 0x28        |       |
| 22°C | 0x24        |       |
| 23°C | 0x60        |       |
| 24°C | 0x78        | *hybrid encoding (see below)* |
| 25°C | 0x78        | *same byte1 as 24°C, different encoding* |
| 26°C | 0x74        |       |
| 27°C | 0x50        |       |
| 28°C | 0x5c        |       |
| 29°C | 0x48        |       |
| 30°C | 0x44        |       |

### Special Case: 24°C

24°C uses a hybrid encoding scheme:
- **byte1**: 0x78 (same as 25°C!)
- **6 extra bits** (30 bits per frame instead of 31)
- **Fan speed byte0**: Uses standard fan bytes (auto=0x74, low=0x60, med=0x5c, high=0x78)
- **Key difference**: Uses LS-LM alternation (no LL) for byte1's `1111` sequence instead of `LS LL LM`

The distinction between 24°C and 25°C is subtle:
- 24°C: byte1=0x78 encoded with no-LL for the 1111 nibble, 30 bits total
- 25°C: byte1=0x78 encoded with standard LL encoding, 31 bits total

Symbol sequences for byte1's `01111000`:
- 24°C: `SS LS LM LS LM SS SS SS` (8 symbols for 8 bits)
- 25°C: `SS LS LL LM SS SS SS` (7 symbols for 8 bits via LL encoding)

### Power Toggle Temperature Encoding

Power toggle uses **bit-reversed** versions of the heat mode bytes:

| Temp | Heat byte | Power byte (reversed) |
|------|-----------|----------------------|
| 16°C | 0x0c      | 0x30                 |
| 17°C | 0x18      | 0x18                 |
| 18°C | 0x14      | 0x28                 |
| 19°C | 0x30      | 0x0c                 |
| 20°C | 0x3c      | 0x3c                 |
| 21°C | 0x28      | 0x14                 |
| 22°C | 0x24      | 0x24                 |
| 23°C | 0x60      | 0x06                 |
| 24°C | 0x78      | 0x1e                 |
| 25°C | 0x78      | 0x1e                 |
| 26°C | 0x74      | 0x2e                 |
| 27°C | 0x50      | 0x0a                 |
| 28°C | 0x5c      | 0x3a                 |
| 29°C | 0x48      | 0x12                 |
| 30°C | 0x44      | 0x22                 |

## Bit Order

All bytes are transmitted **MSB-first** (most significant bit first).

## Implementation

See `ir_encoder.py` for a complete Python implementation:

```python
from ir_encoder import generate_power_toggle, generate_heat, send_raw_timings

# Power toggle at 21°C
timings = generate_power_toggle(temp=21)
send_raw_timings(timings)

# Heat mode at 21°C with auto fan
timings = generate_heat(21, fan='auto')
send_raw_timings(timings)

# Heat mode at 24°C (only med/high fan available)
timings = generate_heat(24, fan='med')
send_raw_timings(timings)
```

## Files

- `ir_encoder.py` - Main encoder with `generate_power_toggle()`, `generate_heat()`, and low-level command functions
- `test_encoder.py` - Test harness with encoding comparisons
- `decode_timings.py` - Analysis tool for captured IR data
