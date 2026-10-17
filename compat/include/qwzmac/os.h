/*
 * QwzWifi: the boundary between the OpenBSD compat layer (C) and macOS
 * (IOKit/Mach, C++). Only plain C types cross this line. Implemented in
 * kext/os_bridge.cpp.
 *
 * Concurrency model: one global recursive lock stands in for OpenBSD's
 * spl levels (splnet() takes it, splx() drops it). Interrupt handlers,
 * timeouts and task queues all run in threads and take it like splnet()
 * would. qwzmac_sleep() fully releases it while sleeping and re-takes it
 * afterwards, which is what tsleep(9) at raised spl effectively does.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef _QWZMAC_OS_H_
#define _QWZMAC_OS_H_

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Logging (IOLog). */
void	qwzmac_vlog(const char *fmt, va_list ap);
void	qwzmac_log(const char *fmt, ...) __attribute__((__format__(__printf__, 1, 2)));
void	qwzmac_backtrace(void);

/* Kernel heap. */
void	*qwzmac_malloc(size_t size, int canwait);
void	qwzmac_free(void *p, size_t size);

/* Time. */
uint64_t qwzmac_nsecuptime(void);
void	qwzmac_delay_us(unsigned int us);

/* Global "spl" lock and sleep/wakeup on it. */
void	qwzmac_spl_lock(void);
void	qwzmac_spl_unlock(void);
int	qwzmac_spl_held(void);
/* Returns 0 when woken, EWOULDBLOCK on timeout. nsecs == 0: no timeout. */
int	qwzmac_sleep(const void *ident, uint64_t nsecs);
void	qwzmac_wakeup(const void *ident, int one);

/* Kernel threads. */
int	qwzmac_thread_start(void (*fn)(void *), void *arg);
/* Opaque non-zero identity of the calling thread. */
unsigned long qwzmac_thread_self(void);

/* One-shot timers (thread_call). The callback runs in a thread. */
void	*qwzmac_timer_alloc(void (*fn)(void *), void *arg);
void	qwzmac_timer_free(void *t);
/* Both return 1 if the timer was pending before the call. */
int	qwzmac_timer_enter(void *t, uint64_t nsecs);
int	qwzmac_timer_cancel(void *t);
/* Cancel and wait for a running callback to finish. */
int	qwzmac_timer_cancel_wait(void *t);

/*
 * Physically contiguous, wired DMA memory with lo <= pa && pa + size - 1 <= hi.
 * Returns 0 and fills cookie/va/pa, or an errno.
 */
int	qwzmac_dma_alloc(size_t size, size_t align, uint64_t lo, uint64_t hi,
	    int canwait, void **cookie, void **va, uint64_t *pa);
void	qwzmac_dma_free(void *cookie);

/* PCI device access (the one device we are attached to). */
uint32_t qwzmac_pci_conf_read(void *dev, int reg);
void	qwzmac_pci_conf_write(void *dev, int reg, uint32_t val);
/* Map a memory BAR; returns 0 and the kernel VA, physical base and size. */
int	qwzmac_pci_map_bar(void *dev, int reg, uintptr_t *va, uint64_t *pa,
	    uint64_t *size);
void	qwzmac_pci_unmap_bar(void *dev, int reg);
/* MSI interrupt. Handler runs in a workloop thread with the spl lock held. */
int	qwzmac_pci_msi_index(void *dev);
void	*qwzmac_pci_intr_establish(void *dev, int index, int (*fn)(void *),
	    void *arg, const char *name);
void	qwzmac_pci_intr_disestablish(void *dev, void *cookie);

/* Embedded firmware: returns 0 on success; data stays owned by the kext. */
int	qwzmac_firmware_get(const char *name, const uint8_t **data, size_t *len);

/* Randomness. */
void	qwzmac_random_buf(void *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* _QWZMAC_OS_H_ */
