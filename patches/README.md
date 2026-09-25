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

Current state:

- `net80211/` — three patches against the netbsd-11 pin (all local
  deviations the port needs, registered in `IMPORT-INFO.md`):
  - `0001-compile-out-the-sysctl-tree.patch` — the sysctl configuration
    tree compiles out (`IEEE80211_PORT_NO_SYSCTL`); attach/detach keep
    empty implementations.
  - `0002-aes-ccm-mbuf-offset-order-for-compat-mbufs.patch` — the CCM
    mbuf walker advances the offset before the length test, matching the
    compat mbuf model (one contiguous cluster per mbuf).
  - `0003-usbdi-miss-out-ack-log-without-lwp-chain.patch` — one DPRINTF
    (only compiled under `USB_DEBUG`, which the port turns on) logs
    `curlwp->l_proc->p_pid`/`l_lid`. This port models `curlwp` as the
    CMSIS thread handle, so dereferencing it as a `struct lwp` would fault
    from a log line; the two arguments become 0.
- `threadx/`, `cherrysh/`, `cherryrb/`, `lwip/` — no patches; used
  byte-identical to their pins.
