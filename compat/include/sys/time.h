/*
 * QwzWifi compat: OpenBSD <sys/time.h> (kernel subset).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef _QWZMAC_SYS_TIME_H_
#define _QWZMAC_SYS_TIME_H_

#include <sys/types.h>

struct timeval {
	time_t		tv_sec;
	long		tv_usec;
};

struct timespec {
	time_t		tv_sec;
	long		tv_nsec;
};

#define timerclear(tvp)		(tvp)->tv_sec = (tvp)->tv_usec = 0
#define timerisset(tvp)		((tvp)->tv_sec || (tvp)->tv_usec)
#define timercmp(tvp, uvp, cmp)						\
	(((tvp)->tv_sec == (uvp)->tv_sec) ?				\
	    ((tvp)->tv_usec cmp (uvp)->tv_usec) :			\
	    ((tvp)->tv_sec cmp (uvp)->tv_sec))
#define timeradd(tvp, uvp, vvp)						\
	do {								\
		(vvp)->tv_sec = (tvp)->tv_sec + (uvp)->tv_sec;		\
		(vvp)->tv_usec = (tvp)->tv_usec + (uvp)->tv_usec;	\
		if ((vvp)->tv_usec >= 1000000) {			\
			(vvp)->tv_sec++;				\
			(vvp)->tv_usec -= 1000000;			\
		}							\
	} while (0)
#define timersub(tvp, uvp, vvp)						\
	do {								\
		(vvp)->tv_sec = (tvp)->tv_sec - (uvp)->tv_sec;		\
		(vvp)->tv_usec = (tvp)->tv_usec - (uvp)->tv_usec;	\
		if ((vvp)->tv_usec < 0) {				\
			(vvp)->tv_sec--;				\
			(vvp)->tv_usec += 1000000;			\
		}							\
	} while (0)

static inline uint64_t
SEC_TO_NSEC(uint64_t sec)
{
	if (sec > UINT64_MAX / 1000000000ULL)
		return UINT64_MAX;
	return sec * 1000000000ULL;
}

static inline uint64_t
MSEC_TO_NSEC(uint64_t ms)
{
	if (ms > UINT64_MAX / 1000000ULL)
		return UINT64_MAX;
	return ms * 1000000ULL;
}

static inline uint64_t
USEC_TO_NSEC(uint64_t us)
{
	if (us > UINT64_MAX / 1000ULL)
		return UINT64_MAX;
	return us * 1000ULL;
}

static inline uint64_t
TIMEVAL_TO_NSEC(const struct timeval *tv)
{
	return (uint64_t)tv->tv_sec * 1000000000ULL +
	    (uint64_t)tv->tv_usec * 1000ULL;
}

static inline void
NSEC_TO_TIMEVAL(uint64_t ns, struct timeval *tv)
{
	tv->tv_sec = ns / 1000000000ULL;
	tv->tv_usec = (ns % 1000000000ULL) / 1000;
}

static inline void
NSEC_TO_TIMESPEC(uint64_t ns, struct timespec *ts)
{
	ts->tv_sec = ns / 1000000000ULL;
	ts->tv_nsec = ns % 1000000000ULL;
}

uint64_t	getnsecuptime(void);
uint64_t	nsecuptime(void);
time_t		getuptime(void);
void		getmicrouptime(struct timeval *);
void		microuptime(struct timeval *);
void		getnanouptime(struct timespec *);
void		nanouptime(struct timespec *);
void		getmicrotime(struct timeval *);
void		microtime(struct timeval *);
void		nanotime(struct timespec *);
time_t		gettime(void);
int		ratecheck(struct timeval *, const struct timeval *);
int		ppsratecheck(struct timeval *, int *, int);

#endif /* _QWZMAC_SYS_TIME_H_ */
