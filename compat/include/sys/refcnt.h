/*
 * QwzWifi compat: OpenBSD refcnt(9), real signatures.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef _QWZMAC_SYS_REFCNT_H_
#define _QWZMAC_SYS_REFCNT_H_

struct refcnt {
	unsigned int	r_refs;
	int		r_traceidx;
};

#define REFCNT_INITIALIZER()		{ .r_refs = 1, .r_traceidx = 0 }

void	refcnt_init(struct refcnt *);
#define refcnt_init_trace(r, idx)	refcnt_init(r)
void	refcnt_take(struct refcnt *);
int	refcnt_rele(struct refcnt *);
void	refcnt_rele_wake(struct refcnt *);
void	refcnt_finalize(struct refcnt *, const char *);
unsigned int	refcnt_read(const struct refcnt *);

#define refcnt_shared(_r) (refcnt_read((_r)) > 1)

#endif /* _QWZMAC_SYS_REFCNT_H_ */
