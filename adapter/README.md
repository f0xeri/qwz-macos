# qwz/net80211 adapter

Our code that connects the OpenBSD driver to macOS. Unlike `compat/`, which implements general
OpenBSD APIs, this directory controls one specific device. OpenBSD headers are used only inside the
C part; macOS clients go through `compat/include/qwzmac/attach.h`.

- `device.c`: attach/detach, firmware start, passing frames and events to the kext, shutdown and
  sleep/wake. It also holds `config_mountroot`: its thread starts the firmware and the interface,
  and detach waits for it to finish.
- `control.c`: join/leave, radio power, scan requests and MAC changes. It turns client requests
  into existing net80211/qwz operations.
- `status.c`: reads the state of the station, channels and scan results, computes the transfer
  rate, and builds the snapshot for native Wi-Fi.
- `internal.h`: private device state, the qwz/net80211 headers and the five functions several files
  need. Do not include it from the kext or from compat.

Control path: `QwzUserClient`, or `QwzAirport → airport_api`, then `control.c`, then net80211/qwz.
The return path of frames and events goes through `device.c` and `qwzmac_netif_ops`; state is read
through `status.c`.

Locking rules stay with the functions: spl serializes access to net80211, and `ioctl_rwl` protects
sequences of device configuration. Moving a function between files does not by itself change the
context it runs in.
