#!/bin/sh
# Turn a postmarketOS (Alpine, OpenRC) aarch64 rootfs directory into the
# headless server described in docs/TUTORIAL.md.
#
# Run on your PC as root, against an UNMOUNTED-from-the-phone copy of the rootfs
# (a pmbootstrap chroot copy, or a loop-mounted image). On an x86 host the
# chroot steps need qemu-user + binfmt for aarch64 (apt install qemu-user-static).
#
# Required environment (nothing here is ever printed or logged):
#   ROOTFS              rootfs directory to modify
#   DEVICE_IP           static address for the phone, e.g. 192.168.1.50
#   GATEWAY_IP          your router, e.g. 192.168.1.1
#   LAN_CIDR            network allowed to reach SSH, e.g. 192.168.1.0/24
#   PREFIX_LEN          netmask length, e.g. 24
#   SSH_PUBLIC_KEY      path to your ssh-ed25519 PUBLIC key (never the private key)
#   WPA_CONF            path to a wpa_supplicant.conf for YOUR network (kept mode 0600)
#   WIFI_NVRAM          Wi-Fi calibration copied from YOUR phone (see the tutorial)
#   VENDOR_FIRMWARE_DIR Wi-Fi firmware files copied from YOUR phone's /vendor/firmware
#   HELPERS_DIR         output of tools/build-helpers.sh (default: ./out)
set -eu

: "${ROOTFS:?}" "${DEVICE_IP:?}" "${GATEWAY_IP:?}" "${LAN_CIDR:?}" "${PREFIX_LEN:?}"
: "${SSH_PUBLIC_KEY:?}" "${WPA_CONF:?}" "${WIFI_NVRAM:?}" "${VENDOR_FIRMWARE_DIR:?}"
HERE=$(cd "$(dirname "$0")/.." && pwd)
HELPERS_DIR=${HELPERS_DIR:-$HERE/out}
TEMPLATES=$HERE/rootfs
ROOT=$ROOTFS

[ "$(id -u)" = 0 ] || { echo 'run as root (device nodes, chroot)'; exit 1; }
for value in "$DEVICE_IP" "$GATEWAY_IP"; do
    printf '%s' "$value" | grep -Eq '^[0-9]{1,3}(\.[0-9]{1,3}){3}$' || { echo 'invalid IPv4 address'; exit 1; }
done
printf '%s' "$LAN_CIDR" | grep -Eq '^[0-9.]+/[0-9]{1,2}$' || { echo 'invalid LAN_CIDR'; exit 1; }
printf '%s' "$PREFIX_LEN" | grep -Eq '^[0-9]{1,2}$' || { echo 'invalid PREFIX_LEN'; exit 1; }
test -d "$ROOT/etc" && test -x "$ROOT/sbin/openrc-run" || { echo 'ROOTFS does not look like an Alpine/OpenRC tree'; exit 1; }
grep -q '^ssh-ed25519 ' "$SSH_PUBLIC_KEY" || { echo 'SSH_PUBLIC_KEY must be an ssh-ed25519 public key'; exit 1; }
test -s "$WPA_CONF" && test -s "$WIFI_NVRAM" && test -d "$VENDOR_FIRMWARE_DIR" || { echo 'missing Wi-Fi inputs'; exit 1; }
for f in init wifi-init redmi-thermal-daemon redmi-reboot-bootloader; do
    test -x "$HELPERS_DIR/$f" || { echo "missing $HELPERS_DIR/$f: run tools/build-helpers.sh"; exit 1; }
done

# Substitute the network placeholders while installing a template.
render() {  # source destination mode
    install -D -m "$3" /dev/null "$2"
    sed -e "s|@DEVICE_IP@|$DEVICE_IP|g" -e "s|@GATEWAY_IP@|$GATEWAY_IP|g" \
        -e "s|@LAN_CIDR@|$LAN_CIDR|g" -e "s|@PREFIX_LEN@|$PREFIX_LEN|g" "$1" > "$2"
}

# --- mount points the initramfs moves /dev, /proc and /sys onto, and device
#     nodes it hands over before udev runs
mkdir -p "$ROOT/dev" "$ROOT/proc" "$ROOT/sys" "$ROOT/run"
for entry in 'null 1 3 0666' 'zero 1 5 0666' 'random 1 8 0666' \
    'urandom 1 9 0666' 'tty 5 0 0666' 'console 5 1 0600'; do
    set -- $entry
    [ -e "$ROOT/dev/$1" ] || mknod -m "$4" "$ROOT/dev/$1" c "$2" "$3"
done

# --- Wi-Fi inputs (opaque copies; contents are never printed)
install -D -m 0600 "$WPA_CONF" "$ROOT/etc/wpa_supplicant/redmi-migration.conf"
install -D -m 0600 "$WIFI_NVRAM" "$ROOT/etc/hardware/wifi-nvram.bin"
mkdir -p "$ROOT/vendor/firmware"
cp -a "$VENDOR_FIRMWARE_DIR/." "$ROOT/vendor/firmware/"

# --- helpers
install -D -m 0755 "$HELPERS_DIR/wifi-init" "$ROOT/usr/local/sbin/wifi-init"
install -D -m 0755 "$HELPERS_DIR/redmi-thermal-daemon" "$ROOT/usr/local/sbin/redmi-thermal-daemon"
install -D -m 0755 "$HELPERS_DIR/redmi-reboot-bootloader" "$ROOT/usr/local/sbin/redmi-reboot-bootloader"

# --- SSH: key only. Only the PUBLIC key is copied; the host key is generated
#     inside the rootfs and never leaves it.
install -d -m 0700 "$ROOT/root/.ssh"
install -m 0600 "$SSH_PUBLIC_KEY" "$ROOT/root/.ssh/authorized_keys"
if [ ! -s "$ROOT/etc/ssh/ssh_host_ed25519_key" ]; then
    chroot "$ROOT" /usr/bin/ssh-keygen -q -t ed25519 -N '' -f /etc/ssh/ssh_host_ed25519_key
fi
chmod 0600 "$ROOT/etc/ssh/ssh_host_ed25519_key"
render "$TEMPLATES/etc/ssh/sshd_config" "$ROOT/etc/ssh/sshd_config" 0644
render "$TEMPLATES/etc/conf.d/sshd" "$ROOT/etc/conf.d/sshd" 0644
render "$TEMPLATES/etc/conf.d/redmi-wifi" "$ROOT/etc/conf.d/redmi-wifi" 0644

# OpenSSH rejects a locked account ("!") even for public-key logins, so root gets
# "NP": no password can match it and OpenSSH does not treat it as locked. Password
# and keyboard-interactive stay disabled. Every other account is locked, including
# the user pmbootstrap created with the throwaway install password.
python3 - "$ROOT/etc/shadow" <<'PY'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
lines = p.read_text().splitlines(keepends=True)
matches = [i for i, line in enumerate(lines) if line.split(':', 1)[0] == 'root']
if len(matches) != 1:
    raise SystemExit('Unexpected root account entry count; refusing the auth edit')
for i, line in enumerate(lines):
    newline = '\n' if line.endswith('\n') else ''
    fields = line.rstrip('\n').split(':')
    if len(fields) < 2:
        raise SystemExit('Malformed shadow entry; refusing the auth edit')
    if fields[0] == 'root':
        if fields[1] in ('NP', '*NP*'):
            pass
        elif fields[1].startswith(('!', '*')):
            fields[1] = 'NP'
        else:
            raise SystemExit('Root account already has a password field; refusing to replace it')
    elif not fields[1].startswith(('!', '*')):
        fields[1] = '!'
    lines[i] = ':'.join(fields) + newline
p.write_text(''.join(lines))
PY

# Make the packaged sshd wait for our Wi-Fi service.
python3 - "$ROOT/etc/init.d/sshd" <<'PY'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
s = p.read_text()
old = '\tuse logger dns\n\tafter entropy\n\n\tif [ "${rc_need+set}"'
new = '\tuse logger dns\n\tafter entropy\n\tneed redmi-wifi\n\n\tif [ "${rc_need+set}"'
if old in s:
    p.write_text(s.replace(old, new, 1))
elif new not in s:
    raise SystemExit('Unexpected packaged sshd service; refusing an unreviewed edit')
PY

# --- firewall and services
render "$TEMPLATES/etc/nftables.nft.in" "$ROOT/etc/nftables.nft" 0644
for service in redmi-thermal redmi-wifi redmi-boot-confirm redmi-privacy redmi-wifi-guard redmi-soak-logger; do
    render "$TEMPLATES/etc/init.d/$service" "$ROOT/etc/init.d/$service" 0755
done
for tool in redmi-wifi-guard redmi-soak-logger; do
    render "$TEMPLATES/usr/local/sbin/$tool" "$ROOT/usr/local/sbin/$tool" 0755
done

# redmi-wifi owns wlan0 and starts wpa_supplicant itself (see docs/TUTORIAL.md,
# "Wi-Fi"): disable every generic network service so nothing competes for it.
# dbus has no client here, sleep-inhibitor needs elogind (not running), and the
# zram/swapfile services fail or are pointless on this kernel.
for service in wpa_supplicant networkmanager networkmanager-dispatcher routewrangler rfkill networking \
        dbus sleep-inhibitor postmarketos-zram-swap swapfile; do
    chroot "$ROOT" /sbin/rc-update del "$service" default >/dev/null 2>&1 || true
done
for service in redmi-privacy redmi-thermal redmi-wifi nftables sshd redmi-boot-confirm redmi-soak-logger \
        redmi-wifi-guard; do
    chroot "$ROOT" /sbin/rc-update add "$service" default >/dev/null
done

rm -f "$ROOT/etc/resolv.conf"
printf 'nameserver %s\n' "$GATEWAY_IP" > "$ROOT/etc/resolv.conf"
chmod 0644 "$ROOT/etc/resolv.conf"

# --- validation: metadata and syntax only
chroot "$ROOT" /usr/sbin/sshd -t
for f in "$ROOT"/etc/init.d/redmi-* "$ROOT"/usr/local/sbin/redmi-wifi-guard "$ROOT"/usr/local/sbin/redmi-soak-logger; do
    sh -n "$f"
done
for service in redmi-privacy redmi-thermal redmi-wifi nftables sshd redmi-boot-confirm redmi-soak-logger \
        redmi-wifi-guard; do
    test -L "$ROOT/etc/runlevels/default/$service"
done
test ! -L "$ROOT/etc/runlevels/default/wpa_supplicant"
echo 'rootfs configured: key-only SSH, nftables firewall, thermal guard, Wi-Fi services.'
