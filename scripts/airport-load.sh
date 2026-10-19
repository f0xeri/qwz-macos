#!/bin/zsh
# Inspect QwzAirport.kext (OpenCore-only development builds).
#
#   scripts/airport-load.sh check    read-only: ABI and running QwzWifi
#   scripts/airport-load.sh status   read-only: controller, interfaces and recent log
#   scripts/airport-load.sh unload   sudo: attempt unloading the current module
#   scripts/airport-load.sh load     disabled: thread_call_isactive is not a KPI export
#
# Installation is through OpenCore, only with the user's approval for the boot stick.
# kmutil load would stage an auxiliary collection and cannot link this build.
set -e
cd "${0:A:h}/.."
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
  echo "QwzAirport now uses native thread_call_isactive (user choice, OpenCore injection)." >&2
  echo "kmutil load cannot resolve it. Install through OpenCore after boot-stick approval." >&2
  exit 1
  ;;
unload)
  sudo kmutil unload -b $ID
  ;;
status)
  kextstat -l -b $ID | grep $ID || echo "$ID is not loaded"
  ioreg -r -c QwzAirport -d 3 -w0 | grep -E "QwzAirport|IOInterfaceName|BSD Name|IO80211" | head -20 || true
  echo "== log (last 10 min)"
  /usr/bin/log show --last 10m --style compact --predicate 'eventMessage CONTAINS "QwzAirport" OR eventMessage CONTAINS "airport:"' 2>/dev/null | tail -40
  ;;
*)
  sed -n 2,11p "$0"; exit 1 ;;
esac
