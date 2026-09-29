#!/bin/sh
# Offline tests for src/thermal/thermal-daemon.c and the refusal path of
# src/tools/reboot-bootloader.c. Needs aarch64-linux-gnu-gcc and qemu-aarch64
# (binfmt) on an x86 host: sh tools/test-thermal-daemon.sh
# The reboot helper is NEVER run with --confirm-bootloader here: under
# qemu-user its reboot syscall would reach the host kernel and reboot it.
set -eu
exec 2>&1
REPO=$(cd "$(dirname "$0")/.." && pwd)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
CFLAGS='-static -Os -Wall -Wextra -Werror'
# shellcheck disable=SC2086
aarch64-linux-gnu-gcc $CFLAGS -o "$W/redmi-thermal-daemon" "$REPO/src/thermal/thermal-daemon.c"
# shellcheck disable=SC2086
aarch64-linux-gnu-gcc $CFLAGS -o "$W/redmi-reboot-bootloader" "$REPO/src/tools/reboot-bootloader.c"
echo "thermal_daemon_sha256=$(sha256sum "$W/redmi-thermal-daemon" | cut -d' ' -f1)"
echo "reboot_helper_sha256=$(sha256sum "$W/redmi-reboot-bootloader" | cut -d' ' -f1)"
pass=0; failed=0
check() {
    name=$1; expected=$2; shift 2
    actual=$("$W/redmi-thermal-daemon" --decide "$@")
    if [ "$actual" = "$expected" ]; then pass=$((pass + 1)); else failed=$((failed + 1)); echo "FAIL $name: $actual (expected $expected)"; fi
}
#     name                     expected                                                  B     CPU   CHG   CAP A H C
check full_stops_charging      'allowed=0 hot=0 critical=0 reason=window_full'           38700 44000 39700 100 1 0 0
check charges_below_stop       'allowed=1 hot=0 critical=0 reason=window_charge'         30000 40000 30000 79  1 0 0
check stops_at_80              'allowed=0 hot=0 critical=0 reason=window_full'           30000 40000 30000 80  1 0 0
check hysteresis_waits         'allowed=0 hot=0 critical=0 reason=window_full'           30000 40000 30000 70  0 0 0
check resumes_at_60            'allowed=1 hot=0 critical=0 reason=window_charge'         30000 40000 30000 60  0 0 0
check battery_soft_hot         'allowed=0 hot=1 critical=0 reason=hot'                   42000 40000 30000 50  1 0 0
check stays_hot_until_cool     'allowed=0 hot=1 critical=0 reason=hot'                   40000 40000 30000 50  0 1 0
check cools_then_window        'allowed=1 hot=0 critical=0 reason=window_charge'         38000 70000 50000 50  0 1 0
check low_battery_override     'allowed=1 hot=1 critical=0 reason=low_battery_override'  43000 40000 30000 10  0 1 0
check hard_battery_always_off  'allowed=0 hot=1 critical=0 reason=hard_hot'              45000 40000 30000 10  1 1 0
check hard_charger_always_off  'allowed=0 hot=1 critical=0 reason=hard_hot'              30000 40000 60000 10  1 0 0
check cpu_soft_hot             'allowed=0 hot=1 critical=0 reason=hot'                   30000 80000 30000 50  1 0 0
check capacity_invalid         'allowed=0 hot=0 critical=0 reason=sensor_fault'          30000 40000 30000 -1  1 0 0
check battery_invalid          'allowed=0 hot=0 critical=0 reason=sensor_fault'          -1    40000 30000 50  1 0 0
check critical_third_sample    'allowed=0 hot=1 critical=3 reason=critical'              55000 40000 30000 50  0 1 2
check critical_resets          'allowed=0 hot=1 critical=0 reason=hard_hot'              50000 40000 30000 50  0 1 2

# --once against a fake sysfs tree.
F=$W/fake
mkdir -p "$F/sys/class/power_supply/battery" "$F/sys/class/thermal/thermal_zone4" "$F/sys/class/thermal/thermal_zone8" "$F/run"
printf '387\n' > "$F/sys/class/power_supply/battery/temp"
printf '100\n' > "$F/sys/class/power_supply/battery/capacity"
printf '0\n' > "$F/sys/class/power_supply/battery/input_suspend"
printf 'mtktscpu\n' > "$F/sys/class/thermal/thermal_zone4/type"; printf '44186\n' > "$F/sys/class/thermal/thermal_zone4/temp"
printf 'charger_therm\n' > "$F/sys/class/thermal/thermal_zone8/type"; printf '39715\n' > "$F/sys/class/thermal/thermal_zone8/temp"
once() {
    name=$1; suspend=$2; reason=$3
    REDMI_THERMAL_ROOT=$F "$W/redmi-thermal-daemon" --once >/dev/null || true
    got_suspend=$(head -c 1 "$F/sys/class/power_supply/battery/input_suspend")
    got_reason=$(sed -n 's/^reason=//p' "$F/run/redmi-thermal.state")
    if [ "$got_suspend" = "$suspend" ] && [ "$got_reason" = "$reason" ]; then pass=$((pass + 1)); else failed=$((failed + 1)); echo "FAIL once_$name: suspend=$got_suspend reason=$got_reason"; fi
}
once full_suspends 1 window_full
printf '55\n' > "$F/sys/class/power_supply/battery/capacity"; once low_resumes 0 window_charge
printf '430\n' > "$F/sys/class/power_supply/battery/temp"; once hot_suspends 1 hot
printf '360\n' > "$F/sys/class/power_supply/battery/temp"; rm "$F/sys/class/power_supply/battery/capacity"; once missing_capacity_fails_closed 1 sensor_fault
printf '55\n' > "$F/sys/class/power_supply/battery/capacity"; once sensor_recovery_resumes 0 window_charge
state_keys=$(sed 's/=.*//' "$F/run/redmi-thermal.state" | tr '\n' ' ')
[ "$state_keys" = 'sample battery_mC cpu_mC charger_mC capacity charging reason hot charge_control cpu_caps ' ] && pass=$((pass + 1)) || { failed=$((failed + 1)); echo "FAIL state_keys: $state_keys"; }

set +e
"$W/redmi-reboot-bootloader" >/dev/null 2>&1; helper_rc=$?
"$W/redmi-reboot-bootloader" --yes >/dev/null 2>&1; helper_rc2=$?
set -e
[ "$helper_rc" = 64 ] && [ "$helper_rc2" = 64 ] && pass=$((pass + 1)) || { failed=$((failed + 1)); echo "FAIL helper_refusal: $helper_rc $helper_rc2"; }
echo "tests_passed=$pass tests_failed=$failed"
[ "$failed" = 0 ]
