/*
 * QwzWifi compat: OpenBSD <sys/types.h> for a freestanding macOS kext build.
 * Only clang's freestanding headers are used; no Apple headers are pulled
 * into the OpenBSD side of the build.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef _QWZMAC_SYS_TYPES_H_
#define _QWZMAC_SYS_TYPES_H_

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#ifndef _KERNEL
#define _KERNEL 1
#endif

typedef unsigned char	u_char;
typedef unsigned short	u_short;
typedef unsigned int	u_int;
typedef unsigned long	u_long;

typedef unsigned char	unchar;
typedef unsigned short	ushort;
typedef unsigned int	uint;
typedef unsigned long	ulong;

typedef int8_t		__int8_t;
typedef uint8_t		__uint8_t;
typedef int16_t		__int16_t;
typedef uint16_t	__uint16_t;
typedef int32_t		__int32_t;
typedef uint32_t	__uint32_t;
typedef int64_t		__int64_t;
typedef uint64_t	__uint64_t;

typedef uint8_t		u_int8_t;
typedef uint16_t	u_int16_t;
typedef uint32_t	u_int32_t;
typedef uint64_t	u_int64_t;

typedef int64_t		quad_t;
typedef uint64_t	u_quad_t;

typedef char *		caddr_t;
typedef long		ssize_t;
typedef int64_t		off_t;
typedef int64_t		time_t;
typedef int32_t		pid_t;
typedef uint32_t	uid_t;
typedef uint32_t	gid_t;
typedef uint32_t	mode_t;
typedef int		dev_t;
typedef uint64_t	paddr_t;
typedef uint64_t	psize_t;
typedef unsigned long	vaddr_t;
typedef unsigned long	vsize_t;
typedef uint32_t	in_addr_t;
typedef uint16_t	in_port_t;
typedef uint8_t		sa_family_t;
typedef uint32_t	socklen_t;
typedef int		clockid_t;

#ifndef __cplusplus
typedef _Bool		bool;
#define true	1
#define false	0
#endif

#ifndef NULL
#define NULL	((void *)0)
#endif

#define __packed	__attribute__((__packed__))
#define __aligned(x)	__attribute__((__aligned__(x)))
#define __unused	__attribute__((__unused__))
#define __used		__attribute__((__used__))
#define __dead		__attribute__((__noreturn__))
#define __predict_true(x)	__builtin_expect(!!(x), 1)
#define __predict_false(x)	__builtin_expect(!!(x), 0)
#define __BEGIN_DECLS
#define __END_DECLS
#define __P(x)		x
#define __CONCAT(x, y)	x ## y
#define __STRING(x)	#x
#define __bounded(x)
#define __kprintf_attribute__(x)
#define __attribute_maybe_unused__ __attribute__((__unused__))
#define __read_mostly
#define __statement(x)	__extension__(x)
#define __pure
#define __malloc
#define __returns_twice
#define __only_inline	static __inline __attribute__((__gnu_inline__))
#define __inline	inline

#define LITTLE_ENDIAN	1234
#define BIG_ENDIAN	4321
#define BYTE_ORDER	LITTLE_ENDIAN
#define _LITTLE_ENDIAN	LITTLE_ENDIAN
#define _BIG_ENDIAN	BIG_ENDIAN
#define _BYTE_ORDER	BYTE_ORDER

#include <limits.h>

/* Opaque kernel structures referenced in prototypes. */
struct proc;
struct process;
struct rtentry;
struct uio;
struct netstack;
#include <sys/endian.h>

#endif /* _QWZMAC_SYS_TYPES_H_ */
