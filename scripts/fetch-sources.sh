#!/bin/zsh
# Fetches pinned reference sources into third_party/ (not committed).
set -e
cd "${0:A:h}/.."; mkdir -p third_party firmware; cd third_party
clone() { # url dir commit [sparse paths...]
  local url=$1 dir=$2 rev=$3; shift 3
  [[ -d $dir ]] && { echo "skip $dir"; return; }
  if (( $# )); then
    git clone --filter=blob:none --no-checkout "$url" $dir
    git -C $dir sparse-checkout set --no-cone "$@"
  else
    git clone --filter=blob:none --no-checkout "$url" $dir
  fi
  git -C $dir checkout -q $rev
}
# OpenBSD qwz(4) — the driver we port (ISC), at patches/openbsd/BASE. The build
# uses the working copy ../openbsd/ (see scripts/patches.sh), not this clone;
# the sparse checkout is what we read for reference.
clone https://github.com/openbsd/src.git openbsd $(<../patches/openbsd/BASE) \
  '/sys/dev/ic/qwz*' '/sys/dev/pci/if_qwz*' '/sys/dev/ic/qwx*' '/sys/dev/pci/if_qwx*' '/sys/net80211/' \
  /share/man/man4/qwz.4 /sys/dev/pci/pcireg.h /sys/dev/pci/pcidevs.h \
  '/sys/crypto/' /sys/net/if.h /sys/net/if_var.h /sys/net/if_media.h /sys/net/if_media.c \
  /sys/net/if_dl.h /sys/net/if_llc.h /sys/net/if_ethersubr.c /sys/netinet/ip.h /sys/netinet/ip6.h \
  /sys/sys/syslog.h /sys/sys/sysctl.h /sys/kern/subr_tree.c /sys/net/if.c /sys/dev/pci/pci.c \
  /sys/kern/subr_autoconf.c /sys/arch/amd64/amd64/machdep.c /sys/arch/amd64/pci/pci_machdep.c \
  /sys/dev/acpi/acpi.c /sys/dev/acpi/acpi_x86.c
# itlwm — macOS wrapper + itl80211 (old OpenBSD net80211 port) (GPL-2.0)
clone https://github.com/OpenIntelWireless/itlwm.git itlwm 53c51c2cdd6e4b69beb91f310d74c53422b0f8bd
# HeliPort — itlwm's Wi-Fi menu app; its ClientKit is the protocol kext/heliport.h speaks (BSD-3)
clone https://github.com/OpenIntelWireless/HeliPort.git HeliPort 898bcd8366844fdcfe047d7ebdf90631e4642b66
# Kernel headers for building on any machine (acidanthera)
clone https://github.com/acidanthera/MacKernelSDK.git MacKernelSDK 3f750085caa17ec3a7880f11c11bf4f48cd6a164
# Reference only: native-menu Realtek port (IO80211 front, Tahoe legacy stack)
clone https://github.com/FranciumOS-org/Airport_RTW89.git Airport_RTW89 e7f449cd5314671c64a43ef83d88a87fc980f595
# Reference only: Linux ath12k (GPL) — read, do not copy into ISC files
clone https://github.com/torvalds/linux.git linux 70dd4ee72eb2d5793c62e71c14e3aba78e5a4e56 \
  /drivers/net/wireless/ath/ath12k/ /drivers/bus/mhi/
cd ../firmware
# Pinned to linux-firmware tag 20260309: the release OpenBSD's qwz-firmware package
# ships (amss WLAN.HMT.1.1.c5-00302), i.e. what qwz is tested with. The newer
# c7-00108 (2026-03-17) asserts in cmnos_thread_patch.c right after QMI memory setup.
FW=https://gitlab.com/kernel-firmware/linux-firmware/-/raw/20260309/ath12k/WCN7850/hw2.0
for f in amss.bin m3.bin board-2.bin Notice.txt; do
  [[ -f $f ]] || curl -fL -o $f "$FW/$f"
done
shasum -a 256 *
cd ..
# Our working copy of the OpenBSD sources: BASE + patches/openbsd/*.patch.
[[ -d openbsd ]] && echo "skip openbsd/ (scripts/patches.sh check)" || scripts/patches.sh apply
echo "done"
