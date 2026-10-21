# QwzWifi and QwzAirport

Wi-Fi for the **Qualcomm FastConnect 7800 / WCN7850** (PCI `17cb:1107`, e.g. the NCM865 module on
AMD X870E boards) on an x86_64 Hackintosh. It is OpenBSD's [**qwz(4)**](https://man.openbsd.org/qwz)
driver running inside a macOS kext (**QwzWifi**), with an optional second kext, **QwzAirport**, that
makes the card a native Airport-style Wi-Fi interface (Wi-Fi menu, System Settings) on macOS Tahoe.
Without QwzAirport the card works as an Ethernet interface controlled by
[HeliPort](https://github.com/OpenIntelWireless/HeliPort). That mode speaks HeliPort's own interface,
so it is expected to work on the older macOS versions HeliPort supports too (see
[macOS versions](#macos-versions)).

> **Experimental.** Developed and tested on one machine. It runs in the kernel: a bug can panic the
> Mac or, through DMA, corrupt memory. Keep a bootable copy of your EFI before installing.

## Two kexts

| Kext | What it is | Runs on |
|---|---|---|
| `QwzWifi.kext` | The driver: OpenBSD qwz(4) + net80211, firmware, DMA, interrupts. Without the native front it appears as an Ethernet interface ("Qualcomm Wi-Fi") controlled by [HeliPort](https://github.com/OpenIntelWireless/HeliPort) or `qwzctl`. | x86_64. Tested on macOS Tahoe (26.7.1); HeliPort mode is expected to work on earlier versions as well, see [macOS versions](#macos-versions) |
| `QwzAirport.kext` | The native Wi-Fi front: the card appears in the Wi-Fi menu and System Settings as Wi-Fi. No hardware access; it talks to QwzWifi through a small C interface (`kext/airport_api.h`). | macOS Tahoe: works on build **25G241**; other Tahoe builds expected to work but not verified, see [macOS versions](#macos-versions) |

## What works

Tested on: MSI MAG X870E Tomahawk WiFi, Ryzen 7 9800X3D, WCN7850 hw2.0, macOS 26.7.1 (25G241),
OpenCore, firmware WLAN.HMT.1.1.c5-00302 (linux-firmware `20260309`).

- Firmware load, scanning on 2.4 and 5 GHz, background scans while connected.
- WPA2-Personal (CCMP) and open networks; 802.11n, 2×2, 40 MHz (MCS 15, 300 Mbit/s link rate).
- Native mode (QwzAirport): network list in the Wi-Fi menu, joining from the menu with the password
  stored by macOS, auto-join of known networks, private Wi-Fi address (the MAC is programmed into the
  firmware), Wi-Fi on/off from the menu, recovery after sleep (slow: ~25 s until auto-join).
- Ethernet mode: HeliPort (networks in the Keychain) or `qwzctl info|status|scan|join|leave|power`.
- Sleep/wake and shutdown power the chip down cleanly.

## Limitations

- **No 802.11ac/ax**: qwz uses 11n only.
- **Security types**: WPA2-Personal with CCMP and open networks only. No WPA3/SAE, no WPA2-Enterprise
  (802.1X), no TKIP-only networks, no WEP.
- **No AirDrop, AWDL, Continuity, Handoff**: the firmware has no AWDL and this card's Bluetooth
  (USB `0489:e10a`) has no macOS driver.
- **No roaming** between access points of one network while connected (background scan is there,
  the switch is not ported from qwx(4)).
- Latency spikes (up to ~165 ms) while a scan is running; macOS asks for scans often in native mode.
- Joining takes ~4–6 s (full scan of all channels first).
- Native mode starts only on macOS builds whose IO80211 ABI was checked (today 25G241; others are expected to work but are not verified). On any other build,
  without `QwzAirport.kext`, or when it fails to start, QwzWifi falls back by itself to its Ethernet
  interface for HeliPort (the `QwzFallback` property of the PCI device says why). To use HeliPort
  while native mode works: `sudo build/qwzctl mode heliport`, or set `QwzNativeData` to false in
  QwzWifi's Info.plist.
- OpenCore injection only: both kexts use one kernel function (`thread_call_isactive`) that is not in
  a public KPI set, so `kmutil load` cannot link them.
- With `config/wifi.plist` the network password is stored in plain text inside the kext on the EFI
  partition. Prefer HeliPort or native mode.

## macOS versions

Everything was tested on **macOS Tahoe (26.7.1)** only; macOS 27 does not run on Intel Macs.

- **Native Wi-Fi mode (QwzAirport): Tahoe only.** It is expected to work on other Tahoe builds
  too, but this has not been verified: it relies on Apple's undocumented IO80211 ABI, so it starts
  only on builds listed in `QwzAirportBuilds` (today `25G241`). On any other build it stays off and
  QwzWifi falls back to HeliPort mode. To check your own build, run `scripts/airport-check.sh check`;
  if it passes, add the build to both lists in `airport/Info.plist` (see below).
- **HeliPort mode (QwzWifi alone): expected to work on earlier macOS versions too.** HeliPort runs
  there, and QwzWifi implements the interface HeliPort talks to (`kext/heliport.h`); it is built for
  macOS 11 and newer. Only Tahoe has been tried so far, so reports from other versions are welcome.
  In this mode do **not** install `QwzAirport.kext`: it is not needed, and it stays off on any
  build that is not in its allowlist anyway.

| | QwzWifi | QwzAirport |
|---|---|---|
| Hard limit in code | none besides `-qwzon`; x86_64 only | `kern.osversion` must be in `QwzAirportBuilds` (`airport/Info.plist`): today only `25G241` |
| OpenCore entry | no `MinKernel`/`MaxKernel` | `MinKernel 25.0.0`, `MaxKernel 25.99.99` |
| Tested | 26.7.1 (25G241) | 26.7.1 (25G241) |

QwzWifi is built with `-mmacosx-version-min=11.0` and declares KPI 19.0, so older systems can
load it. On another Tahoe build QwzAirport stays off and QwzWifi falls back
to HeliPort (see [Limitations](#limitations)). A build is added to `QwzAirportBuilds` only after
`scripts/kc-abi.py --check` and `--returns` pass on it (they compare our IO80211 headers with the
kernel's actual class layouts).

## Building

Command Line Tools only (`xcode-select --install`), no Xcode:

```sh
scripts/fetch-sources.sh     # OpenBSD at the pinned commit, firmware (SHA-256 checked)
scripts/patches.sh apply     # openbsd/ = pristine OpenBSD + patches/openbsd
scripts/build.sh             # build/QwzWifi.kext, build/QwzAirport.kext, build/qwzctl
```

Pins and hashes are in `config/sources.json`; `docs/DEPENDENCIES.md` explains updates.

## Installing (OpenCore)

1. Back up your EFI.
2. Copy `build/QwzWifi.kext` (and `build/QwzAirport.kext` for native Airport mode on Tahoe; skip it
   for HeliPort mode) into `EFI/OC/Kexts/`.
3. `python3 scripts/oc-enable.py on [path/to/EFI/OC]`: backs up `config.plist`, adds the kext entries
   and the boot-arg `-qwzon`. The driver does nothing without `-qwzon`.
4. Reboot. Driver log: `scripts/qwzlog.sh` (saves `~/Desktop/qwz.log`).
5. Undo: `python3 scripts/oc-enable.py off`, or restore the backup.

## Safety

- No writes to the chip's persistent storage (no OTP/eFuse/flash paths; qwz was audited).
- Firmware is used unmodified and pinned by hash; regulatory and TX power data are untouched.
- Every DMA address the chip sees is checked to be below 4 GB and inside a known allocation (the
  IOMMU is usually off on AMD Hackintoshes).
- Passwords and keys are never logged and are wiped after use.
- The HeliPort/`qwzctl` control interface opens only for the user logged in at the console and for
  root (switching to HeliPort mode needs root).

## Disclaimer

This software is provided **as is, without warranty of any kind**, express or implied, as stated in
sections 11 and 12 of the GNU GPL v2 (`LICENSE`). It is a kernel driver for unsupported hardware on
an unsupported platform: use it at your own risk. The authors are not liable for any damage it may
cause, including kernel panics, data loss, memory corruption, an unbootable system or damage to
hardware.

This project is independent. It is not affiliated with or endorsed by Qualcomm, Apple, OpenBSD or the
authors of the projects it builds on. Qualcomm, macOS, Wi-Fi and other names are trademarks of their
respective owners.

## License and credits

QwzWifi is free software under the **GNU General Public License, version 2 only** (`LICENSE`).
It builds on other people's work; `NOTICE.md` lists every third-party part and its license:

- **[OpenBSD](https://github.com/openbsd/src)**: the [qwz(4)](https://man.openbsd.org/qwz) driver by
  Stefan Sperling (derived from [Linux ath12k](https://wireless.docs.kernel.org/en/latest/en/users/drivers/ath12k.html)
  by The Linux Foundation and Qualcomm), the net80211 stack and crypto code (ISC, BSD and similar
  licenses). OpenBSD's files are not copied into this repository; our changes to them are in
  `patches/openbsd/`.
- **GPL code by other authors**: `QwzAirport.kext` is adapted from the native Wi-Fi front of
  [**laobamac/itlwm**](https://github.com/laobamac/itlwm), itself based on
  [**AirportItlwm**](https://github.com/OpenIntelWireless/itlwm) (the `AirportItlwm` part of
  OpenIntelWireless/itlwm) by zxystd and the OpenIntelWireless contributors (GPL-2.0). Its IO80211
  headers come from laobamac/itlwm (`airport/abi/`, which also contains Apple headers under
  APSL-2.0). `airport/CREDITS` lists the details.
- **[HeliPort](https://github.com/OpenIntelWireless/HeliPort)** (BSD-3-Clause): the user-client
  protocol in `kext/heliport.h`.
- **Qualcomm firmware** from [linux-firmware](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git)
  (proprietary but redistributable): embedded in `QwzWifi.kext`, not covered by the GPL; ship
  `firmware/Notice.txt` with any built kext.
