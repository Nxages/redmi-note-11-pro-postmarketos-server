#!/usr/bin/env python3
"""Build a boot image that starts your own kernel and pivots into a Linux rootfs.

Inputs come from *your* phone: the current boot_b image (to reuse its DTB,
command line, addresses and AVB envelope), a kernel Image.gz and the static
init binary from src/initramfs. Nothing device-specific is embedded here and
the kernel command line is never printed (it can carry device identifiers).

This script only writes the output file. It never talks to a phone.

    python3 build-boot-image.py \\
        --original boot_b.img --kernel Image.gz --init init \\
        --mkbootimg path/to/mkbootimg.py --out boot-linux.img

Get mkbootimg.py from AOSP (platform/system/tools/mkbootimg).
"""
import argparse
import gzip
import pathlib
import stat
import struct
import subprocess
import sys
import tempfile


def cpio_entry(name, data, mode, ino):
    """One entry of a 'newc' (070701) cpio archive."""
    encoded = name.encode() + b'\0'
    fields = [ino, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(encoded), 0]
    item = b'070701' + b''.join(f'{v:08x}'.encode() for v in fields) + encoded
    item += b'\0' * ((-len(item)) % 4) + data
    return item + b'\0' * ((-len(data)) % 4)


def parse_header_v2(image):
    """Read the fields of an Android boot image header, version 2."""
    if image[:8] != b'ANDROID!':
        sys.exit('not an Android boot image')
    u32 = lambda offset: struct.unpack_from('<I', image, offset)[0]
    u64 = lambda offset: struct.unpack_from('<Q', image, offset)[0]
    if u32(40) != 2:
        sys.exit('only boot image header version 2 is supported (found %d)' % u32(40))
    page = u32(36)
    align = lambda size: (size + page - 1) // page * page
    kernel_size, ramdisk_size, second_size = u32(8), u32(16), u32(24)
    recovery_size = u32(1632)
    dtb_offset = page + align(kernel_size) + align(ramdisk_size) + align(second_size) + align(recovery_size)
    dtb_size = u32(1648)
    if dtb_offset + dtb_size > len(image):
        sys.exit('DTB range is outside the image')
    version = u32(44)
    osver, patch = version >> 11, version & 2047
    return {
        'page_size': page,
        'kernel_address': u32(12),
        'ramdisk_address': u32(20),
        'tags_address': u32(32),
        'dtb_address': u64(1652),
        'dtb': image[dtb_offset:dtb_offset + dtb_size],
        'os_version': f'{(osver >> 14) & 127}.{(osver >> 7) & 127}.{osver & 127}',
        'os_patch_level': f'{(patch >> 4) + 2000:04d}-{patch & 15:02d}',
        'cmdline': (image[64:576].split(b'\0')[0] + b' ' + image[608:1632].split(b'\0')[0]).decode().strip(),
    }


# Options that would make the kernel skip our initramfs or panic-reboot too soon.
DROPPED_CMDLINE_PREFIXES = ('skip_initramfs', 'root=', 'rdinit=', 'init=', 'panic=',
                            'androidboot.force_normal_boot=')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--original', required=True, type=pathlib.Path, help='your current boot_b image (read only)')
    ap.add_argument('--kernel', required=True, type=pathlib.Path, help='kernel Image.gz')
    ap.add_argument('--init', required=True, type=pathlib.Path, help='static aarch64 init (built from src/initramfs)')
    ap.add_argument('--mkbootimg', required=True, type=pathlib.Path, help='AOSP mkbootimg.py')
    ap.add_argument('--out', required=True, type=pathlib.Path)
    args = ap.parse_args()

    original = args.original.read_bytes()
    header = parse_header_v2(original)

    init = args.init.read_bytes()
    if init[:4] != b'\x7fELF' or struct.unpack_from('<H', init, 18)[0] != 183:
        sys.exit('init must be an aarch64 ELF (build it with aarch64-linux-gnu-gcc -static)')

    payload = b''
    for ino, name in enumerate(['dev', 'proc', 'sys', 'newroot'], 1):
        payload += cpio_entry(name, b'', stat.S_IFDIR | 0o755, ino)
    payload += cpio_entry('init', init, stat.S_IFREG | 0o700, 10)
    payload += cpio_entry('TRAILER!!!', b'', 0, 11)
    ramdisk = gzip.compress(payload, mtime=0)

    kept = [part for part in header['cmdline'].split() if not part.startswith(DROPPED_CMDLINE_PREFIXES)]
    cmdline = ' '.join(kept + ['root=/dev/ram', 'rdinit=/init', 'panic=10', 'selinux=0'])

    with tempfile.TemporaryDirectory() as work:
        work = pathlib.Path(work)
        (work / 'dtb').write_bytes(header['dtb'])
        (work / 'ramdisk.gz').write_bytes(ramdisk)
        unsigned = work / 'unsigned.img'
        command = [sys.executable, str(args.mkbootimg),
                   '--header_version', '2', '--pagesize', str(header['page_size']),
                   '--base', '0', '--kernel_offset', hex(header['kernel_address']),
                   '--ramdisk_offset', hex(header['ramdisk_address']), '--second_offset', '0',
                   '--tags_offset', hex(header['tags_address']), '--dtb_offset', hex(header['dtb_address']),
                   '--os_version', header['os_version'], '--os_patch_level', header['os_patch_level'],
                   '--kernel', str(args.kernel), '--ramdisk', str(work / 'ramdisk.gz'),
                   '--dtb', str(work / 'dtb'), '--cmdline', cmdline, '--output', str(unsigned)]
        result = subprocess.run(command, capture_output=True)
        if result.returncode:
            # The command line may carry device identifiers: do not echo it.
            sys.exit('mkbootimg failed (exit %d); its output is not shown because it can include the command line'
                     % result.returncode)
        raw = unsigned.read_bytes()

    # Keep the original AVB footer and vbmeta so the bootloader still finds the
    # structure it expects. The hash no longer matches, which is only tolerated
    # with an UNLOCKED bootloader.
    footer = struct.unpack('>4sIIQQQ', original[-64:-28])
    if footer[0] != b'AVBf':
        sys.exit('the original image has no AVB footer; this tool expects one')
    original_size, vbmeta_offset, vbmeta_size = footer[3], footer[4], footer[5]
    if not len(raw) < original_size <= vbmeta_offset:
        sys.exit('the new boot payload does not fit before the original vbmeta')
    image = bytearray(len(original))
    image[:len(raw)] = raw
    image[vbmeta_offset:vbmeta_offset + vbmeta_size] = original[vbmeta_offset:vbmeta_offset + vbmeta_size]
    image[-64:] = original[-64:]
    args.out.write_bytes(image)
    args.out.chmod(0o600)
    print(f'wrote {args.out} ({len(image)} bytes, same size as the original partition image)')


if __name__ == '__main__':
    main()
