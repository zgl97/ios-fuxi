#!/usr/bin/env python3
"""Host-side regression for the A740 generated texture nested IB (P1-5b, r408).

The r408 draw authors the 30 CP_SET_DRAW_STATE groups itself instead of replaying
them, and refuses to submit unless the generated stream matches the r405 capture
byte for byte.  That gate is the whole safety story for the node, so it is pinned
here: parse the group table out of the shipped generator source, rebuild the
nested IB the same way the tool does, and compare it against the captured words
embedded in a740-tex-payload.h.

No GPU, no device, no Vulkan headers: pure byte accounting over two text sources.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GEN = ROOT / 'tools' / 'a740-draw-tex-gen.cpp'
PAYLOAD = ROOT.parents[1] / 'build' / 'fuxi-tools' / 'a740-tex-payload.h'

NESTED_IOVA = 0x20030000
TOP_IOVA = 0x2003c000
PREAMBLE_WORDS = 245
EXPECTED_WORDS = 340
EXPECTED_GROUPS = 30


def parity(x):
    x ^= x >> 16
    x ^= x >> 8
    x ^= x >> 4
    return (0x9669 >> (x & 15)) & 1


def payload_buffers(text):
    out = {}
    for m in re.finditer(r'\{(0x[0-9a-f]+)ull,\s*(\d+)u,\s*"([0-9a-f]*)"\}', text):
        out[int(m.group(1), 16)] = (int(m.group(2)), m.group(3))
    return out


def words_of(hexstr):
    raw = bytes.fromhex(hexstr)
    raw += b'\x00' * (-len(raw) % 4)
    return [int.from_bytes(raw[i:i + 4], 'little') for i in range(0, len(raw), 4)]


def parse_groups(source):
    m = re.search(r'static const Group kGroups\[\]=\{(.*?)\};', source, re.S)
    assert m, 'kGroups table not found in the generator source'
    rows = []
    for g in re.finditer(r'\{(\d+),(0x[0-9a-f]+),(\d+),(0x[0-9a-f]+|\d+)\}', m.group(1)):
        addr = int(g.group(4), 16) if g.group(4).startswith('0x') else int(g.group(4))
        rows.append((int(g.group(1)), int(g.group(2), 16), int(g.group(3)), addr))
    return rows


def build_nested(preamble, groups):
    w = list(preamble)
    n = len(groups) * 3
    w.append(0x70000000 | n | (parity(n) << 15) | (0x43 << 16) | (parity(0x43) << 23))
    for gid, enable, count, addr in groups:
        w.append(count | (enable << 16) | (gid << 24))
        w.append(addr & 0xffffffff)
        w.append(addr >> 32)
    w.extend([0x70388003, 0xd84, 0x1, 0x3])
    return w


def main():
    if not GEN.is_file():
        print('SKIP: %s absent from this tree' % GEN)
        return 0
    if not PAYLOAD.is_file():
        print('SKIP: %s absent (build artifacts not staged)' % PAYLOAD)
        return 0

    source = GEN.read_text()
    captured = words_of(payload_buffers(PAYLOAD.read_text())[NESTED_IOVA][1])
    groups = parse_groups(source)

    assert len(groups) == EXPECTED_GROUPS, 'group count %d != %d' % (len(groups), EXPECTED_GROUPS)
    assert len(captured) == EXPECTED_WORDS, 'captured nested %d words != %d' % (len(captured), EXPECTED_WORDS)

    ids = [g[0] for g in groups]
    assert len(set(ids)) == len(ids), 'duplicate group ids: %s' % ids

    generated = build_nested(captured[:PREAMBLE_WORDS], groups)
    assert len(generated) == EXPECTED_WORDS, 'generated %d words != %d' % (len(generated), EXPECTED_WORDS)

    diffs = [(i, a, b) for i, (a, b) in enumerate(zip(generated, captured)) if a != b]
    assert not diffs, 'generated differs from capture at %r' % diffs[:4]

    # The top IB's CP_INDIRECT_BUFFER length must already equal the generated word
    # count, otherwise the tool would have to patch it (r401 needed that; r408 not).
    top = words_of(payload_buffers(PAYLOAD.read_text())[TOP_IOVA][1])
    indirect = [i for i in range(len(top) - 4) if top[i] == 0x70bf8003 and top[i + 1] == NESTED_IOVA]
    assert len(indirect) == 1, 'expected exactly one CP_INDIRECT_BUFFER to the nested IB, got %d' % len(indirect)
    assert top[indirect[0] + 3] == EXPECTED_WORDS, \
        'top indirect len %d != %d' % (top[indirect[0] + 3], EXPECTED_WORDS)

    print('PASS a740 tex-gen nested: %d words, %d groups, diffs=0, top indirect len=%d'
          % (len(generated), len(groups), top[indirect[0] + 3]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
