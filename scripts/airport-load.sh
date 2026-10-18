#!/bin/zsh
# Load, unload or inspect QwzAirport.kext by hand (stage C2 test loop, PLAN.md).
#
#   scripts/airport-load.sh check    read-only: ABI check of this macOS build, QwzWifi running?
#   scripts/airport-load.sh load     sudo: copy build/QwzAirport.kext to a root-owned place, load it
#   scripts/airport-load.sh unload   sudo: unload it
#   scripts/airport-load.sh status   read-only: is it loaded, its interfaces, its log lines
#
# The boot stick is not touched: QwzWifi stays injected by OpenCore, QwzAirport only lives
# until the next reboot. A panic costs one reboot back into today's Ethernet setup.
# kmutil wants a kext owned by root:wheel, hence the copy into /private/var/tmp/qwzairport.
set -e
cd "${0:A:h}/.."
DST=/private/var/tmp/qwzairport
ID=com.local.QwzAirport

case "$1" in
check)
  echo "== macOS $(sw_vers -productVersion) ($(sysctl -n kern.osversion))"
  python3 scripts/kc-abi.py --check
  python3 scripts/kc-abi.py --returns
  build=$(sysctl -n kern.osversion)
  plutil -extract IOKitPersonalities.QwzAirport.QwzAirportBuilds xml1 -o - airport/Info.plist |
    grep -q "<string>$build</string>" && echo "$build is in QwzAirportBuilds" ||
    { echo "$build is NOT in QwzAirportBuilds (airport/Info.plist): QwzAirport would refuse to start"; exit 1; }
  kextstat -l -b com.local.QwzWifi | grep -q QwzWifi ||
    { echo "QwzWifi is not loaded: boot with it first"; exit 1; }
  ioreg -r -c QwzWifi -d 1 | grep -q QwzWifi && echo "QwzWifi is running" ||
    { echo "no QwzWifi service (firmware not up?)"; exit 1; }
  ;;
load)
  [[ -d build/QwzAirport.kext ]] || { echo "build/QwzAirport.kext missing: scripts/build.sh"; exit 1; }
  sudo rm -rf $DST && sudo mkdir -p $DST
  sudo cp -R build/QwzAirport.kext $DST/
  sudo chown -R root:wheel $DST/QwzAirport.kext
  sudo kmutil load -p $DST/QwzAirport.kext
  sleep 2
  "$0" status
  ;;
unload)
  sudo kmutil unload -b $ID
  ;;
status)
  kextstat -l -b $ID | grep $ID || echo "$ID is not loaded"
  ioreg -r -c QwzAirport -d 3 -w0 | grep -E "QwzAirport|IOInterfaceName|BSD Name|IO80211" | head -20 || true
  echo "== log (last 10 min)"
  log show --last 10m --style compact --predicate 'eventMessage CONTAINS "QwzAirport" OR eventMessage CONTAINS "airport:"' 2>/dev/null | tail -40
  ;;
*)
  sed -n 2,11p "$0"; exit 1 ;;
esac
