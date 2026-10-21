# Host tests

Run `scripts/test-compat.sh` (and `scripts/test-airport-scan.sh` for the QwzAirport headers). It builds C compat/net80211 code and the same
`kext/os_timer.cpp` and `kext/os_lock.cpp` used by the kext, with AddressSanitizer.
There is no test implementation of `qwzmac_timer_*`, `qwzmac_spl_*` or
`qwzmac_sleep`.

`kernel_api.cpp` substitutes only system APIs: thread calls, IOKit locks,
allocation, clock and thread identity. `kernel/` supplies their host headers.
Dispatch can be paused before entering the production callback. Allocation
counters and API probes let tests observe cleanup and barrier races without
duplicating driver state. The system work-item mock serializes execution;
the production implementation requests `THREAD_CALL_OPTIONS_ONCE`.

`node_test.c` links the real `ieee80211_node.c` cleanup/copy/free paths. Its
protocol stubs are not timer implementations; invoking one fails the test.

These tests execute production lifecycle logic under a host system-API model.
They do not execute XNU or replace hardware boot/connection testing.

`qwz_htc_test.c` uses complete, unmodified function bodies selected from the
working `qwz.c` by `extract_qwz_htc.py` (including its upstream license).
The real HTC send, HTT request, CE completion and post-reset cleanup run with
compat DMA/mbufs. Only hardware submission/completion and packet-header setup
are substituted. Cases cover ownership, repeated reset/reuse, live-device
retention, submission failure, credit rollback and map-load errors without
freeing caller-owned packets. To compare an older source without editing the
working copy: `QWZ_HTC_SOURCE=/absolute/old-qwz.c scripts/test-compat.sh`.
