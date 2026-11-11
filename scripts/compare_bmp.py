"""Compare two folders of 32-bit BMP snapshots written by `CppDesk.exe --ui-snapshot <dir>`.

Usage: python scripts/compare_bmp.py <dirA> <dirB> [--tolerance N]

Prints one line per snapshot: `same`, `DIFF <n> pixels max delta <d>`, or why it could not be compared.
Exits 1 if any snapshot differs by more than the tolerance (per channel) or is missing.
"""
import os
import struct
import sys


def read_bmp(path):
    with open(path, 'rb') as f:
        data = f.read()
    if data[:2] != b'BM':
        raise ValueError('not a BMP')
    offset = struct.unpack_from('<I', data, 10)[0]
    width, height = struct.unpack_from('<ii', data, 18)
    bpp = struct.unpack_from('<H', data, 28)[0]
    if bpp != 32:
        raise ValueError('expected 32 bits per pixel, got %d' % bpp)
    return abs(width), abs(height), data[offset:offset + abs(width) * abs(height) * 4]


def compare(a, b, tolerance):
    wa, ha, pa = read_bmp(a)
    wb, hb, pb = read_bmp(b)
    if (wa, ha) != (wb, hb):
        return None, 'size %dx%d vs %dx%d' % (wa, ha, wb, hb)
    diff = 0
    worst = 0
    box = [wa, ha, -1, -1]
    for i in range(0, len(pa), 4):
        if pa[i:i + 4] == pb[i:i + 4]:
            continue
        d = max(abs(pa[i] - pb[i]), abs(pa[i + 1] - pb[i + 1]), abs(pa[i + 2] - pb[i + 2]))
        worst = max(worst, d)
        if d > tolerance:
            diff += 1
            x, y = (i // 4) % wa, (i // 4) // wa
            box = [min(box[0], x), min(box[1], y), max(box[2], x), max(box[3], y)]
    where = ' in box (%d,%d)-(%d,%d)' % tuple(box) if diff else ''
    return diff, 'max delta %d%s' % (worst, where)


def main(argv):
    args = [a for a in argv[1:] if not a.startswith('--')]
    tolerance = 0
    if '--tolerance' in argv:
        tolerance = int(argv[argv.index('--tolerance') + 1])
        args = [a for a in args if a != str(tolerance)]
    if len(args) != 2:
        print(__doc__)
        return 2
    dir_a, dir_b = args
    names = sorted(n for n in os.listdir(dir_a) if n.endswith('.bmp'))
    failed = False
    for name in names:
        other = os.path.join(dir_b, name)
        if not os.path.exists(other):
            print('%s: MISSING in %s' % (name, dir_b))
            failed = True
            continue
        diff, detail = compare(os.path.join(dir_a, name), other, tolerance)
        if diff is None:
            print('%s: DIFF %s' % (name, detail))
            failed = True
        elif diff:
            print('%s: DIFF %d pixels %s' % (name, diff, detail))
            failed = True
        else:
            print('%s: same' % name)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
