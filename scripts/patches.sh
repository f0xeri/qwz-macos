#!/bin/zsh
# OpenBSD sources as upstream + patches. Only patches/openbsd/ is committed:
#   BASE      the OpenBSD commit we build on
#   FILES     which OpenBSD files we use (paths as in openbsd/src)
#   *.patch   our changes, one git commit each (git format-patch)
# The working copy openbsd/ (gitignored) is a git repo of its own: tag
# "upstream" holds the pristine files, every commit on top of it is one patch.
#
# usage: scripts/patches.sh apply [--force]  build openbsd/ from BASE + patches
#        scripts/patches.sh export           rewrite patches/openbsd/*.patch from openbsd/
#        scripts/patches.sh check            is openbsd/ in sync with patches/? (build.sh runs it)
#        scripts/patches.sh update <commit>  move to another OpenBSD commit, replaying the patches
#
# Changing the driver: edit files in openbsd/, commit there (git -C openbsd
# commit -a for a new patch, or --amend / --fixup + rebase -i for an existing
# one), run "scripts/patches.sh export", then commit patches/ in this repo.
set -e
cd "${0:A:h}/.."
P=patches/openbsd U=third_party/openbsd W=openbsd
BASE=$(<$P/BASE)
FILES=(${(f)"$(grep -v '^#' $P/FILES)"})
SYNCED=$W/.git/qwzmac-synced	# HEAD of openbsd/ when it last matched patches/

die() { print -u2 "patches.sh: $*"; exit 1 }
g() { git -C $W "$@" }
dirty() { [[ -n $(g status --porcelain) ]] }
busy() { [[ -d $W/.git/rebase-merge || -d $W/.git/rebase-apply ]] }
upstream_rev() { g log -1 --format=%s upstream | sed -n 's/^OpenBSD //p' }

# Commit rev of OpenBSD's FILES into openbsd/ (on top of HEAD, if any).
import() {
	local rev=$1 f
	[[ -d $U/.git ]] || die "no $U: run scripts/fetch-sources.sh"
	git -C $U cat-file -e "$rev^{commit}" 2>/dev/null || git -C $U fetch -q origin $rev
	rev=$(git -C $U rev-parse "$rev^{commit}")
	for f in $FILES; do
		mkdir -p $W/${f:h}
		# The clone is partial: show fetches blobs outside the sparse checkout.
		git -C $U show "$rev:$f" > $W/$f 2>/dev/null || die "$f is not in OpenBSD $rev"
	done
	g add -A
	GIT_AUTHOR_NAME=OpenBSD GIT_AUTHOR_EMAIL=upstream@openbsd.invalid \
	GIT_COMMITTER_NAME=OpenBSD GIT_COMMITTER_EMAIL=upstream@openbsd.invalid \
	    g commit -q --allow-empty -m "OpenBSD $rev"
}

# Our commits as patches. Fixed options, so the output does not depend on
# the git config or the size of the repo, and does not change unless the
# patch itself does.
format() {
	g -c diff.noprefix=false -c diff.mnemonicPrefix=false format-patch -q \
	    --no-numbered --zero-commit --no-signature --full-index \
	    --diff-algorithm=myers --no-renames -o ${1:A} upstream..HEAD
}

apply() {
	if [[ -e $W ]]; then
		[[ -d $W/.git ]] || die "$W exists but is not a git repo: move it away"
		if [[ $1 != --force ]]; then
			busy && die "$W: a rebase or am is in progress: finish it, then export"
			dirty && die "$W has uncommitted changes: commit them there and export, or apply --force to drop them"
			[[ -f $SYNCED && $(g rev-parse HEAD) == $(<$SYNCED) ]] ||
			    die "$W has commits that are not in $P: export them, or apply --force to drop them"
		fi
		rm -rf $W
	fi
	mkdir -p $W
	git -C $W init -q -b main
	import $BASE
	g tag upstream
	local p=($P/*.patch(N))
	if (( $#p )); then
		g am -q --3way --committer-date-is-author-date $p:A ||
		    die "a patch does not apply to $BASE: fix it in $W (git am --continue), then export"
	fi
	g rev-parse HEAD > $SYNCED
	print "$W: OpenBSD ${BASE[1,11]} + $#p patches"
}

export_() {
	[[ -d $W/.git ]] || die "no $W: run scripts/patches.sh apply"
	busy && die "$W: a rebase or am is in progress: finish it first"
	dirty && die "$W has uncommitted changes: commit them there first (git -C $W commit -a, or --amend)"
	[[ $(upstream_rev) == $BASE ]] || die "$W is on OpenBSD $(upstream_rev), $P/BASE says $BASE"
	rm -f $P/*.patch(N)
	format $P
	g rev-parse HEAD > $SYNCED
	local p=($P/*.patch(N))
	print "exported $#p patches to $P"
	git status --short -- $P 2>/dev/null || true
}

# 0: in sync. 1: something to do (printed).
check() {
	[[ -d $W/.git ]] || { print "$W is missing: run scripts/patches.sh apply"; return 1 }
	busy && { print "$W: a rebase or am is in progress"; return 1 }
	[[ $(upstream_rev) == $BASE ]] || { print "$W is on another OpenBSD commit than $P/BASE: run scripts/patches.sh apply"; return 1 }
	[[ ${(pj:\n:)${(o)FILES}} == $(g ls-tree -r --name-only upstream | sort) ]] ||
	    { print "$P/FILES changed: run scripts/patches.sh apply"; return 1 }
	local t=$(mktemp -d) f same=1 a b
	format $t
	a=($t/*.patch(N:t)) b=($P/*.patch(N:t))
	[[ "$a" == "$b" ]] || same=0
	(( same )) && for f in $a; do cmp -s $t/$f $P/$f || { same=0; break }; done
	rm -rf $t
	local rc=0
	if (( ! same )); then
		if [[ -f $SYNCED && $(g rev-parse HEAD) == $(<$SYNCED) ]]; then
			print "$P changed (git pull?): run scripts/patches.sh apply"
		else
			print "$W has commits that are not in $P: run scripts/patches.sh export"
		fi
		rc=1
	fi
	dirty && { print "$W has uncommitted changes (not in any patch yet)"; rc=1 }
	return rc
}

update() {
	[[ -n $1 ]] || die "usage: scripts/patches.sh update <openbsd commit>"
	[[ -d $W/.git ]] || die "no $W: run scripts/patches.sh apply"
	busy && die "$W: a rebase or am is in progress: finish it first"
	dirty && die "$W has uncommitted changes: commit or drop them first"
	local old=$(g rev-parse upstream) br=$(g symbolic-ref --short HEAD)
	g checkout -q --detach upstream
	g rm -rq .
	import $1
	local new=$(g rev-parse HEAD) rev=$(g log -1 --format=%s | sed 's/^OpenBSD //')
	g checkout -q $br
	g tag -f upstream $new >/dev/null
	print $rev > $P/BASE
	BASE=$rev
	print "OpenBSD ${old[1,7]} -> ${rev[1,11]}: what changed upstream: git -C $W diff $old upstream"
	if ! g rebase -q --onto upstream $old $br; then
		print -u2 "\nA patch conflicts with the new OpenBSD code. Fix the files in $W,"
		print -u2 "then: git -C $W add <files> && git -C $W rebase --continue"
		print -u2 "and:  scripts/patches.sh export"
		exit 1
	fi
	export_
}

case $1 in
apply)	apply $2 ;;
export)	export_ ;;
check)	check ;;
update)	update $2 ;;
*)	sed -n '2,15s/^# \{0,1\}//p' $0; exit 1 ;;
esac
