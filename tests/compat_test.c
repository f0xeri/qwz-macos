/*
 * QwzWifi tests: unit tests for the OpenBSD compat layer (user space).
 * Run: scripts/test-compat.sh
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/task.h>
#include <sys/timeout.h>
#include <sys/rwlock.h>
#include <sys/refcnt.h>
#include <sys/device.h>
#include <machine/bus.h>
#include <net/if.h>
#include <netinet/if_ether.h>

extern int test_dma_fake_high;
extern int test_quiet;
extern unsigned test_panics;

static int failures, checks;

#define CHECK(c) do {							\
	checks++;							\
	if (!(c)) {							\
		failures++;						\
		qwzmac_log("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);	\
	}								\
} while (0)

static void
fill(uint8_t *p, int n, int seed)
{
	int i;

	for (i = 0; i < n; i++)
		p[i] = (uint8_t)(seed + i * 7);
}

static int
check_chain(struct mbuf *m, int off, int n, int seed)
{
	uint8_t buf[70000];
	int i;

	m_copydata(m, off, n, buf);
	for (i = 0; i < n; i++)
		if (buf[i] != (uint8_t)(seed + (i + off) * 7))
			return 0;
	return 1;
}

static void
test_mbuf(void)
{
	uint8_t data[9000];
	struct mbuf *m, *m2, *n;
	int off;

	fill(data, sizeof(data), 3);

	/* Cluster of arbitrary size, copyback/copydata round trip. */
	m = m_gethdr(M_WAIT, MT_DATA);
	CHECK(m != NULL);
	CHECK(MCLGETL(m, M_WAIT, 8192) != NULL);
	CHECK(m->m_flags & M_EXT);
	m->m_len = m->m_pkthdr.len = 0;
	CHECK(m_copyback(m, 0, sizeof(data), data, M_WAIT) == 0);
	CHECK(m->m_pkthdr.len == (int)sizeof(data));
	CHECK(m->m_next != NULL);	/* 9000 > 8192: chain grew */
	CHECK(check_chain(m, 0, sizeof(data), 3));
	CHECK(m_length(m) == sizeof(data));

	/* m_adj from head and from tail. */
	m_adj(m, 100);
	CHECK(m->m_pkthdr.len == (int)sizeof(data) - 100);
	CHECK(check_chain(m, 0, 50, 3 + 100 * 7));
	m_adj(m, -900);
	CHECK(m->m_pkthdr.len == (int)sizeof(data) - 1000);
	CHECK(m_length(m) == sizeof(data) - 1000);

	/* m_defrag makes one buffer, same content. */
	CHECK(m_defrag(m, M_WAIT) == 0);
	CHECK(m->m_next == NULL);
	CHECK(m->m_len == (int)sizeof(data) - 1000);
	CHECK(check_chain(m, 0, m->m_len, 3 + 100 * 7));

	/* m_pullup on a two-mbuf chain. */
	m2 = m_get(M_WAIT, MT_DATA);
	memcpy(mtod(m2, void *), "ABCDEFGH", 8);
	m2->m_len = 8;
	n = m_gethdr(M_WAIT, MT_DATA);
	memcpy(mtod(n, void *), "0123", 4);
	n->m_len = 4;
	n->m_next = m2;
	n->m_pkthdr.len = 12;
	n = m_pullup(n, 10);
	CHECK(n != NULL && n->m_len >= 10);
	CHECK(n != NULL && memcmp(mtod(n, char *), "0123ABCDEF", 10) == 0);
	m_freem(n);

	/* m_makespace inserts room in the middle. */
	n = m_makespace(m, 10, 8, &off);
	CHECK(n != NULL);
	CHECK(m->m_pkthdr.len == (int)sizeof(data) - 1000 + 8);
	if (n != NULL)
		memset(mtod(n, char *) + off, 0xee, 8);
	CHECK(check_chain(m, 0, 10, 3 + 100 * 7));

	/* m_prepend, m_copym with shared clusters, m_split. */
	M_PREPEND(m, 16, M_WAIT);
	CHECK(m != NULL && m->m_pkthdr.len == (int)sizeof(data) - 1000 + 24);
	m2 = m_copym(m, 0, M_COPYALL, M_WAIT);
	CHECK(m2 != NULL);
	CHECK(m2 != NULL && m_length(m2) == m_length(m));
	n = m_split(m2, 100, M_WAIT);
	CHECK(n != NULL && m_length(m2) == 100);
	m_freem(m2);
	m_freem(n);
	m_freem(m);

	/* Lists. */
	{
		struct mbuf_list ml = MBUF_LIST_INITIALIZER();
		int i;

		for (i = 0; i < 5; i++)
			ml_enqueue(&ml, m_gethdr(M_WAIT, MT_DATA));
		CHECK(ml_len(&ml) == 5);
		m_freem(ml_dequeue(&ml));
		CHECK(ml_purge(&ml) == 4);
		CHECK(ml_empty(&ml));
	}
}

static struct bus_dma_tag tag32 = { 0, 0xffffffffUL };

static void
test_bus_dma(void)
{
	bus_dma_segment_t seg;
	bus_dmamap_t map;
	caddr_t kva;
	struct mbuf *m;
	uint8_t pat[1500];
	int nsegs, i;

	/* Allocation, mapping, load_raw. */
	CHECK(bus_dmamem_alloc_range(&tag32, 10000, 0, 0, &seg, 1, &nsegs,
	    BUS_DMA_WAITOK | BUS_DMA_ZERO, 0, 0xffffffff) == 0);
	CHECK(nsegs == 1);
	CHECK(seg.ds_addr + seg.ds_len - 1 <= 0xffffffffUL);
	CHECK((seg.ds_addr & PAGE_MASK) == 0);
	CHECK(bus_dmamem_map(&tag32, &seg, 1, 10000, &kva, 0) == 0);
	CHECK(bus_dmamap_create(&tag32, 10000, 1, 10000, 0, 0, &map) == 0);
	CHECK(bus_dmamap_load_raw(&tag32, map, &seg, 1, 10000, 0) == 0);
	CHECK(map->dm_nsegs == 1 && map->dm_segs[0].ds_addr == seg.ds_addr);
	bus_dmamap_unload(&tag32, map);

	/* bus_dmamap_load of a pointer inside the allocation. */
	CHECK(bus_dmamap_load(&tag32, map, kva + 256, 1000, NULL, 0) == 0);
	CHECK(map->dm_segs[0].ds_addr == seg.ds_addr + 256);
	bus_dmamap_unload(&tag32, map);

	/* ...but not of random kernel memory, or past the end. */
	test_quiet = 1;
	CHECK(bus_dmamap_load(&tag32, map, pat, sizeof(pat), NULL, 0) != 0);
	CHECK(bus_dmamap_load(&tag32, map, kva + 9000, 9000, NULL, 0) != 0);
	test_quiet = 0;
	CHECK(map->dm_nsegs == 0);

	/* A forged segment is refused by load_raw. */
	{
		bus_dma_segment_t fake = seg;

		fake.ds_addr += 0x100000;
		test_quiet = 1;
		CHECK(bus_dmamap_load_raw(&tag32, map, &fake, 1, 4096, 0) != 0);
		test_quiet = 0;
	}
	bus_dmamap_destroy(&tag32, map);
	bus_dmamem_free(&tag32, &seg, 1);

	/* Memory above 4 GB from the allocator is rejected. */
	test_dma_fake_high = 1;
	test_quiet = 1;
	CHECK(bus_dmamem_alloc_range(&tag32, 4096, 0, 0, &seg, 1, &nsegs,
	    BUS_DMA_WAITOK, 0, 0xffffffff) != 0);
	test_quiet = 0;
	test_dma_fake_high = 0;

	/* mbuf TX: data must be in the bounce buffer, not exposed in place. */
	CHECK(bus_dmamap_create(&tag32, MCLBYTES, 1, MCLBYTES, 0, 0, &map) == 0);
	m = m_gethdr(M_WAIT, MT_DATA);
	MCLGET(m, M_WAIT);
	fill(pat, sizeof(pat), 9);
	memcpy(mtod(m, void *), pat, sizeof(pat));
	m->m_len = m->m_pkthdr.len = sizeof(pat);
	CHECK(bus_dmamap_load_mbuf(&tag32, map, m, BUS_DMA_WRITE) == 0);
	CHECK(map->dm_segs[0].ds_addr <= 0xffffffffUL);
	CHECK(map->dm_mapsize == sizeof(pat));
	CHECK(memcmp(map->_dm_bounce._ds_va, pat, sizeof(pat)) == 0);
	/* Changes after load reach the device after PREWRITE. */
	mtod(m, uint8_t *)[0] ^= 0xff;
	bus_dmamap_sync(&tag32, map, 0, sizeof(pat), BUS_DMASYNC_PREWRITE);
	CHECK(((uint8_t *)map->_dm_bounce._ds_va)[0] == (uint8_t)(pat[0] ^ 0xff));
	bus_dmamap_unload(&tag32, map);
	m_freem(m);

	/* mbuf RX with POSTREAD. */
	m = m_gethdr(M_WAIT, MT_DATA);
	MCLGET(m, M_WAIT);
	m->m_len = m->m_pkthdr.len = MCLBYTES;
	memset(mtod(m, void *), 0, MCLBYTES);
	CHECK(bus_dmamap_load_mbuf(&tag32, map, m, BUS_DMA_READ) == 0);
	bus_dmamap_sync(&tag32, map, 0, MCLBYTES, BUS_DMASYNC_PREREAD);
	memcpy(map->_dm_bounce._ds_va, pat, sizeof(pat));	/* "device" */
	bus_dmamap_sync(&tag32, map, 0, MCLBYTES, BUS_DMASYNC_POSTREAD);
	CHECK(memcmp(mtod(m, void *), pat, sizeof(pat)) == 0);
	/* CPU edits after POSTREAD must survive unload. */
	mtod(m, uint8_t *)[0] = 0x42;
	bus_dmamap_unload(&tag32, map);
	CHECK(mtod(m, uint8_t *)[0] == 0x42);
	m_freem(m);

	/* mbuf RX without any sync (qwz CE receive path): unload copies. */
	m = m_gethdr(M_WAIT, MT_DATA);
	MCLGET(m, M_WAIT);
	m->m_len = m->m_pkthdr.len = MCLBYTES;
	memset(mtod(m, void *), 0, MCLBYTES);
	CHECK(bus_dmamap_load_mbuf(&tag32, map, m, BUS_DMA_READ) == 0);
	memcpy(map->_dm_bounce._ds_va, pat, sizeof(pat));
	bus_dmamap_unload(&tag32, map);
	CHECK(memcmp(mtod(m, void *), pat, sizeof(pat)) == 0);

	/* Too big, and chained receive buffers, are refused. */
	m->m_len = m->m_pkthdr.len = MCLBYTES;
	m_copyback(m, MCLBYTES, 100, pat, M_WAIT);
	CHECK(bus_dmamap_load_mbuf(&tag32, map, m, BUS_DMA_WRITE) == EFBIG);
	m_freem(m);
	bus_dmamap_destroy(&tag32, map);

	{
		unsigned long n;
		unsigned long long bytes;

		qwzmac_dma_stats(&n, &bytes);
		CHECK(n == 0 && bytes == 0);	/* everything was freed */
	}
	(void)i;
}

static volatile int task_ran;

static void
task_fn(void *arg)
{
	(void)arg;
	qwzmac_spl_lock();
	task_ran++;
	wakeup(&task_ran);
	qwzmac_spl_unlock();
}

static void
slow_task_fn(void *arg)
{
	(void)arg;
	delay(50000);
	task_fn(arg);
}

static void
test_task(void)
{
	struct taskq *tq;
	struct task t1, t2;
	int s, i;

	CHECK(qwzmac_taskq_init() == 0);
	task_set(&t1, task_fn, NULL);
	task_ran = 0;
	CHECK(task_add(systq, &t1) == 1);
	s = splnet();
	while (task_ran == 0)
		CHECK(tsleep_nsec(&task_ran, 0, "t", SEC_TO_NSEC(2)) == 0);
	splx(s);
	CHECK(task_ran == 1);

	tq = taskq_create("test", 1, IPL_NET, 0);
	CHECK(tq != NULL);
	task_set(&t2, slow_task_fn, NULL);
	task_ran = 0;
	task_add(tq, &t2);
	task_add(tq, &t1);
	CHECK(task_add(tq, &t1) == 0);	/* already queued */
	taskq_barrier(tq);
	CHECK(task_ran == 2);

	/* task_del removes a queued task. */
	task_ran = 0;
	task_add(tq, &t2);
	task_add(tq, &t1);
	CHECK(task_del(tq, &t1) == 1);
	taskq_barrier(tq);
	CHECK(task_ran == 1);
	taskq_destroy(tq);

	/* tsleep times out. */
	s = splnet();
	CHECK(tsleep_nsec(&s, 0, "t", MSEC_TO_NSEC(20)) == EWOULDBLOCK);
	splx(s);
	for (i = 0; i < 3; i++)
		CHECK(tsleep_nsec(&s, 0, "t", MSEC_TO_NSEC(1)) == EWOULDBLOCK);
	qwzmac_taskq_fini();
}

static volatile int to_ran;

static void
to_fn(void *arg)
{
	(void)arg;
	to_ran++;
}

static void
test_timeout(void)
{
	struct timeout to;

	timeout_set(&to, to_fn, NULL);
	to_ran = 0;
	CHECK(timeout_add_msec(&to, 20) == 1);
	CHECK(timeout_pending(&to));
	CHECK(timeout_add_msec(&to, 20) == 0);	/* re-arm */
	delay(100000);
	CHECK(to_ran == 1);
	CHECK(!timeout_pending(&to));
	CHECK(timeout_triggered(&to));

	timeout_add_msec(&to, 30);
	CHECK(timeout_del(&to) == 1);
	delay(80000);
	CHECK(to_ran == 1);
	CHECK(timeout_del(&to) == 0);
	timeout_destroy(&to);
}

static struct rwlock rwl;
static volatile int in_write;

static void
rw_task(void *arg)
{
	(void)arg;
	rw_enter_write(&rwl);
	in_write = 1;
	rw_exit_write(&rwl);
}

static void
test_locks(void)
{
	struct refcnt r;
	struct task t;
	int i;

	rw_init(&rwl, "test");
	rw_enter_write(&rwl);
	rw_assert_wrlock(&rwl);
	CHECK(rw_enter(&rwl, RW_WRITE | RW_NOSLEEP | RW_RECURSEFAIL) ==
	    EDEADLK);

	CHECK(qwzmac_taskq_init() == 0);
	in_write = 0;
	task_set(&t, rw_task, NULL);
	task_add(systq, &t);
	delay(30000);
	CHECK(in_write == 0);		/* blocked behind us */
	rw_exit(&rwl);
	for (i = 0; i < 100 && !in_write; i++)
		delay(1000);
	CHECK(in_write == 1);
	qwzmac_taskq_fini();

	CHECK(rw_enter(&rwl, RW_READ) == 0);
	CHECK(rw_enter(&rwl, RW_READ) == 0);
	CHECK(rw_enter(&rwl, RW_WRITE | RW_NOSLEEP) == EBUSY);
	rw_exit(&rwl);
	rw_exit(&rwl);
	CHECK(rw_enter(&rwl, RW_WRITE | RW_NOSLEEP) == 0);
	rw_exit(&rwl);

	refcnt_init(&r);
	refcnt_take(&r);
	CHECK(refcnt_read(&r) == 2);
	CHECK(refcnt_rele(&r) == 0);
	refcnt_finalize(&r, "test");	/* drops the last ref, no wait */
	CHECK(refcnt_read(&r) == 0);
}

static void
test_misc(void)
{
	void *p;
	u_char *fw;
	size_t len;

	p = malloc(100, M_DEVBUF, M_WAITOK | M_ZERO);
	CHECK(p != NULL && ((char *)p)[99] == 0);
	free(p, M_DEVBUF, 0);		/* size 0 = unknown, allowed */
	CHECK(mallocarray(SIZE_MAX / 2, 4, M_DEVBUF, M_NOWAIT) == NULL);

	CHECK(loadfirmware("test-fw", &fw, &len) == 0);
	CHECK(len == 9 && memcmp(fw, "firmware", 9) == 0);
	free(fw, M_DEVBUF, len);
	test_quiet = 1;
	CHECK(loadfirmware("missing", &fw, &len) != 0);
	test_quiet = 0;

	CHECK(arc4random_uniform(10) < 10);
	CHECK(howmany(10, 4) == 3 && roundup(10, 4) == 12);
	CHECK(fls(0x80) == 8 && ffs(0x80) == 8);
	CHECK(htole32(0x12345678) == 0x12345678);
	CHECK(betoh16(0x1234) == 0x3412);

	test_quiet = 1;
	KASSERT(1 == 2);		/* logs, must not crash */
	test_quiet = 0;
}

/* WEP/TKIP ICVs are ether_crc32_le_update() without the final xor. */
static void
test_crc32(void)
{
	static const u_int8_t check[] = "123456789";
	u_int32_t crc;

	CHECK(ether_crc32_le(check, 9) == ~0xcbf43926U);
	crc = ether_crc32_le_update(0xffffffff, check, 4);
	crc = ether_crc32_le_update(crc, check + 4, 5);
	CHECK(crc == ~0xcbf43926U);
	/* Same CRC with the register not reflected: bit-reversed value. */
	CHECK(ether_crc32_be(check, 9) == 0x9b63d02cU);
}

static void
test_rddm(void)
{
	static uint8_t dump[4096];

	/* Empty dump: reported, no crash. */
	qwzmac_rddm_report(dump, sizeof(dump));
	memcpy(dump + 100, "dlpager: TPZ auth failed in pager.c:123", 39);
	memcpy(dump + 4096 - 5, "abcde", 5);	/* string at the very end */
	qwzmac_rddm_report(dump, sizeof(dump));
	CHECK(1);
}

int
main(void)
{
	test_rddm();
	test_crc32();
	test_misc();
	test_mbuf();
	test_bus_dma();
	test_task();
	test_timeout();
	test_locks();
	qwzmac_log("compat tests: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
