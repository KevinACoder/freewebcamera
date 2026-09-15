/*
 * @file   ahci.h
 * @brief  SATA AHCI controller interface - a CMSIS-Driver gap.
 *
 * CMSIS-Driver covers generic storage (Driver_Storage.h) but not SATA/AHCI.
 * Keeps the CMSIS ops-struct idiom so a future standard header can replace it
 * without changing callers.
 *
 * Scope at M0: interface FROZEN, no implementation. The driver lands with M3.
 *
 * CONTRACT:
 *  - Calls are task-context only.
 *  - Two ports on this board (SATA0 @0xFC000000, SATA1 @0xFC400000); `port`
 *    selects one and ports are independent.
 *  - Transfer is asynchronous: the callback runs in ISR context. The caller
 *    owns the buffer and must not reuse it until completion is signalled.
 *  - LBA is 48-bit capable (LBA48); use the Identify data for capacity.
 */

#ifndef FREEWEBCAMERA_AHCI_H
#define FREEWEBCAMERA_AHCI_H

#include <stdbool.h>
#include <stdint.h>

#include "Driver_Common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AHCI_API_VERSION ARM_DRIVER_VERSION_MAJOR_MINOR(0, 1)

#define AHCI_EVENT_READ_DONE   (1UL << 0)
#define AHCI_EVENT_WRITE_DONE  (1UL << 1)
#define AHCI_EVENT_ERROR       (1UL << 2)
#define AHCI_EVENT_PORT_LINK_UP (1UL << 3)

typedef void (*AHCI_SignalEvent_t)(uint32_t event, uint32_t port);

typedef struct _AHCI_CAPABILITIES {
    uint64_t sector_count;
    uint32_t sector_size;       /* bytes per logical block, normally 512 */
    uint32_t max_transfer;      /* largest single transfer, in bytes */
    uint8_t  link_speed;        /* 1 = Gen1 (1.5G), 2 = Gen2 (3G), 3 = Gen3 (6G) */
    uint8_t  port_count;
    uint8_t  supports_ncq;
} AHCI_CAPABILITIES;

typedef struct _ARM_DRIVER_AHCI {
    ARM_DRIVER_VERSION  (*GetVersion)      (void);
    AHCI_CAPABILITIES   (*GetCapabilities)(uint32_t port);
    int32_t             (*Initialize)      (uint32_t port, AHCI_SignalEvent_t cb_event);
    int32_t             (*Uninitialize)    (uint32_t port);
    int32_t             (*PowerControl)    (uint32_t port, uint32_t state);

    /* Link bring-up. Until this succeeds no transfer may be issued; the
     * signature readback is the anchor (compare against the required value). */
    int32_t             (*PortStart)       (uint32_t port);
    int32_t             (*GetSignature)    (uint32_t port, uint32_t *signature);
    int32_t             (*Identify)        (uint32_t port, void *buffer);

    int32_t             (*ReadBlocks)      (uint32_t port, uint64_t lba,
                                            void *data, uint32_t block_count);
    int32_t             (*WriteBlocks)     (uint32_t port, uint64_t lba,
                                            const void *data, uint32_t block_count);
    int32_t             (*Flush)           (uint32_t port);

    int32_t             (*Control)         (uint32_t port, uint32_t control, uint32_t arg);
} ARM_DRIVER_AHCI;

#ifdef __cplusplus
}
#endif

#endif /* FREEWEBCAMERA_AHCI_H */
