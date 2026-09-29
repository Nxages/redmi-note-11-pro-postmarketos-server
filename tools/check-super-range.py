#!/usr/bin/env python3
"""Check a byte range of the `super` partition against the Android logical partitions.

Reads `lpdump` text (run it on the phone as root) and tells you whether the range
you plan to overwrite overlaps partitions of the ACTIVE slot (that would break the
Android you are running: do not proceed) or of the INACTIVE slot (allowed, but see
docs/TUTORIAL.md: an OTA update or a slot switch can then destroy your rootfs).

    adb exec-out "su -c 'lpdump --slot=1'" > lp-active.txt      # slot b = 1 (check yours!)
    adb exec-out "su -c 'lpdump --slot=0'" > lp-inactive.txt
    python3 check-super-range.py --active lp-active.txt --inactive lp-inactive.txt

It only reads text files. It never talks to a phone.
"""
import argparse
import pathlib
import re
import sys

SECTOR = 512
LAYOUT = re.compile(r'^super: (\d+) \.\. (\d+): (\S+) ', re.M)   # end is exclusive
SUPER_SIZE = re.compile(r'Partition name: super\s+First sector: \d+\s+Size: (\d+) bytes')
VIRTUAL_AB = re.compile(r'^Header flags:.*\bvirtual_ab_device\b', re.M)


def extents(text):
    return [(int(a) * SECTOR, int(b) * SECTOR, name) for a, b, name in LAYOUT.findall(text)]


def overlaps(items, start, end):
    return [name for a, b, name in items if a < end and b > start]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--active', required=True, type=pathlib.Path, help='lpdump of the slot Android is running from')
    ap.add_argument('--inactive', type=pathlib.Path, help='lpdump of the other slot')
    ap.add_argument('--offset', type=lambda v: int(v, 0), default=4 * 1024**3, help='range start in bytes (default 4 GiB)')
    ap.add_argument('--length', type=lambda v: int(v, 0), default=4 * 1024**3, help='range length in bytes (default 4 GiB)')
    ap.add_argument('--super-bytes', type=int, help='size of super; read from the lpdump when omitted')
    args = ap.parse_args()

    active_text = args.active.read_text()
    active = extents(active_text)
    if not active:
        sys.exit('no "super: START .. END: name" lines found in the active lpdump')
    super_bytes = args.super_bytes
    if super_bytes is None:
        found = SUPER_SIZE.search(active_text)
        if not found:
            sys.exit('super size not found; pass --super-bytes')
        super_bytes = int(found.group(1))
    start, end = args.offset, args.offset + args.length

    problems = []
    if start % 4096 or args.length % 4096:
        problems.append('offset and length must be multiples of 4096')
    if end > super_bytes:
        problems.append(f'range ends at {end}, beyond super ({super_bytes} bytes)')
    hit_active = overlaps(active, start, end)
    if hit_active:
        problems.append('overlaps ACTIVE-slot partitions: ' + ', '.join(hit_active))

    print(f'super size            : {super_bytes} bytes ({super_bytes / 1024**3:.2f} GiB)')
    print(f'planned range         : [{start}, {end})  = {start / 1024**3:.2f} .. {end / 1024**3:.2f} GiB')
    print(f'active slot ends at   : {max(b for _, b, _ in active) / 1024**3:.2f} GiB')
    if VIRTUAL_AB.search(active_text):
        print('virtual A/B device    : yes -> an OTA writes its snapshot data into free space of super;')
        print('                        keep OTA updates off while your rootfs lives there.')
    if args.inactive:
        inactive = extents(args.inactive.read_text())
        hit_inactive = overlaps(inactive, start, end)
        print('inactive-slot overlap : ' + (', '.join(hit_inactive) if hit_inactive else 'none'))
        if hit_inactive:
            print('  -> allowed, but an OTA or a switch to the other slot can overwrite your rootfs.')
    else:
        print('inactive-slot overlap : not checked (pass --inactive)')
    for problem in problems:
        print('PROBLEM:', problem)
    print('RESULT: ' + ('UNSAFE, do not write this range' if problems else 'no overlap with the active slot'))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
