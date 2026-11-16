#!/usr/bin/env python3
"""Host-side regression for the P1-6 combo state-group self-generation (r413).

The combo stream carries 61 CP_SET_DRAW_STATE groups spread over five arrays. The
generator must own those groups rather than replay the captured bytes, and the
regenerated stream must still match the r411 capture word for word — otherwise
the on-device run silently draws something else.

This test is the host twin of the in-binary diff gate: it parses the group table
out of the shipped generator source, rebuilds the stream the same way the tool
does (captured template, arrays replaced by regenerated ones), and demands zero
differing words against the captured nested IB.

No device, no GPU, no Vulkan headers.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GEN = ROOT / 'tools' / 'a740-combo-stategen.cpp'
PAYLOAD = ROOT.parents[1] / 'build' / 'fuxi-tools' / 'a740-combo-payload.h'

NESTED_IOVA = 0x20030000
SET_STATE_OP = 0x43
EXPECTED_WORDS = 1056
EXPECTED_GROUPS = 61
EXPECTED_ARRAYS = 5


def parity(x):
    x ^= x >> 16
    x ^= x >> 8
    x ^= x >> 4
    return (0x9669 >> (x & 15)) & 1


def payload_buffers(text):
    pat = re.compile(r'\{(0x[0-9a-f]+)ull,\s*(\d+)u,\s*((?:"[0-9a-f]*"\s*)+|nullptr)\}')
    out = {}
    for m in pat.finditer(text):
        hexs = '' if m.group(3) == 'nullptr' else ''.join(re.findall(r'"([0-9a-f]*)"', m.group(3)))
        out[int(m.group(1), 16)] = (int(m.group(2)), hexs)
    return out


def words(hexstr):
    raw = bytes.fromhex(hexstr)
    raw += b'\x00' * (-len(raw) % 4)
    return [int.from_bytes(raw[i:i + 4], 'little') for i in range(0, len(raw), 4)]


def parse_groups(source):
    m = re.search(r'static const Group kGroups\[\]=\{(.*?)\};', source, re.S)
    assert m, 'kGroups table not found in the generator source'
    rows = []
    for g in re.finditer(r'\{(\d+),(0x[0-9a-f]+),(\d+),(0x[0-9a-f]+)\}', m.group(1)):
        rows.append((int(g.group(1)), int(g.group(2), 16), int(g.group(3)), int(g.group(4), 16)))
    return rows


def parse_array_sizes(source):
    m = re.search(r'static const uint8_t kArraySizes\[\]=\{([\d,\s]+)\};', source)
    assert m, 'kArraySizes not found in the generator source'
    return [int(x) for x in m.group(1).replace(' ', '').split(',') if x]


def build(captured, groups, sizes):
    """Walk the capture; swap each captured CP_SET_DRAW_STATE array for a
    regenerated one built from the group table, byte for byte identical."""
    out = []
    cursor = 0        # which array we are on
    offset = 0        # group-table offset for that array
    i = 0
    while i < len(captured):
        header = captured[i]
        kind = (header >> 28) & 0xF
        if kind == 7:
            n = header & 0x3FFF
            op = (header >> 16) & 0x7F
            if op == SET_STATE_OP:
                assert cursor < len(sizes), 'more set-draw-state arrays in the capture than kArraySizes'
                take = sizes[cursor]
                slice_ = groups[offset: offset + take]
                assert len(slice_) == take, 'kGroups too short for array %d' % cursor
                params = []
                for gid, enable, count, addr in slice_:
                    params.append(count | (enable << 16) | (gid << 24))
                    params.append(addr & 0xFFFFFFFF)
                    params.append(addr >> 32)
                pn = len(params)
                out.append(0x70000000 | pn | (parity(pn) << 15) | (SET_STATE_OP << 16) | (parity(SET_STATE_OP) << 23))
                out.extend(params)
                offset += take
                cursor += 1
                i += 1 + n
                continue
            out.extend(captured[i:i + 1 + n])
            i += 1 + n
            continue
        if kind == 4:
            n = header & 0x7F
            out.extend(captured[i:i + 1 + n])
            i += 1 + n
            continue
        out.append(header)
        i += 1
    return out, cursor


def main():
    assert GEN.is_file(), 'generator source %s is missing (P1-6 self-generation not implemented yet)' % GEN
    if not PAYLOAD.is_file():
        print('SKIP: %s absent (capture not staged)' % PAYLOAD)
        return 0

    source = GEN.read_text()
    groups = parse_groups(source)
    sizes = parse_array_sizes(source)
    captured = words(payload_buffers(PAYLOAD.read_text())[NESTED_IOVA][1])

    assert len(captured) == EXPECTED_WORDS, 'captured nested %d words != %d' % (len(captured), EXPECTED_WORDS)
    assert len(sizes) == EXPECTED_ARRAYS, 'kArraySizes has %d entries, expected %d' % (len(sizes), EXPECTED_ARRAYS)
    assert sum(sizes) == EXPECTED_GROUPS, 'group table covers %d groups, expected %d' % (sum(sizes), EXPECTED_GROUPS)
    assert len(groups) == EXPECTED_GROUPS, 'kGroups has %d entries, expected %d' % (len(groups), EXPECTED_GROUPS)

    generated, used = build(captured, groups, sizes)
    assert used == EXPECTED_ARRAYS, 'spliced %d arrays, expected %d' % (used, EXPECTED_ARRAYS)
    assert len(generated) == EXPECTED_WORDS, 'generated %d words != %d' % (len(generated), EXPECTED_WORDS)

    diffs = [(i, a, b) for i, (a, b) in enumerate(zip(generated, captured)) if a != b]
    assert not diffs, 'regenerated stream differs from capture at %r' % diffs[:4]

    print('PASS a740 combo stategen: %d words, %d arrays, %d groups, diffs=0'
          % (len(generated), len(sizes), len(groups)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
