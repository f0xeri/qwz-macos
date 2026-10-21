#!/bin/zsh
# Builds the kexts from the committed tree and packs a release archive into dist/:
# the archive, SHA256SUMS.txt and RELEASE_NOTES.md. It tags and pushes nothing.
#
# Usage: scripts/release.sh
# Needs: a clean git tree, patches applied (scripts/patches.sh apply), firmware fetched.
set -e
cd "${0:A:h}/.."

[[ -z "$(git status --porcelain)" ]] || { echo "working tree is not clean: commit or stash first"; exit 1; }
# build.sh would embed this network and its password into QwzWifi.kext.
[[ ! -f config/wifi.plist ]] || { echo "config/wifi.plist exists: move it away, a release must not carry a network"; exit 1; }

scripts/build.sh

WIFI=$(plutil -extract CFBundleVersion raw -o - kext/Info.plist)
AIRPORT=$(plutil -extract CFBundleVersion raw -o - airport/Info.plist)
if plutil -extract QwzNetwork raw -o - build/QwzWifi.kext/Contents/Info.plist >/dev/null 2>&1; then
  echo "QwzNetwork found in the built Info.plist: refusing to pack"; exit 1
fi

TAG=v$WIFI
NAME=QwzWifi-${WIFI}_QwzAirport-${AIRPORT}
DIST=dist
STAGE=$DIST/stage/$NAME
rm -rf $DIST; mkdir -p $STAGE

cp -R build/QwzWifi.kext build/QwzAirport.kext $STAGE/
cp build/qwzctl LICENSE NOTICE.md README.md $STAGE/
cp firmware/Notice.txt $STAGE/FIRMWARE-NOTICE.txt   # Qualcomm notice for the firmware inside QwzWifi.kext
cp airport/CREDITS $STAGE/AIRPORT-CREDITS.txt
find $STAGE -name .DS_Store -delete

(cd $DIST/stage && ditto -c -k --keepParent --norsrc $NAME ../$NAME-macOS-x86_64.zip)
rm -rf $DIST/stage
(cd $DIST && shasum -a 256 *.zip > SHA256SUMS.txt)

cat > $DIST/RELEASE_NOTES.md <<NOTES
**QwzWifi $WIFI** (driver) and **QwzAirport $AIRPORT** (native Airport Wi-Fi front).

Wi-Fi for the Qualcomm FastConnect 7800 / WCN7850 (PCI \`17cb:1107\`) on an x86_64 Hackintosh, based on
OpenBSD's qwz(4). See the README for features, limitations and the OpenCore install steps.

**Experimental.** It runs in the kernel: a bug can panic the Mac or, through DMA, corrupt memory.
Back up your EFI first.

## What is in the archive
- \`QwzWifi.kext\`: the driver. On its own it gives an Ethernet interface controlled by HeliPort or
  \`qwzctl\`; this mode is expected to work on earlier macOS versions too
  (HeliPort supports them; only Tahoe has been tried so far).
- \`QwzAirport.kext\`: the native Wi-Fi front (Wi-Fi menu, System Settings). Expected to work on
  other Tahoe builds too, but verified only on 25G241; it starts only on builds in its allowlist
  (see README). Skip it for HeliPort mode.
- \`qwzctl\`: command-line client.
- \`LICENSE\` (GPL-2.0), \`NOTICE.md\`, \`AIRPORT-CREDITS.txt\`, and \`FIRMWARE-NOTICE.txt\` for the
  Qualcomm firmware embedded in \`QwzWifi.kext\`. Keep these files with any copy of the kexts.

## Install
Copy the kext(s) into \`EFI/OC/Kexts/\`, add them to \`config.plist\` and the boot-arg \`-qwzon\` (the
driver does nothing without it); \`scripts/oc-enable.py\` in the source does this and backs up the
config. The kexts have no developer-certificate signature (QwzAirport carries only an ad-hoc one) and
are meant for OpenCore injection. The build contains no Wi-Fi network or password: join with HeliPort
or from the Wi-Fi menu.

Source for this build: tag \`$TAG\`. Build it yourself with \`scripts/fetch-sources.sh\`,
\`scripts/patches.sh apply\`, \`scripts/build.sh\`. SHA-256 of the archive is in \`SHA256SUMS.txt\`.
NOTES

echo
echo "Release files in $DIST/:"; ls -1 $DIST
cat <<NEXT

Next (nothing below has been done):
  git tag -a $TAG -m "QwzWifi $WIFI, QwzAirport $AIRPORT" && git push origin $TAG
  then create a pre-release on GitHub from tag $TAG, with $DIST/RELEASE_NOTES.md as the text and
  the .zip and SHA256SUMS.txt as assets (or: gh release create $TAG --prerelease \\
  --title "QwzWifi $WIFI + QwzAirport $AIRPORT" --notes-file $DIST/RELEASE_NOTES.md $DIST/*.zip $DIST/SHA256SUMS.txt)
NEXT
