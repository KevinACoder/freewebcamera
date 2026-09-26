# patches/ — local deviations from the pinned upstreams

Every `third-party/` component is a git submodule pinned to an upstream
commit (see `IMPORT-INFO.md` for the pins). Any change we need on top of an
upstream tree lives here as a numbered patch, **never** as an in-tree edit of
the submodule working tree:

    patches/<component>/NNNN-what-and-why.patch

`make modules` re-initializes the submodules at their pins and replays every
patch (`git apply`, idempotent: an already-applied patch is reported and
kept).  A replayed working tree is an intermediate state, not an end state:
`make sync` materializes it as a commit on branch `fwc/<component>` inside
the submodule (that commit's parent IS the pin) and stages the gitlink bump
for the round's parent commit — so the repo ends every round with a clean
`git status`.  The patches here stay the source of truth; the `fwc/*` commits
are mechanical and are never pushed to a submodule's origin.

Rules:

- One logical change per patch; the commit subject says what and why.
- Every patch is registered in `IMPORT-INFO.md` in the same commit that adds
  it.
- If an upstream update rewords the touched lines, rebase the patch onto the
  new pin, bump the pin and the registration together.
- End of every round: `make sync`, commit the staged gitlink, `git status`
  clean.
- Components without a usable upstream git home (today: `tlsf`, vendored from
  the baisoku.org 3.1 package) are vendored instead of submoduled; their
  provenance lives in the tree (`third-party/tlsf/PROVENANCE.md`) and their
  registration in `IMPORT-INFO.md`.

Current state:

- `net80211/` — five patches against the netbsd-11 pin (all local
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
  - `0004-urtwn-cmd-ring-keep-oldest-on-overflow.patch` — a full host
    command ring used to advance `ring->cur` unconditionally, letting the
    new command clobber the oldest unprocessed one (whose callback then
    never runs). The scan heartbeat's only `callout_schedule` re-arm
    lives at the end of `urtwn_newstate_cb`, so one clobbered newstate
    command parked the state machine mid-bitmap on the first cold-boot
    scan (evidence 20260925, chan 2422). The ring also grows 32 → 64
    slots; on overflow the new command is dropped loudly instead.
  - `0005-xhci-debug-level-from-opt.patch` — `xhcidebug` initializes from
    `XHCI_DEBUG_DEFAULT` (same treatment as ehci's debug level) instead of
    a hard 0, so the xHCI debug verbosity is compile-time configurable via
    the compat `opt_usb.h` (introduced with the xHCI line).
  - `0007-iwm-rx-rearm-slot-before-early-returns.patch` — `iwm_rx_rx_mpdu()`
    re-points the ring slot's mbuf at the frame *before* its two early
    returns (bad phy-info, bad CRC/overrun), so any dropped frame used to
    leave the slot poisoned: the RBD keeps the buffer's original address
    (the device rebuilds it as `rbd << 8`) while the driver reads from the
    moved `m_data` — a 12-byte shift that turns every later DMA into that
    slot into a garbage packet.  The garbage is dropped (the slot never
    recovers) and, when it looks like a TX response, walks `txd->in ==
    NULL` into a null dereference; the same stream feeds bogus indices to
    the command ring, which is how the trunk image flooded the console and
    then crashed.  The re-arm now happens before both returns, and a failed
    re-arm restores the slot's DMA view before dropping the frame.  Board
    evidence: flood gone, no KASSERT/crash, WPA2 association succeeds
    (evidence 20260926-feat-net80211_refine).
  - `0008-iwm-throttle-unhandled-response-print.patch` — the `default:` of
    `iwm_notif_intr()`'s response switch printed unconditionally; one
    corrupted ring slot is re-read on every wrap, so it buried the console
    at tens of lines per second.  First 8 lines, then one per thousand,
    each carrying an `n=` counter (same intent as 0003: keep the
    diagnostic, lose the flood).
- `threadx/`, `cherrysh/`, `cherryrb/`, `lwip/` — no patches; used
  byte-identical to their pins.
