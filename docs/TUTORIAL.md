# Tutorial: a headless postmarketOS server on a Redmi Note 11 Pro (MT6877)

Portuguese version: [TUTORIAL.pt-BR.md](TUTORIAL.pt-BR.md)

This is how one Redmi Note 11 Pro (codename `pissarro`, MediaTek MT6877, kernel
4.14) was turned into an always-on Linux server that boots on its own, joins Wi-Fi,
answers SSH on the LAN, protects its battery, and can be put back to Android on
demand. Everything below was done on **one** unit. Treat it as a field report and a
starting point, not as a supported procedure.

## 0. Read this first

**You can brick the phone.** The procedure overwrites part of the `super`
partition and replaces `boot_b`. It needs an unlocked bootloader. Do not start
without complete, hash-verified backups and a working `fastboot` connection.

The design has sharp edges you must understand before you copy it:

| Fact | Consequence |
|---|---|
| The rootfs lives in a 4 GiB **range inside `super`**, not in its own partition. | The range is only "free" because Android runs from slot B and the range overlaps **inactive slot A** extents. |
| An OTA update, or switching the active slot, can overwrite that range. | Your Linux can be destroyed by Android. Disable OTA and never switch slots. |
| `boot_b` is replaced by the Linux boot image. | Android does not boot until you restore the original `boot_b` (procedure in section 11). |
| Android's `userdata` (F2FS, encrypted) is left alone. | You only get 4 GiB of rootfs. Using `userdata` would mean erasing Android for good: not covered here. |
| Only the Wi-Fi radio works. | No mobile data, GPU, audio, camera, display, Bluetooth. That is fine for a headless server. |

Nothing here is endorsed by Xiaomi, MediaTek or postmarketOS.

## 1. Architecture

```
 PC ── USB ──► boot_b (temporary, ours)
               kernel (built from the public 4.14 "hydrogen" tree) + your stock DTB
               + tiny initramfs whose only file is /init (static aarch64, src/initramfs)
                  │
                  ├─ USB gadget (ACM serial) ── rescue SSH: sshd -i over /dev/ttyGS0
                  ├─ 600 s boot watchdog ──────► reboot to Fastboot if never confirmed
                  └─ loop-mount ext4 rootfs from  super @ 4 GiB, length 4 GiB
                       └─ switch_root → OpenRC (postmarketOS/Alpine)
                            wifi ─ nftables ─ sshd ─ thermal/charge ─ boot-confirm
                                                              │
               boot-confirm checks Wi-Fi, IP, route, gateway, firewall, SSH port
               and only then SIGSTOPs the watchdog ("this boot is good").
```

Two safety ideas carry the whole thing:

1. **Nothing is trusted until proven.** A boot that never confirms itself returns
   to Fastboot by itself, where you restore Android from a PC.
2. **Every write has a verified way back.** Before writing a range you save its
   original bytes and check the hash; after writing you read it back and check
   again.

## 2. What you need

- A Redmi Note 11 Pro (pissarro) with an **unlocked bootloader** and root on Android
  (we used Magisk) to read partitions and calibration data.
- A Linux x86_64 build host (a VM or WSL works). Tools: `git`, `python3`, `gcc-aarch64-linux-gnu`,
  `qemu-user-static` with binfmt for aarch64, `e2fsprogs`, `android-tools` (`adb`,
  `fastboot`), `pyserial`. Roughly 60 GB free disk.
- AOSP `mkbootimg.py` and `unpack_bootimg.py` (platform/system/tools/mkbootimg).
- A USB cable, a router you control, and an SSH key pair used only for this phone.
- Time to read every command before you run it.

Adapt the examples: the documentation uses `192.168.1.50` for the phone and
`192.168.1.1` for the router.

## 3. Back up everything first

Check which slot Android runs from (`ro.boot.slot_suffix`); this tutorial assumes
**`_b`**. If yours is `_a`, mirror every `_b`/`_a` below and re-do the range check
in section 4 for your layout.

On the phone, as root, record identity and hashes; keep all backups **off** the
phone and private:

```sh
getprop ro.product.device            # pissarro
getprop ro.boot.slot_suffix          # _b
sha256sum /dev/block/by-name/boot_b
blockdev --getsize64 /dev/block/by-name/super
```

Copy these to your PC and verify the hash after each transfer (`adb exec-out` from
Linux/macOS/WSL, never through a Windows console pipe, which corrupts binary data):

```sh
adb exec-out su -c 'cat /dev/block/by-name/boot_b' > boot_b.img
adb exec-out su -c 'cat /dev/block/by-name/super'  > super-full.img     # ~8.5 GiB
sha256sum boot_b.img super-full.img   # compare with the phone's sha256sum output
chmod 0444 boot_b.img super-full.img
```

Also keep `boot_a`, `vbmeta*` and anything else you can. Do **not** touch
`userdata`, `persist`, `nvdata` or bootloader partitions.

## 4. Choose the storage range inside `super`

`super` holds Android's logical partitions. Dump both slots' tables:

```sh
adb shell su -c 'lpdump --slot=1' > lp-active.txt     # slot b
adb shell su -c 'lpdump --slot=0' > lp-inactive.txt   # slot a
python3 tools/check-super-range.py --active lp-active.txt --inactive lp-inactive.txt
```

On our unit the active slot B partitions ended at 3.47 GiB of an 8.5 GiB `super`,
so `[4 GiB, 8 GiB)` did not touch them. It **did** overlap slot A's table, which is
why this is a hack, not a layout. The script exits with an error if the range
overlaps the active slot or runs past the end. Never write a range it rejects.

Save the original bytes of exactly that range and verify them:

```sh
adb exec-out su -c 'dd if=/dev/block/by-name/super bs=4M skip=1024 count=1024 2>/dev/null' \
  | gzip -1 > super-range-original.bin.gz
# on the phone: dd if=/dev/block/by-name/super bs=4M skip=1024 count=1024 | sha256sum
# on the PC:    gzip -dc super-range-original.bin.gz | sha256sum      (must match)
```

(`bs=4M skip=1024` is 4 GiB in; `count=1024` is 4 GiB long.)

## 5. Get the Wi-Fi firmware and calibration from your phone

The MT6877 connectivity chip needs its firmware and a per-unit calibration blob.
**Both belong to your phone and must never be published**; the calibration also
contains your Wi-Fi hardware address. Copy them from Android (root) and keep them
private:

```sh
# firmware (8 files)
for f in conninfra.cfg wifi.cfg WIFI_RAM_CODE_soc5_0_1_1.bin soc5_0_ram_mcu_1_1_hdr.bin \
         soc5_0_ram_wmmcu_1_1_hdr.bin soc5_0_ram_bt_1_1_hdr.bin BT_FW.cfg fm_cust.cfg; do
  adb exec-out su -c "cat /vendor/firmware/$f" > private/vendor-firmware/$f
done
# calibration (a small file, at most 8192 bytes)
adb exec-out su -c 'cat /mnt/vendor/nvdata/APCFG/APRDEB/WIFI' > private/wifi-nvram.bin
```

Create a `wpa_supplicant.conf` for your own network (mode 0600, never committed):

```
ctrl_interface=/run/wpa_supplicant
network={
  ssid="YOUR-NETWORK"
  psk="YOUR-PASSPHRASE"
  key_mgmt=WPA-PSK
}
```

## 6. Build the kernel and a base rootfs

*This section describes what we did; we did not re-run it end to end for this
write-up. Expect to adapt it.*

We used [pmbootstrap](https://gitlab.postmarketos.org/postmarketOS/pmbootstrap) and
[pmaports](https://gitlab.postmarketos.org/postmarketOS/pmaports) at the commits
below, inside a dedicated build VM:

```
pmbootstrap  edb3097c7307216b088478b7c424ee07d636f41b
pmaports     972f578fc9e87831adc4b3c7ba4c0d66461f2060
```

Copy `port/device-xiaomi-pissarro` and `port/linux-xiaomi-pissarro` into
`pmaports/device/downstream/`, then configure and build:

```sh
pmbootstrap config device xiaomi-pissarro
pmbootstrap config ui console
pmbootstrap config service_manager openrc
pmbootstrap config extra_packages openssh,wpa_supplicant,nftables
pmbootstrap build linux-xiaomi-pissarro
pmbootstrap build device-xiaomi-pissarro
```

Notes from the kernel build:

- The kernel is the public MT6877 "hydrogen" 4.14.356 tree named in
  `port/linux-xiaomi-pissarro/APKBUILD` (pinned commit), built with Clang/LLVM
  (we used LLVM 23.1.2 and DTC 1.7.2).
- Fixes needed: kernel headers for the host SELinux tools, removing the 32-bit
  compat VDSO, using the host `dtc` instead of the vendor's prebuilt glibc binary
  (`0001-use-host-dtc.patch`).
- `deviceinfo_flash_method="none"` is intentional: pmbootstrap must never flash
  this phone.
- The device package is only a vehicle for the kernel. We did **not** use
  pmbootstrap's boot image or flasher.

Extract the kernel from the package (the `.apk` is a concatenation of gzip tar
streams):

```sh
# the built package is somewhere under pmbootstrap's work/packages/ directory
tar --ignore-zeros -xzf linux-xiaomi-pissarro-4.14.356-r0.apk boot/vmlinuz
mv boot/vmlinuz Image.gz
```

For the rootfs we used the directory tree that `pmbootstrap install` leaves in
its work directory (`work/chroot_rootfs_xiaomi-pissarro`): a plain Alpine/OpenRC
aarch64 tree, about 560 MB, without a display manager. Copy it somewhere you own.

## 7. Build the helpers and the boot image

```sh
sh tools/build-helpers.sh          # -> out/init, wifi-init, redmi-thermal-daemon, redmi-reboot-bootloader
python3 tools/build-boot-image.py \
    --original boot_b.img --kernel Image.gz --init out/init \
    --mkbootimg path/to/mkbootimg.py --out boot-linux.img
```

`build-boot-image.py` reads the layout, DTB, command line and AVB footer from
**your** `boot_b.img`. It drops `root=`, `rdinit=`, `init=`, `panic=`,
`skip_initramfs` and `androidboot.force_normal_boot=` from your command line and
adds `root=/dev/ram rdinit=/init panic=10 selinux=0`. It never prints the command
line (it can contain device identifiers). The AVB footer of the original image is
kept so the bootloader still finds the structure it expects; the hash inside no
longer matches, which only works with an **unlocked** bootloader.

*Verified:* on our unit this script reproduced the installed boot image
byte for byte (same SHA-256) from the original `boot_b`, the compiled `Image.gz`
and the `init` built from the same source.

Sanity-check your result before flashing anything:

```sh
python3 unpack_bootimg.py --boot_img boot-linux.img --out unpacked   # header v2, sizes as expected
sha256sum boot-linux.img && ls -l boot-linux.img boot_b.img         # same size
```

## 8. Configure the rootfs and create the ext4 image

```sh
sudo ROOTFS=/path/to/rootfs-copy \
     DEVICE_IP=192.168.1.50 GATEWAY_IP=192.168.1.1 LAN_CIDR=192.168.1.0/24 PREFIX_LEN=24 \
     SSH_PUBLIC_KEY=~/.ssh/redmi_ed25519.pub \
     WPA_CONF=private/wpa_supplicant.conf WIFI_NVRAM=private/wifi-nvram.bin \
     VENDOR_FIRMWARE_DIR=private/vendor-firmware HELPERS_DIR=out \
     sh tools/configure-rootfs.sh
```

It installs the helpers and services, sets up key-only SSH, renders the firewall
for your network, disables competing network services and enables ours. It only
copies your **public** key; the SSH host key is generated inside the rootfs.

*Verified:* run against a fresh pmbootstrap rootfs copy, it completed, produced the
expected default runlevel and the same helper binaries (hashes) that run on the
phone. Read the script before running it.

Create the image. The ext4 features matter: the kernel is 4.14, so avoid anything
newer than it understands (`orphan_file` and `metadata_csum_seed` are the two that
newer e2fsprogs enable):

```sh
truncate -s 4G rootfs.img
mkfs.ext4 -F -L redmi-linux -m 0 -O ^orphan_file,^metadata_csum_seed \
          -E lazy_itable_init=0,lazy_journal_init=0 rootfs.img
sudo mount -o loop rootfs.img /mnt/rootfs
sudo tar --one-file-system --acls --xattrs --numeric-owner -cpf - -C /path/to/rootfs-copy . \
  | sudo tar --acls --xattrs --numeric-owner -xpf - -C /mnt/rootfs
sudo umount /mnt/rootfs
e2fsck -fn rootfs.img && gzip -1 -k rootfs.img && sha256sum rootfs.img rootfs.img.gz
```

Keep `rootfs.img` exactly 4 GiB: the initramfs checks an ext4 magic number at
byte `4 GiB + 1080` of `super` and refuses to mount anything else.

## 9. Write the image into the super range

Only do this after sections 3 and 4 are complete and verified.

1. Battery below about 38 °C and Android idle. Writing 4 GiB heats the phone; we
   refused to start above 40.0 °C and stopped at 41.0 °C
   (`/sys/class/power_supply/battery/temp` is in tenths of a degree).
2. Put the compressed image on the phone and verify it there:

   ```sh
   adb push rootfs.img.gz /data/local/tmp/
   adb shell su -c 'sha256sum /data/local/tmp/rootfs.img.gz'      # must match the PC
   ```

3. Write it. `bs=4194304 seek=1024` is exactly 4 GiB into `super`:

   ```sh
   adb shell su -c 'test "$(blockdev --getro /dev/block/by-name/super)" = 0 &&
     gzip -dc /data/local/tmp/rootfs.img.gz |
     dd of=/dev/block/by-name/super bs=4194304 seek=1024 conv=notrunc,fsync && sync'
   ```

4. Read it back and compare with the SHA-256 of `rootfs.img`:

   ```sh
   adb shell su -c 'dd if=/dev/block/by-name/super bs=4194304 skip=1024 count=1024 2>/dev/null | sha256sum'
   ```

   If it differs, restore the original range immediately (section 11) and stop.

We streamed the image over SSH instead of `adb push`, with a temperature gate
every 30 s, but the write itself is the same `gzip -dc | dd`.

## 10. First boot

```sh
adb reboot bootloader
fastboot getvar product          # pissarro
fastboot getvar current-slot     # b
fastboot getvar unlocked         # yes
fastboot flash boot_b boot-linux.img
fastboot reboot
```

(`fastboot boot` failed on our unit with `usb_read failed (31)`, so we flashed
`boot_b`, which is why the original must be restored afterwards.)

A USB serial device appears (vendor ID `1d6b`, product `0104`). The init prints
`BOOT_STAGE=...` lines on it. Open a rescue SSH session through it:

```sh
ssh -o "ProxyCommand=python3 tools/serial-ssh-proxy.py /dev/ttyACM0" \
    -o HostKeyAlias=redmi-linux-rootfs root@rescue
```

(On Windows use the COM port, e.g. `COM5`. Put the rootfs SSH host key, from
`/etc/ssh/ssh_host_ed25519_key.pub` inside your image, in your `known_hosts` under
the alias `redmi-linux-rootfs`, and use `StrictHostKeyChecking=yes`.)

**You have 600 seconds.** If `redmi-boot-confirm` has not proven the server
healthy by then, the watchdog reboots to Fastboot. Once Wi-Fi, address, route,
gateway ping, firewall and the SSH port are all good, the service pauses the
watchdog and creates `/run/redmi-boot-confirmed`. From then on the phone stays up
and SSH works over the LAN: `ssh -p 2222 root@192.168.1.50`.

If the USB cable is replugged and the PC no longer sees the serial port, rebind the
gadget from the phone: `echo "" > /sys/kernel/config/usb_gadget/redmi_probe/UDC;
echo musb-hdrc > /sys/kernel/config/usb_gadget/redmi_probe/UDC`.

Then run `tools/reboot-cycle-test.sh 3` and let `tools/soak-monitor.sh` watch it for
24 hours before you trust it. Both scripts read `DEVICE_IP`, `KNOWN_HOSTS` (a file
holding only the rootfs host key) and `SSH_KEY` from the environment; see their
headers.

## 11. Recovery: back to Android

**From the running Linux** (either works; both send the phone to Fastboot):

```sh
# on the phone:
/usr/local/sbin/redmi-reboot-bootloader --confirm-bootloader
# or, if that is not available, poke the paused watchdog:
kill -USR1 "$(cat /run/redmi-watchdog.pid)"; kill -CONT "$(cat /run/redmi-watchdog.pid)"
```

**From Fastboot on the PC:**

```sh
fastboot flash boot_b boot_b.img      # your ORIGINAL, hash-verified backup
fastboot reboot
```

Android boots again. Then put the original bytes back in the `super` range so
Android's own data is intact (the range overlapped slot A):

```sh
adb push super-range-original.bin.gz /data/local/tmp/
adb shell su -c 'gzip -dc /data/local/tmp/super-range-original.bin.gz |
  dd of=/dev/block/by-name/super bs=4194304 seek=1024 conv=notrunc,fsync && sync'
adb shell su -c 'dd if=/dev/block/by-name/super bs=4194304 skip=1024 count=1024 2>/dev/null | sha256sum'
```

Compare with the hash you recorded in section 4. On our unit the whole round trip,
starting from the running Linux, took 3 minutes 18 seconds, with `boot_b` and the
range verified by hash. If you cannot reach Fastboot at all, use the hardware key
combination for Fastboot on your phone; the bootloader itself was never modified.

## 12. What each service does

| Service | Purpose |
|---|---|
| `redmi-privacy` | Turns the backlight off and removes the camera, microphone-capture and unused radio (Bluetooth, GPS, IR, fingerprint) device nodes at every boot. It deliberately does **not** write the flash/torch LEDs: the mt6360 driver fires the flash on any brightness write, even `0`. |
| `redmi-thermal` (`redmi-thermal-daemon`) | Every 10 s reads battery, charger and CPU temperature. Keeps the battery in a **60–80 % charge window** by toggling `input_suspend`; suspends charging when hot (soft 42 °C, hard 45 °C battery), fails closed on bad sensors, powers off at 55 °C sustained, and caps CPU frequency through the MediaTek PPM. |
| `redmi-wifi` | Runs `wifi-init` (opens the connectivity driver, feeds it your calibration, enables the station interface), then `wpa_supplicant`, then sets a static IP and route. |
| `redmi-wifi-guard` | Escalating recovery when the association is lost: scan at ~1 min, reassociate at ~2 min, restart only `wpa_supplicant` at ~5 min. |
| `nftables` | Default-drop input; allows loopback, established traffic, and SSH + ICMP from your LAN only. IPv6 dropped. |
| `sshd` | Port 2222, root with key only, no forwarding, waits for the firewall and Wi-Fi. |
| `redmi-boot-confirm` | Verifies the whole stack, then pauses the boot watchdog. |
| `redmi-soak-logger` | One health line every 10 minutes in `/var/lib/redmi-soak/health.log`. |
| `chronyd` | Time sync (needs `rc_provide="net"` on `redmi-wifi`, see below). |

## 13. Things that went wrong (and the fixes)

- **`wpa_supplicant` respawning forever.** Alpine's `wpa_supplicant` 2.11-r4 is
  built without `CONFIG_DEBUG_FILE`. Passing `-f /dev/null` made it print its usage
  and exit with status 0 before creating its control socket, and `supervise-daemon`
  restarted it in a loop. Do not pass `-f`. Also note that `supervise-daemon`'s
  pidfile holds the supervisor's PID, not the daemon's.
- **Firewall on kernel 4.14.** The kernel has no `NFT_META` and no nf_tables set
  backends. Rules must use only payload, cmp, bitwise and ct expressions: an `ip`
  table (not `inet`), `ip protocol tcp` before every port match, one rule per port
  (no sets), and loopback matched by address (`ip saddr 127.0.0.0/8`), not by
  `iifname`. Enabling `CONFIG_NFT_META` etc. in the kernel would lift this.
- **Wi-Fi stuck in `SCANNING` for 5 hours** after the router dropped the link at
  night, with the network visible at -40 dBm; one manual `wpa_cli scan` fixed it.
  The cause could not be proven (supplicant output is discarded and the kernel log
  is flooded by a repeating thermal driver message). `redmi-wifi-guard` is the
  mitigation; it was tested by forcing a disconnect (recovered in about two
  minutes) and by killing the supplicant (recovered in about 9 seconds).
- **`chronyd` never started.** It `need`s `net`, and the only provider was the
  `networking` service we had disabled, which then failed. `rc_provide="net"` in
  `/etc/conf.d/redmi-wifi` fixes it. Also make sure only **one** `logbookd` runs:
  a second copy took over `/dev/log` and blocked `chronyd` on a full socket.
- **The camera flash blinked** when a service wrote `0` to `flash-light*`/`torch-light*`
  LEDs. The driver pulses the flash on every write. Leave those LEDs alone. Two
  pulses at ~2 s of every boot come from the kernel's driver probe and cannot be
  avoided from userspace.
- **Load average around 20** is cosmetic: about 20 MediaTek kernel threads sit in
  uninterruptible sleep (`D`) while the CPU is ~99 % idle.
- **`scaling_cur_freq` lies.** It is an index kept by the driver; real PLL
  frequency differed, and even `scaling_max_freq` did not contain the big cluster.
  The MediaTek PPM limit (`/proc/ppm/policy/hard_userlimit_max_cpu_freq`) did.
- **OpenSSH refused key logins for the root account** while its password field was
  locked (`!`). Use the non-password marker `NP` (done by `configure-rootfs.sh`);
  password and keyboard-interactive stay disabled.
- **Windows and USB serial.** Windows purges serial input when a COM port opens, so
  the rescue listener waits for a `STARTSSH` line and only then answers
  `BOOTSTRAP_READY` and starts `sshd -i`. Use `serial-ssh-proxy.py`, which does
  that handshake.

## 14. Verification checklist

- [ ] Boot from cold, unattended, reaches `redmi-boot-confirmed` in about 30 s.
- [ ] Three reboots in a row, each with a new boot ID (`tools/reboot-cycle-test.sh 3`).
- [ ] Wi-Fi reconnects after the router restarts; the guard log shows what it did.
- [ ] From another LAN host, only SSH and ping answer; from off-LAN nothing.
- [ ] Charging pauses at 80 % and resumes at 60 %; temperatures stay in range.
- [ ] Rollback to Android rehearsed once **before** you depend on the server.
- [ ] 24 hours of `tools/soak-monitor.sh` without an unexpected reboot.

## 15. Known limitations

- 4 GiB rootfs, in a range that Android's OTA can overwrite.
- Requires an unlocked bootloader and root on Android; verified-boot is bypassed.
- Kernel 4.14 with vendor drivers; no mainline support.
- Tested on one unit and one ROM. Partition layouts and calibration paths may differ.
- The modem is left as the bootloader loaded it; there is no safe way to stop it
  without the Android userspace.

## Privacy: what never goes in a public repository

Run `python3 tools/audit-privacy.py` before every commit. Do not publish: disk or
boot images, firmware, the Wi-Fi calibration, `wpa_supplicant.conf`, SSH keys or
`known_hosts`, logs, your addresses, or anything read from `userdata`. Use a
GitHub no-reply address for commits (`ID+USERNAME@users.noreply.github.com`) so your
real e-mail is not embedded in the history.
