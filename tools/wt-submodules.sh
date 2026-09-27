#!/bin/sh
# Bootstrap a fresh worktree's submodules from an existing sibling checkout,
# without re-downloading anything.
#
# WHY THIS EXISTS
#
# Submodule clones are per-worktree: `git submodule update --init` populates
# .git/worktrees/<name>/modules/ from the URL in .gitmodules, so every new
# worktree pays for the whole clone itself.  For third-party/libbsd that URL
# is the netbsd-src fork and the objects are multi-GB (measured 3.6 GB, ~100k
# loose/packed objects), fetched over SSH: minutes per fresh worktree, and the
# only slow step in `make modules` - the other six submodules are small.
#
# MEASURED (2026-09-28, this repo)
#
#     plain `make modules` in a fresh worktree      > 25 min (SSH clone, aborted)
#     URL redirected to the main checkout            1 min 46 s (hardlink clone:
#                                                    sys time is the object store)
#     THIS script, shared clones                      1 s  (alternates, zero copies)
#
# HOW
#
# Two worktree-scoped settings:
#
#   1. `submodule.<name>.url` -> the local checkout, so any later `git submodule
#      update` fetches from disk instead of the network;
#   2. a pre-seeded shared clone of each submodule's git dir, so `git submodule
#      update --init` finds the repository already present and only checks out
#      the gitlink - `--shared` writes an alternates file, so no object is
#      copied at all.
#
# Both live in this worktree's `.git/worktrees/<name>/config.worktree` (or the
# modules dir), so the shared config and .gitmodules keep the recorded upstream
# URLs: provenance, `make sync` and patch replay are unchanged.  The upstream
# URL is still what a plain `git submodule update` uses in a checkout that never
# ran this script.
#
# USAGE (fresh worktree, before the first `make modules`):
#
#     tools/wt-submodules.sh [<source-root>]
#
# <source-root> defaults to the repository's main worktree.  Afterwards run
# `make modules` (or `make wt-modules`, which runs both).

set -eu

root=$(git rev-parse --show-toplevel)
cd "$root"

src=${1:-$(git worktree list --porcelain | awk '/^worktree / { print $2; exit }')}
src=$(cd "$src" && pwd -P)
here=$(pwd -P)
if [ ! -d "$src/third-party" ]; then
	echo "wt-submodules: source root $src has no third-party/ - pass one explicitly" >&2
	exit 1
fi
if [ "$src" = "$here" ]; then
	echo "wt-submodules: this IS the source root; nothing to do"
	exit 0
fi

git config extensions.worktreeConfig true

# Submodule git dirs for this worktree (git lays them out under the worktree's
# git dir; the main worktree instead shares .git/modules).
gitdir=$(git rev-parse --git-dir)
modules="$gitdir/modules"

for path in $(git config -f .gitmodules --get-regexp '^submodule\..*\.path$' | awk '{ print $2 }'); do
	if [ ! -d "$src/$path/.git" ] && [ ! -f "$src/$path/.git" ]; then
		echo "wt-submodules: $src/$path not initialized, keeping the recorded URL"
		continue
	fi

	git config --worktree "submodule.$path.url" "$src/$path"

	if [ -d "$modules/$path" ]; then
		echo "wt-submodules: $path already seeded"
		continue
	fi
	mkdir -p "$(dirname "$modules/$path")"
	# --separate-git-dir puts the git dir exactly where git's own submodule
	# layout wants it (worktree/.git is then a file pointing there, which is
	# how the superproject's `submodule update` recognizes it as populated);
	# --shared writes an alternates file instead of copying objects - 45 ms
	# for the 3.6 GB netbsd-src store, against 1m46 hardlinking it.
	rm -rf "$root/$path"
	git clone --shared --no-checkout --quiet \
		--separate-git-dir "$modules/$path" "$src/$path" "$root/$path"

	# Carry the source's sparse mask over BEFORE the checkout below: git then
	# materializes only the sparse subset instead of writing and pruning the
	# full tree (the checkout is the bulk of the remaining time; libbsd's
	# full tree is ~7 GB against a 33-line mask).  The mask itself stays the
	# source's, so it cannot drift from what `make modules` later enforces.
	srcgit=$src/$path/.git
	if [ -f "$srcgit" ]; then
		srcgit=$(cd "$src/$path" && git rev-parse --git-dir)
	fi
	if [ -f "$srcgit/info/sparse-checkout" ]; then
		mkdir -p "$modules/$path/info"
		cp "$srcgit/info/sparse-checkout" "$modules/$path/info/sparse-checkout"
		git -C "$root/$path" config core.sparseCheckout true
		git -C "$root/$path" config core.sparseCheckoutCone false
		echo "wt-submodules: $path seeded (shared clone + sparse mask)"
	else
		echo "wt-submodules: $path seeded (shared clone)"
	fi
done

# The seeded HEAD already holds the gitlink commit, and plain `submodule
# update` treats "HEAD is the wanted commit" as nothing to do - it would leave
# the working tree empty.  --force materializes it, honoring the sparse mask
# installed above.
git -c protocol.file.allow=always submodule update --init --recursive --force

echo "wt-submodules: done; now run make modules (patch replay)"
