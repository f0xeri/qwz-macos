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
grep -E 'stage [123]|bring-up took|Asserted|fatal firmware|MAC address|timeout|failed|PANIC|refused|joined|network interface|link (up|down)|interface (enabled|disabled)|system (sleep|wake)|sleep: |wake: |power message' "$out" | cut -c1-160 | head -60
# Stage 3: association (net80211 state changes) and the WPA2 4-way handshake.
echo "--- association ---"
grep -E 'qwz0: [A-Z]+ -> [A-Z]+|handshake|authentication failed|association failed|deauth|disassoc|with .* ssid|no beacons' "$out" |
  grep -v 'SCAN -> SCAN' | cut -c1-160 | head -30
# Stage 2: net80211 lists every network after each scan (IFF_DEBUG):
#   qwz0: - <bssid>! <chan> <rssi> <rate> ess <privacy> <rsn> <ssid>
# '!' marks why it was not joined (the scan-only bssid filter fails all).
# ni_rssi is a u_int8_t, so -71 dBm prints as +185: convert back.
nets=$(sed -E 's/^\[[^]]*\] //' "$out" | grep -E '^qwz0: [+-] ([0-9a-f]{2}:){5}[0-9a-f]{2}' |
  awk '{ b = substr($3, 1, 17); ch = $4 + 0; r = $5 + 0; if (r > 127) r -= 256
         s = $0; sub(/^[^"]*/, "", s); sub(/!$/, "", s); if (s == "\"\"") s = "(hidden)"
         n[b]++; line[b] = sprintf("%4d dBm  ch %3d  %s  %s", r, ch, b, s) }
       END { for (b in line) printf "%s  (%d scans)\n", line[b], n[b] }' | sort -n -r)
scans=$(grep -c 'end active scan\|end passive scan' "$out")
echo "--- networks seen: $(print -r -- "$nets" | grep -c .) (scans finished: $scans) ---"
[[ -n $nets ]] && print -r -- "$nets"
