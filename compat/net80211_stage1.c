/*
 * QwzWifi: TEMPORARY stage-1 stand-ins for net80211.
 *
 * Stage 1 only has to reach "firmware ready" (MHI, QMI, firmware download,
 * MAC address). Upstream OpenBSD net80211 replaces this file in stage 2.
 * The pure helpers below are copied from OpenBSD net80211 (BSD license,
 * see the notice); everything else only logs that it was called.
 *
 * Copyright (c) 2001 Atsushi Onoe
 * Copyright (c) 2002, 2003 Sam Leffler, Errno Consulting
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/tree.h>

#include <net/if.h>
#include <net/if_media.h>
#include <netinet/in.h>
#include <netinet/if_ether.h>

#include <net80211/ieee80211_var.h>

#define STUB() do {							\
	static int warned;						\
	if (!warned) {							\
		warned = 1;						\
		printf("QwzWifi: net80211 stub called: %s\n", __func__);\
	}								\
} while (0)

/* Pure helpers, copied from OpenBSD net80211. */

const struct ieee80211_rateset ieee80211_std_rateset_11a =
	{ 8, { 12, 18, 24, 36, 48, 72, 96, 108 } };

const struct ieee80211_rateset ieee80211_std_rateset_11b =
	{ 4, { 2, 4, 11, 22 } };

const struct ieee80211_rateset ieee80211_std_rateset_11g =
	{ 12, { 2, 4, 11, 22, 12, 18, 24, 36, 48, 72, 96, 108 } };

const char * const ieee80211_state_name[IEEE80211_S_MAX] = {
	"INIT",		/* IEEE80211_S_INIT */
	"SCAN",		/* IEEE80211_S_SCAN */
	"AUTH",		/* IEEE80211_S_AUTH */
	"ASSOC",	/* IEEE80211_S_ASSOC */
	"RUN"		/* IEEE80211_S_RUN */
};

u_int
ieee80211_mhz2ieee(u_int freq, u_int flags)
{
	if (flags & IEEE80211_CHAN_2GHZ) {	/* 2GHz band */
		if (freq == 2484)
			return 14;
		if (freq < 2484)
			return (freq - 2407) / 5;
		else
			return 15 + ((freq - 2512) / 20);
	} else if (flags & IEEE80211_CHAN_5GHZ) {	/* 5GHz band */
		return (freq - 5000) / 5;
	} else {				/* either, guess */
		if (freq == 2484)
			return 14;
		if (freq < 2484)
			return (freq - 2407) / 5;
		if (freq < 5000)
			return 15 + ((freq - 2512) / 20);
		return (freq - 5000) / 5;
	}
}

u_int
ieee80211_chan2ieee(struct ieee80211com *ic, const struct ieee80211_channel *c)
{
	struct ifnet *ifp = &ic->ic_if;
	if (ic->ic_channels <= c && c <= &ic->ic_channels[IEEE80211_CHAN_MAX])
		return c - ic->ic_channels;
	else if (c == IEEE80211_CHAN_ANYC)
		return IEEE80211_CHAN_ANY;

	panic("%s: bogus channel pointer", ifp->if_xname);
	return IEEE80211_CHAN_ANY;	/* QWZMAC: our panic returns */
}

u_int
ieee80211_ieee2mhz(u_int chan, u_int flags)
{
	if (flags & IEEE80211_CHAN_2GHZ) {	/* 2GHz band */
		if (chan == 14)
			return 2484;
		if (chan < 14)
			return 2407 + chan*5;
		else
			return 2512 + ((chan-15)*20);
	} else if (flags & IEEE80211_CHAN_5GHZ) {/* 5GHz band */
		return 5000 + (chan*5);
	} else {				/* either, guess */
		if (chan == 14)
			return 2484;
		if (chan < 14)			/* 0-13 */
			return 2407 + chan*5;
		if (chan < 27)			/* 15-26 */
			return 2512 + ((chan-15)*20);
		return 5000 + (chan*5);
	}
}

int
ieee80211_min_basic_rate(struct ieee80211com *ic)
{
	struct ieee80211_rateset *rs = &ic->ic_bss->ni_rates;
	int i, min, rval;

	min = -1;

	for (i = 0; i < rs->rs_nrates; i++) {
		if ((rs->rs_rates[i] & IEEE80211_RATE_BASIC) == 0)
			continue;
		rval = (rs->rs_rates[i] & IEEE80211_RATE_VAL);
		if (min == -1)
			min = rval;
		else if (rval < min)
			min = rval;
	}

	/* Default to 1 Mbit/s on 2GHz and 6 Mbit/s on 5GHz. */
	if (min == -1)
		min = IEEE80211_IS_CHAN_2GHZ(ic->ic_bss->ni_chan) ? 2 : 12;

	return min;
}

u_int
ieee80211_get_hdrlen(const struct ieee80211_frame *wh)
{
	u_int size = sizeof(*wh);

	/* NB: does not work with control frames */
	KASSERT(ieee80211_has_seq(wh));

	if (ieee80211_has_addr4(wh))
		size += IEEE80211_ADDR_LEN;	/* i_addr4 */
	if (ieee80211_has_qos(wh))
		size += sizeof(u_int16_t);	/* i_qos */
	if (ieee80211_has_htc(wh))
		size += sizeof(u_int32_t);	/* i_ht */
	return size;
}

int
ieee80211_node_cmp(const struct ieee80211_node *b1,
    const struct ieee80211_node *b2)
{
	return (memcmp(b1->ni_macaddr, b2->ni_macaddr, IEEE80211_ADDR_LEN));
}

RBT_GENERATE(ieee80211_tree, ieee80211_node, ni_node, ieee80211_node_cmp);

/* Logging stand-ins. */

void
ieee80211_ifattach(struct ifnet *ifp)
{
	struct ieee80211com *ic = (void *)ifp;

	STUB();
	/* The minimum qwz relies on before stage 2. */
	ic->ic_bss = NULL;
	RBT_INIT(ieee80211_tree, &ic->ic_tree);
}

void
ieee80211_channel_init(struct ifnet *ifp)
{
	struct ieee80211com *ic = (void *)ifp;
	int i, n2 = 0, n5 = 0;

	for (i = 0; i <= IEEE80211_CHAN_MAX; i++) {
		if (IEEE80211_IS_CHAN_2GHZ(&ic->ic_channels[i]))
			n2++;
		else if (IEEE80211_IS_CHAN_5GHZ(&ic->ic_channels[i]))
			n5++;
	}
	printf("%s: firmware reported %d 2GHz and %d 5GHz channels\n",
	    ifp->if_xname, n2, n5);
}

void
ieee80211_media_init(struct ifnet *ifp, ifm_change_cb_t media_change,
    ifm_stat_cb_t media_stat)
{
	(void)ifp; (void)media_change; (void)media_stat;
	STUB();
}

int
ieee80211_media_change(struct ifnet *ifp)
{
	(void)ifp;
	STUB();
	return EOPNOTSUPP;
}

void
ieee80211_media_status(struct ifnet *ifp, struct ifmediareq *imr)
{
	(void)ifp; (void)imr;
	STUB();
}

int
ieee80211_ioctl(struct ifnet *ifp, u_long cmd, caddr_t data)
{
	(void)ifp; (void)cmd; (void)data;
	STUB();
	return ENOTTY;
}

void
ieee80211_watchdog(struct ifnet *ifp)
{
	(void)ifp;
	STUB();
}

int
ieee80211_setmode(struct ieee80211com *ic, enum ieee80211_phymode mode)
{
	(void)ic; (void)mode;
	STUB();
	return 0;
}

void
ieee80211_begin_scan(struct ifnet *ifp)
{
	(void)ifp;
	STUB();
}

void
ieee80211_end_scan(struct ifnet *ifp)
{
	(void)ifp;
	STUB();
}

void
ieee80211_set_link_state(struct ieee80211com *ic, int nstate)
{
	(void)ic; (void)nstate;
	STUB();
}

void
ieee80211_input(struct ifnet *ifp, struct mbuf *m, struct ieee80211_node *ni,
    struct ieee80211_rxinfo *rxi)
{
	(void)ifp; (void)ni; (void)rxi;
	STUB();
	m_freem(m);
}

struct mbuf *
ieee80211_encap(struct ifnet *ifp, struct mbuf *m, struct ieee80211_node **pni)
{
	(void)ifp;
	STUB();
	*pni = NULL;
	m_freem(m);
	return NULL;
}

struct mbuf *
ieee80211_encrypt(struct ieee80211com *ic, struct mbuf *m0,
    struct ieee80211_key *k)
{
	(void)ic; (void)k;
	STUB();
	m_freem(m0);
	return NULL;
}

struct ieee80211_key *
ieee80211_get_txkey(struct ieee80211com *ic, const struct ieee80211_frame *wh,
    struct ieee80211_node *ni)
{
	(void)ic; (void)wh; (void)ni;
	STUB();
	return NULL;
}

int
ieee80211_set_key(struct ieee80211com *ic, struct ieee80211_node *ni,
    struct ieee80211_key *k)
{
	(void)ic; (void)ni; (void)k;
	STUB();
	return EOPNOTSUPP;
}

void
ieee80211_delete_key(struct ieee80211com *ic, struct ieee80211_node *ni,
    struct ieee80211_key *k)
{
	(void)ic; (void)ni; (void)k;
	STUB();
}

struct ieee80211_node *
ieee80211_find_node(struct ieee80211com *ic, const u_int8_t *macaddr)
{
	(void)ic; (void)macaddr;
	STUB();
	return NULL;
}

struct ieee80211_node *
ieee80211_find_rxnode(struct ieee80211com *ic,
    const struct ieee80211_frame *wh)
{
	(void)wh;
	STUB();
	return ic->ic_bss;
}

void
ieee80211_release_node(struct ieee80211com *ic, struct ieee80211_node *ni)
{
	(void)ic; (void)ni;
	STUB();
}

void
ieee80211_node_cleanup(struct ieee80211com *ic, struct ieee80211_node *ni)
{
	(void)ic; (void)ni;
	STUB();
}

void
ieee80211_free_allnodes(struct ieee80211com *ic, int clear_ic_bss)
{
	(void)ic; (void)clear_ic_bss;
	STUB();
}

void
ieee80211_addba_req_accept(struct ieee80211com *ic, struct ieee80211_node *ni,
    uint8_t tid)
{
	(void)ic; (void)ni; (void)tid;
	STUB();
}

void
ieee80211_addba_req_refuse(struct ieee80211com *ic, struct ieee80211_node *ni,
    uint8_t tid)
{
	(void)ic; (void)ni; (void)tid;
	STUB();
}
