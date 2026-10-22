#!/bin/zsh
# Builds the kexts from the committed tree and packs a release archive into dist/:
# the archive, SHA256SUMS.txt and RELEASE_NOTES.md. It tags and pushes nothing.
#
# Usage: scripts/release.sh [notes-file]
#   notes-file: what changed (Markdown), put at the top of RELEASE_NOTES.md
# Needs: a clean git tree, patches applied (scripts/patches.sh apply), firmware fetched.
set -e
cd "${0:A:h}/.."
NOTES_FILE=${1:+${1:A}}

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

{
  [[ -n "$NOTES_FILE" ]] && { cat "$NOTES_FILE"; echo; }
  echo 'Experimental kernel driver: back up your EFI first. See the README for install steps. SHA-256 of the archive is in `SHA256SUMS.txt`.'
} > $DIST/RELEASE_NOTES.md

echo
echo "Release files in $DIST/:"; ls -1 $DIST
cat <<NEXT

Next (nothing below has been done):
  git tag -a $TAG -m "QwzWifi $WIFI, QwzAirport $AIRPORT" && git push origin $TAG
  then create a pre-release on GitHub from tag $TAG, with $DIST/RELEASE_NOTES.md as the text and
  the .zip and SHA256SUMS.txt as assets (or: gh release create $TAG --prerelease \\
  --title "QwzWifi $WIFI + QwzAirport $AIRPORT" --notes-file $DIST/RELEASE_NOTES.md $DIST/*.zip $DIST/SHA256SUMS.txt)
NEXT
