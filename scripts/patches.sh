#!/bin/zsh
# OpenBSD sources as upstream + patches. Only patches/openbsd/ is committed:
#   BASE            the OpenBSD commit we build on
#   FILES           which OpenBSD files we use (paths as in openbsd/src)
#   <path>.patch    our changes to one file, e.g. sys/dev/ic/qwz.c.patch
# The working copy openbsd/ (gitignored) is the pristine files with the
# patches applied. Just edit it; the "why" of a change goes into a QWZMAC
# comment in the code. Nothing is committed there: inside it is a git repo
# whose only commit (tag "upstream") is the pristine OpenBSD, so
# "git -C openbsd diff" shows all our changes.
#
# usage: scripts/patches.sh apply [--force]  build openbsd/ from BASE + patches
#        scripts/patches.sh export           rewrite the patches from openbsd/
#        scripts/patches.sh check            is openbsd/ in sync with patches/? (build.sh runs it)
#        scripts/patches.sh update <commit>  move to another OpenBSD commit, keeping our changes
set -e
cd "${0:A:h}/.."
P=patches/openbsd U=third_party/openbsd W=openbsd
BASE=$(<$P/BASE)
FILES=(${(f)"$(grep -v '^#' $P/FILES)"})
SYNCED=$W/.git/qwzmac-synced	# fingerprint of the patches when openbsd/ last matched them

die() { print -u2 "patches.sh: $*"; exit 1 }
g() { git -C $W "$@" }
upstream_rev() { g log -1 --format=%s upstream | sed -n 's/^OpenBSD //p' }
fetch() { # rev -> full hash, fetched into the clone if needed
	[[ -d $U/.git ]] || die "no $U: run scripts/fetch-sources.sh"
	git -C $U cat-file -e "$1^{commit}" 2>/dev/null || git -C $U fetch -q origin $1
	git -C $U rev-parse "$1^{commit}"
}

# The patches for openbsd/ as it is now, into directory $1. Fixed options,
# so the output depends only on the files, not on the git config.
gen() {
	local f
	for f in $FILES; do
		if [[ ! -f $W/$f ]]; then
			# New in FILES since the last apply: nothing of ours in it yet.
			g cat-file -e upstream:$f 2>/dev/null || continue
			die "$W/$f is missing (deleted?): restore it or apply --force"
		fi
		g diff --quiet upstream -- $f && continue
		mkdir -p $1/${f:h}
		g -c diff.noprefix=false -c diff.mnemonicPrefix=false diff --no-color \
		    --no-ext-diff --diff-algorithm=myers --no-renames upstream -- $f |
		    sed '/^index /d' > $1/$f.patch
	done
}
patches() { (cd $1 && print -l **/*.patch(N.)) }
fingerprint() { # dir: one hash for all patches in it
	(cd $1 && for f in **/*.patch(N.); do print -r -- "== $f"; cat $f; done) | shasum | cut -c1-40
}
work_fp() { local t=$(mktemp -d); gen $t; fingerprint $t; rm -rf $t }

# Commit OpenBSD rev's FILES as the new tag "upstream", without touching
# the working tree.
commit_upstream() {
	local rev=$1 f idx=$(mktemp)
	rm -f $idx
	for f in $FILES; do
		git -C $U cat-file -e "$rev:$f" 2>/dev/null || die "$f is not in OpenBSD $rev"
	done
	for f in $FILES; do
		# The clone is partial: show fetches blobs outside its sparse checkout.
		GIT_INDEX_FILE=$idx g update-index --add --cacheinfo \
		    100644,$(git -C $U show "$rev:$f" | g hash-object -w --stdin),$f
	done
	local tree=$(GIT_INDEX_FILE=$idx g write-tree) parent=() c
	rm -f $idx
	g rev-parse -q --verify upstream >/dev/null && parent=(-p upstream)
	c=$(GIT_AUTHOR_NAME=OpenBSD GIT_AUTHOR_EMAIL=upstream@openbsd.invalid \
	    GIT_COMMITTER_NAME=OpenBSD GIT_COMMITTER_EMAIL=upstream@openbsd.invalid \
	    g commit-tree $tree $parent -m "OpenBSD $rev")
	g update-ref refs/heads/main $c
	g tag -f upstream $c >/dev/null
	g reset -q upstream	# index = new upstream, working tree untouched
}

apply() {
	if [[ -e $W ]]; then
		[[ -d $W/.git ]] || die "$W exists but is not a git repo: move it away"
		if [[ $1 != --force ]]; then
			local w=$(work_fp)
			[[ $w == $(fingerprint $P) || ( -f $SYNCED && $w == $(<$SYNCED) ) ]] ||
			    die "$W has changes that are not in $P: export them, or apply --force to drop them"
		fi
		rm -rf $W
	fi
	mkdir -p $W
	git -C $W init -q -b main
	commit_upstream $(fetch $BASE)
	g checkout -q -- .
	local p=(${(f)"$(patches $P)"})
	if (( $#p )); then
		g apply --whitespace=nowarn $P/${^p}(:A) ||
		    die "the patches do not apply to OpenBSD $BASE"
	fi
	fingerprint $P > $SYNCED
	print "$W: OpenBSD ${BASE[1,11]} + $#p patched files"
}

export_() {
	[[ -d $W/.git ]] || die "no $W: run scripts/patches.sh apply"
	[[ $(upstream_rev) == $BASE ]] || die "$W is on OpenBSD $(upstream_rev), $P/BASE says $BASE"
	local c=(${(f)"$(grep -lr '^<<<<<<< ' $W/sys 2>/dev/null)"})
	(( $#c )) && die "$W still has conflict markers in: $c"
	local t=$(mktemp -d) f
	gen $t
	find $P -name '*.patch' -delete
	find $P -mindepth 1 -type d -empty -delete
	for f in ${(f)"$(patches $t)"}; do
		mkdir -p $P/${f:h}; cp $t/$f $P/$f
	done
	rm -rf $t
	fingerprint $P > $SYNCED
	print "exported $(patches $P | grep -c .) patched files to $P"
	git status --short -- $P 2>/dev/null || true
}

# 0: in sync. 1: something to do (printed).
check() {
	[[ -d $W/.git ]] || { print "$W is missing: run scripts/patches.sh apply"; return 1 }
	[[ $(upstream_rev) == $BASE ]] ||
	    { print "$W is on another OpenBSD commit than $P/BASE: run scripts/patches.sh apply"; return 1 }
	[[ ${(pj:\n:)${(o)FILES}} == $(g ls-tree -r --name-only upstream | sort) ]] ||
	    { print "$P/FILES changed: run scripts/patches.sh apply"; return 1 }
	local w=$(work_fp) p=$(fingerprint $P) s=
	[[ -f $SYNCED ]] && s=$(<$SYNCED)
	[[ $w == $p ]] && return 0
	if [[ $w == $s ]]; then
		print "$P changed (git pull?): run scripts/patches.sh apply"
	elif [[ $p == $s ]]; then
		print "$W has changes that are not in $P yet: run scripts/patches.sh export"
	else
		print "both $W and $P changed: export would overwrite the new patches, apply would drop your edits"
	fi
	return 1
}

update() {
	[[ -n $1 ]] || die "usage: scripts/patches.sh update <openbsd commit>"
	[[ -d $W/.git ]] || die "no $W: run scripts/patches.sh apply"
	check >/dev/null || die "$W is not in sync with $P: run scripts/patches.sh check"
	local rev=$(fetch $1) old=$(g rev-parse upstream) t=$(mktemp -d) f n conflicts=()
	for f in $FILES; do
		mkdir -p $t/old/${f:h}
		g show upstream:$f > $t/old/$f
	done
	commit_upstream $rev
	for f in $FILES; do
		if cmp -s $W/$f $t/old/$f; then	# not ours: take OpenBSD's
			g checkout -q upstream -- $f
			continue
		fi
		g show upstream:$f > $t/new
		# Three-way merge: our version, old upstream, new upstream.
		n=0; git merge-file -L ours -L "OpenBSD ${old[1,11]}" -L "OpenBSD ${rev[1,11]}" \
		    $W/$f $t/old/$f $t/new || n=$?
		(( n > 127 )) && die "merge-file failed on $f"
		(( n )) && conflicts+=$f
	done
	rm -rf $t
	print $rev > $P/BASE
	BASE=$rev
	print "OpenBSD ${old[1,11]} -> ${rev[1,11]}. What changed upstream: git -C $W diff upstream^ upstream"
	if (( $#conflicts )); then
		print -u2 "\nOur changes conflict with the new OpenBSD code in: $conflicts"
		print -u2 "Fix the <<<<<<< ======= >>>>>>> blocks there, then: scripts/patches.sh export"
		exit 1
	fi
	export_
}

case $1 in
apply)	apply $2 ;;
export)	export_ ;;
check)	check ;;
update)	update $2 ;;
*)	sed -n '2,16s/^# \{0,1\}//p' $0; exit 1 ;;
esac
