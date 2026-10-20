/*	$OpenBSD: uipc_mbuf.c,v 1.307 2026/07/03 11:51:57 dlg Exp $	*/
/*	$NetBSD: uipc_mbuf.c,v 1.15.4.1 1996/06/13 17:11:44 cgd Exp $	*/

/*
 * Copyright (c) 1982, 1986, 1988, 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)uipc_mbuf.c	8.2 (Berkeley) 1/4/94
 */

/*
 *	@(#)COPYRIGHT	1.1 (NRL) 17 January 1995
 *
 * NRL grants permission for redistribution and use in source and binary
 * forms, with or without modification, of the software and documentation
 * created at NRL provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgements:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 *	This product includes software developed at the Information
 *	Technology Division, US Naval Research Laboratory.
 * 4. Neither the name of the NRL nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THE SOFTWARE PROVIDED BY NRL IS PROVIDED BY NRL AND CONTRIBUTORS ``AS
 * IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL NRL OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * The views and conclusions contained in the software and documentation
 * are those of the authors and should not be interpreted as representing
 * official policies, either expressed or implied, of the US Naval
 * Research Laboratory (NRL).
 */

/*
 * QWZMAC: malloc-backed mbufs/clusters and shared-cluster lifetime, without
 * OpenBSD pools, tags or pf state. Derived from the pinned uipc_mbuf.c;
 * chain/list algorithms live in openbsd/sys/kern/uipc_mbuf.c + its patch.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/atomic.h>
#include <sys/mbuf.h>
#include <sys/refcnt.h>
#include <qwzmac/mbuf.h>

#include <sys/socket.h>
#include <net/if.h>

#define IFQ_DEFPRIO			3

int max_linkhdr = 16;		/* QWZMAC: ether + llc headroom */
int max_protohdr = 40;
int max_hdr = 56;

CTASSERT(sizeof(struct mbuf) == MSIZE);

static void		m_extfree_heap(caddr_t, u_int, void *);
static void		m_extfree_refs(caddr_t, u_int, void *);
u_int			m_extfree_refs_fn = 1;

void			m_zero(struct mbuf *);

/* QWZMAC: MEXTFREE_POOL (0) means "cluster from malloc(9)". */
static void (*mextfree_fns[4])(caddr_t, u_int, void *) = {
	m_extfree_heap, m_extfree_refs,
};
static u_int num_extfree_fns = 2;

static int
m_how(int how)
{
	return how == M_WAIT ? M_WAITOK : M_NOWAIT;
}

struct mbuf *
m_get(int nowait, int type)
{
	struct mbuf *m;

	KASSERT(type >= 0 && type < MT_NTYPES);

	m = malloc(MSIZE, M_DEVBUF, m_how(nowait) | M_CANFAIL);
	if (m == NULL)
		return (NULL);

	m->m_type = type;
	m->m_next = NULL;
	m->m_nextpkt = NULL;
	m->m_data = m->m_dat;
	m->m_flags = 0;
	m->m_len = 0;

	return (m);
}

struct mbuf *
m_gethdr(int nowait, int type)
{
	struct mbuf *m;

	KASSERT(type >= 0 && type < MT_NTYPES);

	m = malloc(MSIZE, M_DEVBUF, m_how(nowait) | M_CANFAIL);
	if (m == NULL)
		return (NULL);

	m->m_type = type;
	m->m_len = 0;

	return (m_inithdr(m));
}

struct mbuf *
m_inithdr(struct mbuf *m)
{
	m->m_next = NULL;
	m->m_nextpkt = NULL;
	m->m_data = m->m_pktdat;
	m->m_flags = M_PKTHDR;
	memset(&m->m_pkthdr, 0, sizeof(m->m_pkthdr));
	m->m_pkthdr.pf.prio = IFQ_DEFPRIO;

	return (m);
}

static inline void
m_clearhdr(struct mbuf *m)
{
	memset(&m->m_pkthdr, 0, sizeof(m->m_pkthdr));
}

void
m_removehdr(struct mbuf *m)
{
	KASSERT(m->m_flags & M_PKTHDR);
	m_clearhdr(m);
	m->m_flags &= ~M_PKTHDR;
}

void
m_resethdr(struct mbuf *m)
{
	int len = m->m_pkthdr.len;
	u_int8_t loopcnt = m->m_pkthdr.ph_loopcnt;

	KASSERT(m->m_flags & M_PKTHDR);
	m->m_flags &= (M_EXT|M_PKTHDR|M_EOR|M_EXTWR|M_ZEROIZE);
	m_clearhdr(m);
	m->m_pkthdr.pf.prio = IFQ_DEFPRIO;
	m->m_pkthdr.len = len;
	m->m_pkthdr.ph_loopcnt = loopcnt;
}

void
m_calchdrlen(struct mbuf *m)
{
	struct mbuf *n;
	int plen = 0;

	KASSERT(m->m_flags & M_PKTHDR);
	for (n = m; n; n = n->m_next)
		plen += n->m_len;
	m->m_pkthdr.len = plen;
}

struct mbuf *
m_getclr(int nowait, int type)
{
	struct mbuf *m;

	MGET(m, nowait, type);
	if (m == NULL)
		return (NULL);
	memset(mtod(m, caddr_t), 0, MLEN);
	return (m);
}

struct mbuf *
m_clget(struct mbuf *m, int how, u_int pktlen)
{
	struct mbuf *m0 = NULL;
	caddr_t buf;

	if (pktlen == 0 || pktlen > MAXMCLBYTES) {
		panic("m_clget: request for %u byte cluster", pktlen);
		return (NULL);
	}

	if (m == NULL) {
		m0 = m_gethdr(how, MT_DATA);
		if (m0 == NULL)
			return (NULL);

		m = m0;
	}
	buf = malloc(pktlen, M_DEVBUF, m_how(how) | M_CANFAIL);
	if (buf == NULL) {
		m_freem(m0);
		return (NULL);
	}

	MEXTADD(m, buf, pktlen, M_EXTWR, MEXTFREE_POOL, NULL);
	return (m);
}

static void
m_extfree_heap(caddr_t buf, u_int size, void *arg)
{
	(void)arg;
	free(buf, M_DEVBUF, size);
}

int
m_ext_refs_shared(struct mbuf *m)
{
	struct m_ext_refs *mrefs = m->m_ext.ext_arg;

	return (refcnt_shared(&mrefs->refs));
}

static void
m_extfree_refs(caddr_t buf, u_int size, void *arg)
{
	struct m_ext_refs *mrefs = arg;

	if (refcnt_rele(&mrefs->refs)) {
		if (mrefs->zero)
			explicit_bzero(buf, size);

		KASSERT(mrefs->free_fn < num_extfree_fns);
		KASSERT(mrefs->free_fn != m_extfree_refs_fn);

		mextfree_fns[mrefs->free_fn](buf, size, mrefs->arg);

		free(mrefs, M_DEVBUF, sizeof(*mrefs));
	}
}

struct mbuf *
m_free(struct mbuf *m)
{
	struct mbuf *n;

	if (m == NULL)
		return (NULL);

	n = m->m_next;
	if (m->m_flags & M_ZEROIZE) {
		m_zero(m);
		if (n)
			n->m_flags |= M_ZEROIZE;
	}
	if (m->m_flags & M_EXT)
		m_extfree(m);

	free(m, M_DEVBUF, MSIZE);

	return (n);
}

int
m_extref(struct mbuf *m, struct mbuf *n, int how)
{
	struct m_ext_refs *mrefs;

	if (m->m_ext.ext_free_fn == m_extfree_refs_fn)
		mrefs = m->m_ext.ext_arg;
	else {
		mrefs = malloc(sizeof(*mrefs), M_DEVBUF,
		    m_how(how) | M_CANFAIL);
		if (mrefs == NULL)
			return (ENOMEM);

		refcnt_init(&mrefs->refs);
		mrefs->arg = m->m_ext.ext_arg;
		mrefs->free_fn = m->m_ext.ext_free_fn;
		mrefs->zero = 0;

		m->m_ext.ext_arg = mrefs;
		m->m_ext.ext_free_fn = m_extfree_refs_fn;
	}

	refcnt_take(&mrefs->refs);

	MEXTADD(n, m->m_ext.ext_buf, m->m_ext.ext_size,
	    m->m_flags & M_EXTWR, m_extfree_refs_fn, mrefs);

	return (0);
}

u_int
mextfree_register(void (*fn)(caddr_t, u_int, void *))
{
	KASSERT(num_extfree_fns < nitems(mextfree_fns));
	mextfree_fns[num_extfree_fns] = fn;
	return num_extfree_fns++;
}

void
m_extfree(struct mbuf *m)
{
	KASSERT(m->m_ext.ext_free_fn < num_extfree_fns);
	mextfree_fns[m->m_ext.ext_free_fn](m->m_ext.ext_buf,
	    m->m_ext.ext_size, m->m_ext.ext_arg);

	m->m_flags &= ~(M_EXT|M_EXTWR);
}

struct mbuf *
m_freem(struct mbuf *m)
{
	struct mbuf *n;

	if (m == NULL)
		return (NULL);

	n = m->m_nextpkt;

	do
		m = m_free(m);
	while (m != NULL);

	return (n);
}

void
m_purge(struct mbuf *m)
{
	while (m != NULL)
		m = m_freem(m);
}

/* QWZMAC: helper used by the bus_dma bounce code. */
unsigned int
m_length(struct mbuf *m)
{
	unsigned int len = 0;

	for (; m != NULL; m = m->m_next)
		len += m->m_len;
	return len;
}
