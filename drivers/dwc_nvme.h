/*
 * @file   dwc_nvme.h
 * @brief  NVMe driver: internal entry points, internal to drivers/.
 *
 * The rest of the project sees include/nvme.h (ARM_DRIVER_NVMe). This header
 * carries what the block-device binding needs before it can address a
 * controller: bring-up and a count. Same split as dwc_ahci.h.
 *
 * PROVENANCE AND PORTING POLICY. The register sequence, queue setup, PRP
 * handling, doorbell order and cache discipline are ported from the author's
 * own embox driver (embox/embox, BSD-2, src/drivers/nvme/nvme.c + nvme.h,
 * commit c1ec19b210, board-verified on this SoC 2026-09-07 with MSI-X
 * interrupts and a write/read loop on the low and high end of the namespace).
 *
 * @author zhugengyu
 * @date   16.09.2026
 */

#ifndef FREEWEBCAMERA_DWC_NVME_H
#define FREEWEBCAMERA_DWC_NVME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Controllers the core can manage (the embox original's ctrl_quantity). This
 * board has one NVMe endpoint, on pcie2x1. */
#define DWC_NVME_CTRL_MAX 2

/* Bring up the PCIe backend if it is not up, scan for class 0108h functions
 * and probe each one: controller enable, admin queue, MSI-X, I/O queue pair,
 * namespace geometry. Idempotent; a controller whose link or BAR never
 * appeared is simply reported and skipped. */
void dwc_nvme_init(void);

/* Controllers that came up. */
uint32_t dwc_nvme_ctrl_count(void);

#ifdef __cplusplus
}
#endif

#endif /* FREEWEBCAMERA_DWC_NVME_H */