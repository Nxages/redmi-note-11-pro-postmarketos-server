#!/bin/sh
# Cross-compile the static aarch64 helpers into ./out (or $OUT).
# Needs gcc-aarch64-linux-gnu (Debian/Ubuntu: apt install gcc-aarch64-linux-gnu).
# Never touches a phone.
set -eu
HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$HERE/out}
CC=${CC:-aarch64-linux-gnu-gcc}
CFLAGS='-static -Os -Wall -Wextra -Werror'
mkdir -p "$OUT"

# PID 1 of the initramfs. -Wno-unused-function: a few helpers are only used
# on some boot paths.
# shellcheck disable=SC2086
$CC $CFLAGS -Wno-unused-function -o "$OUT/init" "$HERE/src/initramfs/persistent-init.c"
# shellcheck disable=SC2086
$CC $CFLAGS -o "$OUT/wifi-init" "$HERE/src/wifi/wifi-init.c"
# shellcheck disable=SC2086
$CC $CFLAGS -o "$OUT/redmi-thermal-daemon" "$HERE/src/thermal/thermal-daemon.c"
# shellcheck disable=SC2086
$CC $CFLAGS -o "$OUT/redmi-reboot-bootloader" "$HERE/src/tools/reboot-bootloader.c"

for f in init wifi-init redmi-thermal-daemon redmi-reboot-bootloader; do
    printf '%s  %s\n' "$(sha256sum "$OUT/$f" | cut -d' ' -f1)" "$f"
done
