/*
 * QwzWifi tests: the WPA2 4-way handshake crypto of upstream net80211
 * (ieee80211_derive_ptk, ieee80211_eapol_key_mic), built with our compat
 * headers. Expected values come from an independent Python implementation
 * (hashlib/hmac) of IEEE 802.11i PRF-384 and the HMAC-SHA1 EAPOL MIC.
 * Run: scripts/test-compat.sh
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <net/if.h>
#include <net/if_media.h>
#include <netinet/in.h>
#include <netinet/if_ether.h>
#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_crypto.h>
#include <qwzmac/pbkdf2.h>

static int
hexeq(const uint8_t *b, const char *hex)
{
	static const char digits[] = "0123456789abcdef";
	size_t i;

	for (i = 0; hex[2 * i] != '\0'; i++)
		if (hex[2 * i] != digits[b[i] >> 4] ||
		    hex[2 * i + 1] != digits[b[i] & 0xf])
			return 0;
	return 1;
}

int
test_wpa(int *checks, int *failures)
{
	uint8_t pmk[32], an[32], sn[32], frame[121];
	const uint8_t aa[6] = { 0x50, 0xeb, 0xf6, 0x87, 0x1e, 0xac };
	const uint8_t spa[6] = { 0xac, 0xf2, 0x3c, 0x16, 0x94, 0x65 };
	struct ieee80211_eapol_key *key = (void *)frame;
	struct ieee80211_ptk ptk;
	int i, ok[4];

	pkcs5_pbkdf2("password", 8, (const uint8_t *)"IEEE", 4, pmk, 32, 4096);
	for (i = 0; i < 32; i++) {
		an[i] = i;
		sn[i] = 0xff - i;
	}
	ieee80211_derive_ptk(IEEE80211_AKM_PSK, pmk, aa, spa, an, sn, &ptk);
	ok[0] = hexeq(ptk.kck, "faf276859c9c10cde3981b3a7c58298f");
	ok[1] = hexeq(ptk.kek, "4b355b0122a9d448fd48fff2ad636922");
	ok[2] = hexeq(ptk.tk, "2c093c0b799cebc07f751bfd232b12b6");

	for (i = 0; i < (int)sizeof(frame); i++)
		frame[i] = (uint8_t)(i * 7);
	key->version = 1;
	key->type = EAPOL_KEY;
	key->len[0] = 0;
	key->len[1] = sizeof(frame) - 4;
	key->info[0] = 0x01;	/* MIC */
	key->info[1] = 0x0a;	/* pairwise, descriptor v2 (HMAC-SHA1) */
	memset(key->mic, 0, sizeof(key->mic));
	ieee80211_eapol_key_mic(key, ptk.kck);
	ok[3] = hexeq(key->mic, "015df162e86af1864fdc2988d1e409f0");

	for (i = 0; i < 4; i++) {
		(*checks)++;
		if (!ok[i]) {
			(*failures)++;
			qwzmac_log("FAIL wpa_test check %d\n", i);
		}
	}
	return 0;
}
