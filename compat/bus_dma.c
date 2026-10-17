/*
 * QwzWifi compat: bus_dma(9). See the safety model in <machine/bus.h>.
 *
 * Every allocation is recorded in a registry. A physical address reaches
 * the device only via dm_segs[] of a loaded map, and every load path
 * checks that the address range belongs to a registered allocation and
 * lies inside the tag's window (0..4 GB for qwz).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/queue.h>
#include <machine/bus.h>

#define DMA_LOAD_NONE	0
#define DMA_LOAD_RAW	1
#define DMA_LOAD_KVA	2
#define DMA_LOAD_MBUF	3

struct dma_alloc {
	LIST_ENTRY(dma_alloc)	da_list;
	void			*da_cookie;
	vaddr_t			 da_va;
	bus_addr_t		 da_pa;
	bus_size_t		 da_size;
};

static LIST_HEAD(, dma_alloc) dma_allocs = LIST_HEAD_INITIALIZER(dma_allocs);
static unsigned long dma_alloc_count;
static unsigned long long dma_alloc_bytes;

/* QWZMAC: private _dm_flags bit: a POSTREAD sync happened since load. */
#define DMA_POSTREAD_DONE	0x40000000


static struct dma_alloc *
dma_find_pa(bus_addr_t pa, bus_size_t len)
{
	struct dma_alloc *da;

	LIST_FOREACH(da, &dma_allocs, da_list) {
		if (pa >= da->da_pa && len <= da->da_size &&
		    pa - da->da_pa <= da->da_size - len)
			return da;
	}
	return NULL;
}

static struct dma_alloc *
dma_find_va(vaddr_t va, bus_size_t len)
{
	struct dma_alloc *da;

	LIST_FOREACH(da, &dma_allocs, da_list) {
		if (va >= da->da_va && len <= da->da_size &&
		    va - da->da_va <= da->da_size - len)
			return da;
	}
	return NULL;
}

/* Final check before an address is written into dm_segs[]. */
static int
dma_check_range(bus_dma_tag_t t, bus_addr_t pa, bus_size_t len,
    const char *who)
{
	if (len == 0 || pa < t->_low || pa + len - 1 < pa ||
	    pa + len - 1 > t->_high) {
		printf("QwzWifi: DMA %s: 0x%llx+0x%llx outside 0x%llx..0x%llx,"
		    " refused\n", who, (unsigned long long)pa,
		    (unsigned long long)len, (unsigned long long)t->_low,
		    (unsigned long long)t->_high);
		qwzmac_backtrace();
		return EINVAL;
	}
	return 0;
}

/*
 * Allocate one physically contiguous, wired, zeroed segment in [low, high]
 * intersected with the tag window, and register it.
 */
static int
dma_alloc_seg(bus_dma_tag_t t, bus_size_t size, bus_size_t alignment,
    bus_addr_t low, bus_addr_t high, int canwait, bus_dma_segment_t *seg)
{
	struct dma_alloc *da;
	void *cookie, *va;
	uint64_t pa, t0;
	int error;

	if (low < t->_low)
		low = t->_low;
	if (high > t->_high)
		high = t->_high;
	if (size == 0 || low > high)
		return EINVAL;
	if (alignment < PAGE_SIZE)
		alignment = PAGE_SIZE;
	if (!powerof2(alignment))
		return EINVAL;
	size = roundup(size, PAGE_SIZE);

	da = malloc(sizeof(*da), M_DEVBUF,
	    (canwait ? M_WAITOK : M_NOWAIT) | M_CANFAIL | M_ZERO);
	if (da == NULL)
		return ENOMEM;

	t0 = getnsecuptime();
	error = qwzmac_dma_alloc(size, alignment, low, high, canwait,
	    &cookie, &va, &pa);
	if (error) {
		printf("QwzWifi: DMA alloc of %llu bytes failed: %d\n",
		    (unsigned long long)size, error);
		free(da, M_DEVBUF, sizeof(*da));
		return error;
	}

	/* Trust nothing: re-check what IOKit gave us. */
	if (pa < low || pa + size - 1 > high || (pa & (alignment - 1)) != 0 ||
	    va == NULL) {
		printf("QwzWifi: DMA alloc returned bad memory pa 0x%llx size"
		    " 0x%llx (want 0x%llx..0x%llx align 0x%llx), refused\n",
		    (unsigned long long)pa, (unsigned long long)size,
		    (unsigned long long)low, (unsigned long long)high,
		    (unsigned long long)alignment);
		qwzmac_dma_free(cookie);
		free(da, M_DEVBUF, sizeof(*da));
		return ENOMEM;
	}

	memset(va, 0, size);

	da->da_cookie = cookie;
	da->da_va = (vaddr_t)va;
	da->da_pa = pa;
	da->da_size = size;

	qwzmac_spl_lock();
	LIST_INSERT_HEAD(&dma_allocs, da, da_list);
	dma_alloc_count++;
	dma_alloc_bytes += size;
	qwzmac_spl_unlock();

	if (size >= 1024 * 1024)
		printf("QwzWifi: DMA %llu KB contiguous at 0x%llx (%llu ms)\n",
		    (unsigned long long)size / 1024, (unsigned long long)pa,
		    (unsigned long long)((getnsecuptime() - t0) / 1000000));

	seg->ds_addr = pa;
	seg->ds_len = size;
	seg->_ds_cookie = da;
	seg->_ds_va = va;
	return 0;
}

static void
dma_free_seg(bus_dma_segment_t *seg)
{
	struct dma_alloc *da = seg->_ds_cookie;
	struct dma_alloc *it;

	if (da == NULL)
		return;

	qwzmac_spl_lock();
	LIST_FOREACH(it, &dma_allocs, da_list) {
		if (it == da)
			break;
	}
	if (it == NULL) {
		qwzmac_spl_unlock();
		printf("QwzWifi: DMA free of unknown segment 0x%llx, leaking\n",
		    (unsigned long long)seg->ds_addr);
		qwzmac_backtrace();
		return;
	}
	LIST_REMOVE(da, da_list);
	dma_alloc_count--;
	dma_alloc_bytes -= da->da_size;
	qwzmac_spl_unlock();

	qwzmac_dma_free(da->da_cookie);
	free(da, M_DEVBUF, sizeof(*da));
	seg->_ds_cookie = NULL;
	seg->_ds_va = NULL;
}

int
bus_dmamem_alloc_range(bus_dma_tag_t t, bus_size_t size, bus_size_t alignment,
    bus_size_t boundary, bus_dma_segment_t *segs, int nsegs, int *rsegs,
    int flags, bus_addr_t low, bus_addr_t high)
{
	int error;

	if (nsegs < 1)
		return EINVAL;
	if (boundary != 0 && roundup(size, PAGE_SIZE) > boundary) {
		printf("QwzWifi: bus_dmamem_alloc: size 0x%llx crosses "
		    "boundary 0x%llx\n", (unsigned long long)size,
		    (unsigned long long)boundary);
		return EINVAL;
	}
	if (boundary != 0 && alignment < boundary)
		alignment = boundary;	/* contiguous + aligned => no crossing */

	error = dma_alloc_seg(t, size, alignment, low, high,
	    (flags & BUS_DMA_NOWAIT) == 0, &segs[0]);
	if (error)
		return error;
	*rsegs = 1;
	return 0;
}

int
bus_dmamem_alloc(bus_dma_tag_t t, bus_size_t size, bus_size_t alignment,
    bus_size_t boundary, bus_dma_segment_t *segs, int nsegs, int *rsegs,
    int flags)
{
	return bus_dmamem_alloc_range(t, size, alignment, boundary, segs,
	    nsegs, rsegs, flags, 0, (bus_addr_t)-1);
}

void
bus_dmamem_free(bus_dma_tag_t t, bus_dma_segment_t *segs, int nsegs)
{
	int i;

	(void)t;
	for (i = 0; i < nsegs; i++)
		dma_free_seg(&segs[i]);
}

int
bus_dmamem_map(bus_dma_tag_t t, bus_dma_segment_t *segs, int nsegs,
    size_t size, caddr_t *kvap, int flags)
{
	(void)t; (void)flags;
	if (nsegs != 1 || segs[0]._ds_va == NULL || size > segs[0].ds_len) {
		printf("QwzWifi: bus_dmamem_map: unsupported (%d segs)\n",
		    nsegs);
		return EINVAL;
	}
	*kvap = segs[0]._ds_va;
	return 0;
}

void
bus_dmamem_unmap(bus_dma_tag_t t, caddr_t kva, size_t size)
{
	/* The mapping lives as long as the segment. */
	(void)t; (void)kva; (void)size;
}

int
bus_dmamap_create(bus_dma_tag_t t, bus_size_t size, int nsegments,
    bus_size_t maxsegsz, bus_size_t boundary, int flags, bus_dmamap_t *dmamp)
{
	bus_dmamap_t map;

	(void)t;
	if (nsegments != 1 || size == 0) {
		printf("QwzWifi: bus_dmamap_create: %d segments unsupported\n",
		    nsegments);
		return EINVAL;
	}
	map = malloc(sizeof(*map), M_DEVBUF,
	    ((flags & BUS_DMA_NOWAIT) ? M_NOWAIT : M_WAITOK) | M_CANFAIL |
	    M_ZERO);
	if (map == NULL)
		return ENOMEM;
	map->_dm_size = size;
	map->_dm_segcnt = nsegments;
	map->_dm_maxsegsz = maxsegsz;
	map->_dm_boundary = boundary;
	map->_dm_flags = flags;
	*dmamp = map;
	return 0;
}

void
bus_dmamap_destroy(bus_dma_tag_t t, bus_dmamap_t map)
{
	if (map == NULL)
		return;
	if (map->_dm_loadtype != DMA_LOAD_NONE)
		bus_dmamap_unload(t, map);
	if (map->_dm_has_bounce)
		dma_free_seg(&map->_dm_bounce);
	free(map, M_DEVBUF, sizeof(*map));
}

static int
dmamap_set_seg(bus_dma_tag_t t, bus_dmamap_t map, bus_addr_t pa,
    bus_size_t len, int type, const char *who)
{
	if (map->_dm_loadtype != DMA_LOAD_NONE) {
		printf("QwzWifi: %s: map already loaded\n", who);
		return EINVAL;
	}
	if (len > map->_dm_size)
		return EFBIG;
	if (dma_check_range(t, pa, len, who) != 0)
		return EINVAL;
	map->dm_segs[0].ds_addr = pa;
	map->dm_segs[0].ds_len = len;
	map->dm_segs[0]._ds_cookie = NULL;
	map->dm_segs[0]._ds_va = NULL;
	map->dm_nsegs = 1;
	map->dm_mapsize = len;
	map->_dm_loadtype = type;
	return 0;
}

int
bus_dmamap_load_raw(bus_dma_tag_t t, bus_dmamap_t map, bus_dma_segment_t *segs,
    int nsegs, bus_size_t size, int flags)
{
	struct dma_alloc *da;

	(void)flags;
	if (nsegs != 1 || size > segs[0].ds_len)
		return EINVAL;
	qwzmac_spl_lock();
	da = dma_find_pa(segs[0].ds_addr, size);
	qwzmac_spl_unlock();
	if (da == NULL || da != segs[0]._ds_cookie) {
		printf("QwzWifi: bus_dmamap_load_raw: 0x%llx is not ours, "
		    "refused\n", (unsigned long long)segs[0].ds_addr);
		qwzmac_backtrace();
		return EINVAL;
	}
	return dmamap_set_seg(t, map, segs[0].ds_addr, size, DMA_LOAD_RAW,
	    "load_raw");
}

int
bus_dmamap_load(bus_dma_tag_t t, bus_dmamap_t map, void *buf,
    bus_size_t buflen, struct proc *p, int flags)
{
	struct dma_alloc *da;
	bus_addr_t pa;

	(void)p; (void)flags;
	qwzmac_spl_lock();
	da = dma_find_va((vaddr_t)buf, buflen);
	pa = da ? da->da_pa + ((vaddr_t)buf - da->da_va) : 0;
	qwzmac_spl_unlock();
	if (da == NULL) {
		/*
		 * QWZMAC: arbitrary kernel memory has no stable physical
		 * address we may hand out; only bus_dmamem memory is allowed.
		 */
		printf("QwzWifi: bus_dmamap_load: %p is not DMA memory, "
		    "refused\n", buf);
		qwzmac_backtrace();
		return EINVAL;
	}
	return dmamap_set_seg(t, map, pa, buflen, DMA_LOAD_KVA, "load");
}

/*
 * mbuf loads use the per-map bounce buffer. Outbound data is copied in at
 * load time and again at PREWRITE; inbound data is copied out at POSTREAD,
 * or at unload when no POSTREAD sync was done (qwz's CE receive path reads
 * the mbuf right after bus_dmamap_unload()).
 */
int
bus_dmamap_load_mbuf(bus_dma_tag_t t, bus_dmamap_t map, struct mbuf *m,
    int flags)
{
	bus_size_t len;
	int error;

	len = (m->m_flags & M_PKTHDR) ? (bus_size_t)m->m_pkthdr.len :
	    m_length(m);
	if (len == 0)
		return EINVAL;
	if (len > map->_dm_size)
		return EFBIG;
	if ((flags & BUS_DMA_WRITE) == 0 && m->m_next != NULL) {
		/* Receive buffers must be one contiguous mbuf. */
		return EFBIG;
	}
	if (m_length(m) < len)
		return EINVAL;

	if (!map->_dm_has_bounce) {
		error = dma_alloc_seg(t, map->_dm_size, PAGE_SIZE, t->_low,
		    t->_high, (flags & BUS_DMA_NOWAIT) == 0, &map->_dm_bounce);
		if (error)
			return error;
		map->_dm_has_bounce = 1;
	}

	error = dmamap_set_seg(t, map, map->_dm_bounce.ds_addr, len,
	    DMA_LOAD_MBUF, "load_mbuf");
	if (error)
		return error;
	map->_dm_mbuf = m;
	map->_dm_flags = (map->_dm_flags & ~(BUS_DMA_READ | BUS_DMA_WRITE |
	    DMA_POSTREAD_DONE)) | (flags & (BUS_DMA_READ | BUS_DMA_WRITE));
	/* Remember where inbound data goes, independent of later m_adj(). */
	map->dm_segs[0]._ds_va = mtod(m, void *);

	if (flags & BUS_DMA_WRITE)
		m_copydata(m, 0, len, map->_dm_bounce._ds_va);
	return 0;
}

static void
dmamap_copy_out(bus_dmamap_t map, bus_addr_t off, bus_size_t len)
{
	caddr_t dst = map->dm_segs[0]._ds_va;

	if (dst == NULL || off >= map->dm_mapsize)
		return;
	if (len > map->dm_mapsize - off)
		len = map->dm_mapsize - off;
	memcpy(dst + off, (caddr_t)map->_dm_bounce._ds_va + off, len);
}

void
bus_dmamap_unload(bus_dma_tag_t t, bus_dmamap_t map)
{
	(void)t;
	if (map->_dm_loadtype == DMA_LOAD_MBUF &&
	    (map->_dm_flags & BUS_DMA_WRITE) == 0 &&
	    (map->_dm_flags & DMA_POSTREAD_DONE) == 0)
		dmamap_copy_out(map, 0, map->dm_mapsize);
	map->_dm_flags &= ~(BUS_DMA_READ | BUS_DMA_WRITE | DMA_POSTREAD_DONE);
	map->_dm_mbuf = NULL;
	map->_dm_loadtype = DMA_LOAD_NONE;
	map->dm_nsegs = 0;
	map->dm_mapsize = 0;
	map->dm_segs[0].ds_addr = 0;
	map->dm_segs[0].ds_len = 0;
	map->dm_segs[0]._ds_va = NULL;
}

void
bus_dmamap_sync(bus_dma_tag_t t, bus_dmamap_t map, bus_addr_t off,
    bus_size_t len, int ops)
{
	(void)t;
	if (map->_dm_loadtype == DMA_LOAD_NONE || off >= map->dm_mapsize) {
		__asm__ volatile("mfence" ::: "memory");
		return;
	}
	if (len > map->dm_mapsize - off)
		len = map->dm_mapsize - off;

	if (map->_dm_loadtype == DMA_LOAD_MBUF) {
		if (ops & BUS_DMASYNC_PREWRITE)
			m_copydata(map->_dm_mbuf, off, len,
			    (caddr_t)map->_dm_bounce._ds_va + off);
		if (ops & BUS_DMASYNC_POSTREAD) {
			dmamap_copy_out(map, off, len);
			map->_dm_flags |= DMA_POSTREAD_DONE;
		}
		if (ops & BUS_DMASYNC_PREREAD)
			map->_dm_flags &= ~DMA_POSTREAD_DONE;
	}
	/* x86 DMA is cache coherent; order CPU stores against the device. */
	__asm__ volatile("mfence" ::: "memory");
}

/* QWZMAC: DMA usage counters for the log. */
void
qwzmac_dma_stats(unsigned long *count, unsigned long long *bytes)
{
	qwzmac_spl_lock();
	*count = dma_alloc_count;
	*bytes = dma_alloc_bytes;
	qwzmac_spl_unlock();
}
