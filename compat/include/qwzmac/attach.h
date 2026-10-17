/*
 * QwzWifi: entry points the kext uses to run qwz the way OpenBSD's
 * autoconf(9) would (compat/autoconf.c).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef _QWZMAC_ATTACH_H_
#define _QWZMAC_ATTACH_H_

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Match and attach qwz to the IOPCIDevice. Returns 0 and an opaque device
 * handle, or an errno. Firmware bring-up continues in a kernel thread.
 */
int	qwzmac_qwz_attach(void *pcidev, void **devp);

/* Wait for bring-up to finish, detach qwz and free everything. */
void	qwzmac_qwz_detach(void *dev);

#ifdef __cplusplus
}
#endif

#endif /* _QWZMAC_ATTACH_H_ */
