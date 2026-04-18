"""CRC-16/CCITT-FALSE: poly=0x1021, init=0xFFFF, no reflect, no XOR-out.

Matches the bitwise C implementation in stm32/src/protocol/crc16.c exactly.
Both crc16_ccitt (bulk) and crc16_step (single byte) are provided so the
parser can accumulate the CRC inline without allocating per-byte objects.
"""
from __future__ import annotations

_POLY: int = 0x1021

CRC16_INIT: int = 0xFFFF


def crc16_ccitt(data: bytes | bytearray, seed: int = CRC16_INIT) -> int:
    """Compute or continue a CRC-16/CCITT-FALSE over a byte buffer.

    Pass seed=CRC16_INIT (the default) for a fresh computation.
    Pass the previous return value to continue across non-contiguous chunks.
    """
    crc = seed
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ _POLY) if (crc & 0x8000) else (crc << 1)
        crc &= 0xFFFF
    return crc


def crc16_step(crc: int, byte: int) -> int:
    """Advance the CRC by exactly one byte.

    Equivalent to crc16_ccitt(bytes([byte]), crc) but without the allocation.
    Used by the parser's hot path.
    """
    crc ^= byte << 8
    for _ in range(8):
        crc = ((crc << 1) ^ _POLY) if (crc & 0x8000) else (crc << 1)
    return crc & 0xFFFF
