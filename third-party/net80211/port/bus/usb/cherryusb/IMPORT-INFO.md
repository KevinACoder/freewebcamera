<!--
  @file
  @brief Provenance of the CherryUSB USB backend glue.
-->

# CherryUSB backend glue

This directory holds the glue between the chip drivers (NetBSD
`usbd_*` surface) and an **external** CherryUSB host stack. The
CherryUSB tree itself is no longer vendored here (until 2026-09-22 a
1.6.1 copy lived under `cherryusb/`; it moved out with the library
clean-up - see the repository `IMPORT-INFO.md`). The integrator puts
the CherryUSB host-mode headers on the include path and compiles
`usbh_core.c` + an EHCI/xHCI port driver with its own OSAL.

- **Upstream**: https://github.com/cherry-embedded/CherryUSB (fork
  KevinACoder/CherryUSB)
- **Reference revision the glue was written against**: `1fd876d0`
  (VERSION `1.6.1`, 2026-09-02, "fix usb_osal_freertos:
  sem_take/timer_stop in isr"); CherryUSB API changes after that
  revision are the integrator's to reconcile.
- **License**: Apache-2.0 (upstream tree).

## Files

- `usbdi_compat.c` - the `usbd_*(9)` shim over
  `usbh_control_transfer`/`usbh_submit_urb`; URB completions land in
  an ipl-protected ring and a per-device worker thread runs the
  driver callbacks.
- `usbh_urtwn_class.c` - the CherryUSB class hook: matches VID/PID
  against the chip driver registry and attaches the claimed device.
- `usb_glue_rk3568.c`, `net_bridge.c`, `osal_usb_embox.c` - the embox
  lane's platform glue, worker bridge and CherryUSB OSAL.
- `usb_config.h` - the CherryUSB configuration the glue expects.
- `wlan_port_cherryusb.h` - internal glue header.

## Historical: local patches on the removed 1.6.1 tree

Applied on top of the verbatim 1.6.1 import, all in service of the
embox port; integrators should check these against their own
CherryUSB revision (the first two are RK3568-board-proven and worth
preserving upstream):

1. `port/ehci/usb_hc_ehci.c` `ehci_urb_waitup()`: force a dcache
   invalidate of the URB transfer buffer on completion when
   `CONFIG_USB_DCACHE_ENABLE` is set. The control-transfer direction
   lives in `setup[0]`, not in the endpoint descriptor, so it cannot
   be filtered by EP direction. Without it IN data stays stale in
   cache.
2. `port/ehci/usb_hc_ehci.c` `usb_hc_init()`: call a new weak
   `usb_hc_ehci_post_init(bus)` at the end of host-controller init
   (after RUN/CONFIGFLAG/port power). The RK3568 glue overrides it to
   handle devices that were already plugged at reset (no
   connect-change edge is generated for them; the glue forces a port
   power toggle and seeds the roothub change bitmap).
3. `core/usbh_core.h` + `core/usbh_core.c`: the class-info collector
   uses a leading-dot section `.usbh_class_info` on GNUC. Embox links
   with `--gc-sections` and a generated linker script without a KEEP
   for that section, so the section is renamed to the C-identifier
   `usbh_class_info` and the GNUC branch reads the auto-generated
   `__start_usbh_class_info` / `__stop_usbh_class_info` symbols
   instead. The variables also carry `used, retain` so garbage
   collection keeps them.
