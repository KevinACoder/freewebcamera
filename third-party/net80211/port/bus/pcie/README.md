# PCIe bus backend

Backends for the PCIe bus role: enumerating the PCIe hierarchy,
claiming matched wlan devices and driving their transfers behind the
port interface (`port/port.h`, with PCIe-specific types in
`port_pcie.h`, mirroring `port/bus/usb/port_usb.h`). The USB backend
under `port/bus/usb/cherryusb/` is the reference for the role a bus
backend plays.

## Backends

- `embox/pcie_embox.c` - placeholder for the embox presentation lane
  (embox drives iwm through its own driver framework).

## Status

No production PCIe backend ships with the library yet. A backend
brings its own controller core (for RK3568: a DWC PCIe host driver
derived from a NetBSD `rkdwpcie` reference, keeping the
firmware-trained link untouched) plus the bus_dma/bus_space glue the
iwm driver needs.
