/*
 * @file   dwc_ahci.h
 * @brief  DesignWare AHCI: board coordinates, internal to drivers/.
 *
 * The interface the rest of the project sees is include/ahci.h
 * (ARM_DRIVER_AHCI). This header only carries the per-board coordinates, the
 * same split as dwc_eqos/dwc_eqos_rk3568.h: the core knows registers and
 * command formats, rk3568_sata.c knows where the controllers are.
 *
 * PROVENANCE AND PORTING POLICY. The register sequences, FIS/PRD/command-list
 * layouts, poll bounds and cache discipline are ported from the author's own
 * embox driver (embox/embox, BSD-2, src/drivers/ahci/dwc_ahci.c, commit
 * 2092447348, board-verified on this SoC 2026-09-11 with both SATA ports
 * reading and writing). Where that driver makes a choice - the firmware-owned
 * PHY, "slot 0 only, polled", the 64 KiB chunk, the unconditional flush before
 * the PRD is fetched - the choice is kept.
 *
 * @author zhugengyu
 * @date   16.09.2026
 */

#ifndef FREEWEBCAMERA_DWC_AHCI_H
#define FREEWEBCAMERA_DWC_AHCI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One AHCI controller's coordinates. This board has two, each with a single
 * implemented port behind its own combo PHY: SATA0 at 0xFC000000 (the upright
 * socket) and SATA1 at 0xFC400000 (the M.2 2280 slot). */
struct dwc_ahci_plat {
	uintptr_t base_addr;
	uint32_t ctrl_count;
	uint32_t port_count;	/* implemented ports per controller */
	uint32_t stride;	/* controller-to-controller distance */
};

extern const struct dwc_ahci_plat dwc_ahci_plat;

/* Ports this core can manage (2 controllers x 1 port today). */
#define DWC_AHCI_PORT_MAX 4

/* Probe every port: inherit the firmware-trained link, take the command
 * engine over and read the device's geometry. Idempotent. Ports whose link is
 * down (no device, unwired controller) are skipped, which is a reportable
 * state rather than an error. */
void dwc_ahci_init(void);

/* Number of ports that came up. */
uint32_t dwc_ahci_port_count(void);

#ifdef __cplusplus
}
#endif

#endif /* FREEWEBCAMERA_DWC_AHCI_H */