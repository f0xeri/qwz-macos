/* Real qwz HTC ownership through completion, reset and send failures.
 * Hardware submission/completion are substituted; DMA + mbufs are real compat.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "../build/test/qwz_htc_impl.c"

static int send_error, submitted, owner_missing;
static struct qwz_tx_data *completed;
static struct qwz_softc sc;
static struct bus_dma_tag tag = {0, 0xffffffffUL};
static struct qwz_tx_data tx;
static struct { struct qwz_ce_ring ring; void *context; } storage;
#define ring storage.ring

void qwz_htc_prepare_tx_mbuf(struct qwz_htc_ep *ep, struct mbuf *m)
{ (void)ep; (void)m; }
int qwz_ce_send(struct qwz_softc *dev, struct mbuf *m, uint8_t pipe, uint16_t eid)
{
    (void)dev; (void)pipe; (void)eid;
    ++submitted;
    if (tx.m != m) ++owner_missing;
    return send_error;
}
struct qwz_tx_data *qwz_ce_completed_send_next(struct qwz_ce_pipe *pipe)
{
    (void)pipe;
    struct qwz_tx_data *p = completed;
    completed = NULL;
    return p;
}
void qwz_ce_poll_send_completed(struct qwz_softc *dev, uint8_t pipe)
{ (void)dev; (void)pipe; }

#define CHECK(c) do { ++*checks; if (!(c)) { ++*failures; \
    qwzmac_log("FAIL HTC %d: %s\n", __LINE__, #c); } } while (0)

static struct mbuf *packet(void)
{
    struct mbuf *m = m_gethdr(M_WAIT, MT_DATA);
    m->m_len = m->m_pkthdr.len = 32;
    memset(mtod(m, void *), 0, 32);
    return m;
}

void test_qwz_htc(int *checks, int *failures)
{
    memset(&sc, 0, sizeof(sc));
    memset(&tx, 0, sizeof(tx));
    memset(&ring, 0, sizeof(ring));
    sc.sc_dmat = &tag;
    sc.htc.sc = &sc;
    sc.htc.target_credit_size = 256;
    sc.hw_params.ce_count = 1;
    sc.hw_params.num_rxmda_per_pdev = 1;
    sc.ce.ce_pipe[0].sc = &sc;
    sc.ce.ce_pipe[0].src_ring = &ring;
    ring.nentries = 1;
    ring.per_transfer_context[0] = &tx;
    CHECK(bus_dmamap_create(&tag, 512, 1, 512, 0, 0, &tx.map) == 0);
    for (int cycle = 0; cycle < 4; ++cycle) {
        struct mbuf *m = packet();
        CHECK(qwz_htc_send(&sc.htc, 0, m) == 0);
        CHECK(tx.m == m && owner_missing == 0);
        if (cycle & 1) {
            completed = &tx;
            CHECK(qwz_ce_send_done_cb(&sc.ce.ce_pipe[0]) == 1);
        } else {
            /* A live device may still own this transfer: no blind unmap. */
            sc.ce_initialized = 1;
            qwz_ce_cleanup_pipes(&sc);
            CHECK(tx.m == m && tx.map->dm_nsegs == 1);
            /* Device reset has stopped DMA; no completion will arrive. */
            sc.ce_initialized = 0;
            qwz_ce_cleanup_pipes(&sc);
        }
        CHECK(tx.m == NULL && tx.map->dm_nsegs == 0);
        qwz_ce_cleanup_pipes(&sc); /* idempotent */
    }
    /* Hardware rejects submission: unload, return ownership to caller. */
    struct mbuf *m = packet();
    send_error = ENOBUFS;
    CHECK(qwz_htc_send(&sc.htc, 0, m) == ENOBUFS);
    CHECK(tx.m == NULL && tx.map->dm_nsegs == 0);
    m_freem(m);
    send_error = 0;
    /* Force the observed map-load failure; caller must still own new m. */
    struct mbuf *old = packet();
    CHECK(bus_dmamap_load_mbuf(&tag, tx.map, old, BUS_DMA_WRITE) == 0);
    m = packet();
    int before = submitted;
    sc.htc.endpoint[0].tx_credit_flow_enabled = 1;
    sc.htc.endpoint[0].tx_credits = 7;
    CHECK(qwz_htc_send(&sc.htc, 0, m) == EINVAL);
    CHECK(submitted == before && sc.htc.endpoint[0].tx_credits == 7);
    CHECK(m->m_len == 32); /* ASan catches premature free here */
    m_freem(m);
    /* The real HTT caller also frees exactly once after a map-load error. */
    CHECK(qwz_dp_tx_htt_h2t_ppdu_stats_req(&sc, 0, 0) == EINVAL);
    CHECK(sc.htc.endpoint[0].tx_credits == 7 && submitted == before);
    bus_dmamap_unload(&tag, tx.map);
    m_freem(old);
    /* Insufficient credits: never submit or consume the caller's packet. */
    sc.htc.endpoint[0].tx_credits = 0;
    m = packet();
    CHECK(qwz_htc_send(&sc.htc, 0, m) == EAGAIN);
    CHECK(m->m_len == 32 && tx.map->dm_nsegs == 0);
    m_freem(m);
    bus_dmamap_destroy(&tag, tx.map);
}
