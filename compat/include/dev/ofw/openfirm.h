/* QwzWifi compat: no OpenFirmware/FDT on x86 macOS. SPDX-License-Identifier: GPL-2.0-only */
#ifndef _QWZMAC_OPENFIRM_H_
#define _QWZMAC_OPENFIRM_H_
static inline int OF_getprop(int node, const char *name, void *buf, int len)
{ (void)node; (void)name; (void)buf; (void)len; return -1; }
static inline int OF_getproplen(int node, const char *name)
{ (void)node; (void)name; return -1; }
#endif
