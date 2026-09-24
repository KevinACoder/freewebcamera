# patches/ — local deviations from the pinned upstreams

Every `third-party/` component is a git submodule pinned to an upstream
commit (see `IMPORT-INFO.md` for the pins). Any change we need on top of an
upstream tree lives here as a numbered patch, **never** as an in-tree edit of
the submodule working tree:

    patches/<component>/NNNN-what-and-why.patch

`make modules` re-initializes the submodules at their pins and replays every
patch (`git apply`, idempotent: an already-applied patch is reported and
kept).

Rules:

- One logical change per patch; the commit subject says what and why.
- Every patch is registered in `IMPORT-INFO.md` in the same commit that adds
  it.
- If an upstream update rewords the touched lines, rebase the patch onto the
  new pin, bump the pin and the registration together.
- Components without a usable upstream git home (today: `tlsf`, vendored from
  the baisoku.org 3.1 package) are vendored instead of submoduled; their
  provenance lives in the tree (`third-party/tlsf/PROVENANCE.md`) and their
  registration in `IMPORT-INFO.md`.

Current state: **no patches** — all three submoduled components are used
byte-identical to their pins (verified by diff against the pinned commits).
