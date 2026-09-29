#!/bin/bash
# PC-side view of a stability run. Every 10 minutes it appends one line with:
# a LAN ping and the latest device-side health line read over SSH with the
# rootfs host key. The device keeps its own log (/var/lib/redmi-soak/health.log)
# even if this PC is off.
#
#   DEVICE_IP=192.168.1.50 KNOWN_HOSTS=~/.ssh/redmi_known_hosts \
#     SSH_KEY=~/.ssh/id_ed25519 bash tools/soak-monitor.sh soak.log 24
#
# Do not commit the log: it contains your device's health history and address.
set -uo pipefail
OUT=${1:?usage: soak-monitor.sh OUTPUT_FILE [HOURS]}
HOURS=${2:-24}
: "${DEVICE_IP:?set DEVICE_IP}" "${KNOWN_HOSTS:?set KNOWN_HOSTS}" "${SSH_KEY:?set SSH_KEY}"
SSH_PORT=${SSH_PORT:-2222}
end=$(( $(date +%s) + HOURS * 3600 ))

dssh() {
    ssh -T -o BatchMode=yes -o IdentitiesOnly=yes -o StrictHostKeyChecking=yes -o ConnectTimeout=15 \
        -o HostKeyAlias=redmi-linux-rootfs -o "UserKnownHostsFile=$KNOWN_HOSTS" \
        -i "$SSH_KEY" -p "$SSH_PORT" "root@$DEVICE_IP" "$@"
}

while [ "$(date +%s)" -lt "$end" ]; do
    if ping -c 1 -W 2 "$DEVICE_IP" >/dev/null 2>&1 || ping -n 1 -w 2000 "$DEVICE_IP" 2>/dev/null | grep -q 'TTL='; then
        lan_ping=yes
    else
        lan_ping=no
    fi
    device=$(dssh 'tail -n 1 /var/lib/redmi-soak/health.log' 2>/dev/null | tr -d '\r')
    [ -n "$device" ] || device='device_ssh=failed'
    echo "host_ts=$(date -u +%Y-%m-%dT%H:%M:%SZ) lan_ping=$lan_ping | $device" >> "$OUT"
    sleep 600
done
echo "host_ts=$(date -u +%Y-%m-%dT%H:%M:%SZ) monitor=finished" >> "$OUT"
