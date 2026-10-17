/*
 * QwzWifi compat: OpenBSD <sys/endian.h> (x86_64 is little endian).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef _QWZMAC_SYS_ENDIAN_H_
#define _QWZMAC_SYS_ENDIAN_H_

#include <sys/types.h>

#define swap16(x)	__builtin_bswap16((uint16_t)(x))
#define swap32(x)	__builtin_bswap32((uint32_t)(x))
#define swap64(x)	__builtin_bswap64((uint64_t)(x))

#define htole16(x)	((uint16_t)(x))
#define htole32(x)	((uint32_t)(x))
#define htole64(x)	((uint64_t)(x))
#define letoh16(x)	((uint16_t)(x))
#define letoh32(x)	((uint32_t)(x))
#define letoh64(x)	((uint64_t)(x))
#define le16toh(x)	letoh16(x)
#define le32toh(x)	letoh32(x)
#define le64toh(x)	letoh64(x)

#define htobe16(x)	swap16(x)
#define htobe32(x)	swap32(x)
#define htobe64(x)	swap64(x)
#define betoh16(x)	swap16(x)
#define betoh32(x)	swap32(x)
#define betoh64(x)	swap64(x)
#define be16toh(x)	betoh16(x)
#define be32toh(x)	betoh32(x)
#define be64toh(x)	betoh64(x)

#define htons(x)	swap16(x)
#define ntohs(x)	swap16(x)
#define htonl(x)	swap32(x)
#define ntohl(x)	swap32(x)

static inline uint16_t
bemtoh16(const void *p)
{
	const uint8_t *b = p;
	return (uint16_t)(b[0] << 8 | b[1]);
}

static inline uint32_t
bemtoh32(const void *p)
{
	const uint8_t *b = p;
	return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 |
	    (uint32_t)b[2] << 8 | b[3];
}

static inline uint16_t
lemtoh16(const void *p)
{
	const uint8_t *b = p;
	return (uint16_t)(b[1] << 8 | b[0]);
}

static inline uint32_t
lemtoh32(const void *p)
{
	const uint8_t *b = p;
	return (uint32_t)b[3] << 24 | (uint32_t)b[2] << 16 |
	    (uint32_t)b[1] << 8 | b[0];
}

static inline void
htobem16(void *p, uint16_t v)
{
	uint8_t *b = p;
	b[0] = v >> 8; b[1] = v;
}

static inline void
htobem32(void *p, uint32_t v)
{
	uint8_t *b = p;
	b[0] = v >> 24; b[1] = v >> 16; b[2] = v >> 8; b[3] = v;
}

static inline void
htolem16(void *p, uint16_t v)
{
	uint8_t *b = p;
	b[0] = v; b[1] = v >> 8;
}

static inline void
htolem32(void *p, uint32_t v)
{
	uint8_t *b = p;
	b[0] = v; b[1] = v >> 8; b[2] = v >> 16; b[3] = v >> 24;
}

#endif /* _QWZMAC_SYS_ENDIAN_H_ */
