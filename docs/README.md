# Docs

## Current

- [`PROTOCOL_BUS.md`](PROTOCOL_BUS.md) — the controller ↔ panel bus of a
  USP 5010. Two frame types, the complete field map of the state record,
  and the two checksums that authenticate a frame.
- [`PROTOCOL_IR.md`](PROTOCOL_IR.md) — the IR command language the remote
  (and this board) speaks: symbols, framing, the code tables, and the
  encoder state machines.
- [`photos.md`](photos.md) — pictures of the thing.

The cable pinout that matters for building one is in the
[main README](../README.md#wiring) and the
[board spec](../pcb/BREEZY_V4_SPEC.md).

The two protocol specs are written to be implementable on their own, with
no history needed. **Where anything else disagrees with them, they win.**

## [`historical/`](historical/) — superseded working notes

Raw engineering notes, kept because they show how the conclusions were
actually reached, and published close to unedited. Do not build on them:

- they reference scripts, capture files, hostnames and tooling from the
  development environment that are **not in this repository**;
- earlier notes were not rewritten when later work overturned them —
  supersession is marked in place, but several conclusions in them are
  simply wrong;
- much of the early work was done downstream of the USP5020 splitter,
  which serves a padded variant of the bus, so frame lengths and framing
  details there do not match the direct tap.

| file | what it is |
|------|-----------|
| [`historical/protocol_dump.md`](historical/protocol_dump.md) | the original bus reverse engineering, through the splitter. Its byte-2 field map was right on the first try and got re-derived the hard way months later |
| [`historical/IR_ENCODING.md`](historical/IR_ENCODING.md) | the first symbol-grammar write-up. Its power-frame section is superseded by `PROTOCOL_IR.md` §5 |
| [`historical/ir_commands.md`](historical/ir_commands.md) | the IR lab notebook: verified byte tables, raw captures, and a running record of what got retracted |
| [`historical/wires.md`](historical/wires.md) | wire colours mapped to signals, from the bench rig (extension cables, Bus Pirate hookup) |
