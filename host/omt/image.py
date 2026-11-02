# SPDX-License-Identifier: Apache-2.0
"""Firmware image builder (Python mirror of firmware/src/image.c).

The release tooling builds images without a C toolchain; a test asserts the
output is byte-identical to the firmware's own builder.
"""
from __future__ import annotations

import struct
import zlib

MAGIC = 0x49544D4F
FLAG_CRASH_ON_BOOT = 0x1
HDR_FMT = "<IBBHIII16s"          # + uint32 hdr_crc = 40 bytes
DEFAULT_TUNABLES = (0.010, 0.35, 0.08, 50.0, -14.0, -13.0)


def build(major: int, minor: int, build: int = 0, flags: int = 0, tag: str = "",
          tunables=DEFAULT_TUNABLES) -> bytes:
    tun = struct.pack("<6f", *tunables)
    plen = len(tun) + 256
    payload = bytearray(tun)
    for i in range(len(tun), plen):
        payload.append((i * 37 + major * 11 + minor) & 0xFF)
    hdr = struct.pack(HDR_FMT, MAGIC, major, minor, build, plen, zlib.crc32(payload), flags,
                      tag.encode()[:16].ljust(16, b"\0"))
    return hdr + struct.pack("<I", zlib.crc32(hdr)) + bytes(payload)


def parse(img: bytes) -> dict:
    magic, ma, mi, bld, plen, pcrc, flags, tag = struct.unpack_from(HDR_FMT, img)
    (hcrc,) = struct.unpack_from("<I", img, 36)
    payload = img[40:40 + plen]
    return {"magic_ok": magic == MAGIC, "version": f"{ma}.{mi}", "build": bld,
            "flags": flags, "tag": tag.rstrip(b"\0").decode(errors="replace"),
            "hdr_crc_ok": zlib.crc32(img[:36]) == hcrc,
            "payload_crc_ok": len(payload) == plen and zlib.crc32(payload) == pcrc,
            "size": 40 + plen}
