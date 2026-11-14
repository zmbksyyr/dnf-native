"""Encode the original DNF database setting using its XTEA/ECB wire format."""

import hashlib
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]
ORIGINAL_SHA256 = "0979ef164615e5e3eae006fde74ca2a39a920367dae6a143847dbc0a8d49a967"


def encode_password(password):
    plain = password.encode("ascii")
    if not 1 <= len(plain) < 20 or b"\0" in plain:
        raise ValueError("Original database password must contain 1-19 ASCII bytes")
    original = (ROOT / "originals/dp123/home/neople/dbmw_guild/df_dbmw_r").read_bytes()
    if hashlib.sha256(original).hexdigest() != ORIGINAL_SHA256:
        raise ValueError("Original credential format requires the recorded DBMW ELF")
    # Original CAppConfig constructor initializes mode 0 from this ELF data.
    # Read private input material from the pinned original, never a reference table.
    offset = struct.unpack_from("<I", original, 28)[0]
    stride, count = struct.unpack_from("<HH", original, 42)
    key_address = 0x081AB9C0
    for index in range(count):
        kind, start, address, _, length, _, _, _ = struct.unpack_from("<8I", original, offset + stride * index)
        if kind == 1 and address <= key_address and key_address + 16 <= address + length:
            key = struct.unpack_from(">4I", original, start + key_address - address)
            break
    else:
        raise ValueError("Original credential key data is not file-backed")
    encrypted = bytearray()
    padded = plain.ljust(24, b"\0")
    mask = 0xFFFFFFFF
    for start in range(0, 24, 8):
        left, right = struct.unpack_from(">2I", padded, start)
        total = 0
        for _ in range(32):
            left = (left + ((((right << 4) ^ (right >> 5)) + right) ^ (total + key[total & 3]))) & mask
            total = (total + 0x9E3779B9) & mask
            right = (right + ((((left << 4) ^ (left >> 5)) + left) ^ (total + key[(total >> 11) & 3]))) & mask
        encrypted += struct.pack(">2I", left, right)
    return encrypted.hex()
