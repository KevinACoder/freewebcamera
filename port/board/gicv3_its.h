/*
 * @file   gicv3_its.h
 * @brief  Board-side ITS/LPI interface.
 *
 * This is not an interface layer: nothing above the board layer should include
 * it. Interrupt handlers are registered through CMSIS irq_ctrl.h's IRQ_SetHandler
 * using the LPI's INTID, exactly as for SPIs and PPIs, so an LPI is not a
 * special case for callers. What is board-specific, and what this header
 * carries, is the addressing and the bring-up entry points.
 *
 * The two table addresses are fixed physical addresses on purpose: the ITS and
 * the redistributor reach them over non-coherent ports, so they must live in
 * the Normal-cacheable window that port/board/mmu.c maps at 0xc0000000, and
 * the cache maintenance in gicv3_its.c is only meaningful if both sides agree
 * on where they are. A macro here and a macro there that drifted apart would
 * fail with total silence.
 */

#ifndef FREEWEBCAMERA_GICV3_ITS_H
#define FREEWEBCAMERA_GICV3_ITS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* LPI configuration (property) and pending tables, inside the Normal window
 * mapped by mmu.c. Must stay within LPI_TABLE_BASE..LPI_TABLE_END there. */
#define LPI_PROP_BASE		0xc0010000UL
#define LPI_PEND_BASE		0xc0020000UL
#define LPI_TABLE_BYTES		0x10000UL	/* 64KiB: 2^16 one-byte entries */

/* The first LPI INTID. LPIs are numbered from here, not from 0: INTID 8192 is
 * LPI 0. Handlers are registered as (IRQ_LPI_INTID_FIRST + lpi). */
#define IRQ_LPI_INTID_FIRST	8192U

/* Bring up the ITS: command queue, device and collection tables, the
 * redistributor's LPI tables, and the collection for this CPU. Idempotent.
 * Returns 0 on success. */
int its_init(void);

/* Map a device's interrupt translation table. `events` is how many event IDs
 * the device may use. Returns 0 on success. */
int its_device_map(uint32_t devid, uint32_t events);

/* Bind one (device, event) pair to an LPI INTID, then make the change visible
 * with INV + SYNC. Returns 0 on success. */
int its_event_map(uint32_t devid, uint32_t eventid, uint32_t lpi);

/* Remove a binding. */
void its_event_unmap(uint32_t devid, uint32_t eventid);

/* LPI configuration. `priority` is the raw 8-bit GIC value; the low two bits
 * are ignored because only four priority bits are implemented. */
void its_lpi_enable(uint32_t lpi, uint8_t priority);
void its_lpi_disable(uint32_t lpi);
uint8_t its_lpi_get_config(uint32_t lpi);

/* Pending state, read from the redistributor's table (one BIT per LPI). */
int its_lpi_is_pending(uint32_t lpi);
void its_clear_pending(uint32_t lpi);

/* The address an endpoint writes to raise an MSI. The value written is the
 * EVENT id; the device is identified by the Requester ID of the write, not by
 * anything in the data. */
uint32_t its_doorbell_addr(void);

/* Make event `eventid` of device `devid` fire.
 *
 * Implemented with the ITS's INT command, NOT a store to the doorbell address.
 * A CPU store has no Requester ID, so the ITS cannot attribute it to a device
 * and drops it - which makes a doorbell-based self-test fail even when every
 * mapping is correct. */
void its_ring_doorbell(uint32_t devid, uint32_t eventid);

/* Exercise the whole path in a four-rung ladder and report how many
 * deliveries were observed. Returns 0 if every rung passed. */
int its_selftest(uint32_t *lpiondelivered);

#ifdef __cplusplus
}
#endif

#endif /* FREEWEBCAMERA_GICV3_ITS_H */
