#!/bin/zsh
# Compile-only check of the OpenBSD side (qwz + compat C files).
# usage: scripts/check-qwz.sh [file.c ...]   (default: qwz/*.c compat/*.c)
cd "${0:A:h}/.."
RD=$(clang -print-resource-dir)
CF=(-x c -std=gnu11 -arch x86_64 -mmacosx-version-min=11.0 -mkernel -ffreestanding
    -nostdinc -isystem $RD/include -D_KERNEL -DQWZMAC -DQWZ_DEBUG -I compat/include -I .
    -fno-common -O2 -Wall -Wno-unused-function -Wno-unused-variable
    -Wno-unused-but-set-variable -Wno-sign-conversion -Wno-shorten-64-to-32
    -Wno-implicit-int-conversion -ferror-limit=200)
mkdir -p build/obj
srcs=("$@"); (( $#srcs )) || srcs=(qwz/*.c compat/*.c(N))
rc=0
for f in $srcs; do
  clang "${CF[@]}" -c $f -o build/obj/${f:t:r}.o || rc=1
done
exit $rc
