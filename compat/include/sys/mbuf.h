/*
 * QwzWifi compat: OpenBSD mbuf(9) with the real field names and signatures.
 *
 * Differences from OpenBSD: clusters are plain kernel heap blocks (no pools)
 * and there are no mbuf tags. No mbuf memory is ever handed to the device
 * directly. bus_dmamap_load_mbuf() bounces the
 * data through DMA memory below 4 GB (compat/bus_dma.c).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef _QWZMAC_SYS_MBUF_H_
#define _QWZMAC_SYS_MBUF_H_

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/malloc.h>
#include <sys/mutex.h>

#define	MSIZE		256
#define	MLEN		(MSIZE - sizeof(struct m_hdr))
#define	MHLEN		(MLEN - sizeof(struct pkthdr))
#define	MAXMCLBYTES	(64 * 1024)
#define	MINCLSIZE	(MHLEN + MLEN + 1)
#define	MCLSHIFT	11
#define	MCLBYTES	(1 << MCLSHIFT)

struct m_tag {
	SLIST_ENTRY(m_tag)	m_tag_link;
	u_int16_t		m_tag_id;
	u_int16_t		m_tag_len;
};

#define	mtod(m,t)	((t)((m)->m_data))

struct m_hdr {
	struct	mbuf *mh_next;
	struct	mbuf *mh_nextpkt;
	caddr_t	mh_data;
	u_int	mh_len;
	short	mh_type;
	u_short	mh_flags;
};

struct pkthdr_pf {
	void		*statekey;
	void		*inp;
	u_int32_t	 qid;
	u_int16_t	 tag;
	u_int16_t	 delay;
	u_int8_t	 flags;
	u_int8_t	 routed;
	u_int8_t	 prio;
	u_int8_t	 pad[1];
};

struct	pkthdr {
	void			*ph_cookie;
	SLIST_HEAD(, m_tag)	 ph_tags;
	int64_t			 ph_timestamp;
	int			 len;
	u_int			 ph_rtableid;
	u_int			 ph_ifidx;
	u_int16_t		 ph_tagsset;
	u_int16_t		 ph_flowid;
	u_int16_t		 csum_flags;
	u_int16_t		 ether_vtag;
	u_int16_t		 ph_mss;
	u_int8_t		 ph_loopcnt;
	u_int8_t		 ph_family;
	struct pkthdr_pf	 pf;
};

struct mbuf_ext {
	caddr_t	ext_buf;
	void	*ext_arg;
	u_int	ext_free_fn;
	u_int	ext_size;
};

struct mbuf {
	struct	m_hdr m_hdr;
	union {
		struct {
			struct	pkthdr MH_pkthdr;
			union {
				struct	mbuf_ext MH_ext;
				char	MH_databuf[MHLEN];
			} MH_dat;
		} MH;
		char	M_databuf[MLEN];
	} M_dat;
};
#define	m_next		m_hdr.mh_next
#define	m_len		m_hdr.mh_len
#define	m_data		m_hdr.mh_data
#define	m_type		m_hdr.mh_type
#define	m_flags		m_hdr.mh_flags
#define	m_nextpkt	m_hdr.mh_nextpkt
#define	m_pkthdr	M_dat.MH.MH_pkthdr
#define	m_ext		M_dat.MH.MH_dat.MH_ext
#define	m_pktdat	M_dat.MH.MH_dat.MH_databuf
#define	m_dat		M_dat.M_databuf

#define	M_EXT		0x0001
#define	M_PKTHDR	0x0002
#define	M_EOR		0x0004
#define M_EXTWR		0x0008
#define	M_PROTO1	0x0010
#define M_VLANTAG	0x0020
#define M_LOOP		0x0040
#define M_BCAST		0x0100
#define M_MCAST		0x0200
#define M_CONF		0x0400
#define M_AUTH		0x0800
#define M_TUNNEL	0x1000
#define M_ZEROIZE	0x2000
#define M_COMP		0x4000
#define M_LINK0		0x8000

#define	M_COPYFLAGS	(M_PKTHDR|M_EOR|M_PROTO1|M_BCAST|M_MCAST|M_CONF|M_COMP|\
			 M_AUTH|M_LOOP|M_TUNNEL|M_LINK0|M_VLANTAG|M_ZEROIZE)

#define	M_IPV4_CSUM_OUT		0x0001
#define	M_TCP_CSUM_OUT		0x0002
#define	M_UDP_CSUM_OUT		0x0004
#define	M_IPV4_CSUM_IN_OK	0x0008
#define	M_IPV4_CSUM_IN_BAD	0x0010
#define	M_TCP_CSUM_IN_OK	0x0020
#define	M_TCP_CSUM_IN_BAD	0x0040
#define	M_UDP_CSUM_IN_OK	0x0080
#define	M_UDP_CSUM_IN_BAD	0x0100
#define	M_ICMP_CSUM_OUT		0x0200
#define	M_ICMP_CSUM_IN_OK	0x0400
#define	M_ICMP_CSUM_IN_BAD	0x0800
#define	M_IPV6_DF_OUT		0x1000
#define	M_TIMESTAMP		0x2000
#define	M_FLOWID		0x4000
#define	M_TCP_TSO		0x8000

#define	MT_FREE		0
#define	MT_DATA		1
#define	MT_HEADER	2
#define	MT_SONAME	3
#define	MT_SOOPTS	4
#define	MT_FTABLE	5
#define	MT_CONTROL	6
#define	MT_OOBDATA	7
#define	MT_NTYPES	8

#define	M_DONTWAIT	M_NOWAIT
#define	M_WAIT		M_WAITOK

#define MGET(m, how, type) m = m_get((how), (type))
#define MGETHDR(m, how, type) m = m_gethdr((how), (type))

int	m_ext_refs_shared(struct mbuf *);

static inline int
m_extreferenced(struct mbuf *m)
{
	extern u_int m_extfree_refs_fn;

	if (m->m_ext.ext_free_fn != m_extfree_refs_fn)
		return (0);

	return (m_ext_refs_shared(m));
}

#define	MCLISREFERENCED(m)	m_extreferenced(m)
#define	MCLINITREFERENCE(m)	do { } while (0)

#define	MEXTADD(m, buf, size, mflags, freefn, arg) do {			\
	(m)->m_data = (m)->m_ext.ext_buf = (caddr_t)(buf);		\
	(m)->m_flags |= M_EXT | (mflags & M_EXTWR);			\
	(m)->m_ext.ext_size = (size);					\
	(m)->m_ext.ext_free_fn = (freefn);				\
	(m)->m_ext.ext_arg = (arg);					\
	MCLINITREFERENCE(m);						\
} while (/* CONSTCOND */ 0)

u_int mextfree_register(void (*)(caddr_t, u_int, void *));
#define	MEXTFREE_POOL 0

#define MCLGET(m, how) (void) m_clget((m), (how), MCLBYTES)
#define MCLGETL(m, how, l) m_clget((m), (how), (l))

#define M_MOVE_HDR(to, from) do {					\
	(to)->m_pkthdr = (from)->m_pkthdr;				\
	(from)->m_flags &= ~M_PKTHDR;					\
	SLIST_INIT(&(from)->m_pkthdr.ph_tags);				\
	(from)->m_pkthdr.pf.statekey = NULL;				\
} while (/* CONSTCOND */ 0)

#define	M_MOVE_PKTHDR(to, from) do {					\
	(to)->m_flags = ((to)->m_flags & (M_EXT | M_EXTWR));		\
	(to)->m_flags |= (from)->m_flags & M_COPYFLAGS;			\
	M_MOVE_HDR((to), (from));					\
	if (((to)->m_flags & M_EXT) == 0)				\
		(to)->m_data = (to)->m_pktdat;				\
} while (/* CONSTCOND */ 0)

#define	M_READONLY(m)							\
	(((m)->m_flags & M_EXT) != 0 &&					\
	  (((m)->m_flags & M_EXTWR) == 0 || MCLISREFERENCED(m)))

#define	M_PREPEND(m, plen, how) \
		(m) = m_prepend((m), (plen), (how))

#define	M_COPYALL	1000000000

struct	mbuf *m_copym(struct mbuf *, int, int, int);
struct	mbuf *m_free(struct mbuf *);
struct	mbuf *m_get(int, int);
struct	mbuf *m_getclr(int, int);
struct	mbuf *m_gethdr(int, int);
struct	mbuf *m_inithdr(struct mbuf *);
void	m_removehdr(struct mbuf *);
void	m_resethdr(struct mbuf *);
void	m_calchdrlen(struct mbuf *);
int	m_defrag(struct mbuf *, int);
struct	mbuf *m_prepend(struct mbuf *, int, int);
struct	mbuf *m_pulldown(struct mbuf *, int, int, int *);
struct	mbuf *m_pullup(struct mbuf *, int);
struct	mbuf *m_split(struct mbuf *, int, int);
struct	mbuf *m_makespace(struct mbuf *, int, int, int *);
struct  mbuf *m_getptr(struct mbuf *, int, int *);
int	m_leadingspace(struct mbuf *);
int	m_trailingspace(struct mbuf *);
void	m_align(struct mbuf *, int);
struct mbuf *m_clget(struct mbuf *, int, u_int);
void	m_adj(struct mbuf *, int);
int	m_copyback(struct mbuf *, int, int, const void *, int);
struct mbuf *m_freem(struct mbuf *);
void	m_purge(struct mbuf *);
void	m_copydata(struct mbuf *, int, int, void *);
void	m_cat(struct mbuf *, struct mbuf *);
struct mbuf *m_devget(char *, int, int);
struct mbuf *m_dup_pkt(struct mbuf *, unsigned int, int);
int	m_dup_pkthdr(struct mbuf *, struct mbuf *, int);
unsigned int m_length(struct mbuf *);	/* QWZMAC: helper */
void	m_zero(struct mbuf *);
int	m_apply(struct mbuf *, int, int,
	    int (*)(caddr_t, caddr_t, unsigned int), caddr_t);

static inline struct mbuf *
m_freemp(struct mbuf **mp)
{
	struct mbuf *m = *mp;

	*mp = NULL;
	return m_freem(m);
}

struct mbuf_list {
	struct mbuf		*ml_head;
	struct mbuf		*ml_tail;
	u_int			ml_len;
};

struct mbuf_queue {
	struct mutex		mq_mtx;
	struct mbuf_list	mq_list;
	u_int			mq_maxlen;
	u_int			mq_drops;
};

#define MBUF_LIST_INITIALIZER() { NULL, NULL, 0 }

void			ml_init(struct mbuf_list *);
void			ml_enqueue(struct mbuf_list *, struct mbuf *);
struct mbuf *		ml_dequeue(struct mbuf_list *);
void			ml_enlist(struct mbuf_list *, struct mbuf_list *);
struct mbuf *		ml_dechain(struct mbuf_list *);
unsigned int		ml_purge(struct mbuf_list *);
unsigned int		ml_hdatalen(struct mbuf_list *);

#define	ml_len(_ml)		((_ml)->ml_len)
#define	ml_empty(_ml)		((_ml)->ml_len == 0)

#define MBUF_LIST_FIRST(_ml)	((_ml)->ml_head)
#define MBUF_LIST_NEXT(_m)	((_m)->m_nextpkt)

#define MBUF_LIST_FOREACH(_ml, _m)					\
	for ((_m) = MBUF_LIST_FIRST(_ml);				\
	    (_m) != NULL;						\
	    (_m) = MBUF_LIST_NEXT(_m))

#define MBUF_QUEUE_INITIALIZER(_maxlen, _ipl) \
    { MUTEX_INITIALIZER(_ipl), MBUF_LIST_INITIALIZER(), (_maxlen), 0 }

#ifndef READ_ONCE
#define READ_ONCE(x)	(*(volatile __typeof(x) *)&(x))
#define WRITE_ONCE(x, v) (*(volatile __typeof(x) *)&(x) = (v))
#endif

void			mq_init(struct mbuf_queue *, u_int, int);
int			mq_push(struct mbuf_queue *, struct mbuf *);
int			mq_enqueue(struct mbuf_queue *, struct mbuf *);
struct mbuf *		mq_dequeue(struct mbuf_queue *);
int			mq_enlist(struct mbuf_queue *, struct mbuf_list *);
void			mq_delist(struct mbuf_queue *, struct mbuf_list *);
unsigned int		mq_purge(struct mbuf_queue *);
unsigned int		mq_hdatalen(struct mbuf_queue *);
void			mq_set_maxlen(struct mbuf_queue *, u_int);

#define mq_len(_mq)		READ_ONCE((_mq)->mq_list.ml_len)
#define mq_empty(_mq)		(mq_len(_mq) == 0)
#define mq_full(_mq)		(mq_len((_mq)) >= READ_ONCE((_mq)->mq_maxlen))
#define mq_drops(_mq)		READ_ONCE((_mq)->mq_drops)

#endif /* _QWZMAC_SYS_MBUF_H_ */
