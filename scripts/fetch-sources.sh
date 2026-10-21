#!/bin/zsh
# Fetches the build inputs pinned in config/sources.json (OpenBSD, firmware).
set -e
cd "${0:A:h}/.."
clone() { # url dir commit [sparse paths...]
  local url=$1 dir=$2 rev=$3; shift 3
  if [[ -d $dir ]]; then
    [[ $(git -C $dir remote get-url origin) == $url &&
       $(git -C $dir rev-parse HEAD) == $rev &&
       -z $(git -C $dir status --porcelain) ]] || {
      echo "$dir differs from configured $rev (URL, commit or local edits); inspect it before fetching" >&2
      return 1
    }
    echo "verified $dir $rev"
  else
    git clone --filter=blob:none --no-checkout "$url" $dir
  fi
  if (( $# )); then
    git -C $dir sparse-checkout set --no-cone "$@"
  fi
  git -C $dir checkout -q $rev
}
for name in ${(f)"$(python3 scripts/sources.py list)"}; do
  kind=$(python3 scripts/sources.py get sources.$name.kind)
  url=$(python3 scripts/sources.py get sources.$name.url)
  rev=$(python3 scripts/sources.py get sources.$name.revision)
  destination=$(python3 scripts/sources.py get sources.$name.directory)
  mkdir -p ${destination:h}
  if [[ $kind == git ]]; then
    sparse=(${(f)"$(python3 scripts/sources.py paths $name)"})
    clone "$url" "$destination" "$rev" $sparse
  elif [[ $kind == files ]]; then
    mkdir -p $destination
    for f in ${(f)"$(python3 scripts/sources.py get sources.$name.files)"}; do
      [[ -f $destination/$f ]] || curl -fL -o $destination/$f "$(python3 scripts/sources.py file-url $name $f)"
    done
  else
    echo "unknown source kind: $kind" >&2; exit 1
  fi
done
python3 scripts/check-firmware.py
[[ -d openbsd ]] && echo "skip openbsd/ (scripts/patches.sh check)" || scripts/patches.sh apply
echo "done"
