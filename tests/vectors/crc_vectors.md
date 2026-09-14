# Shared CRC / wire-frame test vectors

For Person A and Person C to check their own implementations against.
Both sides must produce byte-identical output for these — if either
doesn't, the two implementations will silently disagree on every packet
on the real link, per `A's_guide.txt` §8's warning about exactly this.

## CRC32C (Castagnoli) — settled, no ambiguity

Input: ASCII `"123456789"` (9 bytes, no null terminator)
Output: `0xE3069283`

This is the standard industry check value for CRC32C (poly 0x1EDC6F41 /
0x82F63B78 reflected, init 0xFFFFFFFF, input+output reflected, final XOR
0xFFFFFFFF). `A's_guide.txt` §8 already uses this exact convention with
working hardware code, so this one is not in question.

## CRC16 — ✅ CONFIRMED against Person A's real code

Input: ASCII `"123456789"` (9 bytes, no null terminator)
Output: `0x29B1`

Variant used: **CRC-16/CCITT-FALSE** — poly `0x1021`, init `0xFFFF`,
refin=false, refout=false, xorout `0x0000`, MSB-first.

Neither `AGENT_IMPLEMENTATION.md` nor `A's_guide.txt` names a specific
CRC16 variant — both just say "CRC16" — and `A's_guide.txt` §8 explicitly
flags this as an open contradiction to resolve with Person B on day 1.
CCITT-FALSE was picked here as a concrete, well-known default (see
`src/common/crc.hpp` for the reasoning), and has since been checked
directly against `sender/src/common/crc.{hpp,cpp}`: Person A's real
implementation is the identical variant, and A's own `test_crc.cpp`
asserts the same `0x29B1` check value (`docs/CROSS_TEAM_ANSWERS.md` Q1).
No longer at risk of a silent mismatch. Other common variants that would
have produced a *different* value for the same bytes, kept here for
reference in case this ever needs re-diagnosing: CRC-16/XMODEM (`0x31C3`),
CRC-16/ARC (`0xBB3D`), CRC-16/MODBUS (`0x4B37`).

## Header-CRC span — settled here, cross-check with A

`hdr_crc16` covers **6 bytes**: `magic` (4) + `proto_len` (2) — i.e.
`crc16(frame, 6)`, computed before `hdr_crc16` and `body_crc32c` are
written. `A's_guide.txt` §8 flags the guide's own field table as
self-contradictory here ("CRC16 over the preceding 8 bytes", but only 6
bytes precede it) and says 6 is almost certainly correct — this is what's
implemented.

## Byte order

Little-endian for all three multi-byte numeric fields (`proto_len`,
`hdr_crc16`, `body_crc32c`). `magic` is 4 raw ASCII bytes, compared with
`memcmp` — byte order doesn't apply to it.

## A full worked frame

Body = ASCII `"123456789"` (the same 9 bytes as the CRC32C vector above,
chosen so this frame's own body_crc32c can be cross-checked against that
vector directly).

```
magic       = 55 4E 49 46            ("UNIF")
proto_len   = 09 00                  (9, little-endian)
hdr_crc16   = 3B 6B                  (0x6B3B, little-endian -- crc16_ccitt_false over the 6 bytes above)
body_crc32c = 83 92 06 E3            (0xE3069283, little-endian -- matches the CRC32C vector above)
body        = 31 32 33 34 35 36 37 38 39   ("123456789")

full frame (21 bytes):
55 4E 49 46 09 00 3B 6B 83 92 06 E3 31 32 33 34 35 36 37 38 39
```

Produced by this repo's `build_frame()` (`src/common/wire.cpp`) and
round-tripped through `validate_frame_prefix()` in
`tests/test_wire.cpp:test_build_then_validate_round_trip` (with a
different body; this exact frame isn't a checked-in test case yet, just
handed to A/C here for cross-checking byte-for-byte).

## Status

- [x] CRC32C variant — settled, matches `A's_guide.txt`.
- [x] hdr_crc16 span (6 bytes, not 8) — settled per `A's_guide.txt` §8's
      own resolution of its guide's self-contradiction.
- [x] Byte order (little-endian) — settled per `A's_guide.txt` §8.
- [x] **CRC16 variant** — CCITT-FALSE, confirmed against Person A's real
      `sender/src/common/crc.cpp`/`test_crc.cpp` (`docs/CROSS_TEAM_ANSWERS.md` Q1).
