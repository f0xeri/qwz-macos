/*
 * QwzWifi compat: OpenBSD <sys/errno.h> (BSD numbering, same as macOS).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef _QWZMAC_SYS_ERRNO_H_
#define _QWZMAC_SYS_ERRNO_H_

#define EPERM		1
#define ENOENT		2
#define ESRCH		3
#define EINTR		4
#define EIO		5
#define ENXIO		6
#define E2BIG		7
#define ENOEXEC		8
#define EBADF		9
#define ECHILD		10
#define EDEADLK		11
#define ENOMEM		12
#define EACCES		13
#define EFAULT		14
#define EBUSY		16
#define EEXIST		17
#define ENODEV		19
#define ENOTDIR		20
#define EISDIR		21
#define EINVAL		22
#define ENFILE		23
#define EMFILE		24
#define ENOTTY		25
#define EFBIG		27
#define ENOSPC		28
#define ESPIPE		29
#define EROFS		30
#define EPIPE		32
#define EDOM		33
#define ERANGE		34
#define EAGAIN		35
#define EWOULDBLOCK	EAGAIN
#define EINPROGRESS	36
#define EALREADY	37
#define ENOTSOCK	38
#define EDESTADDRREQ	39
#define EMSGSIZE	40
#define EPROTOTYPE	41
#define ENOPROTOOPT	42
#define EPROTONOSUPPORT	43
#define EOPNOTSUPP	45
#define EAFNOSUPPORT	47
#define EADDRINUSE	48
#define EADDRNOTAVAIL	49
#define ENETDOWN	50
#define ENETUNREACH	51
#define ENETRESET	52
#define ECONNABORTED	53
#define ECONNRESET	54
#define ENOBUFS		55
#define EISCONN		56
#define ENOTCONN	57
#define ESHUTDOWN	58
#define ETOOMANYREFS	59
#define ETIMEDOUT	60
#define ECONNREFUSED	61
#define ELOOP		62
#define ENAMETOOLONG	63
#define EHOSTDOWN	64
#define EHOSTUNREACH	65
#define ENOTEMPTY	66
#define ENOSYS		78
#define EFTYPE		79
#define EAUTH		80
#define ENEEDAUTH	81
#define EIPSEC		82
#define ENOATTR		83
#define EILSEQ		84
#define ENOMEDIUM	85
#define EMEDIUMTYPE	86
#define EOVERFLOW	87
#define ECANCELED	88
#define EIDRM		89
#define ENOMSG		90
#define ENOTSUP		91
#define EBADMSG		92
#define ENOTRECOVERABLE	93
#define EOWNERDEAD	94
#define EPROTO		95

#define ERESTART	-1
#define EJUSTRETURN	-2

#endif /* _QWZMAC_SYS_ERRNO_H_ */
