# QwzWifi: Qualcomm WCN7850 Wi-Fi driver for a Hackintosh

Explain hardware and kernel concepts in plain words: assume a C++ developer who is not a kernel
developer. Ask for approval before anything that touches the boot USB stick (the OpenCore EFI).

## Goal
Get a Qualcomm FastConnect 7800 / WCN7850 (PCI `17cb:1107`) to connect under macOS Tahoe 26 on a
Hackintosh. We take the simplest working path: port OpenBSD **qwz(4)**
into an **itlwm-style** kext. macOS sees an Ethernet interface (`IOEthernetController`), and Wi-Fi
is controlled through HeliPort or a hardcoded SSID. A native Wi-Fi menu (IO80211) is out of scope
until a connection works. Bluetooth (`0489:e10a`, USB) is out of scope.

## Machine-specific notes
Facts about the development PC (hardware, boot stick paths, OpenCore state, recovery) belong in
`local/` and `CLAUDE.local.md`, both gitignored. Never commit them.

## Build (no Xcode needed)
- `scripts/build.sh` uses only Command Line Tools (clang 21, the SDK's Kernel.framework) and
  outputs `build/QwzWifi.kext`.
- `config/sources.json` is the single source of build/import URLs, commits, sparse paths
  and firmware version/hashes. Research-only settings belong in gitignored `local/`.
  `scripts/fetch-sources.sh` fetches pinned build sources into `third_party/` and firmware into
  `firmware/`, then creates the working copy `openbsd/` (see below).
- Compile-only checks also work on Linux with clang and MacKernelSDK headers:
  `-target x86_64-apple-macos11.0 -nostdinc -nostdinc++ -mkernel -fapple-kext -fno-rtti
  -fno-exceptions -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -I MacKernelSDK/Headers`.
  Linking a kext needs Apple `ld` (`-Xlinker -kext`). lld cannot link one.
- The OpenBSD side (`openbsd/sys/**/*.c`, `compat/*.c`, `adapter/*.c`) compiles **as C** (`-x c -std=gnu11
  -ffreestanding -nostdinc`, include roots `compat/include` then `openbsd/sys`, `-DQWZMAC -DQWZ_DEBUG`). It never
  sees Apple headers; it reaches macOS only through `compat/include/qwzmac/os.h`, which
  `kext/os_bridge.cpp` implements on IOKit. (itlwm compiles OpenBSD code as C++, but that needs
  hand edits all over qwz.c, so we don't.)
- `scripts/check-qwz.sh [files]` is a compile-only check of the OpenBSD side.
- `build.sh` also builds `build/QwzAirport.kext` (native Wi-Fi front, `airport/`): C++ against
  `airport/abi/` (IO80211 headers, once copied from laobamac/itlwm, now our files edited in
  place, `QWZAIRPORT`; provenance and a diff command in `airport/abi/UPSTREAM`) and the SDK.
  `scripts/kc-abi.py --check` / `--returns` verify those headers against this Mac's kernel
  collections (slots, sizes, return types); run both before adding a macOS build to
  `QwzAirportBuilds` (airport/Info.plist). `scripts/airport-load.sh` loads it by hand (no stick).
- `scripts/test-compat.sh` runs user-space unit tests of `compat/` (ASan, fake DMA addresses,
  `tests/os_user.c` stands in for the kernel). Run it after touching compat.

## OpenBSD sources: upstream + patches
The OpenBSD revision is in `config/sources.json` (`sources.openbsd.revision`).
See `docs/DEPENDENCIES.md` for the common configuration and import workflow.

We do not commit OpenBSD's files, only our changes to them, in `patches/openbsd/`: `FILES` (the files we use, paths as in openbsd/src) and one `<path>.patch` per
changed file (e.g. `sys/dev/ic/qwz.c.patch`), like OpenBSD ports' `make update-patches`.
`scripts/patches.sh apply` builds the working copy `openbsd/` (gitignored): pristine files plus
the patches, uncommitted. Inside it is a git repo whose only commit (tag `upstream`) is pristine
OpenBSD, so `git -C openbsd diff` shows all our changes. Never commit in `openbsd/`.
- Change the driver: edit `openbsd/` directly, then `scripts/patches.sh export`, then commit
  `patches/` here. Patches carry no message: explain each change in its `QWZMAC` comment.
- After `git pull`: `scripts/patches.sh apply`. `build.sh` runs `patches.sh check` and stops
  if `openbsd/` and `patches/` differ or the working copy contains undeclared files. `apply` refuses to drop unexported edits unless `--force`.
- New OpenBSD: `scripts/patches.sh update <commit>` commits the new upstream files and merges our
  changes into them (3-way). On a conflict, fix the `<<<<<<<` blocks, then `export`. Review
  `git -C openbsd diff upstream^ upstream` (what OpenBSD changed), build, test on hardware.
- Using another OpenBSD file: add it to `FILES`, then `apply`. Only `.c` files declared in `FILES` are built.

## Install / test loop
0. The Wi-Fi network to join is in `config/wifi.plist` (gitignored, never commit it; template
   `config/wifi.plist.example`). `build.sh` copies it into the kext's Info.plist (key
   `QwzNetwork`), because nothing else is readable that early in boot. Exactly that SSID is
   joined (no auto-join). Without the file the driver only scans until an app picks a network:
   HeliPort (itlwm's menu app; the kext poses as IOClass `itlwm` for it) or `build/qwzctl`
   (`kext/heliport.h` is the protocol). Never log the password.
1. `scripts/build.sh`
2. Copy `build/QwzWifi.kext` into the OpenCore `Kexts/` folder (your own path). Before replacing the kext, check that
   nothing in it on the stick changed since the last copy (the user may have edited its Info.plist
   there); carry such edits back into `config/wifi.plist` first.
3. `python3 scripts/oc-enable.py on` (OC path from `local/oc-dir` or the 2nd argument; or `probe` for a read-only stage-0 run with `-qwzprobe`).
   It backs up the config first, then adds the kext entry and the boot-args. `qwzdebug=N` sets
   the qwz debug mask (default `0`; `0x7` = MISC|MHI|QMI is slow enough to trip qwz's 1 s QMI
   timeouts, so use it only for a specific question).
4. Reboot. Then run `scripts/qwzlog.sh` (saves `~/Desktop/qwz.log` and prints a summary). The kext keeps its own
   log and publishes it as the `QwzLog` property of the PCI device, because its early-boot IOLog
   output does not reach `log show` there. With `-v` the IOLog
   lines still show on screen during boot.
5. To disable: `python3 scripts/oc-enable.py off`, or restore a backup config.

## Safety rules (hard requirements)
- **Never write to the chip's persistent storage.** There must be no OTP/eFuse/flash writes, and no
  `WMI_TRANSFER_DATA_TO_FLASH*` or `MHI_EE_FP` (flash programmer) paths. qwz was audited and has
  none. Keep it that way.
- Firmware files are used **unmodified**, from linux-firmware `ath12k/WCN7850/hw2.0`, pinned to tag
  `20260309` (amss c5-00302), the release OpenBSD's qwz-firmware package uses. Newer c7-00108
  crashes with qwz. Don't bump it without testing.
- Do not change regulatory or TX power data (board-2.bin, regdb).
- All DMA buffers must be below 4 GB, matching qwz's `0..0xffffffff` range, and must be physically
  contiguous where qwz asks for one segment. On AMD hackintoshes the IOMMU is usually off, so a
  bad DMA address corrupts host RAM. Validate every physical address handed to the device.
- The driver starts only with the boot-arg `-qwzon`. Keep the probe gate.
- Back up `config.plist` before every change. `oc-enable.py` does this automatically.
- Do not touch other OpenCore settings.

## Code layout
```
kext/            IOKit glue (QwzWifi IOEthernetController: frames are copied to/from qwz via
                 qwzmac/attach.h), QwzUserClient (HeliPort protocol, heliport.h), os_bridge.cpp
                 (qwzmac/os.h on IOKit), firmware.S (.incbin of firmware/), kmod_info
airport/         QwzAirport.kext: IO80211Controller + Wi-Fi interface + BSD interface; talks to
                 QwzWifi only through kext/airport_api.h; abi/ = vendored IO80211 headers;
                 CREDITS says what comes from laobamac/itlwm
tools/           qwzctl: command-line client of the HeliPort protocol
adapter/         Our qwz/net80211 integration: device.c (lifetime, frames, sleep/wake),
                 control.c (join, radio, scan requests), status.c (read-only snapshots).
                 internal.h is private; clients still use qwzmac/attach.h.
compat/          OpenBSD kernel API shim with REAL OpenBSD signatures (bus_space, bus_dma,
                 pci, intr/MSI, timeout, task, rwlock, refcnt, tsleep, mbuf, loadfirmware,
                 device types). compat/include/ holds the OpenBSD-named headers we wrote.
                 if_ether.c/if.c are our minimal stand-ins for the OpenBSD network stack
                 (no routing, no ARP, no bpf). uipc_mbuf.c is our mbuf adaptation; review
                 upstream changes manually, not through patches.sh update.
patches/openbsd/ FILES and our per-file patches to OpenBSD (the only OpenBSD code in git)
openbsd/         working copy, gitignored (scripts/patches.sh): sys/dev/ic/qwz*,
                 sys/dev/pci/if_qwz_pci.c, sys/net80211 (not itl80211), the sys/crypto files
                 net80211 needs, and a few upstream net/, netinet/, kern/ files
tests/           user-space tests of compat/
firmware/        fetched blobs (gitignored), embedded into the kext at build time
```

## Conventions
- Keep qwz diffs minimal and greppable (`QWZMAC`), each with a comment saying why. Prefer shims
  over editing the driver.
- OpenBSD code stays C; macOS code is C++ in `kext/`. Only plain C types cross `qwzmac/os.h`.
- `panic()` does not return (code after it may rely on that): it logs and parks the calling
  thread forever, so the driver dies but the machine keeps running. `KASSERT` and compat's own
  checks (`QWZMAC_WARN`) only log.
- Concurrency: one global recursive lock stands in for every spl level. Interrupt actions,
  timeouts and taskqs run in threads; tsleep releases the lock while sleeping.
- DMA: every device-visible address comes from `bus_dmamem_alloc*` (contiguous, < 4 GB,
  re-checked). mbuf DMA always goes through per-map bounce buffers (see machine/bus.h).
- Public KPIs by default. **Approved exception (2026-10-19):** both QwzWifi and
  QwzAirport use native `thread_call_isactive` for OpenCore injection; it is declared in the
  SDK but absent from every KPI export set on 25G241. The maintainer explicitly asked to remove
  the wrapper from both kexts. Validate with `python3 scripts/kpi-check.py --opencore
  build/QwzWifi.kext build/QwzAirport.kext`: only this one symbol in these two bundles may be
  unresolved through public exports. Ordinary `kpi-check.py` must still flag it, and manual
  `kmutil load` is unsupported for these builds. Do not add com.apple.kpi.private.
  One planned exception (native Wi-Fi): a separate
  `QwzAirport.kext` may link `com.apple.iokit.IO80211Family`, `com.apple.iokit.IOSkywalkFamily`
  and `com.apple.driver.corecapture` (undocumented C++ ABI, reverse-engineered headers). It holds
  no OpenBSD code and no hardware access, starts only on macOS builds whose ABI was checked
  (allowlist), and QwzWifi falls back to Ethernet without it. `QwzWifi.kext` itself stays on
  public KPIs.
- Logs go through `IOLog("QwzWifi: ...")`, and the first version is verbose.
- License: kext glue GPL-2.0. qwz files keep their ISC headers.

## Native Wi-Fi front: rules
What was tested on hardware at each stage is recorded in the commit messages.
- Provider chain QwzWifi -> QwzAirportNub -> QwzAirport. The nub returns from start at once and
  publishes after `kern.osversion` passes the exact allowlist; keep both personality allowlists
  (`airport/Info.plist`) identical. Do not restore `IOResourceMatch=WindowServer`: on 25G241
  checkResource waits without timeout before start.
- If the front will not run (build not allowlisted, kext missing, start failed), QwzWifi falls
  back to Ethernet for HeliPort by itself (`QWZ_AIRPORT_UNAVAILABLE`). Otherwise never detach
  native interfaces, add other automatic fallbacks, rewrite packets or change the on-air MAC to
  fix ifnet identity.
- Runtime MAC changes wait for the matching vdev DELETE ACK before recreating it and fault on
  uncertainty until reboot. Firmware, regulatory data, DMA rings/maps and addressing stay as is.
- Never hold the WCL gate across firmware or slow net80211 calls; RX under the spl lock never
  takes the gate. LqmUpdate/beacon data only from real reception, no synthetic heartbeat.
- No direct Keychain writes, no RSN/SAE IE rewriting; never read or log password material.
- Distinguish local checks from hardware tests; do not call a stage done without the hardware
  result. Commit completed changes regularly. Installing to the EFI needs the user's approval.
