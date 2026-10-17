#!/bin/zsh
# Saves the QwzWifi driver log, which the kext publishes as the "QwzLog"
# property of the Wi-Fi PCI device (IOLog does not reach `log show` in early
# boot), and prints a short summary.
# usage: scripts/qwzlog.sh [file]     default file: ~/Desktop/qwz.log
#        scripts/qwzlog.sh -          print the whole log to stdout
out=${1:-$HOME/Desktop/qwz.log}
log=$(ioreg -a -r -k QwzLog -d1 2>/dev/null | plutil -extract 0.QwzLog raw -o - - 2>/dev/null) || {
  echo "No QwzLog property found: the kext did not start (check boot-args and kextstat)." >&2
  exit 1
}
if [[ $out == - ]]; then
  print -r -- "$log"
  exit 0
fi
print -r -- "$log" > "$out"
echo "saved $(wc -l < "$out" | tr -d ' ') lines to $out"
echo "--- summary ---"
grep -E 'stage 1 |bring-up took|Asserted|fatal firmware|MAC address|timeout|failed|PANIC|refused' "$out" | cut -c1-160 | head -20
