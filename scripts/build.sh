#!/bin/zsh
# Builds both kexts and command-line tools with Command Line Tools only.
#
# Two worlds are compiled separately:
#  - OpenBSD side (openbsd/sys/**/*.c, compat/*.c, adapter/*.c): C, freestanding, only
#    compat/include, openbsd/sys and clang's own headers, never Apple headers;
#  - macOS side (kext/*.cpp): C++ against Kernel.framework.
set -e
cd "${0:A:h}/.."
SDK=$(xcrun --sdk macosx --show-sdk-path 2>/dev/null) || { echo "No Command Line Tools: run xcode-select --install"; exit 1; }
KH="$SDK/System/Library/Frameworks/Kernel.framework/Headers"
[[ -d "$KH" ]] || { echo "Kernel.framework headers not found in $SDK"; exit 1; }
python3 scripts/check-firmware.py
# Build only the declared OpenBSD files, with every local edit exported.
[[ -d openbsd/.git ]] || { echo "openbsd/ missing: run scripts/patches.sh apply"; exit 1; }
scripts/patches.sh check
RD=$(clang -print-resource-dir)
OUT=build; mkdir -p $OUT/obj
ARCH=(-arch x86_64 -mmacosx-version-min=11.0)
# Info.plist owns each bundle version, including log strings and kmod metadata.
WIFI_VERSION=$(plutil -extract CFBundleVersion raw -o - kext/Info.plist)
AIRPORT_VERSION=$(plutil -extract CFBundleVersion raw -o - airport/Info.plist)
COMMON=($ARCH -mkernel -fno-builtin -fno-common -O2 -Wall
        -isysroot "$SDK" -I"$KH" -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT
        "-DQWZ_VERSION=\"$WIFI_VERSION\"" "-DQWZ_AIRPORT_VERSION=\"$AIRPORT_VERSION\"")
# IOPCIDevice/IONetworking/mbuf KPIs are marked deprecated in favour of DriverKit; still public.
CXXFLAGS=(-std=gnu++17 -fapple-kext -fno-rtti -fno-exceptions -Wno-deprecated-declarations)
BSDFLAGS=($ARCH -x c -std=gnu11 -mkernel -ffreestanding -fno-builtin -fno-common -O2
          -nostdinc -isystem "$RD/include" -D_KERNEL -DQWZMAC -DQWZ_DEBUG -I compat/include -I openbsd/sys
          -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable
          -Wno-sign-conversion -Wno-shorten-64-to-32 -Wno-implicit-int-conversion -Wno-pointer-sign)
OBJS=()
BSD_MANIFEST=$(python3 scripts/sources.py get sources.openbsd.manifest)
BSD_SOURCES=(${(f)"$(grep '\.c$' $BSD_MANIFEST)"})
for src in openbsd/${^BSD_SOURCES} compat/*.c adapter/*.c; do
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

# QwzAirport.kext: the native Wi-Fi front (airport/).
# A separate kext so that QwzWifi.kext keeps to public KPIs: only this one links
# IO80211Family, IOSkywalkFamily and corecapture, through airport/abi/ (checked against
# the running build by scripts/kc-abi.py --check). No OpenBSD code in it.
python3 scripts/airport-stubs.py
AIRFLAGS=(-Iairport/abi -DQWZAIRPORT -D__PRIVATE_SPI__ -DAIRPORT_WCL
          -D__IO80211_TARGET=__MAC_26_0 -DIO80211FAMILY_V2 -Wno-inconsistent-missing-override
          -Wno-overloaded-virtual -Wno-unused-private-field -Wno-invalid-offsetof)
AOBJS=()
for src in airport/*.cpp; do
  o=$OUT/obj/air_${src:t:r}.o; clang++ "${COMMON[@]}" "${CXXFLAGS[@]}" "${AIRFLAGS[@]}" -c $src -o $o; AOBJS+=$o
done
clang "${COMMON[@]}" -c airport/kmod_info.c -o $OUT/obj/air_kmod_info.o; AOBJS+=$OUT/obj/air_kmod_info.o
clang++ $ARCH -isysroot "$SDK" -nostdlib -Xlinker -kext \
        "${AOBJS[@]}" -L"$SDK/usr/lib" -lkmodc++ -lkmod -o $OUT/QwzAirport.bin
rm -rf $OUT/QwzAirport.kext; mkdir -p $OUT/QwzAirport.kext/Contents/MacOS
cp airport/Info.plist $OUT/QwzAirport.kext/Contents/Info.plist
cp $OUT/QwzAirport.bin $OUT/QwzAirport.kext/Contents/MacOS/QwzAirport
plutil -lint $OUT/QwzAirport.kext/Contents/Info.plist
# Keep an ad-hoc signature on the native bundle. Deployment uses OpenCore.
codesign -s - -f $OUT/QwzAirport.kext 2>/dev/null || echo "WARNING: QwzAirport.kext not signed"
echo "OK: $(pwd)/$OUT/QwzWifi.kext, $(pwd)/$OUT/QwzAirport.kext, $(pwd)/$OUT/qwzctl"
