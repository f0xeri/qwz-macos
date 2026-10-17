/*
 * QwzWifi compat: the small part of if(9)/ifq(9) a driver touches.
 * In stage 3 if_input()/link state are forwarded to the IOEthernetController
 * through the if_qwzmac_* hooks in struct ifnet.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/mbuf.h>
#include <sys/socket.h>

#include <net/if.h>
#include <netinet/in.h>
#include <netinet/if_ether.h>

static void
if_slowtimo(void *arg)
{
	struct ifnet *ifp = arg;
	int s = splnet();

	if (ifp->if_watchdog != NULL && ifp->if_timer > 0 &&
	    --ifp->if_timer == 0) {
		splx(s);
		ifp->if_watchdog(ifp);
		s = splnet();
	}
	if (ifp->if_flags & IFF_UP)
		timeout_add_sec(&ifp->if_slowtimo, 1);
	splx(s);
}

void
ifq_init(struct ifqueue *ifq, struct ifnet *ifp, unsigned int idx)
{
	(void)idx;
	ifq->ifq_if = ifp;
	ml_init(&ifq->ifq_list);
	ifq->ifq_len = 0;
	ifq->ifq_oactive = 0;
	ifq->ifq_maxlen = IFQ_MAXLEN;
	ifq->ifq_qdrops = 0;
}

int
ifq_enqueue(struct ifqueue *ifq, struct mbuf *m)
{
	int s = splnet();

	if (ifq->ifq_len >= ifq->ifq_maxlen) {
		ifq->ifq_qdrops++;
		splx(s);
		m_freem(m);
		return ENOBUFS;
	}
	ml_enqueue(&ifq->ifq_list, m);
	ifq->ifq_len++;
	splx(s);
	return 0;
}

struct mbuf *
ifq_dequeue(struct ifqueue *ifq)
{
	struct mbuf *m;
	int s = splnet();

	m = ml_dequeue(&ifq->ifq_list);
	if (m != NULL)
		ifq->ifq_len--;
	splx(s);
	return m;
}

unsigned int
ifq_purge(struct ifqueue *ifq)
{
	unsigned int n;
	int s = splnet();

	n = ml_purge(&ifq->ifq_list);
	ifq->ifq_len = 0;
	splx(s);
	return n;
}

void
if_attach(struct ifnet *ifp)
{
	ifq_init(&ifp->if_snd, ifp, 0);
	timeout_set(&ifp->if_slowtimo, if_slowtimo, ifp);
	ifp->if_link_state = LINK_STATE_DOWN;
	printf("%s: interface attached\n", ifp->if_xname);
}

void
if_detach(struct ifnet *ifp)
{
	timeout_destroy(&ifp->if_slowtimo);
	ifq_purge(&ifp->if_snd);
}

void
if_up(struct ifnet *ifp)
{
	ifp->if_flags |= IFF_UP;
	timeout_add_sec(&ifp->if_slowtimo, 1);
}

void
if_down(struct ifnet *ifp)
{
	ifp->if_flags &= ~IFF_UP;
	timeout_del(&ifp->if_slowtimo);
}

int
if_setlladdr(struct ifnet *ifp, const uint8_t *lladdr)
{
	struct arpcom *ac = (struct arpcom *)ifp;

	memcpy(ac->ac_enaddr, lladdr, ETHER_ADDR_LEN);
	printf("%s: MAC address %s\n", ifp->if_xname,
	    ether_sprintf(ac->ac_enaddr));
	return 0;
}

void
if_link_state_change(struct ifnet *ifp)
{
	printf("%s: link state %d\n", ifp->if_xname, ifp->if_link_state);
	if (ifp->if_qwzmac_link_state != NULL)
		ifp->if_qwzmac_link_state(ifp);
}

void
if_input(struct ifnet *ifp, struct mbuf_list *ml)
{
	struct mbuf *m;

	while ((m = ml_dequeue(ml)) != NULL) {
		ifp->if_ipackets++;
		if (ifp->if_qwzmac_input != NULL)
			ifp->if_qwzmac_input(ifp, m);
		else
			m_freem(m);
	}
}

int
if_enqueue(struct ifnet *ifp, struct mbuf *m)
{
	int error;

	error = ifq_enqueue(&ifp->if_snd, m);
	if (error)
		return error;
	if_start(ifp);
	return 0;
}

void
if_start(struct ifnet *ifp)
{
	if (ifp->if_start != NULL && !ifq_is_oactive(&ifp->if_snd))
		ifp->if_start(ifp);
}

int
if_output(struct ifnet *ifp, struct mbuf *m)
{
	return if_enqueue(ifp, m);
}

void
if_purge(struct ifnet *ifp)
{
	ifq_purge(&ifp->if_snd);
}

int
ifpromisc(struct ifnet *ifp, int pswitch)
{
	(void)ifp; (void)pswitch;
	return 0;
}

struct ifnet *
if_get(unsigned int idx)
{
	(void)idx;
	return NULL;
}

void
if_put(struct ifnet *ifp)
{
	(void)ifp;
}
