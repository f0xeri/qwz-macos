/*
 * QwzWifi compat: OpenBSD bus_space(9) and bus_dma(9), real signatures.
 *
 * bus_space: the handle is the kernel virtual address of the mapped BAR.
 *
 * bus_dma safety model (AMD IOMMU is off on the target, so a bad address
 * handed to the device corrupts host RAM):
 *  - every address the device ever sees comes from qwzmac_dma_alloc(), which
 *    only returns physically contiguous wired memory inside the tag's range
 *    (0..4 GB for qwz);
 *  - bus_dmamap_load_raw()/bus_dmamap_load() only accept memory that came
 *    from bus_dmamem_alloc*() and re-check the range;
 *  - bus_dmamap_load_mbuf() never exposes mbuf memory: it copies through a
 *    per-map bounce buffer (in at PREWRITE sync, out at POSTREAD sync).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef _QWZMAC_MACHINE_BUS_H_
#define _QWZMAC_MACHINE_BUS_H_

#include <sys/types.h>

typedef u_long bus_addr_t;
typedef u_long bus_size_t;

typedef int bus_space_tag_t;	/* QWZMAC: only memory space is supported */
typedef u_long bus_space_handle_t;

#define BUS_SPACE_MAP_CACHEABLE		0x0001
#define BUS_SPACE_MAP_LINEAR		0x0002
#define BUS_SPACE_MAP_PREFETCHABLE	0x0008

#define BUS_SPACE_BARRIER_READ		0x01
#define BUS_SPACE_BARRIER_WRITE		0x02

static inline uint8_t
bus_space_read_1(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o)
{
	(void)t;
	return *(volatile uint8_t *)(h + o);
}

static inline uint16_t
bus_space_read_2(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o)
{
	(void)t;
	return *(volatile uint16_t *)(h + o);
}

static inline uint32_t
bus_space_read_4(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o)
{
	(void)t;
	return *(volatile uint32_t *)(h + o);
}

static inline void
bus_space_write_1(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o,
    uint8_t v)
{
	(void)t;
	*(volatile uint8_t *)(h + o) = v;
}

static inline void
bus_space_write_2(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o,
    uint16_t v)
{
	(void)t;
	*(volatile uint16_t *)(h + o) = v;
}

static inline void
bus_space_write_4(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o,
    uint32_t v)
{
	(void)t;
	*(volatile uint32_t *)(h + o) = v;
}

static inline void
bus_space_barrier(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o,
    bus_size_t l, int f)
{
	(void)t; (void)h; (void)o; (void)l; (void)f;
	__asm__ volatile("mfence" ::: "memory");
}

/* bus_dma */
#define	BUS_DMA_WAITOK		0x00000
#define	BUS_DMA_NOWAIT		0x00001
#define	BUS_DMA_ALLOCNOW	0x00002
#define	BUS_DMA_COHERENT	0x00004
#define	BUS_DMA_BUS1		0x00010
#define	BUS_DMA_BUS2		0x00020
#define	BUS_DMA_32BIT		0x00020
#define	BUS_DMA_24BIT		0x00040
#define	BUS_DMA_STREAMING	0x00100
#define	BUS_DMA_READ		0x00200
#define	BUS_DMA_WRITE		0x00400
#define	BUS_DMA_NOCACHE		0x00800
#define	BUS_DMA_ZERO		0x01000
#define	BUS_DMA_64BIT		0x02000

#define	BUS_DMASYNC_PREREAD	0x01
#define	BUS_DMASYNC_POSTREAD	0x02
#define	BUS_DMASYNC_PREWRITE	0x04
#define	BUS_DMASYNC_POSTWRITE	0x08

struct mbuf;
struct uio;

typedef struct bus_dma_tag		*bus_dma_tag_t;
typedef struct bus_dmamap		*bus_dmamap_t;

struct bus_dma_segment {
	bus_addr_t	ds_addr;
	bus_size_t	ds_len;
	/* QWZMAC: private; filled by bus_dmamem_alloc*() */
	void		*_ds_cookie;	/* qwzmac_dma_alloc() cookie */
	void		*_ds_va;	/* kernel VA of ds_addr */
};
typedef struct bus_dma_segment	bus_dma_segment_t;

/*
 * QWZMAC: a tag only describes the address range the device can reach.
 * The PCI glue hands out one tag limited to 0..0xffffffff.
 */
struct bus_dma_tag {
	bus_addr_t	_low;
	bus_addr_t	_high;
};

struct bus_dmamap {
	bus_size_t	_dm_size;
	int		_dm_segcnt;
	bus_size_t	_dm_maxsegsz;
	bus_size_t	_dm_boundary;
	int		_dm_flags;

	/* QWZMAC: bounce buffer for mbuf loads */
	bus_dma_segment_t _dm_bounce;
	int		_dm_has_bounce;
	struct mbuf	*_dm_mbuf;
	int		_dm_loadtype;

	bus_size_t	dm_mapsize;
	int		dm_nsegs;
	bus_dma_segment_t dm_segs[1];
};

int	bus_dmamap_create(bus_dma_tag_t, bus_size_t, int, bus_size_t,
	    bus_size_t, int, bus_dmamap_t *);
void	bus_dmamap_destroy(bus_dma_tag_t, bus_dmamap_t);
int	bus_dmamap_load(bus_dma_tag_t, bus_dmamap_t, void *, bus_size_t,
	    struct proc *, int);
int	bus_dmamap_load_mbuf(bus_dma_tag_t, bus_dmamap_t, struct mbuf *, int);
int	bus_dmamap_load_raw(bus_dma_tag_t, bus_dmamap_t, bus_dma_segment_t *,
	    int, bus_size_t, int);
void	bus_dmamap_unload(bus_dma_tag_t, bus_dmamap_t);
void	bus_dmamap_sync(bus_dma_tag_t, bus_dmamap_t, bus_addr_t, bus_size_t,
	    int);

int	bus_dmamem_alloc(bus_dma_tag_t, bus_size_t, bus_size_t, bus_size_t,
	    bus_dma_segment_t *, int, int *, int);
int	bus_dmamem_alloc_range(bus_dma_tag_t, bus_size_t, bus_size_t,
	    bus_size_t, bus_dma_segment_t *, int, int *, int, bus_addr_t,
	    bus_addr_t);
void	bus_dmamem_free(bus_dma_tag_t, bus_dma_segment_t *, int);
int	bus_dmamem_map(bus_dma_tag_t, bus_dma_segment_t *, int, size_t,
	    caddr_t *, int);
void	bus_dmamem_unmap(bus_dma_tag_t, caddr_t, size_t);

/* QWZMAC: number and total size of live DMA allocations. */
void	qwzmac_dma_stats(unsigned long *, unsigned long long *);

#endif /* _QWZMAC_MACHINE_BUS_H_ */
