# CRC-16 on the Wire

Every frame on the BBB <-> STM32 link ends with two CRC bytes. This note
explains exactly which CRC variant we use, what it covers, and how the
computation works, so anyone writing a second implementation (e.g. on the
BBB side) can match it byte-for-byte.

## Variant: CRC-16/CCITT-FALSE

| Parameter    | Value    |
|--------------|----------|
| Width        | 16 bits  |
| Polynomial   | `0x1021` |
| Init         | `0xFFFF` |
| RefIn        | false    |
| RefOut       | false    |
| XorOut       | `0x0000` |
| Check (`"123456789"`) | `0x29B1` |

There are a handful of different "CCITT" CRCs floating around (Kermit uses
init 0 and reflects, XMODEM uses init 0 and doesn't reflect, ...). We picked
the **FALSE** variant because it's the most common one in embedded code and
has an unambiguous test vector (`0x29B1` for the standard check string).

## The polynomial `0x1021`

The hex literal encodes the coefficients of a binary polynomial, one bit
per term. `0x1021` is `0001 0000 0010 0001` plus an implicit leading `1`
for the `x^16` term (a CRC-16 generator always has that bit, so it's
omitted from the 16-bit literal to save room):

```
       x^16 + x^12 + x^5 + 1

  bit  16  15  14  13  12  11  10   9   8   7   6   5   4   3   2   1   0
       ───────────────────────────────────────────────────────────────────
        1   0   0   0   1   0   0   0   0   0   0   1   0   0   0   0   1
       └─implicit─┘   └──────── 0x1021 (the 16 bits we store) ────────────┘
```

This particular polynomial was standardized by the CCITT (now ITU-T) in
recommendation V.41 and shows up again in **XMODEM**, **HDLC**,
**Bluetooth**, **IrDA**, **SD cards**, and a long list of other framing
protocols. We use it because it's:

- **Strong for short frames.** It catches every single-bit error, every
  double-bit error, every odd number of flipped bits, and every burst of
  up to 16 bit-flips in a row. Longer bursts slip through only with
  probability `2^-16` (~0.0015%). Our frames max out at 263 bytes -- well
  inside the range where CRC-CCITT's detection guarantees are tightest.
- **Cheap in firmware.** Each bit takes one shift and at most one XOR;
  no table, no multiplication. The bitwise loop in `crc16.c` runs in a
  handful of microseconds per frame at 115200 baud -- not worth
  table-driving yet.
- **Ubiquitously tested.** Every reference implementation, test vector,
  and online calculator agrees on what CRC-16/CCITT-FALSE produces, so
  writing a second implementation on the BBB side and cross-checking it
  against ours is trivial.

## What the CRC covers

```
+--------+--------+--------+--------+----------+--------+
| START  |  TYPE  |  LEN   |  SEQ   |  PAYLOAD |  CRC16 |
| 1 byte | 1 byte | 2 bytes| 1 byte | N bytes  | 2 bytes|
+--------+--------+--------+--------+----------+--------+
         ^-------------------- CRC input --------^
```

The CRC is taken over **TYPE + LEN + SEQ + PAYLOAD** -- every byte of the
frame except the `START` sync byte and the CRC itself.

The two CRC bytes are written **little-endian** on the wire: low byte first,
then high byte. Watch the endianness if you copy a reference implementation
that emits big-endian by default.

## Algorithm

This is a classic bitwise MSB-first shift register. Pseudocode:

```
crc = 0xFFFF
for each byte b in data:
    crc = crc XOR (b << 8)          # line up byte with top of register
    repeat 8 times:
        if (crc AND 0x8000) != 0:
            crc = (crc << 1) XOR 0x1021
        else:
            crc = crc << 1
        crc = crc AND 0xFFFF        # keep it 16 bits
return crc
```

Per-byte cost is 8 iterations of shift + conditional XOR. No table, no
reflections, no final XOR. At 115200 baud we spend maybe a few microseconds
per frame on this -- not worth optimizing until we raise the baud rate.

The STM32 implementation lives in [`stm32/src/protocol/crc16.c`](../../stm32/src/protocol/crc16.c)
and matches the pseudocode above line-for-line.

## Minimal worked example

Input: one byte, `0x31` (`'1'`), starting from the init value:

```
crc = 0xFFFF
crc ^= 0x3100        -> 0xCEFF
bit 0: 0xCEFF & 0x8000 -> set,   shift+XOR: (0x9DFE) ^ 0x1021 = 0x8DDF
bit 1: 0x8DDF & 0x8000 -> set,   shift+XOR: (0x1BBE) ^ 0x1021 = 0x0B9F
bit 2: 0x0B9F & 0x8000 -> clear, shift:     0x173E
bit 3: 0x173E & 0x8000 -> clear, shift:     0x2E7C
bit 4: 0x2E7C & 0x8000 -> clear, shift:     0x5CF8
bit 5: 0x5CF8 & 0x8000 -> clear, shift:     0xB9F0
bit 6: 0xB9F0 & 0x8000 -> set,   shift+XOR: (0x73E0) ^ 0x1021 = 0x63C1
bit 7: 0x63C1 & 0x8000 -> clear, shift:     0xC782
crc = 0xC782
```

Extending to the full check string `"123456789"` yields `0x29B1`, matching
the table at the top. Any second implementation should reproduce that.

## Receive path

Two equivalent ways to verify a received frame:

1. Compute the CRC over `TYPE..end-of-PAYLOAD` and compare to the two CRC
   bytes read from the wire. (This is what the STM32 parser does; it keeps
   a running CRC as each byte arrives and compares on completion.)
2. Compute the CRC over `TYPE..CRC_HI` (i.e. including the received CRC
   bytes themselves); the result must equal `0x0000`. Useful if you have
   the whole frame in one buffer already.

On mismatch, the STM32 silently drops the frame and resyncs on the next
`0xAA`. Retransmission is the peer's responsibility once its ACK timeout
fires (see [protocol-spec.md](protocol-spec.md) §"Error Handling").
