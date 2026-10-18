/*
 * QwzWifi compat: PBKDF2-HMAC-SHA1 (RFC 2898), for the WPA2 PSK.
 *
 * In OpenBSD, ifconfig(8) turns the passphrase into the 32-byte PSK in
 * user space (pkcs5_pbkdf2(3) from libutil) and hands only the PSK to
 * net80211. There is no ifconfig here, so the kext does it.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <sys/param.h>
#include <sys/systm.h>

#include <crypto/md5.h>
#include <crypto/sha1.h>
#include <crypto/sha2.h>
#include <crypto/hmac.h>

#include <qwzmac/pbkdf2.h>

int
pkcs5_pbkdf2(const char *pass, size_t pass_len, const uint8_t *salt,
    size_t salt_len, uint8_t *key, size_t key_len, unsigned int rounds)
{
	HMAC_SHA1_CTX ctx;
	uint8_t u[SHA1_DIGEST_LENGTH], t[SHA1_DIGEST_LENGTH], cnt[4];
	uint32_t block;
	unsigned int r;
	size_t n, i;

	if (rounds < 1 || key_len == 0 || pass_len > UINT_MAX ||
	    salt_len > UINT_MAX)
		return -1;

	for (block = 1; key_len > 0; block++) {
		cnt[0] = block >> 24;
		cnt[1] = block >> 16;
		cnt[2] = block >> 8;
		cnt[3] = block;

		/* U1 = PRF(P, S || INT(i)) */
		HMAC_SHA1_Init(&ctx, (const uint8_t *)pass, pass_len);
		HMAC_SHA1_Update(&ctx, salt, salt_len);
		HMAC_SHA1_Update(&ctx, cnt, sizeof(cnt));
		HMAC_SHA1_Final(u, &ctx);
		memcpy(t, u, sizeof(t));

		/* T = U1 ^ U2 ^ ... ^ Uc, Uj = PRF(P, Uj-1) */
		for (r = 1; r < rounds; r++) {
			HMAC_SHA1_Init(&ctx, (const uint8_t *)pass, pass_len);
			HMAC_SHA1_Update(&ctx, u, sizeof(u));
			HMAC_SHA1_Final(u, &ctx);
			for (i = 0; i < sizeof(t); i++)
				t[i] ^= u[i];
		}

		n = MIN(key_len, sizeof(t));
		memcpy(key, t, n);
		key += n;
		key_len -= n;
	}

	explicit_bzero(&ctx, sizeof(ctx));
	explicit_bzero(u, sizeof(u));
	explicit_bzero(t, sizeof(t));
	return 0;
}
