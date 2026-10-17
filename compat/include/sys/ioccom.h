/* QwzWifi compat: OpenBSD <sys/ioccom.h>. SPDX-License-Identifier: GPL-2.0-only */
#ifndef _QWZMAC_SYS_IOCCOM_H_
#define _QWZMAC_SYS_IOCCOM_H_
#define IOCPARM_MASK	0x1fff
#define IOCPARM_LEN(x)	(((x) >> 16) & IOCPARM_MASK)
#define IOCBASECMD(x)	((x) & ~(IOCPARM_MASK << 16))
#define IOCGROUP(x)	(((x) >> 8) & 0xff)
#define IOCPARM_MAX	(1 << 12)
#define IOC_VOID	(unsigned long)0x20000000
#define IOC_OUT		(unsigned long)0x40000000
#define IOC_IN		(unsigned long)0x80000000
#define IOC_INOUT	(IOC_IN | IOC_OUT)
#define IOC_DIRMASK	(unsigned long)0xe0000000
#define _IOC(inout, group, num, len) \
	((inout) | (((len) & IOCPARM_MASK) << 16) | ((group) << 8) | (num))
#define _IO(g, n)	_IOC(IOC_VOID, (g), (n), 0)
#define _IOR(g, n, t)	_IOC(IOC_OUT, (g), (n), sizeof(t))
#define _IOW(g, n, t)	_IOC(IOC_IN, (g), (n), sizeof(t))
#define _IOWR(g, n, t)	_IOC(IOC_INOUT, (g), (n), sizeof(t))
#endif
