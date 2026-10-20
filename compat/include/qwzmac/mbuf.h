/* Internal boundary between upstream mbuf algorithms and our heap allocator.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef QWZMAC_MBUF_H
#define QWZMAC_MBUF_H

#include <sys/refcnt.h>

struct m_ext_refs {
	void		*arg;
	u_int		 free_fn;
	u_int		 zero;
	struct refcnt	 refs;
};

extern u_int m_extfree_refs_fn;
extern int max_linkhdr;

struct mbuf;
int m_extref(struct mbuf *, struct mbuf *, int);
void m_extfree(struct mbuf *);

#endif
