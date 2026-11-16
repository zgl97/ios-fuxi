#!/usr/bin/env python3
"""Host-side regression for the A740 P1-6 combo replay (r412).

The combo stream carries depth + blend + MSAA + sampled texture in one command
stream. Two properties must hold for the replay to be meaningful, and both are
checkable without a GPU:

  1. the captured top IB's CP_INDIRECT_BUFFER length equals the captured nested
     IB's actual length, otherwise the draw silently truncates;
  2. the nested IB really contains the feature mix (two draws, state groups for
     the texture descriptor and the blend/MSAA state) rather than a plain draw.

No device, no Vulkan headers: pure parsing of the generated payload header.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PAYLOAD = ROOT.parents[1] / 'build' / 'fuxi-tools' / 'a740-combo-payload.h'

TOP_IOVA = 0x2003c000
NESTED_IOVA = 0x20030000
INDIRECT_OP = 0x70bf8003  # CP_INDIRECT_BUFFER, type7 opcode 0x3f
DRAW_OP = 0x38            # CP_DRAW_INDX_OFFSET, type7 opcode 0x38
SET_STATE_OP = 0x43       # CP_SET_DRAW_STATE, type7 opcode 0x43


def buffers(text):
    out = {}
    pat = re.compile(r'\{(0x[0-9a-f]+)ull,\s*(\d+)u,\s*((?:"[0-9a-f]*"\s*)+|nullptr)\}')
    for m in pat.finditer(text):
        hexs = '' if m.group(3) == 'nullptr' else ''.join(re.findall(r'"([0-9a-f]*)"', m.group(3)))
        out[int(m.group(1), 16)] = (int(m.group(2)), hexs)
    return out


def words(hexstr):
    raw = bytes.fromhex(hexstr)
    raw += b'\x00' * (-len(raw) % 4)
    return [int.from_bytes(raw[i:i + 4], 'little') for i in range(0, len(raw), 4)]


def walk(w):
    """Yield (index, opcode, params) for every PM4 packet; skip type4 register writes."""
    i = 0
    while i < len(w):
        header = w[i]
        kind = (header >> 28) & 0xF
        if kind == 7:
            n = header & 0x3FFF
            yield i, (header >> 16) & 0x7F, w[i + 1:i + 1 + n]
            i += 1 + n
        elif kind == 4:
            i += 1 + (header & 0x7F)
        else:
            i += 1


def main():
    if not PAYLOAD.is_file():
        print('SKIP: %s absent (capture not staged)' % PAYLOAD)
        return 0

    bufs = buffers(PAYLOAD.read_text())
    assert TOP_IOVA in bufs, 'top IB 0x2003c000 missing from payload'
    assert NESTED_IOVA in bufs, 'nested IB 0x20030000 missing from payload'

    top = words(bufs[TOP_IOVA][1])
    nested = words(bufs[NESTED_IOVA][1])

    indirect = [(i, p) for i, op, p in walk(top) if op == 0x3F]
    assert len(indirect) == 1, 'expected one CP_INDIRECT_BUFFER in the top IB, got %d' % len(indirect)
    idx, params = indirect[0]
    assert params[0] == NESTED_IOVA, 'indirect target 0x%x != 0x%x' % (params[0], NESTED_IOVA)
    declared = params[2]
    assert declared == len(nested), \
        'top indirect len %d != nested words %d (would truncate the stream)' % (declared, len(nested))

    # Turnip emits a binning pass and a render pass per pipeline, so two pipelines
    # (near texture+alpha, far opaque red) give four CP_DRAW_INDX_OFFSET packets.
    draws = [p for _, op, p in walk(nested) if op == DRAW_OP]
    set_state = [p for _, op, p in walk(nested) if op == SET_STATE_OP]
    assert len(draws) == 4, 'expected 4 draws (2 pipelines x 2 passes), got %d' % len(draws)
    counts = sorted({d[0] for d in draws})
    assert len(counts) == 2, 'expected two distinct index counts, got %r' % counts
    assert len(set_state) >= 4, 'expected several CP_SET_DRAW_STATE arrays, got %d' % len(set_state)

    group_count = sum((len(p) // 3) for p in set_state)
    assert group_count >= 60, 'expected the full state-group set, got %d groups' % group_count

    print('PASS a740 combo replay: nested %d words, indirect len %d, draws %d (counts %s), state arrays %d, groups %d'
          % (len(nested), declared, len(draws), [hex(c) for c in counts], len(set_state), group_count))
    return 0


if __name__ == '__main__':
    sys.exit(main())
