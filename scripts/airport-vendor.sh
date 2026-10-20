#!/bin/zsh
# Reproduce the pinned ABI/WCL import. Our header changes live in qwzairport.patch.
# apply (default): stage and validate everything before replacing the import.
# check: reproduce in a temporary directory and compare, without writing.
# export: turn edited headers into a patch, verify reproduction, then refresh metadata.
set -e
set -o pipefail
cd "${0:A:h}/.."
mode=${1:-apply}
case $mode in
  apply|check|export) ;;
  *) echo "usage: $0 [apply|check|export]" >&2; exit 1 ;;
esac
SRC=$(python3 scripts/sources.py get sources.laobamac.directory)
REV=$(python3 scripts/sources.py get sources.laobamac.revision)
SOURCE_URL=$(python3 scripts/sources.py get sources.laobamac.url)
[[ -d $SRC/include/Airport ]] || { echo "$SRC missing: run scripts/fetch-sources.sh --imports" >&2; exit 1; }
[[ $(git -C $SRC rev-parse HEAD) == $REV &&
   $(git -C $SRC remote get-url origin) == $SOURCE_URL &&
   -z $(git -C $SRC status --porcelain) ]] || {
  echo "$SRC differs from configured source; inspect it before importing" >&2; exit 1
}
if [[ $mode == apply ]]; then
  # Allow a deliberately edited patch, but never discard edited imported files.
  python3 - <<'PY'
from pathlib import Path
import hashlib
base = Path('airport/abi')
manifest = base / 'SHA256SUMS'
inputs = dict(line.split(maxsplit=1)[::-1] for line in manifest.read_text().splitlines())
expected = {name for name in inputs if name.startswith('airport/abi/')}
actual = {p.as_posix() for p in base.rglob('*') if p.is_file() and p != manifest}
if actual != expected:
    raise SystemExit('ABI inventory changed; inspect and export header edits before applying')
for name, digest in inputs.items():
    p = Path(name)
    if p.is_symlink() or (name != 'airport/abi/qwzairport.patch' and
                         hashlib.sha256(p.read_bytes()).hexdigest() != digest):
        raise SystemExit(f'{name} has local edits; export or restore them before applying')
PY
elif [[ $mode == export ]]; then
  [[ $(head -n 1 airport/abi/UPSTREAM | awk '{print $2}') == $REV ]] || {
    echo "export requires the imported revision; export edits before changing the pin" >&2; exit 1
  }
fi
[[ -z $(find airport/abi -type l -print) && ! -L airport/WCL.hpp ]] || {
  echo "symlink in imported files; inspect before importing or exporting" >&2; exit 1
}
SDK=$(xcrun --sdk macosx --show-sdk-path)
KH="$SDK/System/Library/Frameworks/Kernel.framework/Headers"
TMP=$(mktemp -d); trap 'rm -rf $TMP' EXIT
print '#include <Airport/Apple80211.h>\n#include <Airport/macOS26/FaultReporter.h>' > $TMP/t.cpp
deps=$(clang++ -x c++ -arch x86_64 -mkernel -std=gnu++17 -fapple-kext -fno-rtti -fno-exceptions \
  -isysroot "$SDK" -I"$KH" -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE \
  -D__PRIVATE_SPI__ -DAIRPORT_WCL -D__IO80211_TARGET=__MAC_26_0 -DIO80211FAMILY_V2 \
  -I$SRC/include -M $TMP/t.cpp | tr ' \\' '\n\n' | grep "^$SRC/include/Airport/")
mkdir -p $TMP/a
for f in ${(f)deps}; do
  r=$(python3 -c 'import os,sys; print(os.path.relpath(os.path.normpath(sys.argv[1]), sys.argv[2]))' $f $SRC/include)
  mkdir -p $TMP/a/${r:h}; cp $SRC/include/$r $TMP/a/$r
done
if [[ $mode == export ]]; then
  mkdir -p $TMP/b; cp -R airport/abi/Airport $TMP/b/Airport
  # Prefixes a/Airport and b/Airport make a patch for git apply -p1.
  rc=0
  (cd $TMP && git -c diff.noprefix=false diff --no-index --no-prefix --no-ext-diff \
    --no-color --diff-algorithm=myers a/Airport b/Airport) > $TMP/header.patch || rc=$?
  (( rc <= 1 )) || exit $rc
else
  cp airport/abi/qwzairport.patch $TMP/header.patch
fi
mkdir -p $TMP/out/abi
cp -R $TMP/a/Airport $TMP/out/abi/Airport
cp $SRC/LICENSE $TMP/out/abi/LICENSE
cp $SRC/AirportItlwm/WCL/WCL.hpp $TMP/out/WCL.hpp
cp $TMP/header.patch $TMP/out/abi/qwzairport.patch
if [[ -s $TMP/header.patch ]]; then
  (cd $TMP/out/abi && git apply --check $TMP/header.patch && git apply $TMP/header.patch)
fi
{
  echo "$SOURCE_URL $REV include/Airport + AirportItlwm/WCL/WCL.hpp (GPL-2.0, LICENSE)"
  echo "copied by scripts/airport-vendor.sh, then qwzairport.patch applied"
} > $TMP/out/abi/UPSTREAM
python3 - $TMP/out <<'PY'
from pathlib import Path
import hashlib
import sys
root = Path(sys.argv[1])
files = sorted(p for p in (root / 'abi').rglob('*') if p.is_file())
files.append(root / 'WCL.hpp')
with (root / 'abi/SHA256SUMS').open('w') as out:
    for p in files:
        out.write(hashlib.sha256(p.read_bytes()).hexdigest() + '  airport/' + p.relative_to(root).as_posix() + '\n')
PY
[[ -z $(find $TMP/out -type l -print) ]] || {
  echo "patch produced a symlink; inspect before importing" >&2; exit 1
}
if [[ $mode == check ]]; then
  diff -r $TMP/out/abi airport/abi
  cmp $TMP/out/WCL.hpp airport/WCL.hpp
elif [[ $mode == export ]]; then
  # An exported patch must reproduce the current headers, without changing WCL.
  diff -r $TMP/out/abi/Airport airport/abi/Airport
  cmp $TMP/out/WCL.hpp airport/WCL.hpp
fi
if [[ $mode != check ]]; then
  # Keep the old import until the complete replacement has been validated.
  cp airport/WCL.hpp $TMP/previous-WCL.hpp
  if ! { mv airport/abi $TMP/previous-abi &&
         mv $TMP/out/abi airport/abi &&
         cp $TMP/out/WCL.hpp airport/WCL.hpp; }; then
    if [[ -d $TMP/previous-abi ]]; then
      rm -rf airport/abi
      mv $TMP/previous-abi airport/abi
    fi
    cp $TMP/previous-WCL.hpp airport/WCL.hpp
    exit 1
  fi
fi
echo "airport import $mode: $REV"
