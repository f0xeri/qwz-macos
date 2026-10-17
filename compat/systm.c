/*
 * QwzWifi compat: printf/panic, malloc(9), tsleep(9)/wakeup(9), time,
 * randomness, loadfirmware(9) and small libc bits.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/device.h>
#include <sys/time.h>
#include <netinet/if_ether.h>

int cold = 0;

int
qwzmac_printf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	qwzmac_vlog(fmt, ap);
	va_end(ap);
	return 0;
}

void
qwzmac_panic(const char *file, int line, const char *fmt, ...)
{
	char buf[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	qwzmac_log("QwzWifi: PANIC (not halting) at %s:%d: %s\n",
	    file, line, buf);
	qwzmac_backtrace();
}

/*
 * malloc(9). A 16-byte header keeps the block size so free(9) does not
 * depend on the size the caller passes (OpenBSD allows 0 = unknown).
 */
#define QWZMAC_MALLOC_MAGIC	0x717a6d61u	/* "qzma" */

struct malloc_hdr {
	uint32_t	magic;
	uint32_t	pad;
	uint64_t	size;
};

void *
malloc(size_t size, int type, int flags)
{
	struct malloc_hdr *h;

	(void)type;
	if (size > SIZE_MAX - sizeof(*h))
		return NULL;
	h = qwzmac_malloc(sizeof(*h) + size, (flags & M_NOWAIT) == 0);
	if (h == NULL) {
		if ((flags & (M_NOWAIT | M_CANFAIL)) == 0)
			printf("QwzWifi: malloc(%zu) failed (M_WAITOK)\n",
			    size);
		return NULL;
	}
	h->magic = QWZMAC_MALLOC_MAGIC;
	h->size = size;
	if (flags & M_ZERO)
		memset(h + 1, 0, size);
	return h + 1;
}

void *
mallocarray(size_t nmemb, size_t size, int type, int flags)
{
	if (size != 0 && nmemb > SIZE_MAX / size) {
		printf("QwzWifi: mallocarray overflow %zu * %zu\n",
		    nmemb, size);
		return NULL;
	}
	return malloc(nmemb * size, type, flags);
}

void
free(void *addr, int type, size_t freedsize)
{
	struct malloc_hdr *h;

	(void)type;
	if (addr == NULL)
		return;
	h = (struct malloc_hdr *)addr - 1;
	if (h->magic != QWZMAC_MALLOC_MAGIC) {
		printf("QwzWifi: free(%p): bad magic 0x%x, leaking\n",
		    addr, h->magic);
		qwzmac_backtrace();
		return;
	}
	if (freedsize != 0 && freedsize != h->size)
		printf("QwzWifi: free(%p): size %zu, allocated %llu\n",
		    addr, freedsize, (unsigned long long)h->size);
	h->magic = 0;
	qwzmac_free(h, sizeof(*h) + h->size);
}

/*
 * tsleep(9). All sleeps happen on the global spl lock. If the caller does
 * not hold it, take it just around the sleep.
 */
int
tsleep_nsec(const volatile void *ident, int priority, const char *wmesg,
    uint64_t nsecs)
{
	int held, error;

	(void)priority;
	(void)wmesg;
	held = qwzmac_spl_held();
	if (!held)
		qwzmac_spl_lock();
	error = qwzmac_sleep((const void *)ident,
	    nsecs == INFSLP ? 0 : (nsecs == 0 ? 1 : nsecs));
	if (!held)
		qwzmac_spl_unlock();
	return error;
}

int
tsleep(const volatile void *ident, int priority, const char *wmesg,
    int timo)
{
	uint64_t nsecs = INFSLP;

	if (timo > 0)
		nsecs = (uint64_t)timo * (1000000000ULL / hz);
	return tsleep_nsec(ident, priority, wmesg, nsecs);
}

void
wakeup_n(const volatile void *ident, int n)
{
	qwzmac_spl_lock();
	qwzmac_wakeup((const void *)ident, n == 1);
	qwzmac_spl_unlock();
}

void
wakeup(const volatile void *ident)
{
	wakeup_n(ident, -1);
}

void
delay(int us)
{
	if (us > 0)
		qwzmac_delay_us((unsigned int)us);
}

/* Time. Only the uptime clock is meaningful for a driver. */
uint64_t
nsecuptime(void)
{
	return qwzmac_nsecuptime();
}

uint64_t
getnsecuptime(void)
{
	return qwzmac_nsecuptime();
}

time_t
getuptime(void)
{
	return (time_t)(qwzmac_nsecuptime() / 1000000000ULL);
}

void
microuptime(struct timeval *tv)
{
	NSEC_TO_TIMEVAL(qwzmac_nsecuptime(), tv);
}

void
getmicrouptime(struct timeval *tv)
{
	microuptime(tv);
}

void
nanouptime(struct timespec *ts)
{
	NSEC_TO_TIMESPEC(qwzmac_nsecuptime(), ts);
}

void
getnanouptime(struct timespec *ts)
{
	nanouptime(ts);
}

/* Wall clock is not needed; uptime is a monotonic stand-in. */
void
microtime(struct timeval *tv)
{
	microuptime(tv);
}

void
getmicrotime(struct timeval *tv)
{
	microuptime(tv);
}

void
nanotime(struct timespec *ts)
{
	nanouptime(ts);
}

time_t
gettime(void)
{
	return getuptime();
}

int
ratecheck(struct timeval *lasttime, const struct timeval *mininterval)
{
	struct timeval tv, delta;
	int rv = 0;

	getmicrouptime(&tv);
	timersub(&tv, lasttime, &delta);
	if (timercmp(&delta, mininterval, >=) ||
	    (lasttime->tv_sec == 0 && lasttime->tv_usec == 0)) {
		*lasttime = tv;
		rv = 1;
	}
	return rv;
}

int
ppsratecheck(struct timeval *lasttime, int *curpps, int maxpps)
{
	struct timeval tv, delta;
	int rv;

	microuptime(&tv);
	timersub(&tv, lasttime, &delta);
	if (maxpps == 0)
		return 0;
	if ((lasttime->tv_sec == 0 && lasttime->tv_usec == 0) ||
	    delta.tv_sec >= 1) {
		*lasttime = tv;
		*curpps = 0;
	}
	rv = (maxpps < 0 || *curpps < maxpps);
	if (*curpps + 1 > *curpps)
		*curpps = *curpps + 1;
	return rv;
}

/* Randomness. */
void
arc4random_buf(void *buf, size_t n)
{
	qwzmac_random_buf(buf, n);
}

uint32_t
arc4random(void)
{
	uint32_t v;

	qwzmac_random_buf(&v, sizeof(v));
	return v;
}

uint32_t
arc4random_uniform(uint32_t upper_bound)
{
	uint32_t r, min;

	if (upper_bound < 2)
		return 0;
	min = -upper_bound % upper_bound;
	for (;;) {
		r = arc4random();
		if (r >= min)
			break;
	}
	return r % upper_bound;
}

/* libc bits not exported by the macOS kernel. */
size_t
strnlen(const char *s, size_t maxlen)
{
	size_t n;

	for (n = 0; n < maxlen && s[n] != '\0'; n++)
		;
	return n;
}

void
explicit_bzero(void *p, size_t n)
{
	memset(p, 0, n);
	__asm__ volatile("" : : "r"(p) : "memory");
}

int
timingsafe_bcmp(const void *b1, const void *b2, size_t n)
{
	const unsigned char *p1 = b1, *p2 = b2;
	int ret = 0;

	for (; n > 0; n--)
		ret |= *p1++ ^ *p2++;
	return (ret != 0);
}

/* Ethernet helpers. */
u_int8_t etherbroadcastaddr[ETHER_ADDR_LEN] =
    { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
u_int8_t etheranyaddr[ETHER_ADDR_LEN] =
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

char *
ether_sprintf(u_char *ap)
{
	static const char digits[] = "0123456789abcdef";
	static char etherbuf[4][ETHER_ADDR_LEN * 3];
	static unsigned int idx;
	char *cp;
	int i;

	/* A few rotating buffers so one printf can show several addresses. */
	cp = etherbuf[__atomic_fetch_add(&idx, 1, __ATOMIC_RELAXED) & 3];
	for (i = 0; i < ETHER_ADDR_LEN; i++) {
		*cp++ = digits[*ap >> 4];
		*cp++ = digits[*ap++ & 0xf];
		*cp++ = ':';
	}
	*--cp = '\0';
	return cp - (ETHER_ADDR_LEN * 3 - 1);
}

/*
 * loadfirmware(9). The blobs are linked into the kext; hand out a malloc'd
 * copy because qwz frees it with free(9).
 */
int
loadfirmware(const char *name, u_char **bufp, size_t *buflen)
{
	const uint8_t *data;
	size_t len;
	u_char *buf;
	int error;

	error = qwzmac_firmware_get(name, &data, &len);
	if (error) {
		printf("QwzWifi: firmware %s not embedded\n", name);
		return error;
	}
	buf = malloc(len, M_DEVBUF, M_WAITOK | M_CANFAIL);
	if (buf == NULL)
		return ENOMEM;
	memcpy(buf, data, len);
	*bufp = buf;
	*buflen = len;
	printf("QwzWifi: loadfirmware %s: %zu bytes\n", name, len);
	return 0;
}
