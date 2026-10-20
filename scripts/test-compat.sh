#!/bin/zsh
# User-space unit tests for compat/ (no kernel needed).
set -e
cd "${0:A:h}/.."
RD=$(clang -print-resource-dir)
OUT=build/test; mkdir -p $OUT
# The compat side gets the same freestanding view as in the kext; libc's
# malloc/free names are moved out of the way so both can coexist.
CF=(-std=gnu11 -O1 -g -fsanitize=address -ffunction-sections -fdata-sections -ffreestanding -nostdinc -isystem $RD/include
    -D_KERNEL -DQWZMAC -I compat/include -I openbsd/sys
    -Dmalloc=qwzmac_test_malloc -Dfree=qwzmac_test_free
    -Wall -Wno-unused-function -Wno-sign-conversion)
python3 tests/extract_qwz_htc.py "${QWZ_HTC_SOURCE:-openbsd/sys/dev/ic/qwz.c}" > $OUT/qwz_htc_impl.c
OBJS=()
for src in compat/systm.c compat/sync.c compat/task.c compat/uipc_mbuf.c \
    compat/bus_dma.c openbsd/sys/kern/subr_tree.c compat/qwzmac_debug.c compat/if.c compat/if_ether.c \
    compat/pbkdf2.c openbsd/sys/crypto/{hmac,sha1,sha2,md5}.c openbsd/sys/net80211/ieee80211_crypto.c tests/wpa_test.c \
    openbsd/sys/net80211/ieee80211_node.c tests/node_test.c \
    tests/wpa_stubs.c tests/qwz_htc_test.c tests/compat_test.c; do
  o=$OUT/${src:t:r}.o; clang "${CF[@]}" -c $src -o $o; OBJS+=$o
done
clang -std=gnu11 -O1 -g -fsanitize=address -Wall -c tests/os_user.c -o $OUT/os_user.o
# Compile the SAME timer/lock implementation as the kext. Only system headers
# and APIs are substituted; no test copy of qwzmac_timer_* or qwzmac_spl_*.
for src in kext/os_timer.cpp kext/os_lock.cpp tests/kernel_api.cpp; do
  o=$OUT/${src:t:r}.o
  clang++ -std=c++17 -O1 -g -fsanitize=address -Wall -I tests/kernel -c $src -o $o
  OBJS+=$o
done
clang++ -fsanitize=address -Wl,-dead_strip "${OBJS[@]}" $OUT/os_user.o -o $OUT/compat_test
$OUT/compat_test
