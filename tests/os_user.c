/*
 * QwzWifi tests: a user-space implementation of qwzmac/os.h (pthreads,
 * libc heap, fake physical addresses) so the compat C files can be unit-tested
 * without a kernel. Built without compat/include on the include path.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/random.h>
#include <errno.h>

#include "../compat/include/qwzmac/os.h"

/* Test knobs (see compat_test.c). */
int		test_dma_fake_high;	/* hand out a pa above 4 GB */
int		test_quiet;
unsigned	test_panics;

void
qwzmac_vlog(const char *fmt, va_list ap)
{
	if (!test_quiet)
		vprintf(fmt, ap);
}

void
qwzmac_log(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	qwzmac_vlog(fmt, ap);
	va_end(ap);
	if (strstr(fmt, "PANIC") != NULL)
		test_panics++;
}

void
qwzmac_backtrace(void)
{
}

void *
qwzmac_malloc(size_t size, int canwait)
{
	(void)canwait;
	return malloc(size);
}

void
qwzmac_free(void *p, size_t size)
{
	memset(p, 0xa5, size);	/* catch use after free */
	free(p);
}

uint64_t
qwzmac_nsecuptime(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

void
qwzmac_delay_us(unsigned int us)
{
	usleep(us);
}

struct thr {
	void	(*fn)(void *);
	void	*arg;
};

static void *
thr_main(void *p)
{
	struct thr t = *(struct thr *)p;

	free(p);
	t.fn(t.arg);
	return NULL;
}

unsigned long
qwzmac_thread_self(void)
{
	return (unsigned long)pthread_self();
}

int
qwzmac_thread_start(void (*fn)(void *), void *arg)
{
	struct thr *t = malloc(sizeof(*t));
	pthread_t th;

	t->fn = fn;
	t->arg = arg;
	if (pthread_create(&th, NULL, thr_main, t) != 0)
		return ENOMEM;
	pthread_detach(th);
	return 0;
}

/* Fake DMA: heap memory with made-up physical addresses below 4 GB. */
struct fake_dma {
	void		*va;
	size_t		 size;
};
static uint64_t next_pa = 0x10000000ULL;

int
qwzmac_dma_alloc(size_t size, size_t align, uint64_t lo, uint64_t hi,
    int canwait, void **cookie, void **va, uint64_t *pa)
{
	struct fake_dma *d;
	uint64_t p;

	(void)canwait; (void)lo;
	d = malloc(sizeof(*d));
	d->va = aligned_alloc(align, (size + align - 1) / align * align);
	d->size = size;
	p = (next_pa + align - 1) & ~((uint64_t)align - 1);
	next_pa = p + size;
	if (test_dma_fake_high)
		p += 0x100000000ULL;
	(void)hi;
	*cookie = d;
	*va = d->va;
	*pa = p;
	return 0;
}

void
qwzmac_dma_free(void *cookie)
{
	struct fake_dma *d = cookie;

	free(d->va);
	free(d);
}

/* Not exercised by the unit tests. */
uint32_t qwzmac_pci_conf_read(void *dev, int reg) { (void)dev; (void)reg; return 0; }
void qwzmac_pci_conf_write(void *dev, int reg, uint32_t val) { (void)dev; (void)reg; (void)val; }
int qwzmac_pci_map_bar(void *dev, int reg, uintptr_t *va, uint64_t *pa, uint64_t *size)
{ (void)dev; (void)reg; (void)va; (void)pa; (void)size; return ENXIO; }
void qwzmac_pci_unmap_bar(void *dev, int reg) { (void)dev; (void)reg; }
int qwzmac_pci_msi_index(void *dev) { (void)dev; return -1; }
void *qwzmac_pci_intr_establish(void *dev, int index, int (*fn)(void *), void *arg, const char *name)
{ (void)dev; (void)index; (void)fn; (void)arg; (void)name; return NULL; }
void qwzmac_pci_intr_disestablish(void *dev, void *cookie) { (void)dev; (void)cookie; }

int
qwzmac_firmware_get(const char *name, const uint8_t **data, size_t *len)
{
	static const uint8_t blob[] = "firmware";

	if (strcmp(name, "test-fw") != 0)
		return ENOENT;
	*data = blob;
	*len = sizeof(blob);
	return 0;
}

/* Not arc4random_buf(): compat/systm.c defines that name for the kext. */
void
qwzmac_random_buf(void *buf, size_t len)
{
	uint8_t *p = buf;

	while (len > 0) {
		size_t n = len > 256 ? 256 : len;

		getentropy(p, n);
		p += n;
		len -= n;
	}
}
