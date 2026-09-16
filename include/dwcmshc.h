/*
 * @file   dwcmshc.h
 * @brief  Interface to the DWC MSHC host controller driver (SDHCI 4.20a
 *         register interface, the eMMC controller on this board).
 *         Implemented by drivers/dwc_mshc.c.
 *
 * PROVENANCE. The register sequences - the CRU CCLK/BCLK source selection,
 * the SDHCI clock tree with the 50 MHz source bypass (this board has no 52M
 * oscillator tap; 52 MHz is served from the 50M source), the DLL bypass
 * discipline below 100 MHz, the SDMA engine with its 512 KiB boundary
 * restart, the strict 8/16/32-bit access widths, and the R2 response
 * realignment this controller needs (its 136-bit response arrives shifted
 * by one byte) - are the author's own RK3568 port, debugged on this board
 * in the standalone line and carried over verbatim (decision D27). The
 * surrounding vocabulary changed to this project's primitives; the ADMA2
 * descriptor path of the original was not carried over (the driver runs
 * SDMA; the ADMA2 list code was dead there too).
 *
 * One controller instance (instance 0 = eMMC @0xFE310000), configured by the
 * board table in drivers/rk3568_sdmmc.c.
 *
 * @author zhugengyu
 * @date   16.09.2026
 */

#ifndef FREEWEBCAMERA_DWCMSHC_H
#define FREEWEBCAMERA_DWCMSHC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Error convention: 0 is success, everything else is one of these. */
#define DWCMSHC_SUCCESS		0
#define DWCMSHC_ERR_NOT_READY	(-1)
#define DWCMSHC_ERR_TIMEOUT	(-2)
#define DWCMSHC_ERR_CMD_FAILED	(-3)
#define DWCMSHC_ERR_DATA_FAILED	(-4)
#define DWCMSHC_ERR_CARD_NO_FOUND (-5)
#define DWCMSHC_ERR_INVALID_BUF	(-6)
#define DWCMSHC_ERR_NOT_SUPPORT	(-7)
#define DWCMSHC_ERR_WRITE_DISABLE (-8)

/* Event kinds for dwc_mshc_register_event_handler (interrupt mode). */
enum {
	DWCMSHC_EVT_CARD_REMOVED = 0,
	DWCMSHC_EVT_CMD_DONE,
	DWCMSHC_EVT_CMD_ERROR,
	DWCMSHC_EVT_CMD_RESP_ERROR,
	DWCMSHC_EVT_DATA_ERROR,
	DWCMSHC_EVT_DATA_READ_DONE,
	DWCMSHC_EVT_DATA_WRITE_DONE,

	DWCMSHC_EVT_NUM
};

/* Data descriptor for a data command. The buffer must be 4-byte aligned and
 * word-accessible; SDMA moves it in place (identity map: VA == bus address). */
struct dwc_mshc_data {
	uint8_t *buf;
	uint32_t blksz;
	uint32_t blkcnt;
	uint32_t datalen;
};

/* Command descriptor. flag carries the DWCMSHC_CMD_FLAG_* bits. */
struct dwc_mshc_cmd {
	uint32_t cmdidx;
	uint32_t cmdarg;
	uint32_t resptype;
	uint32_t response[4];
	uint32_t flag;
#define DWCMSHC_CMD_FLAG_NEED_STOP	(1u << 0)
#define DWCMSHC_CMD_FLAG_NEED_INIT	(1u << 1)
#define DWCMSHC_CMD_FLAG_EXP_RESP	(1u << 2)
#define DWCMSHC_CMD_FLAG_EXP_LONG_RESP	(1u << 3)
#define DWCMSHC_CMD_FLAG_NEED_RESP_CRC	(1u << 4)
#define DWCMSHC_CMD_FLAG_EXP_DATA	(1u << 5)
#define DWCMSHC_CMD_FLAG_WRITE_DATA	(1u << 6)
#define DWCMSHC_CMD_FLAG_READ_DATA	(1u << 7)
	struct dwc_mshc_data *data_p;
};

/* Response-type encodings for the resptype field: these are the CMD register
 * values the controller decodes (public vocabulary - the adapter's transfer
 * conversion writes them). */
#define DWCMSHC_RESP_TYPE_NONE		0U
#define DWCMSHC_RESP_TYPE_136		(1u << 0)
#define DWCMSHC_RESP_TYPE_48		(1u << 1)
#define DWCMSHC_RESP_TYPE_48_BUSY	((1u << 1) | (1u << 0))

/* Per-controller coordinates, from the board table (rk3568_sdmmc.c). */
struct dwc_mshc_config {
	uint32_t instance_id;
	uintptr_t base_addr;
	uint32_t irq_num;
};

typedef void (*dwc_mshc_event_handler_t)(void *args);

/* Driver instance, owned by the caller (one static per controller). */
struct dwc_mshc {
	struct dwc_mshc_config config;
	uint32_t is_ready;
	uint32_t cur_source_clock;	/* CRU CCLK_EMMC source, for FREQ_SEL */
	uint32_t cur_card_clk;		/* current card clock (Hz) */
	uintptr_t cur_dma_phy;		/* SDMA transfer start bus address */
	uint32_t cur_dma_len;		/* SDMA transfer length (bytes) */
	uint32_t cur_dma_boundary;	/* 512 KiB boundaries crossed so far */
	bool cur_is_write;		/* data direction, IRQ-mode routing */
	dwc_mshc_event_handler_t evt_handler[DWCMSHC_EVT_NUM];
	void *evt_args[DWCMSHC_EVT_NUM];
};

/* The board table (drivers/rk3568_sdmmc.c). */
const struct dwc_mshc_config *dwc_mshc_config(unsigned int id);

/* Type aliases the host glue uses (the standalone vocabulary, kept so the
 * glue bodies stay verbatim). */
typedef struct dwc_mshc dwc_mshc_t;
typedef struct dwc_mshc_config dwc_mshc_config_t;
typedef struct dwc_mshc_cmd dwc_mshc_cmd_t;
typedef struct dwc_mshc_data dwc_mshc_data_t;

/* Bring the controller up: CRU gates/resets/sources, pin mux, soft reset,
 * internal clock, 8-bit + SDMA mode, 1.8V signaling, power, 400 kHz. */
int dwc_mshc_initialize(struct dwc_mshc *inst,
			const struct dwc_mshc_config *config);

/* Quiesce and clear the instance. */
void dwc_mshc_deinitialize(struct dwc_mshc *inst);

/* Controller soft reset (SW_RST_ALL), waiting for self-clear. */
int dwc_mshc_software_reset(uintptr_t base_addr, int retries);

/* Card clock: CRU source selection + SDHCI divider + DLL bypass/lock. */
int dwc_mshc_set_card_clk(struct dwc_mshc *inst, uint32_t clk_freq_hz);

/* Bus width: 1/4/8. */
void dwc_mshc_set_card_bus_width(struct dwc_mshc *inst, uint32_t bus_width);

/* HS200/HS400 tuning (CMD21, PIO); not reached at this board's 52 MHz cap. */
int dwc_mshc_execute_tuning(struct dwc_mshc *inst, uint32_t tuning_cmd,
			    uint8_t *buf, uint32_t blk_size);

/* Send a command (and its data phase, if any), waiting for completion. */
int dwc_mshc_poll_transfer(struct dwc_mshc *inst, struct dwc_mshc_cmd *cmd);

/* Interrupt mode: start a command, completion arrives via the handlers. */
int dwc_mshc_interrupt_transfer(struct dwc_mshc *inst, struct dwc_mshc_cmd *cmd);

uint32_t dwc_mshc_get_interrupt_mask(struct dwc_mshc *inst);
void dwc_mshc_set_interrupt_mask(struct dwc_mshc *inst, uint32_t mask, bool enable);
void dwc_mshc_enable_interrupt_mode(struct dwc_mshc *inst, bool enable);
void dwc_mshc_interrupt_handler(void *param);
void dwc_mshc_register_event_handler(struct dwc_mshc *inst, uint32_t event,
				     dwc_mshc_event_handler_t handler,
				     void *args);

/* Data-line busy state (SDHCI Present State, DAT[0] low = busy): true = busy. */
bool dwc_mshc_data_busy(struct dwc_mshc *inst);

#ifdef __cplusplus
}
#endif

#endif /* FREEWEBCAMERA_DWCMSHC_H */
