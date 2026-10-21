# Build dependencies

**URLs, revisions and paths of every build source live in `config/sources.json`.** The firmware
source, version and SHA-256 hashes are there too. The scripts keep no copies of these settings.

All sources share one form in the `sources` section (`kind`, `url`, `revision`, `directory`), and
`scripts/fetch-sources.sh` fetches all of them.

- OpenBSD points, through `manifest`, at `patches/openbsd/FILES`. That single list drives the
  sparse checkout, the patch application and the compilation.
- For the firmware, `path` is the subdirectory of the release and `files` holds names and SHA-256
  hashes. Do not change the firmware, the board/regulatory data or their hashes without a separate
  decision and a hardware test.
- Research-only repositories and settings stay in the gitignored `local/` directory and are not
  part of the shared configuration or the build.

A normal build checks everything locally, without downloading: the set of OpenBSD files, whether
they match the exported patches, and the firmware hashes. A mismatch stops the build before
compilation. Only the `.c` files listed in FILES are compiled. To run the checks on their own:

```sh
scripts/patches.sh check
python3 scripts/check-firmware.py
```

`scripts/fetch-sources.sh` reads the config and verifies existing checkouts: URL, commit and the
absence of local changes. It never resets modified repositories and never replaces a damaged cached
firmware file on its own.

Update OpenBSD with `scripts/patches.sh update <commit>`: the script bumps the commit in the shared
config and merges our patches into the new files (3-way). After resolving conflicts, run
`scripts/patches.sh export`. Changing the pin alone, without recreating or updating `openbsd/`, is
not enough.

## mbuf: our adaptation of OpenBSD

`compat/uipc_mbuf.c` combines algorithms taken from OpenBSD with our replacement of the pool
allocator by malloc. It is an adaptation, not the upstream file; authorship and the original
version are stated at the top of the file. `compat/include/sys/mbuf.h` implements the part of the
API we need.

When OpenBSD is updated, changes to `sys/kern/uipc_mbuf.c` are reviewed by hand and the relevant
fixes are ported into compat. `patches.sh update` does not touch this file. After a change, run
`scripts/test-compat.sh` and a full build. An attempt to put a heavily trimmed mbuf into FILES was
dropped: the large deleting patch did not justify the harder updates.

## Airport: IO80211 headers and WCL

`airport/abi/` and `airport/WCL.hpp` are our files. They were taken from laobamac/itlwm (the commit
and a command for comparing with the fork are in `airport/abi/UPSTREAM`) and have been edited in
place since; our changes are marked `QWZAIRPORT`/`QWZMAC`. There is no import step: fixes from the
fork are ported by hand after running the `diff` from UPSTREAM.

The headers describe Apple's undocumented C++ ABI, so the only real check is against the kernel of
the Mac you build for. Before adding a macOS build to `QwzAirportBuilds`, and after any header
change, run:

```sh
scripts/build.sh
python3 scripts/kc-abi.py --check
python3 scripts/kc-abi.py --returns
python3 scripts/kpi-check.py --opencore build/QwzWifi.kext build/QwzAirport.kext
```
