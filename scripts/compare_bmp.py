"""Compare two folders of 32-bit BMP snapshots written by `CppDesk.exe --ui-snapshot <dir>`.

Usage: python scripts/compare_bmp.py <dirA> <dirB> [--tolerance N]

Prints one line per snapshot: `same`, `DIFF <n> pixels max delta <d>`, or why it could not be compared.
Exits 1 if any snapshot differs by more than the tolerance (per channel) or is missing.
"""
import argparse
import os
import struct
import sys


def read_bmp(path):
    with open(path, 'rb') as f:
        data = f.read()
    if len(data) < 54 or data[:2] != b'BM':
        raise ValueError('not a valid BMP')
    offset = struct.unpack_from('<I', data, 10)[0]
    width, height = struct.unpack_from('<ii', data, 18)
    bpp = struct.unpack_from('<H', data, 28)[0]
    if bpp != 32:
        raise ValueError('expected 32 bits per pixel, got %d' % bpp)
    w, h = abs(width), abs(height)
    if w == 0 or h == 0:
        raise ValueError('empty BMP dimensions %dx%d' % (width, height))
    needed = offset + w * h * 4
    if offset < 54 or len(data) < needed:
        raise ValueError('truncated BMP (have %d bytes, need %d)' % (len(data), needed))
    pixels = data[offset:needed]
    if height > 0:
        # Bottom-up BMP: normalize rows to top-down so top-down and bottom-up compare identically.
        stride = w * 4
        pixels = b''.join(pixels[y * stride:(y + 1) * stride] for y in range(h - 1, -1, -1))
    return w, h, pixels


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
    parser = argparse.ArgumentParser(description='Compare two folders of 32-bit BMP snapshots.')
    parser.add_argument('dir_a')
    parser.add_argument('dir_b')
    parser.add_argument('--tolerance', type=int, default=0)
    try:
        ns = parser.parse_args(argv[1:])
    except SystemExit as e:
        return int(e.code) if isinstance(e.code, int) else 2

    names_a = {n for n in os.listdir(ns.dir_a) if n.endswith('.bmp')}
    names_b = {n for n in os.listdir(ns.dir_b) if n.endswith('.bmp')}
    all_names = sorted(names_a | names_b)
    if not all_names:
        print('ERROR: no .bmp files found in %s or %s' % (ns.dir_a, ns.dir_b))
        return 1

    failed = False
    for name in all_names:
        if name not in names_b:
            print('%s: MISSING in %s' % (name, ns.dir_b))
            failed = True
            continue
        if name not in names_a:
            print('%s: MISSING in %s' % (name, ns.dir_a))
            failed = True
            continue
        try:
            diff, detail = compare(os.path.join(ns.dir_a, name), os.path.join(ns.dir_b, name), ns.tolerance)
        except ValueError as err:
            print('%s: ERROR %s' % (name, err))
            failed = True
            continue
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
