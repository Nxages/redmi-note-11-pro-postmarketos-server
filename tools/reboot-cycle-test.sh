#!/bin/bash
# Reboot the installed Linux over LAN SSH and verify each return.
#
#   DEVICE_IP=192.168.1.50 KNOWN_HOSTS=~/.ssh/redmi_known_hosts \
#     SSH_KEY=~/.ssh/id_ed25519 bash tools/reboot-cycle-test.sh 3
#
# Each cycle needs: a NEW kernel boot_id, SSH back with the rootfs host key, the
# boot-confirm marker, a paused (SIGSTOP) boot watchdog and every core service
# started. If the phone does not come back, the initramfs watchdog (600 s) sends
# it to Fastboot and you can restore Android (docs/TUTORIAL.md, "Recovery").
set -uo pipefail
CYCLES=${1:-3}
: "${DEVICE_IP:?set DEVICE_IP}" "${KNOWN_HOSTS:?set KNOWN_HOSTS (file with ONLY the rootfs host key)}" "${SSH_KEY:?set SSH_KEY}"
SSH_PORT=${SSH_PORT:-2222}
SERVICES='redmi-thermal redmi-wifi nftables sshd redmi-boot-confirm redmi-privacy chronyd redmi-soak-logger redmi-wifi-guard'

dssh() {
    ssh -T -o BatchMode=yes -o IdentitiesOnly=yes -o StrictHostKeyChecking=yes -o ConnectTimeout=8 \
        -o HostKeyAlias=redmi-linux-rootfs -o "UserKnownHostsFile=$KNOWN_HOSTS" \
        -i "$SSH_KEY" -p "$SSH_PORT" "root@$DEVICE_IP" "$@"
}

HEALTH="cat /proc/sys/kernel/random/boot_id
cut -d. -f1 /proc/uptime
test -e /run/redmi-boot-confirmed && echo confirmed=yes || echo confirmed=no
wd=\$(cat /run/redmi-watchdog.pid 2>/dev/null || true)
[ -n \"\$wd\" ] && awk '/^State:/ { print \"watchdog=\" \$2 }' \"/proc/\$wd/status\" 2>/dev/null || echo watchdog=unknown
for s in $SERVICES; do
  printf '%s=%s\n' \"\$s\" \"\$(rc-service \"\$s\" status 2>&1 | sed -n 's/.*status: //p')\"
done"

for cycle in $(seq 1 "$CYCLES"); do
    before=$(dssh "$HEALTH" 2>/dev/null) || { echo "CYCLE=$cycle PRECHECK=ssh_failed"; exit 2; }
    old_boot=$(printf '%s\n' "$before" | sed -n 1p)
    started=$(date +%s)
    dssh 'reboot' >/dev/null 2>&1 || true
    down=0
    for _ in $(seq 1 45); do
        dssh true >/dev/null 2>&1 || { down=1; break; }
        sleep 2
    done
    up=0
    for _ in $(seq 1 84); do
        sleep 5
        after=$(dssh "$HEALTH" 2>/dev/null) || continue
        new_boot=$(printf '%s\n' "$after" | sed -n 1p)
        if [ "$new_boot" != "$old_boot" ] && printf '%s\n' "$after" | grep -q '^confirmed=yes$'; then
            up=1
            break
        fi
    done
    elapsed=$(( $(date +%s) - started ))
    if [ "$up" != 1 ]; then
        echo "CYCLE=$cycle RESULT=not_back went_down=$down elapsed_s=$elapsed"
        exit 1
    fi
    pattern=$(printf '%s' "$SERVICES" | tr ' ' '|')
    failed=$(printf '%s\n' "$after" | grep -E "^($pattern)=" | grep -vc '=started$' || true)
    watchdog=$(printf '%s\n' "$after" | sed -n 's/^watchdog=//p')
    uptime=$(printf '%s\n' "$after" | sed -n 2p)
    echo "CYCLE=$cycle RESULT=back elapsed_s=$elapsed uptime_s=$uptime watchdog=$watchdog services_not_started=$failed"
    if [ "$failed" != 0 ] || [ "$watchdog" != T ]; then exit 1; fi
done
echo "REBOOT_CYCLES_PASSED=$CYCLES"
