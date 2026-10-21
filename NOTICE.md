# Third-party code and licenses

QwzWifi as a whole is distributed under the **GNU General Public License, version 2 only**
(`LICENSE`). Files written for this project carry `SPDX-License-Identifier: GPL-2.0-only` or, where
they have no header, fall under the same license. The parts below come from other projects and keep
their own licenses; their notices must stay with them, in source and in binary distributions.

## In QwzWifi.kext

### OpenBSD (ISC, BSD-3-Clause-Clear, BSD-2/3-Clause, MIT, public domain; three BSD-4-Clause files)
The Wi-Fi driver is OpenBSD's **qwz(4)** (Stefan Sperling; derived from Linux ath12k), with OpenBSD's
**net80211** stack, the crypto files net80211 needs and a few network/kernel files. They are not
stored in this repository: `scripts/patches.sh apply` takes them from the OpenBSD commit in
`config/sources.json` (66 files listed in `patches/openbsd/FILES`) and applies our changes
(`patches/openbsd/*.patch`). Every file keeps its copyright header. At the pinned revision:

| License | Files |
|---|---|
| ISC **and** BSD-3-Clause-Clear (The Linux Foundation, Qualcomm Innovation Center; from ath12k) | `dev/ic/qwz.c`, `qwzreg.h`, `qwzvar.h`, `dev/pci/if_qwz_pci.c` |
| ISC | most of `net80211/`, most crypto |
| BSD-2-Clause / BSD-3-Clause | parts of `net80211/`, `net/`, `netinet/`, `sys/queue.h`, `sys/tree.h`, some crypto |
| MIT | `crypto/aes.c`, `crypto/aes.h` (Thomas Pornin) |
| Public domain | `crypto/md5.c`, `crypto/sha1.c` and others |
| **BSD-4-Clause** (advertising clause) | `net/if_media.c`, `net/if_media.h` (Jonathan Stone, Jason R. Thorpe), `dev/pci/pcireg.h` (Charles Hannum) |

BSD-3-Clause-Clear grants no patent rights; it is GPL-compatible.

Our changes to OpenBSD files (`patches/openbsd/`, marked `QWZMAC`) are offered under the license of
the file they change (ISC for qwz), so that they can go back to OpenBSD.

`compat/uipc_mbuf.c` is adapted from OpenBSD `sys/kern/uipc_mbuf.c` and keeps its BSD-3-Clause header
(University of California).

**Note on BSD-4-Clause:** the Free Software Foundation considers the advertising clause incompatible
with the GPL. Binary distributions that include these three files must show the acknowledgements
below; replacing `if_media.c` with a small implementation of our own would remove the conflict.
- This product includes software developed by Jonathan Stone and Jason R. Thorpe for the NetBSD Project.
- This product includes software developed by Charles Hannum.

### HeliPort protocol (BSD-3-Clause)
`kext/heliport.h` reproduces the user-client protocol (structure layouts and request ids) of
HeliPort's ClientKit, https://github.com/OpenIntelWireless/HeliPort,
Copyright (c) 2020, lhy; Copyright (c) 2020, ZhongXianYao; Copyright (c) 2020, OpenIntelWireless.
BSD-3-Clause.

### Qualcomm firmware (proprietary, redistributable)
`QwzWifi.kext` embeds the WCN7850 firmware files `amss.bin`, `m3.bin` and `board-2.bin`, unmodified,
from linux-firmware (`ath12k/WCN7850/hw2.0`, tag `20260309`). They are not GPL and not part of this
repository (`scripts/fetch-sources.sh` downloads them and checks their SHA-256). Their license is
Qualcomm's `Notice.txt`, downloaded next to them into `firmware/`; anyone who distributes a built
`QwzWifi.kext` must include that file.

## In QwzAirport.kext

### laobamac/itlwm and AirportItlwm (GPL-2.0)
The native Wi-Fi front is adapted from the native front of **laobamac/itlwm**
(https://github.com/laobamac/itlwm, commit `738d4c6`), itself based on **AirportItlwm** by zxystd and
the OpenIntelWireless contributors (https://github.com/OpenIntelWireless/itlwm). Both GPL-2.0.
`airport/CREDITS` lists which of our files come from which of theirs. Some request handling was
compared with **mnnobre/AirportMtkWlan** (GPL-2.0, commit `b880d9c`). `airport/WCL.hpp` and
`airport/abi/` are copies from laobamac/itlwm (`airport/abi/UPSTREAM`, `airport/abi/LICENSE`).

### Apple headers in `airport/abi/` (APSL-2.0)
`apple80211_ioctl.h`, `apple80211_var.h` and `apple_private_spi.h` are Apple header files under the
Apple Public Source License 2.0 (header kept), and `IO80211WorkLoop.h` carries an Apple copyright.
They describe the interfaces of Apple's Wi-Fi family (structure layouts, constants) and are used only
to talk to it. APSL-2.0 is not GPL-compatible in the FSF's view; these files are kept as interface
descriptions under their own license, not relicensed.

## Build and development only (not in the kexts)
- Apple's Kernel.framework headers from the Command Line Tools SDK (APSL-2.0), used to compile.
- acidanthera/MacKernelSDK (APSL-2.0 headers), only for compile checks on Linux.
- `scripts/kc-abi.py` and `scripts/kclib.py` read the user's own kernel collections; they contain no
  Apple code.
