#!/bin/zsh
# Copies the IO80211/Skywalk ABI headers QwzAirport needs for Tahoe from the pinned
# laobamac/itlwm clone (third_party/laobamac, scripts/fetch-sources.sh) into airport/abi/,
# and records the commit in airport/abi/UPSTREAM. The file list is what
# <Airport/Apple80211.h> pulls in with the Tahoe defines (clang -M), so nothing unused is
# copied. Our changes (each marked QWZAIRPORT with the reason) are airport/abi/qwzairport.patch,
# applied after the copy; to change a header, edit it, then regenerate the patch with
#   (cd airport/abi && diff -ru ../../third_party/laobamac/include/Airport Airport)
# The headers are only trusted after scripts/kc-abi.py --check on the running build.
set -e
cd "${0:A:h}/.."
SRC=third_party/laobamac
[[ -d $SRC/include/Airport ]] || { echo "$SRC missing: run scripts/fetch-sources.sh"; exit 1; }
SDK=$(xcrun --sdk macosx --show-sdk-path)
KH="$SDK/System/Library/Frameworks/Kernel.framework/Headers"
TMP=$(mktemp -d); trap 'rm -rf $TMP' EXIT
print '#include <Airport/Apple80211.h>\n#include <Airport/macOS26/FaultReporter.h>' > $TMP/t.cpp
deps=$(clang++ -x c++ -arch x86_64 -mkernel -std=gnu++17 -fapple-kext -fno-rtti -fno-exceptions \
  -isysroot "$SDK" -I"$KH" -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE \
  -D__PRIVATE_SPI__ -DAIRPORT_WCL -D__IO80211_TARGET=__MAC_26_0 -DIO80211FAMILY_V2 \
  -I$SRC/include -M $TMP/t.cpp | tr ' \\' '\n\n' | grep "^$SRC/include/Airport/")
rm -rf airport/abi/Airport; mkdir -p airport/abi
for f in ${(f)deps}; do
  r=$(python3 -c 'import os,sys; print(os.path.relpath(os.path.normpath(sys.argv[1]), sys.argv[2]))' $f $SRC/include)
  mkdir -p airport/abi/${r:h}; cp $SRC/include/$r airport/abi/$r
done
cp $SRC/LICENSE airport/abi/LICENSE
[[ -f airport/abi/qwzairport.patch ]] && patch -s -p1 -d airport/abi < airport/abi/qwzairport.patch
{
  echo "laobamac/itlwm $(git -C $SRC rev-parse HEAD) include/Airport (GPL-2.0, LICENSE)"
  echo "copied by scripts/airport-vendor.sh on $(date +%F), then qwzairport.patch applied:"
  (cd airport/abi && find Airport -type f | sort)
} > airport/abi/UPSTREAM
echo "airport/abi: $(find airport/abi/Airport -type f | wc -l | tr -d ' ') headers"
