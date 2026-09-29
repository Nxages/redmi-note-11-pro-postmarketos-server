# Redmi Note 11 Pro (MT6877) as a headless postmarketOS server

Turn a Xiaomi **Redmi Note 11 Pro** (codename `pissarro`, MediaTek MT6877, kernel 4.14)
into an always-on Linux server: it boots on its own, joins Wi-Fi with a static
address, answers key-only SSH on the LAN behind a firewall, keeps its battery
between 60 % and 80 %, and can be sent back to Android on demand.

Português: [README.pt-BR.md](README.pt-BR.md)

> **Warning.** This overwrites part of the `super` partition and replaces `boot_b`.
> It needs an unlocked bootloader and can brick the phone. The storage range it
> uses overlaps the inactive Android slot's old table and is where a Virtual A/B
> OTA keeps its snapshots, so **an OTA update or a slot switch can destroy your
> Linux**. Only for `pissarro` (MT6877 / Dimensity 920): other phones sold as
> Redmi Note 11 Pro, such as the global 4G and 5G models, are different hardware.
> Read [docs/TUTORIAL.md](docs/TUTORIAL.md), section 0, before anything else. This
> is a field report from one unit, not a supported procedure. Not affiliated with
> Xiaomi, MediaTek or postmarketOS.

## What was verified on the reference unit

| Item | Result |
|---|---|
| Independent boot from a temporary `boot_b`, rootfs on a `super` range | works, confirmed healthy about 30 s after boot |
| Wi-Fi association, static IP, reconnection | works; a 5 h stall after a router drop was mitigated by a guard service |
| Key-only SSH over the LAN, default-drop firewall (kernel 4.14 limits) | works |
| Rescue SSH over USB serial, 600 s boot watchdog | works |
| Return to Android from the running Linux | rehearsed, 3 min 18 s, `boot_b` and range restored by hash |
| Reboot cycles | 5 of 5 passed |
| Charge window 60–80 %, thermal protection | working |
| Screen, camera and microphone nodes disabled | working |
| Boot image builder (`tools/build-boot-image.py`) | reproduced the installed image byte for byte from the same inputs |
| Rootfs configuration script | run against a fresh rootfs copy; same binaries and runlevel as the phone |
| 24 h stability run | the first run was ended by the 5 h Wi-Fi stall above; a second run, with the guard, was in progress when this was written |

## What is in here

| Path | What |
|---|---|
| [docs/TUTORIAL.md](docs/TUTORIAL.md) | The full guide: backups, choosing the storage range, building, writing, first boot, recovery, pitfalls |
| `src/initramfs/` | PID 1 of the initramfs: loop-mounts the ext4 rootfs from `super`, USB serial rescue SSH, boot watchdog |
| `src/wifi/` | MT6877 connectivity bring-up (feeds your own calibration to the vendor driver) |
| `src/thermal/` | Thermal guard and 60–80 % charge-window daemon, with offline tests |
| `src/tools/` | Reboot-to-Fastboot helper |
| `rootfs/` | OpenRC services, firewall template, sshd config (network values are `@PLACEHOLDERS@`) |
| `tools/` | Boot image builder, rootfs configurator, super-range checker, serial SSH proxy, reboot and soak tests, privacy audit |
| `port/` | pmbootstrap device and kernel packages used to build the kernel |

## Not included, on purpose

Vendor firmware, your Wi-Fi calibration, disk and boot images, Wi-Fi credentials,
keys and logs. The tutorial shows how to extract the firmware and calibration from
**your own** phone. `tools/audit-privacy.py` checks the tree for leaks before every
commit.

## Quick path

1. Back everything up and verify hashes (tutorial section 3).
2. `python3 tools/check-super-range.py --active lp-active.txt --inactive lp-inactive.txt`
3. Build kernel, helpers and boot image (sections 6-7).
4. Configure the rootfs, create and write the image (sections 8-9).
5. Flash `boot_b`, confirm within 600 s, rehearse the way back (sections 10-11).

## Credits and licenses

Built on [postmarketOS](https://postmarketos.org), Alpine Linux, OpenRC, OpenSSH,
nftables and `wpa_supplicant`, and on the public MT6877 "hydrogen" kernel tree named in
`port/linux-xiaomi-pissarro/APKBUILD`. Our code is MIT (see [LICENSE](LICENSE));
files derived from the Linux kernel under `port/linux-xiaomi-pissarro/` are GPL-2.0-only.
