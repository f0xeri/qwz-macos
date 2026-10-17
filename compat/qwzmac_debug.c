/*
 * QwzWifi: debugging helpers for the bring-up.
 *
 * qwzmac_rddm_report(): after a firmware crash the device writes a RAM
 * dump (RDDM) into host memory. Instead of saving it to a file (which qwz
 * does on OpenBSD) we log the printable strings found in it. The crash
 * reason and source file usually show up among the first ones.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <sys/param.h>
#include <sys/systm.h>

#define RDDM_MIN_STR	6	/* shorter runs are mostly noise */
#define RDDM_MAX_STR	160
#define RDDM_MAX_LINES	300
#define RDDM_HEAD_LINES	120	/* always log the first strings */

static int
is_text(uint8_t c)
{
	return c >= 0x20 && c < 0x7f;
}

static int
is_interesting(const char *s)
{
	static const char *const words[] = {
		"rr", "RR", "atal", "ATAL", "ssert", "SSERT", "xception",
		"rash", "RASH", "anic", "ail", "AIL", ".c", ".h", "imeout",
		"dlpager", "TPZ", "auth", "hash", "abort", "ABORT",
	};
	size_t i;

	for (i = 0; i < nitems(words); i++) {
		const char *w = words[i];
		size_t wl = strlen(w);
		const char *p;

		for (p = s; *p != '\0'; p++)
			if (strncmp(p, w, wl) == 0)
				return 1;
	}
	return 0;
}

void
qwzmac_rddm_report(const uint8_t *buf, size_t len)
{
	char str[RDDM_MAX_STR + 1];
	size_t i, run = 0, nonzero = 0, start = 0;
	int lines = 0, found = 0;

	for (i = 0; i < len; i++)
		if (buf[i] != 0)
			nonzero++;
	printf("QwzWifi: RDDM dump: %zu bytes, %zu non-zero\n", len, nonzero);
	if (nonzero == 0) {
		printf("QwzWifi: RDDM dump is empty (device wrote nothing or "
		    "the CPU sees stale cache lines)\n");
		return;
	}

	for (i = 0; i <= len && lines < RDDM_MAX_LINES; i++) {
		if (i < len && is_text(buf[i])) {
			if (run == 0)
				start = i;
			if (run < RDDM_MAX_STR)
				str[run] = (char)buf[i];
			run++;
			continue;
		}
		if (run >= RDDM_MIN_STR) {
			size_t n = MIN(run, RDDM_MAX_STR);

			str[n] = '\0';
			found++;
			if (found <= RDDM_HEAD_LINES || is_interesting(str)) {
				printf("QwzWifi: RDDM +0x%06zx: %s\n", start, str);
				lines++;
			}
		}
		run = 0;
	}
	printf("QwzWifi: RDDM: %d strings found, %d logged\n", found, lines);
}
