/*
 * QwzWifi: lifecycle of the IOKit side of qwzmac/os.h.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef _QWZWIFI_OS_BRIDGE_H_
#define _QWZWIFI_OS_BRIDGE_H_

class IOService;
class IOPCIDevice;

bool	qwzmac_os_init(IOService *owner, IOPCIDevice *pci);
void	qwzmac_os_fini(void);

/*
 * qwzlog.cpp: publish the driver log as the "QwzLog" property of owner (the
 * PCI device). qwzmac_log_stop() must run before the kext can go away.
 */
void	qwzmac_log_start(IOService *owner);
void	qwzmac_log_publish(void);
void	qwzmac_log_stop(void);

#endif /* _QWZWIFI_OS_BRIDGE_H_ */
