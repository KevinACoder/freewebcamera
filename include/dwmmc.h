/*
 * @file   dwmmc.h
 * @brief  Interface to the DW-MMC host controller driver (the sdmmc0 slot,
 *         an SDIO card on this board). Implemented by drivers/dwc_mmc.c.
 *
 * PROVENANCE. The register sequences - the CRU clock selection, the CIU
 * clock-update protocol (UPD_CLK | WAIT_PRV_DAT, the VOLT_SWITCH variant
 * around CMD11), the IDMAC descriptor discipline and the command poll loops
 * - are the author's own RK3568 port of the DesignWare Mobile Storage Host
 * driver, debugged on this board in the standalone bring-up line and carried
 * over verbatim (decision D27). Only the surrounding vocabulary changed:
 * this project's register/cache primitives, a plain error convention and a
 * static DMA area instead of a non-cacheable heap.
 *
 * One controller instance per index, as configured by the board table in
 * port/board/rk3568/rk3568_sdmmc.c (instance 0 = sdmmc0 @0xFE2B0000). Callers
 * above the drivers layer reach this only through this header.
 *
 * @author zhugengyu
 * @date   16.09.2026
 */

#ifndef FREEWEBCAMERA_DWMMC_H
#define FREEWEBCAMERA_DWMMC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Error convention: 0 is success, everything else is one of these. */
#define DWMMC_SUCCESS		0
#define DWMMC_ERR_NOT_READY	(-1)
#define DWMMC_ERR_TIMEOUT	(-2)
#define DWMMC_ERR_CMD_FAILED	(-3)
#define DWMMC_ERR_DATA_FAILED	(-4)
#define DWMMC_ERR_CARD_NO_FOUND	(-5)
#define DWMMC_ERR_INVALID_BUF	(-6)
#define DWMMC_ERR_NOT_SUPPORT	(-7)
#define DWMMC_ERR_WRITE_DISABLE	(-8)

/* Event kinds for dwc_mmc_register_event_handler (interrupt mode). */
enum {
	DWMMC_EVT_CARD_REMOVED = 0,
	DWMMC_EVT_CMD_DONE,
	DWMMC_EVT_CMD_ERROR,
	DWMMC_EVT_CMD_RESP_ERROR,
	DWMMC_EVT_DATA_ERROR,
	DWMMC_EVT_DATA_READ_DONE,
	DWMMC_EVT_DATA_WRITE_DONE,
	/* SDIO card interrupt (RINTSTS bit 16, the DAT1 line). The handler
	 * runs in ISR context: wake a worker and nothing more - the bus is
	 * not touchable here. The ISR self-masks the bit; the consumer
	 * re-arms it through dwc_mmc_set_interrupt_mask once the chip-side
	 * source has been consumed. */
	DWMMC_EVT_SDIO_INT,

	DWMMC_EVT_NUM
};

/* Data descriptor for a data command. The buffer must be 4-byte aligned and
 * word-accessible; it is DMA'd in place (identity-mapped, so its address is
 * also the bus address). */
struct dwc_mmc_data {
	uint8_t *buf;
	uint32_t blksz;
	uint32_t blkcnt;
	uint32_t datalen;
};

/* Command descriptor. flag carries the DWMMC_CMD_FLAG_* bits. */
struct dwc_mmc_cmd {
	uint32_t cmdidx;
	uint32_t cmdarg;
	uint32_t resptype;
	uint32_t response[4];
	uint32_t flag;
#define DWMMC_CMD_FLAG_NEED_STOP	(1u << 0)
#define DWMMC_CMD_FLAG_NEED_INIT	(1u << 1)
#define DWMMC_CMD_FLAG_EXP_RESP		(1u << 2)
#define DWMMC_CMD_FLAG_EXP_LONG_RESP	(1u << 3)
#define DWMMC_CMD_FLAG_NEED_RESP_CRC	(1u << 4)
#define DWMMC_CMD_FLAG_EXP_DATA		(1u << 5)
#define DWMMC_CMD_FLAG_WRITE_DATA	(1u << 6)
#define DWMMC_CMD_FLAG_READ_DATA	(1u << 7)
#define DWMMC_CMD_FLAG_NEED_AUTO_STOP	(1u << 8)
	struct dwc_mmc_data *data_p;
};

/* Per-controller coordinates, from the board table (rk3568_sdmmc.c). */
struct dwc_mmc_config {
	uint32_t instance_id;
	uintptr_t base_addr;
	uint32_t irq_num;
	/* The slot has no working card-detect line (an SDIO module is
	 * soldered in): never trust CDETECT, behave as always-present. */
	bool cd_broken;
};

typedef void (*dwc_mmc_event_handler_t)(void *args);

/* Driver instance. The owner allocates it (the adapter keeps one static per
 * controller) and passes it to every call. */
struct dwc_mmc {
	struct dwc_mmc_config config;
	uint32_t is_ready;
	uint32_t cur_source_clock;	/* CRU-selected source, for CLKDIV */
	struct dwc_mmc_idma_desc *dma_desc;
	uint32_t dma_desc_num;
	bool cur_is_write;		/* data direction, IRQ-mode DTO routing */
	bool in_volt_switch;		/* CMD11 handshake: UPD_CLK + VOLT_SWITCH */
	dwc_mmc_event_handler_t evt_handler[DWMMC_EVT_NUM];
	void *evt_args[DWMMC_EVT_NUM];
};

/* IDMAC descriptor capacity: 4 KiB per descriptor, 256 cover a 1 MiB
 * transfer (the adapter's largest request). */
#define DWMMC_DMA_DESC_MAX_NUM	256U

/* Type aliases the host glue uses (the standalone vocabulary, kept so the
 * glue bodies stay verbatim). */
typedef struct dwc_mmc dwc_mmc_t;
typedef struct dwc_mmc_config dwc_mmc_config_t;
typedef struct dwc_mmc_cmd dwc_mmc_cmd_t;
typedef struct dwc_mmc_data dwc_mmc_data_t;

/* The board table (port/board/rk3568/rk3568_sdmmc.c). */
const struct dwc_mmc_config *dwc_mmc_config(unsigned int id);

/* Bring the controller up: CRU gates/resets/source clock, soft reset, FIFO
 * thresholds, power, 400 kHz identity clock. Returns DWMMC_ERR_CARD_NO_FOUND
 * when CDETECT reports no card. */
int dwc_mmc_initialize(struct dwc_mmc *inst, const struct dwc_mmc_config *config);

/* Quiesce and clear the instance. */
void dwc_mmc_deinitialize(struct dwc_mmc *inst);

/* Controller/FIFO/DMA soft reset, waiting for self-clear. */
int dwc_mmc_software_reset(uintptr_t base_addr, int retries);

/* Card clock: CRU source selection + CIU divider update. */
int dwc_mmc_set_card_clk(struct dwc_mmc *inst, uint32_t clk_freq_hz);

/* Signal voltage select (UHS_REG bit 0). */
int dwc_mmc_set_signal_voltage(struct dwc_mmc *inst, bool v18);

/* Sample-phase tuning across CRU sdmmc_con phase steps (SDR50/SDR104). */
int dwc_mmc_sample_tuning(struct dwc_mmc *inst, uint32_t tuning_cmd,
			  uint8_t *buf, uint32_t blk_size);

/* Bus width: 1/4/8. */
void dwc_mmc_set_card_bus_width(struct dwc_mmc *inst, uint32_t bus_width);

/* Send a command (and its data phase, if any), waiting for completion. */
int dwc_mmc_poll_transfer(struct dwc_mmc *inst, struct dwc_mmc_cmd *cmd);

/* Interrupt mode: start a command, completion arrives via the handlers. */
int dwc_mmc_interrupt_transfer(struct dwc_mmc *inst, struct dwc_mmc_cmd *cmd);

uint32_t dwc_mmc_get_interrupt_mask(struct dwc_mmc *inst);
void dwc_mmc_set_interrupt_mask(struct dwc_mmc *inst, uint32_t mask, bool enable);
void dwc_mmc_interrupt_handler(void *param);
void dwc_mmc_register_event_handler(struct dwc_mmc *inst, uint32_t event,
				    dwc_mmc_event_handler_t handler, void *args);

/* Recover a wedged controller from an error state. */
int dwc_mmc_restart(struct dwc_mmc *inst);

/* Data-line busy state (STATUS bit 9, inverted DAT0): true = busy. */
bool dwc_mmc_data_busy(struct dwc_mmc *inst);

/* Card-detect line (CDETECT bit 0): true = card present. Note the CDETECT
 * debouncer needs sustained identification clock - a caller after the init
 * sequence is fine, one sampling cold hardware is not. */
bool dwc_mmc_card_present(struct dwc_mmc *inst);

#ifdef __cplusplus
}
#endif

#endif /* FREEWEBCAMERA_DWMMC_H */
