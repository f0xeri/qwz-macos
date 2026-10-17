/*
 * QwzWifi compat: OpenBSD <sys/sysctl.h>. net80211 includes it but only
 * uses it for the ieee80211 debug sysctl, which we don't have.
 */
#ifndef _QWZMAC_SYS_SYSCTL_H_
#define _QWZMAC_SYS_SYSCTL_H_
#include <sys/types.h>
#endif
