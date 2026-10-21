#!/bin/zsh
# Inspect QwzAirport.kext (OpenCore-injected builds).
#
#   scripts/airport-check.sh check    read-only: ABI and running QwzWifi
#   scripts/airport-check.sh status   read-only: controller, interfaces and recent log
#
# Installation is through OpenCore. kmutil load would stage an auxiliary collection and cannot
# link this build, because thread_call_isactive is not a KPI export.
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
status)
  kextstat -l -b $ID | grep $ID || echo "$ID is not loaded"
  ioreg -r -c QwzAirport -d 3 -w0 | grep -E "QwzAirport|IOInterfaceName|BSD Name|IO80211" | head -20 || true
  echo "== log (last 10 min)"
  /usr/bin/log show --last 10m --style compact --predicate 'eventMessage CONTAINS "QwzAirport" OR eventMessage CONTAINS "airport:"' 2>/dev/null | tail -40
  ;;
*)
  sed -n 2,6p "$0"; exit 1 ;;
esac
