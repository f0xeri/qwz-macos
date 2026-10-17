#!/bin/zsh
# User-space unit tests for compat/ (no kernel needed).
set -e
cd "${0:A:h}/.."
RD=$(clang -print-resource-dir)
OUT=build/test; mkdir -p $OUT
# The compat side gets the same freestanding view as in the kext; libc's
# malloc/free names are moved out of the way so both can coexist.
CF=(-std=gnu11 -O1 -g -fsanitize=address -ffreestanding -nostdinc -isystem $RD/include
    -D_KERNEL -DQWZMAC -I compat/include -I .
    -Dmalloc=qwzmac_test_malloc -Dfree=qwzmac_test_free
    -Wall -Wno-unused-function -Wno-sign-conversion)
OBJS=()
for src in compat/systm.c compat/sync.c compat/task.c compat/uipc_mbuf.c \
    compat/bus_dma.c compat/subr_tree.c compat/qwzmac_debug.c tests/compat_test.c; do
  o=$OUT/${src:t:r}.o; clang "${CF[@]}" -c $src -o $o; OBJS+=$o
done
clang -std=gnu11 -O1 -g -fsanitize=address -Wall -c tests/os_user.c -o $OUT/os_user.o
clang -fsanitize=address "${OBJS[@]}" $OUT/os_user.o -o $OUT/compat_test
$OUT/compat_test
