"""Compile the actual qwz ownership/cleanup paths without hardware.

Only named complete functions are selected; their bodies are never rewritten.
The preamble retains upstream's license, headers and field macros.
"""
import pathlib
import sys

source = pathlib.Path(sys.argv[1]).read_text()
names = ('qwz_htc_alloc_mbuf', 'qwz_htc_send', 'qwz_ce_send_done_cb', 'qwz_ce_rx_pipe_cleanup',
         'qwz_ce_cleanup_pipes', 'qwz_dp_tx_htt_h2t_ppdu_stats_req')
print(source[:source.index('\n#include <dev/ic/qwzvar.h>')] + '\n#include <dev/ic/qwzvar.h>')
print('''
void qwz_htc_prepare_tx_mbuf(struct qwz_htc_ep *, struct mbuf *);
int qwz_ce_send(struct qwz_softc *, struct mbuf *, uint8_t, uint16_t);
struct qwz_tx_data *qwz_ce_completed_send_next(struct qwz_ce_pipe *);
void qwz_ce_poll_send_completed(struct qwz_softc *, uint8_t);
''')
for name in names:
    start = source.index('\n' + name + '(')
    start = source.rfind('\n', 0, start) + 1
    end = source.index('\n}\n', start) + 3
    print(source[start:end])
