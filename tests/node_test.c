/* Real net80211 node cleanup/copy/free with the user-space timer backend.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <net/if.h>
#include <net/if_media.h>
#include <netinet/in.h>
#include <netinet/if_ether.h>
#include <net80211/ieee80211_var.h>

#include "kernel_api.h"
void ieee80211_node_free(struct ieee80211com *, struct ieee80211_node *);
void ieee80211_node_copy(struct ieee80211com *, struct ieee80211_node *,
    const struct ieee80211_node *);

static int callbacks, unexpected;

/* Protocol callbacks are outside this lifetime test; none may be invoked. */
void ieee80211_eapol_timeout(void *p) { (void)p; unexpected++; }
void ieee80211_sa_query_timeout(void *p) { (void)p; unexpected++; }
int ieee80211_addba_request(struct ieee80211com *ic,
    struct ieee80211_node *ni, uint16_t ssn, uint8_t tid)
{ (void)ic; (void)ni; (void)ssn; (void)tid; unexpected++; return 0; }
int ieee80211_save_ie(const uint8_t *ie, uint8_t **dst)
{ (void)ie; (void)dst; unexpected++; return ENOMEM; }

static void
count_callback(void *p)
{
	(void)p;
	callbacks++;
}

static void
free_from_callback(void *p)
{
	qwzmac_spl_lock();
	ieee80211_node_free(NULL, p);
	callbacks++;
	qwzmac_spl_unlock();
}

static void
free_during_barrier(void *p)
{
	qwzmac_spl_lock();
	/* Let the external barrier borrow backing before self-destruction. */
	while (!test_timer_waiting())
		qwzmac_sleep(p, 1000000);
	timeout_destroy(p);
	callbacks++;
	qwzmac_spl_unlock();
}

static struct ieee80211_node *
new_node(void)
{
	struct ieee80211_node *ni;

	ni = malloc(sizeof(*ni), M_DEVBUF, M_WAITOK | M_ZERO);
	mq_init(&ni->ni_savedq, IEEE80211_PS_MAX_QUEUE, IPL_NET);
	return ni;
}

static void
arm(struct timeout *to)
{
	timeout_set(to, count_callback, NULL);
	timeout_add_sec(to, 60);
}

void
test_node_timers(int *checks, int *failures)
{
	struct ieee80211_node *ni, *src, *dst;
	unsigned baseline = test_timer_live();
	struct timeout to;
	int i;

#define CHECK(c) do { (*checks)++; if (!(c)) { (*failures)++; \
	qwzmac_log("FAIL node_test:%d: %s\n", __LINE__, #c); } } while (0)
	ni = new_node();
	/* Include allocated-but-cancelled timers, BA_INIT, every TID/family. */
	for (i = 0; i < IEEE80211_NUM_TID; i++) {
		arm(&ni->ni_addba_req_to[i]);
		timeout_del(&ni->ni_addba_req_to[i]);
		arm(&ni->ni_tx_ba[i].ba_to);
		arm(&ni->ni_rx_ba[i].ba_to);
		arm(&ni->ni_rx_ba[i].ba_gap_to);
	}
	arm(&ni->ni_eapol_to);
	arm(&ni->ni_sa_query_to);
	CHECK(test_timer_live() == baseline + 4 * IEEE80211_NUM_TID + 2);
	ieee80211_node_cleanup(NULL, ni);
	CHECK(test_timer_live() == baseline);
	for (i = 0; i < 20; i++) {
		arm(&ni->ni_rx_ba[0].ba_gap_to);
		ieee80211_node_cleanup(NULL, ni);
	}
	CHECK(test_timer_live() == baseline); /* same owner address reused */
	ieee80211_node_free(NULL, ni); /* repeated cleanup is harmless */
	CHECK(test_timer_live() == baseline && callbacks == 0);

	/* Copy destroys only the old destination's backing, never the source. */
	src = new_node(); dst = new_node();
	arm(&src->ni_eapol_to); arm(&dst->ni_eapol_to);
	ieee80211_node_copy(NULL, dst, src);
	CHECK(test_timer_live() == baseline + 1);
	timeout_add_msec(&src->ni_eapol_to, 1);
	delay(50000);
	qwzmac_spl_lock();
	CHECK(callbacks == 1);
	qwzmac_spl_unlock();
	ieee80211_node_free(NULL, dst);
	ieee80211_node_free(NULL, src);
	CHECK(test_timer_live() == baseline);

	/* Dispatch has started, but the callback cannot acquire our spl lock. */
	ni = new_node();
	qwzmac_spl_lock();
	test_timer_pause_entry(1);
	timeout_set(&ni->ni_eapol_to, count_callback, NULL);
	timeout_add_msec(&ni->ni_eapol_to, 1);
	qwzmac_spl_unlock();
	for (i = 0; i < 1000 && !test_timer_active(); i++)
		delay(1000);
	qwzmac_spl_lock();
	CHECK(test_timer_active() == 1);
	test_timer_pause_entry(0);
	ieee80211_node_free(NULL, ni);
	CHECK(qwzmac_spl_held());
	CHECK(test_timer_live() == baseline && callbacks == 1);
	qwzmac_spl_unlock();

	/* No self-wait and no owner access on return from a deleting callback. */
	for (int process = 0; process < 2; process++) {
		ni = new_node();
		if (process)
			timeout_set_proc(&ni->ni_eapol_to, free_from_callback, ni);
		else
			timeout_set(&ni->ni_eapol_to, free_from_callback, ni);
		timeout_add_msec(&ni->ni_eapol_to, 1);
		for (i = 0; i < 1000; i++) {
			qwzmac_spl_lock();
			int done = callbacks == 2 + process && test_timer_live() == baseline;
			qwzmac_spl_unlock();
			if (done) break;
			delay(1000);
		}
		CHECK(callbacks == 2 + process && test_timer_live() == baseline);
	}
	/* An external waiter must keep backing alive across self-destruction. */
	test_timer_reset_waiting();
	timeout_set_proc(&to, free_during_barrier, &to);
	timeout_add_msec(&to, 1);
	for (i = 0; i < 1000 && !test_timer_active(); i++)
		delay(1000);
	CHECK(test_timer_active() == 1);
	timeout_barrier(&to);
	CHECK(callbacks == 4 && test_timer_live() == baseline);
	CHECK(unexpected == 0);
#undef CHECK
}
