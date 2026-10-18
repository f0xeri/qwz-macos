/*
 * QwzWifi tests: link stubs for tests/wpa_test.c. The PTK/MIC path of
 * net80211/ieee80211_crypto.c never calls these ciphers. No headers here,
 * so the stub signatures cannot clash with the real prototypes.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#define STUB(n) void n(void); void n(void) { }
STUB(ieee80211_bip_decap) STUB(ieee80211_bip_delete_key) STUB(ieee80211_bip_encap)
STUB(ieee80211_bip_set_key) STUB(ieee80211_ccmp_decrypt) STUB(ieee80211_ccmp_delete_key)
STUB(ieee80211_ccmp_encrypt) STUB(ieee80211_ccmp_set_key) STUB(ieee80211_get_hdrlen)
STUB(ieee80211_michael_mic_failure_timeout) STUB(ieee80211_tkip_decrypt)
STUB(ieee80211_tkip_delete_key) STUB(ieee80211_tkip_encrypt) STUB(ieee80211_tkip_set_key)
STUB(ieee80211_wep_decrypt) STUB(ieee80211_wep_delete_key) STUB(ieee80211_wep_encrypt)
STUB(ieee80211_wep_set_key) STUB(rc4_crypt) STUB(rc4_keysetup) STUB(rc4_skip)
STUB(AES_CMAC_Final) STUB(AES_CMAC_Init) STUB(AES_CMAC_SetKey) STUB(AES_CMAC_Update)
STUB(aes_key_unwrap) STUB(aes_key_wrap) STUB(aes_key_wrap_set_key)
STUB(aes_key_wrap_set_key_wrap_only)
