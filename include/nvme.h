/*
 * @file   nvme.h
 * @brief  NVMe controller interface - a CMSIS-Driver gap.
 *
 * CMSIS-Driver has Driver_Storage.h for generic block storage but no NVMe
 * (nor PCIe, SATA, Video). This keeps the CMSIS ops-struct idiom so a future
 * standard header can replace it without touching callers.
 *
 * Scope at M0: interface FROZEN, no implementation. The driver lands with M3
 * and depends on the PCIe interface above, since NVMe sits behind it.
 *
 * CONTRACT:
 *  - Calls are task-context only.
 *  - LBA-addressed, 512-byte logical blocks assumed until the Identify data
 *    says otherwise; `GetCapabilities` is authoritative.
 *  - Completion is asynchronous; the callback runs in ISR context and must
 *    not block. A submission may not reuse a buffer until its completion has
 *    been signalled.
 *  - The caller owns all buffers; the queue pair is owned by the driver.
 */

#ifndef FREEWEBCAMERA_NVME_H
#define FREEWEBCAMERA_NVME_H

#include <stdbool.h>
#include <stdint.h>

#include "Driver_Common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NVME_API_VERSION ARM_DRIVER_VERSION_MAJOR_MINOR(0, 1)

#define NVME_EVENT_READ_DONE   (1UL << 0)
#define NVME_EVENT_WRITE_DONE  (1UL << 1)
#define NVME_EVENT_ERROR       (1UL << 2)

typedef void (*NVME_SignalEvent_t)(uint32_t event, uint32_t ctrl);

typedef struct _NVME_CAPABILITIES {
    uint64_t sector_count;      /* total logical blocks */
    uint32_t sector_size;       /* bytes per logical block, normally 512 */
    uint32_t max_transfer;      /* largest single transfer, in bytes */
    uint32_t queue_depth;       /* submission queue entries */
    uint32_t timeout_ms;        /* controller recommended timeout */
} NVME_CAPABILITIES;

typedef struct _ARM_DRIVER_NVME {
    ARM_DRIVER_VERSION  (*GetVersion)      (void);
    NVME_CAPABILITIES   (*GetCapabilities)(uint32_t ctrl);
    int32_t             (*Initialize)      (uint32_t ctrl, NVME_SignalEvent_t cb_event);
    int32_t             (*Uninitialize)    (uint32_t ctrl);
    int32_t             (*PowerControl)    (uint32_t ctrl, uint32_t state);

    /* Controller identity, filled from the Identify Controller response.
     * `model` must hold room for 40 bytes plus a terminator. */
    int32_t             (*Identify)        (uint32_t ctrl, char *model, uint32_t model_size);

    int32_t             (*ReadBlocks)      (uint32_t ctrl, uint64_t lba,
                                            void *data, uint32_t block_count);
    int32_t             (*WriteBlocks)     (uint32_t ctrl, uint64_t lba,
                                            const void *data, uint32_t block_count);
    int32_t             (*Flush)           (uint32_t ctrl);

    /* Polled completion query for synchronous users: returns the number of
     * completed commands since the last call. */
    uint32_t            (*GetTransferStatus)(uint32_t ctrl);
} ARM_DRIVER_NVME;

#ifdef __cplusplus
}
#endif

#endif /* FREEWEBCAMERA_NVME_H */
