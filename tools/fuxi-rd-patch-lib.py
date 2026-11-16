#!/usr/bin/env python3
"""Patch Turnip's tu_queue_wait_fence to succeed (RD dump FULL wait has no
previous fence on the first submission). Input and output must be the exact
libvulkan_freedreno.dylib from the same build."""
import struct
import sys
from pathlib import Path

SRC = Path(sys.argv[1])
DST = Path(sys.argv[2])
OFF = 0xA639C
ORIG = bytes.fromhex("080841f9087d4df9")  # ldr x8,[x0,#0x210]; ldr x8,[x8,#0x1af8]
PATCH = bytes.fromhex("00008052c0035fd6")  # mov w0,#0; ret

data = bytearray(SRC.read_bytes())
if data[OFF:OFF + len(ORIG)] != ORIG:
    raise SystemExit("unexpected bytes at tu_queue_wait_fence")
data[OFF:OFF + len(PATCH)] = PATCH
DST.parent.mkdir(parents=True, exist_ok=True)
DST.write_bytes(data)
print(f"patched {DST} at {OFF:#x}, {len(data)} bytes")
