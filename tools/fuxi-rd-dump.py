#!/usr/bin/env python3
"""Inspect freedreno RD dumps (.rd/.rd.gz) without Mesa host tools.

The iOS capture path writes the same fd_rd_output sections as Mesa:
  u32 type, u32 size, payload
RD_GPUADDR payload is [iova_low, size, iova_high] (12 bytes)
RD_BUFFER_CONTENTS payload is the raw BO snapshot
RD_CMDSTREAM_ADDR payload is [iova_low, size_in_dwords, iova_high] (12 bytes)

This tool only reads local files. It does not touch the device.
"""
import argparse
import gzip
import hashlib
import json
import os
import struct
import sys
from pathlib import Path

SECTION = {
    0: "NONE", 1: "TEST", 2: "CMD", 3: "GPUADDR", 4: "CONTEXT",
    5: "CMDSTREAM", 6: "CMDSTREAM_ADDR", 7: "PARAM", 8: "FLUSH",
    9: "PROGRAM", 10: "VERT_SHADER", 11: "FRAG_SHADER", 12: "BUFFER_CONTENTS",
    13: "GPU_ID", 14: "CHIP_ID", 15: "SHADER_LOG_BUFFER", 16: "CP_LOG_BUFFER",
    17: "WRBUFFER",
}


def read_bytes(path: Path) -> bytes:
    raw = path.read_bytes()
    if raw[:2] == b"\x1f\x8b":
        return gzip.decompress(raw)
    return raw


def parse(path: Path):
    data = read_bytes(path)
    off = 0
    sections = 0
    chip_id = None
    cmd_name = None
    buffers = []
    cmdstreams = []
    last_gpuaddr = None
    while off + 8 <= len(data):
        typ, size = struct.unpack_from("<II", data, off)
        off += 8
        if off + size > len(data):
            raise ValueError(f"{path}: truncated section {typ} size={size} at {off}")
        payload = data[off:off + size]
        off += size
        sections += 1
        if typ == 3:  # RD_GPUADDR
            if size != 12:
                raise ValueError(f"{path}: bad RD_GPUADDR size {size}")
            low, bsize, high = struct.unpack("<III", payload)
            iova = low | (high << 32)
            last_gpuaddr = (iova, bsize)
            buffers.append({"iova": iova, "size": bsize, "content": None})
        elif typ == 12:  # RD_BUFFER_CONTENTS
            if last_gpuaddr is None:
                raise ValueError(f"{path}: RD_BUFFER_CONTENTS without GPUADDR")
            iova, bsize = last_gpuaddr
            # match the most recent buffer; dump order is GPUADDR, CONTENTS
            for b in reversed(buffers):
                if b["iova"] == iova and b["size"] == bsize and b["content"] is None:
                    b["content"] = payload
                    break
            else:
                raise ValueError(f"{path}: no matching GPUADDR for contents iova=0x{iova:x}")
        elif typ == 6:  # RD_CMDSTREAM_ADDR
            if size != 12:
                raise ValueError(f"{path}: bad RD_CMDSTREAM_ADDR size {size}")
            low, dwords, high = struct.unpack("<III", payload)
            iova = low | (high << 32)
            cmdstreams.append({"iova": iova, "bytes": dwords * 4})
        elif typ == 13:  # RD_GPU_ID
            if size >= 4:
                chip_id = struct.unpack_from("<I", payload, 0)[0]
        elif typ == 14:  # RD_CHIP_ID
            if size >= 4:
                chip_id = struct.unpack_from("<I", payload, 0)[0]
        elif typ == 2:  # RD_CMD
            cmd_name = payload.rstrip(b"\x00").decode("utf-8", "replace")
    if off != len(data):
        raise ValueError(f"{path}: trailing {len(data)-off} bytes")
    return {
        "source": str(path),
        "bytes": len(data),
        "sections": sections,
        "chip_id": chip_id,
        "command": cmd_name,
        "buffers": buffers,
        "cmdstreams": cmdstreams,
    }


def parent_for(iova: int, nbytes: int, buffers):
    for b in buffers:
        if b["iova"] <= iova and iova + nbytes <= b["iova"] + b["size"]:
            return b
    return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("rd", nargs="+", type=Path)
    ap.add_argument("--json", type=Path, help="write combined manifest")
    ap.add_argument("--extract", type=Path, help="extract BO contents into this directory")
    ap.add_argument("--cmdstreams", type=Path, help="extract cmdstream slices into this directory")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    if args.extract:
        args.extract.mkdir(parents=True, exist_ok=True)
    if args.cmdstreams:
        args.cmdstreams.mkdir(parents=True, exist_ok=True)
    combined = []
    for path in args.rd:
        info = parse(path)
        if not args.quiet:
            print(f"{path}: bytes={info['bytes']} sections={info['sections']} chip=0x{info['chip_id'] or 0:08x} command={info['command']!r}")
            print(f"  buffers={len(info['buffers'])} cmdstreams={len(info['cmdstreams'])}")
        manifest_buffers = []
        for i, b in enumerate(info["buffers"]):
            entry = {"iova": b["iova"], "size": b["size"]}
            if b["content"] is not None:
                entry["content_size"] = len(b["content"])
                if args.extract:
                    fn = args.extract / f"bo-{b['iova']:016x}.bin"
                    fn.write_bytes(b["content"])
                    entry["content_file"] = str(fn)
                entry["sha256"] = hashlib.sha256(b["content"]).hexdigest()
            manifest_buffers.append(entry)
        manifest_cmds = []
        for c in info["cmdstreams"]:
            parent = parent_for(c["iova"], c["bytes"], info["buffers"])
            entry = {"iova": c["iova"], "bytes": c["bytes"]}
            if parent:
                entry["parent"] = parent["iova"]
                entry["offset"] = c["iova"] - parent["iova"]
            manifest_cmds.append(entry)
            if args.cmdstreams:
                if not parent or parent["content"] is None:
                    print(f"WARN: cmdstream 0x{c['iova']:x} has no parent content", file=sys.stderr)
                    continue
                off = c["iova"] - parent["iova"]
                chunk = parent["content"][off:off + c["bytes"]]
                (args.cmdstreams / f"cs-{c['iova']:016x}.bin").write_bytes(chunk)
        info["buffers"] = manifest_buffers
        info["cmdstreams"] = manifest_cmds
        combined.append(info)
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(combined if len(combined) > 1 else combined[0], indent=2) + "\n")
        print(f"wrote {args.json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
