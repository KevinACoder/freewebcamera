/*
 * @file   periph_cmds.c
 * @brief  Shell commands for the board peripherals brought up this round:
 *         the I2C buses and their devices, the on-chip thermal sensor, and
 *         the read-only SPI-NOR view.
 *
 *   i2c scan <0|1>                          probe every 7-bit address
 *   i2c read <bus> <hexaddr> <hexreg> <len> pointer write + restart read
 *   rtc                                     RX8025T time + seconds tick check
 *   rtc set <sec> <min> <hr> <day> <mon> <yr>   write the clock
 *   pwr                                     INA3221 rails (3.3/5/12 V)
 *   temp                                    TSADC CPU/GPU temperature
 *   sfc info | jedec | sfdp | read <hexaddr> [len]
 *                                           SPI-NOR identity and reads ONLY
 *                                           (the driver has no write path)
 *
 * Note: the image is built -mgeneral-regs-only, so every print here is
 * integer; milli-units are split by hand.
 *
 * @author zhugengyu
 * @date   17.09.2026
 */

#include <stdint.h>
#include <string.h>

#include "cmsis_os2.h"

#include "rk_i2c.h"
#include "rk_sfc.h"
#include "rk_tsadc.h"

#include "cherrysh_adapter.h"
#include "csh.h"

/* -nostdlib build: the decimal parser comes from port/board/minilibc.c,
 * declared ad hoc exactly like cherrysh_adapter.c does. */
extern int atoi(const char *s);

/* --- shared helpers ---------------------------------------------------------- */

static ARM_DRIVER_I2C *i2c_bus(int bus)
{
	if (bus == 0) {
		return &Driver_I2C0;
	}
	if (bus == 1) {
		return &Driver_I2C1;
	}
	return NULL;
}

/* Standard CMSIS bring-up: Initialize first, PowerControl(FULL) second -
 * Initialize resets the power state, so the order is load-bearing (same
 * discovery as the console driver). Idempotent. */
static int i2c_ensure_up(ARM_DRIVER_I2C *drv)
{
	if (drv->Initialize(NULL) != ARM_DRIVER_OK) {
		return -1;
	}
	if (drv->PowerControl(ARM_POWER_FULL) != ARM_DRIVER_OK) {
		return -1;
	}
	return 0;
}

/* Pointer write (no stop) + restart read; the common device pattern. */
static int i2c_read_regs(ARM_DRIVER_I2C *drv, uint8_t addr, uint8_t reg,
			 uint8_t *buf, uint32_t len)
{
	if (drv->MasterTransmit(addr, &reg, 1U, true) != ARM_DRIVER_OK) {
		return -1;
	}
	if (drv->MasterReceive(addr, buf, len, false) != ARM_DRIVER_OK) {
		return -1;
	}
	return 0;
}

static int hex_arg(const char *s, uint32_t *out)
{
	uint32_t val = 0U;

	if (s == NULL || *s == '\0') {
		return -1;
	}
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		s += 2;
	}
	while (*s != '\0') {
		char c = *s++;

		val <<= 4;
		if (c >= '0' && c <= '9') {
			val |= (uint32_t)(c - '0');
		} else if (c >= 'a' && c <= 'f') {
			val |= (uint32_t)(c - 'a' + 10);
		} else if (c >= 'A' && c <= 'F') {
			val |= (uint32_t)(c - 'A' + 10);
		} else {
			return -1;
		}
	}
	*out = val;
	return 0;
}

static void hexdump(chry_shell_t *csh, const uint8_t *buf, uint32_t len,
		    uint32_t base_addr)
{
	uint32_t i;

	for (i = 0U; i < len; i++) {
		if ((i % 16U) == 0U) {
			csh_printf(csh, "%06x:", base_addr + i);
		}
		csh_printf(csh, " %02x", buf[i]);
		if ((i % 16U) == 15U) {
			csh_printf(csh, "\r\n");
		}
	}
	if ((len % 16U) != 0U) {
		csh_printf(csh, "\r\n");
	}
}

/* --- i2c ---------------------------------------------------------------------- */

static uint8_t i2c_buf[160];

static int cmd_i2c(int argc, char **argv)
{
	chry_shell_t *csh = CSH_FROM_ARGV(argc, argv);
	ARM_DRIVER_I2C *drv;
	uint32_t bus;

	if (argc < 3) {
		csh_printf(csh, "usage: i2c scan <0|1> | "
			   "i2c read <bus> <hexaddr> <hexreg> <len>\r\n");
		return -1;
	}
	if (hex_arg(argv[2], &bus) != 0 || bus > 1U) {
		csh_printf(csh, "i2c: bad bus '%s'\r\n", argv[2]);
		return -1;
	}
	drv = i2c_bus((int)bus);
	if (i2c_ensure_up(drv) != 0) {
		csh_printf(csh, "i2c%u: bring-up failed\r\n",
			   (unsigned int)bus);
		return -1;
	}

	if (strcmp(argv[1], "scan") == 0) {
		uint8_t addr;
		unsigned int found = 0U;

		csh_printf(csh, "i2c%u scan:\r\n", (unsigned int)bus);
		for (addr = 0x08U; addr <= 0x77U; addr++) {
			if (drv->MasterTransmit(addr, NULL, 0U, false) ==
			    ARM_DRIVER_OK) {
				csh_printf(csh, "  0x%02x ack\r\n", addr);
				found++;
			}
		}
		csh_printf(csh, "i2c%u: %u device(s)\r\n",
			   (unsigned int)bus, found);
		return 0;
	}

	if (strcmp(argv[1], "read") == 0) {
		uint32_t addr, reg, len;

		if (argc != 6 ||
		    hex_arg(argv[3], &addr) != 0 || addr > 0x7FU ||
		    hex_arg(argv[4], &reg) != 0 || reg > 0xFFU ||
		    hex_arg(argv[5], &len) != 0 || len == 0U ||
		    len > sizeof(i2c_buf)) {
			csh_printf(csh, "usage: i2c read <bus> <hexaddr>"
				   " <hexreg> <len 1..%u>\r\n",
				   (unsigned int)sizeof(i2c_buf));
			return -1;
		}
		if (i2c_read_regs(drv, (uint8_t)addr, (uint8_t)reg,
				  i2c_buf, len) != 0) {
			csh_printf(csh, "i2c%u: read 0x%02x reg 0x%02x"
				   " failed (nak or timeout)\r\n",
				   (unsigned int)bus, addr, reg);
			return -1;
		}
		hexdump(csh, i2c_buf, len, reg);
		return 0;
	}

	csh_printf(csh, "i2c: unknown subcommand '%s'\r\n", argv[1]);
	return -1;
}

/* --- rtc (RX8025T @ i2c1 0x32) -------------------------------------------------- */

#define RX8025_ADDR	0x32U

static uint8_t bcd_encode(uint8_t v)
{
	return (uint8_t)(((v / 10U) << 4) | (v % 10U));
}

static uint8_t bcd_decode(uint8_t raw, uint8_t mask)
{
	raw &= mask;
	return (uint8_t)((((raw >> 4) & 0xFU) * 10U) + (raw & 0xFU));
}

static int rx8025_read_time(ARM_DRIVER_I2C *drv, uint8_t t[7])
{
	return i2c_read_regs(drv, RX8025_ADDR, 0x00U, t, 7U);
}

static void rx8025_print(chry_shell_t *csh, const uint8_t t[7])
{
	csh_printf(csh, "rtc: 20%02u-%02u-%02u wd%u %02u:%02u:%02u"
		   " (raw %02x %02x %02x %02x %02x %02x %02x)\r\n",
		   bcd_decode(t[6], 0xFFU), bcd_decode(t[5], 0x1FU),
		   bcd_decode(t[4], 0x3FU), t[3],
		   bcd_decode(t[2], 0x3FU), bcd_decode(t[1], 0x7FU),
		   bcd_decode(t[0], 0x7FU),
		   t[0], t[1], t[2], t[3], t[4], t[5], t[6]);
}

static int cmd_rtc(int argc, char **argv)
{
	chry_shell_t *csh = CSH_FROM_ARGV(argc, argv);
	ARM_DRIVER_I2C *drv = &Driver_I2C1;
	uint8_t t1[7], t2[7];
	uint32_t sec1, sec2;

	if (i2c_ensure_up(drv) != 0) {
		csh_printf(csh, "rtc: i2c1 bring-up failed\r\n");
		return -1;
	}

	if (argc >= 8 && strcmp(argv[1], "set") == 0) {
		uint8_t wr[8];
		unsigned int v[6];
		int i;

		/* sec min hour mday month year(2-digit) */
		for (i = 0; i < 6; i++) {
			v[i] = (unsigned int)atoi(argv[2 + i]);
		}
		wr[0] = 0x00U;	/* pointer: seconds register */
		wr[1] = bcd_encode((uint8_t)v[0]);	/* sec, STOP bit clear */
		wr[2] = bcd_encode((uint8_t)v[1]);
		wr[3] = bcd_encode((uint8_t)v[2]);
		wr[4] = 0x00U;	/* weekday (not tracked) */
		wr[5] = bcd_encode((uint8_t)v[3]);
		wr[6] = bcd_encode((uint8_t)v[4]);
		wr[7] = bcd_encode((uint8_t)v[5]);
		if (drv->MasterTransmit(RX8025_ADDR, wr, sizeof(wr), false) !=
		    ARM_DRIVER_OK) {
			csh_printf(csh, "rtc: set failed\r\n");
			return -1;
		}
	}

	if (rx8025_read_time(drv, t1) != 0) {
		csh_printf(csh, "rtc: read failed\r\n");
		return -1;
	}
	rx8025_print(csh, t1);

	/* the tick check is the acceptance: the battery path is known broken
	 * on this board, so time does not survive a cold boot - but the
	 * seconds register must advance while powered */
	sec1 = bcd_decode(t1[0], 0x7FU);
	osDelay(1100);
	if (rx8025_read_time(drv, t2) != 0) {
		csh_printf(csh, "rtc: second read failed\r\n");
		return -1;
	}
	rx8025_print(csh, t2);
	sec2 = bcd_decode(t2[0], 0x7FU);
	{
		uint32_t delta = (sec2 >= sec1) ? (sec2 - sec1)
						: (sec2 + 60U - sec1);
		csh_printf(csh, "rtc: seconds tick +%u in 1.1 s\r\n", delta);
	}
	return 0;
}

/* --- pwr (INA3221 @ i2c1 0x40) --------------------------------------------------- */

#define INA3221_ADDR	0x40U

static int ina_read16(ARM_DRIVER_I2C *drv, uint8_t reg, uint16_t *val)
{
	uint8_t b[2];

	if (i2c_read_regs(drv, INA3221_ADDR, reg, b, 2U) != 0) {
		return -1;
	}
	*val = (uint16_t)(((uint16_t)b[0] << 8) | b[1]);	/* MSB first */
	return 0;
}

static int cmd_pwr(int argc, char **argv)
{
	chry_shell_t *csh = CSH_FROM_ARGV(argc, argv);
	ARM_DRIVER_I2C *drv = &Driver_I2C1;
	uint16_t mfr, die, conf;
	static const char *const rails[3] = { "3V3", "5V0", "12V" };
	int c;

	(void)argc; (void)argv;

	if (i2c_ensure_up(drv) != 0) {
		csh_printf(csh, "pwr: i2c1 bring-up failed\r\n");
		return -1;
	}
	if (ina_read16(drv, 0xFEU, &mfr) != 0 ||
	    ina_read16(drv, 0xFFU, &die) != 0 ||
	    ina_read16(drv, 0x00U, &conf) != 0) {
		csh_printf(csh, "pwr: ina3221 not responding\r\n");
		return -1;
	}
	csh_printf(csh, "pwr: ina3221 mfr %04x die %04x conf %04x\r\n",
		   mfr, die, conf);

	for (c = 0; c < 3; c++) {
		uint16_t bus_mv, shunt_raw;

		if (ina_read16(drv, (uint8_t)(0x02U + c * 2U), &bus_mv) != 0 ||
		    ina_read16(drv, (uint8_t)(0x01U + c * 2U), &shunt_raw) != 0) {
			csh_printf(csh, "  %s: read failed\r\n", rails[c]);
			continue;
		}
		/* bus: 1 mV/LSB on this board (confirmed twice).
		 * shunt: 40 uV/LSB over 20 mOhm -> mA = raw * 2. */
		csh_printf(csh, "  %s: %u mV, %d mA\r\n", rails[c],
			   (unsigned int)bus_mv,
			   (int)((int16_t)shunt_raw * 2));
	}
	return 0;
}

/* --- temp (on-chip TSADC) ---------------------------------------------------------- */

static int cmd_temp(int argc, char **argv)
{
	chry_shell_t *csh = CSH_FROM_ARGV(argc, argv);
	int32_t cpu_mc, gpu_mc;

	(void)argc; (void)argv;

	if (rk_tsadc_init() != 0) {
		csh_printf(csh, "temp: tsadc init failed\r\n");
		return -1;
	}
	if (rk_tsadc_read_mc(RK_TSADC_SRC_CPU, &cpu_mc) != 0) {
		csh_printf(csh, "temp: cpu sensor not converting\r\n");
		return -1;
	}
	if (rk_tsadc_read_mc(RK_TSADC_SRC_GPU, &gpu_mc) != 0) {
		csh_printf(csh, "temp: gpu sensor not converting\r\n");
		return -1;
	}
	csh_printf(csh, "temp: cpu %d.%03u C, gpu %d.%03u C\r\n",
		   cpu_mc / 1000,
		   (unsigned int)(cpu_mc % 1000),
		   gpu_mc / 1000,
		   (unsigned int)(gpu_mc % 1000));
	return 0;
}

/* --- sfc (SPI NOR, READ ONLY) -------------------------------------------------------- */

static uint8_t sfc_buf[4096] __attribute__((aligned(4)));

static int cmd_sfc(int argc, char **argv)
{
	chry_shell_t *csh = CSH_FROM_ARGV(argc, argv);
	uint8_t id[3];

	if (Driver_Flash0.Initialize(NULL) != ARM_DRIVER_OK) {
		csh_printf(csh, "sfc: init failed\r\n");
		return -1;
	}
	if (rk_sfc_get_jedec(id) != 0) {
		csh_printf(csh, "sfc: no jedec id\r\n");
		return -1;
	}

	if (argc < 2 || strcmp(argv[1], "info") == 0) {
		ARM_FLASH_INFO *info = Driver_Flash0.GetInfo();

		csh_printf(csh, "sfc: jedec %02x %02x %02x"
			   " (expected ef 60 17 = w25q64dw)\r\n",
			   id[0], id[1], id[2]);
		csh_printf(csh, "sfc: %u KiB, sector %u, page %u,"
			   " READ-ONLY driver\r\n",
			   (unsigned int)info->sector_count *
			   info->sector_size / 1024U,
			   info->sector_size, info->page_size);
		return 0;
	}

	if (strcmp(argv[1], "jedec") == 0) {
		csh_printf(csh, "sfc jedec: %02x %02x %02x\r\n",
			   id[0], id[1], id[2]);
		return 0;
	}

	if (strcmp(argv[1], "sfdp") == 0) {
		if (rk_sfc_read_sfdp(0U, sfc_buf, 8U) != 0) {
			csh_printf(csh, "sfc: sfdp read failed\r\n");
			return -1;
		}
		hexdump(csh, sfc_buf, 8U, 0U);
		csh_printf(csh, "sfc: sfdp magic %c%c%c%c\r\n",
			   sfc_buf[0], sfc_buf[1], sfc_buf[2], sfc_buf[3]);
		return 0;
	}

	if (strcmp(argv[1], "read") == 0) {
		uint32_t addr = 0U, len = 256U;
		int32_t got;
		uint32_t i, not_ff = 0U;

		if (argc >= 3 && hex_arg(argv[2], &addr) != 0) {
			csh_printf(csh, "sfc: bad address\r\n");
			return -1;
		}
		if (argc >= 4 && (hex_arg(argv[3], &len) != 0 || len == 0U ||
				  len > sizeof(sfc_buf))) {
			csh_printf(csh, "sfc: bad length (1..%u)\r\n",
				   (unsigned int)sizeof(sfc_buf));
			return -1;
		}
		got = Driver_Flash0.ReadData(addr, sfc_buf, len);
		if (got < 0) {
			csh_printf(csh, "sfc: read 0x%06x failed (%d)\r\n",
				   addr, got);
			return -1;
		}
		hexdump(csh, sfc_buf, (uint32_t)got, addr);
		for (i = 0U; i < (uint32_t)got; i++) {
			if (sfc_buf[i] != 0xFFU) {
				not_ff++;
			}
		}
		csh_printf(csh, "sfc: read %u bytes from 0x%06x,"
			   " %u non-FF\r\n",
			   (unsigned int)got, addr, (unsigned int)not_ff);
		return 0;
	}

	csh_printf(csh, "usage: sfc info | jedec | sfdp |"
		   " read <hexaddr> [len]   (no write operations exist)\r\n");
	return -1;
}

CSH_CMD_EXPORT_ALIAS_FULL(cmd_i2c, i2c,
			  "i2c scan <0|1> | i2c read <bus> <addr> <reg> <len>",
			  "I2C bus probe and device register read");
CSH_CMD_EXPORT_ALIAS_FULL(cmd_rtc, rtc,
			  "rtc | rtc set <sec> <min> <hr> <day> <mon> <yr>",
			  "RX8025T RTC: read time / set time / tick check");
CSH_CMD_EXPORT_ALIAS_FULL(cmd_pwr, pwr,
			  "pwr",
			  "INA3221 rails: 3.3V / 5V / 12V voltage and current");
CSH_CMD_EXPORT_ALIAS_FULL(cmd_temp, temp,
			  "temp",
			  "on-chip TSADC: CPU and GPU temperature");
CSH_CMD_EXPORT_ALIAS_FULL(cmd_sfc, sfc,
			  "sfc info | jedec | sfdp | read <hexaddr> [len]",
			  "SPI NOR identity and read (read-only, no writes)");
