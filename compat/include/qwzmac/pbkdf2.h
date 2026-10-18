/* QwzWifi compat: pkcs5_pbkdf2(3) (HMAC-SHA1), as in OpenBSD libutil. */
#ifndef _QWZMAC_PBKDF2_H_
#define _QWZMAC_PBKDF2_H_
#include <sys/types.h>
int	pkcs5_pbkdf2(const char *, size_t, const uint8_t *, size_t, uint8_t *,
	    size_t, unsigned int);
#endif
