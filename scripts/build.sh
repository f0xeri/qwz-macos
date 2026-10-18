#!/bin/zsh
# Builds build/QwzWifi.kext with Command Line Tools only (no Xcode).
# Proven pattern: same flags built XHCIProbe.kext on this machine.
#
# Two worlds are compiled separately:
#  - OpenBSD side (openbsd/sys/**/*.c, compat/*.c): C, freestanding, only
#    compat/include, openbsd/sys and clang's own headers, never Apple headers;
#  - macOS side (kext/*.cpp): C++ against Kernel.framework.
set -e
cd "${0:A:h}/.."
SDK=$(xcrun --sdk macosx --show-sdk-path 2>/dev/null) || { echo "No Command Line Tools: run xcode-select --install"; exit 1; }
KH="$SDK/System/Library/Frameworks/Kernel.framework/Headers"
[[ -d "$KH" ]] || { echo "Kernel.framework headers not found in $SDK"; exit 1; }
for f in amss.bin m3.bin board-2.bin; do
  [[ -f firmware/$f ]] || { echo "firmware/$f missing: run scripts/fetch-sources.sh"; exit 1; }
done
# openbsd/ is OpenBSD + patches/openbsd (scripts/patches.sh); warn if it and
# the patches have drifted apart, so a build never silently uses stale code.
[[ -d openbsd/.git ]] || { echo "openbsd/ missing: run scripts/patches.sh apply"; exit 1; }
scripts/patches.sh check || echo "WARNING: building openbsd/ as it is, which differs from patches/openbsd"
RD=$(clang -print-resource-dir)
OUT=build; mkdir -p $OUT/obj
ARCH=(-arch x86_64 -mmacosx-version-min=11.0)
COMMON=($ARCH -mkernel -fno-builtin -fno-common -O2 -Wall
        -isysroot "$SDK" -I"$KH" -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT)
# IOPCIDevice/IONetworking/mbuf KPIs are marked deprecated in favour of DriverKit; still public.
CXXFLAGS=(-std=gnu++17 -fapple-kext -fno-rtti -fno-exceptions -Wno-deprecated-declarations)
BSDFLAGS=($ARCH -x c -std=gnu11 -mkernel -ffreestanding -fno-builtin -fno-common -O2
          -nostdinc -isystem "$RD/include" -D_KERNEL -DQWZMAC -DQWZ_DEBUG -I compat/include -I openbsd/sys
          -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable
          -Wno-sign-conversion -Wno-shorten-64-to-32 -Wno-implicit-int-conversion -Wno-pointer-sign)
OBJS=()
for src in openbsd/sys/**/*.c compat/*.c; do
  o=$OUT/obj/bsd_${src:h:t}_${src:t:r}.o; clang "${BSDFLAGS[@]}" -c $src -o $o; OBJS+=$o
done
for src in kext/*.cpp; do
  o=$OUT/obj/${src:t:r}.o; clang++ "${COMMON[@]}" "${CXXFLAGS[@]}" -c $src -o $o; OBJS+=$o
done
clang $ARCH -c kext/firmware.S -o $OUT/obj/firmware.o; OBJS+=$OUT/obj/firmware.o
clang "${COMMON[@]}" -c kext/kmod_info.c -o $OUT/obj/kmod_info.o; OBJS+=$OUT/obj/kmod_info.o
clang++ $ARCH -isysroot "$SDK" -nostdlib -Xlinker -kext \
        "${OBJS[@]}" -L"$SDK/usr/lib" -lkmodc++ -lkmod -o $OUT/QwzWifi.bin
rm -rf $OUT/QwzWifi.kext; mkdir -p $OUT/QwzWifi.kext/Contents/MacOS
cp kext/Info.plist $OUT/QwzWifi.kext/Contents/Info.plist
# The network to join (config/wifi.plist, gitignored; see wifi.plist.example).
if [[ -f config/wifi.plist ]]; then
  plutil -insert IOKitPersonalities.QwzWifi.QwzNetwork -xml \
    "$(plutil -extract QwzNetwork xml1 -o - config/wifi.plist)" $OUT/QwzWifi.kext/Contents/Info.plist
  echo "network: $(plutil -extract QwzNetwork.SSID raw -o - config/wifi.plist) (from config/wifi.plist)"
else
  echo "network: none (no config/wifi.plist), the driver will only scan"
fi
cp $OUT/QwzWifi.bin $OUT/QwzWifi.kext/Contents/MacOS/QwzWifi
plutil -lint $OUT/QwzWifi.kext/Contents/Info.plist
# Every undefined symbol must be a kernel export; anything from the OpenBSD
# side (no leading "__Z", not a known libkern/Mach name) is a missing shim.
nm -u $OUT/QwzWifi.kext/Contents/MacOS/QwzWifi > $OUT/undefined.txt
echo "undefined symbols: $(wc -l < $OUT/undefined.txt) (list in $OUT/undefined.txt)"
grep -v '^__Z' $OUT/undefined.txt | tr '\n' ' '; echo
ls -la $OUT/QwzWifi.kext/Contents/MacOS/QwzWifi
# qwzctl: command-line client for the same interface HeliPort uses.
clang -Wall -O2 $ARCH -framework IOKit -framework CoreFoundation tools/qwzctl.c -o $OUT/qwzctl
echo "OK: $(pwd)/$OUT/QwzWifi.kext, $(pwd)/$OUT/qwzctl"
