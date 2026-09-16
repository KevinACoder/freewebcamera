/*
 * @file   its_test.c
 * @brief  GICv3 ITS command queue and LPI delivery self test.
 *
 * Ported from the author's embox its_test (commit 5becf0576f, board-verified
 * 2026-09-06). Maps a 4x8 grid of synthetic devices/events onto LPIs through
 * the ITS command queue, then injects INT commands and checks that every LPI
 * reaches its own handler, that disabled LPIs stay silent and that
 * re-enabled LPIs fire again. The rungs, the DeviceID range and the stride
 * are the embox values.
 *
 * CMSIS differences from the embox original: handlers take no argument, so
 * each LPI gets a one-line thunk that records the INTID it was registered
 * for; lines are armed with IRQ_SetHandler + IRQ_Enable on the raw INTID,
 * and the enable reaches the property table through the gicv3.c hook into
 * gic_lpi_set_state - the same arm-on-attach order the embox kernel used.
 */

#include <stddef.h>
#include <stdint.h>

#include "board.h"
#include "gicv3_its.h"
#include "irq_ctrl.h"

/* Synthetic device ids: the values are arbitrary, what matters is that
 * every device owns an independent ITT. Stay out of the low range a PCIe
 * requester id occupies, MAPTI would silently rebind the endpoint's
 * events. */
#define ITS_TEST_DEVID(dev)	((uint32_t)(0xE000U + (dev)))
#define ITS_TEST_DEV_CNT	4U
#define ITS_TEST_EVT_CNT	8U

/* Bounded waits: an LPI arrives within microseconds of the INT command,
 * so exhausting the loop means the interrupt was lost. */
#define ITS_TEST_TRIES		10000000

/* Stride of the interleaved pass, co-prime with the grid sizes so
 * consecutive iterations hop between devices and events. */
#define ITS_TEST_STRIDE		7U

static uint32_t test_irq[ITS_TEST_DEV_CNT][ITS_TEST_EVT_CNT];

static volatile int test_last_irq = -1;

/* CMSIS handlers take no argument, so each LPI in the grid gets a thunk
 * that records the INTID it was registered for. */
#define ITS_TEST_HANDLER(dev, evt) \
	static void its_test_h_##dev##_##evt(void) \
	{ test_last_irq = (int)test_irq[dev][evt]; }

ITS_TEST_HANDLER(0, 0)
ITS_TEST_HANDLER(0, 1)
ITS_TEST_HANDLER(0, 2)
ITS_TEST_HANDLER(0, 3)
ITS_TEST_HANDLER(0, 4)
ITS_TEST_HANDLER(0, 5)
ITS_TEST_HANDLER(0, 6)
ITS_TEST_HANDLER(0, 7)
ITS_TEST_HANDLER(1, 0)
ITS_TEST_HANDLER(1, 1)
ITS_TEST_HANDLER(1, 2)
ITS_TEST_HANDLER(1, 3)
ITS_TEST_HANDLER(1, 4)
ITS_TEST_HANDLER(1, 5)
ITS_TEST_HANDLER(1, 6)
ITS_TEST_HANDLER(1, 7)
ITS_TEST_HANDLER(2, 0)
ITS_TEST_HANDLER(2, 1)
ITS_TEST_HANDLER(2, 2)
ITS_TEST_HANDLER(2, 3)
ITS_TEST_HANDLER(2, 4)
ITS_TEST_HANDLER(2, 5)
ITS_TEST_HANDLER(2, 6)
ITS_TEST_HANDLER(2, 7)
ITS_TEST_HANDLER(3, 0)
ITS_TEST_HANDLER(3, 1)
ITS_TEST_HANDLER(3, 2)
ITS_TEST_HANDLER(3, 3)
ITS_TEST_HANDLER(3, 4)
ITS_TEST_HANDLER(3, 5)
ITS_TEST_HANDLER(3, 6)
ITS_TEST_HANDLER(3, 7)

static const IRQHandler_t
its_test_handlers[ITS_TEST_DEV_CNT][ITS_TEST_EVT_CNT] = {
	{ its_test_h_0_0, its_test_h_0_1, its_test_h_0_2, its_test_h_0_3,
	  its_test_h_0_4, its_test_h_0_5, its_test_h_0_6, its_test_h_0_7 },
	{ its_test_h_1_0, its_test_h_1_1, its_test_h_1_2, its_test_h_1_3,
	  its_test_h_1_4, its_test_h_1_5, its_test_h_1_6, its_test_h_1_7 },
	{ its_test_h_2_0, its_test_h_2_1, its_test_h_2_2, its_test_h_2_3,
	  its_test_h_2_4, its_test_h_2_5, its_test_h_2_6, its_test_h_2_7 },
	{ its_test_h_3_0, its_test_h_3_1, its_test_h_3_2, its_test_h_3_3,
	  its_test_h_3_4, its_test_h_3_5, its_test_h_3_6, its_test_h_3_7 }
};

static int its_test_wait_delivery(int expected_irq)
{
	int tries = ITS_TEST_TRIES;

	while (--tries >= 0) {
		if (test_last_irq == expected_irq) {
			return 0;
		}
	}

	return -1;
}

static int its_test_expect_silence(void)
{
	int tries = ITS_TEST_TRIES;

	test_last_irq = -1;
	while (--tries >= 0) {
		/* Nothing to do: a stray delivery shows up as a change
		 * of test_last_irq. */
	}

	return (test_last_irq == -1) ? 0 : -1;
}

static int its_test_send(unsigned int dev, unsigned int evt)
{
	return gic_its_send_int(ITS_TEST_DEVID(dev), evt);
}

static int its_test_map_grid(void)
{
	unsigned int dev;
	unsigned int evt;

	for (dev = 0U; dev < ITS_TEST_DEV_CNT; dev++) {
		int ret = gic_its_device_attach(ITS_TEST_DEVID(dev));

		if (ret != 0) {
			its_log("its_test: device_attach(%#x) failed: %d\n",
				ITS_TEST_DEVID(dev), ret);
			return ret;
		}

		for (evt = 0U; evt < ITS_TEST_EVT_CNT; evt++) {
			int irq = gic_its_event_map(ITS_TEST_DEVID(dev), evt);

			if (irq < 0) {
				its_log("its_test: event_map(%#x, %u) failed:"
					" %d\n",
					ITS_TEST_DEVID(dev), evt, irq);
				return irq;
			}

			/* Arming the line is the standard SPI path: the
			 * handler first, then the enable - which reaches
			 * the property table through gicv3.c. Attaching
			 * after MAPTI is the embox order: the enable's INV
			 * needs the translation to already exist. */
			(void)IRQ_SetHandler((IRQn_ID_t)irq,
					     its_test_handlers[dev][evt]);
			ret = IRQ_Enable((IRQn_ID_t)irq);
			if (ret != 0) {
				its_log("its_test: IRQ_Enable(%d) failed:"
					" %d\n",
					irq, ret);
				return ret;
			}
			test_irq[dev][evt] = (uint32_t)irq;
		}
	}

	return 0;
}

static void its_test_unmap_grid(void)
{
	unsigned int dev;
	unsigned int evt;

	for (dev = 0U; dev < ITS_TEST_DEV_CNT; dev++) {
		for (evt = 0U; evt < ITS_TEST_EVT_CNT; evt++) {
			uint32_t irq = test_irq[dev][evt];
			int check = (int)irq;

			if (check < 0) {
				continue;
			}
			(void)IRQ_SetHandler((IRQn_ID_t)irq, NULL);
			gic_its_event_unmap(ITS_TEST_DEVID(dev), evt);
		}
	}
}

static int its_test_simple_int(uint32_t *delivered)
{
	int ret = its_test_send(0U, 0U);

	if (ret == 0) {
		ret = its_test_wait_delivery((int)test_irq[0][0]);
	}
	if (ret != 0) {
		its_log("its_test: INT dev %#x event 0 not delivered: %d\n",
			ITS_TEST_DEVID(0U), ret);
		return ret;
	}

	(*delivered)++;
	its_log("its_test: single INT delivered to LPI intid %u\n",
		test_irq[0][0]);

	return 0;
}

static int its_test_disable_enable(uint32_t *delivered)
{
	const unsigned int dev = ITS_TEST_DEV_CNT - 1U;
	const unsigned int evt = ITS_TEST_EVT_CNT - 1U;
	const uint32_t irq = test_irq[dev][evt];
	int ret;

	(void)IRQ_Disable((IRQn_ID_t)irq);
	ret = its_test_send(dev, evt);
	if (ret == 0) {
		ret = its_test_expect_silence();
	}
	if (ret == 0) {
		/* Drop anything the ITS buffered while disabled, then
		 * arm the LPI again and expect the next INT to fire. */
		(void)gic_its_send_clear(ITS_TEST_DEVID(dev), evt);
		(void)IRQ_Enable((IRQn_ID_t)irq);
		ret = its_test_send(dev, evt);
	}
	if (ret == 0) {
		ret = its_test_wait_delivery((int)irq);
	}

	if (ret != 0) {
		its_log("its_test: disable/re-enable of intid %u failed:"
			" %d\n",
			irq, ret);
		return ret;
	}

	(*delivered)++;
	its_log("its_test: disabled LPI stayed silent, re-enabled one fired\n");

	return 0;
}

static int its_test_grid_silent_then_refire(uint32_t *delivered)
{
	unsigned int dev;
	unsigned int evt;
	int ret;

	/* Disable the whole grid, then hit every pair: nothing may be
	 * delivered while the property entries say disabled. */
	for (dev = 0U; dev < ITS_TEST_DEV_CNT; dev++) {
		for (evt = 0U; evt < ITS_TEST_EVT_CNT; evt++) {
			(void)IRQ_Disable((IRQn_ID_t)test_irq[dev][evt]);
		}
	}

	for (dev = 0U; dev < ITS_TEST_DEV_CNT; dev++) {
		for (evt = 0U; evt < ITS_TEST_EVT_CNT; evt++) {
			ret = its_test_send(dev, evt);
			if (ret != 0) {
				its_log("its_test: INT dev %u event %u"
					" failed: %d\n",
					dev, evt, ret);
				return ret;
			}
		}
	}
	ret = its_test_expect_silence();
	if (ret != 0) {
		its_log("its_test: a disabled LPI was delivered: %d\n", ret);
		return ret;
	}

	for (dev = 0U; dev < ITS_TEST_DEV_CNT; dev++) {
		for (evt = 0U; evt < ITS_TEST_EVT_CNT; evt++) {
			(void)gic_its_send_clear(ITS_TEST_DEVID(dev), evt);
			(void)IRQ_Enable((IRQn_ID_t)test_irq[dev][evt]);
		}
	}

	for (dev = 0U; dev < ITS_TEST_DEV_CNT; dev++) {
		for (evt = 0U; evt < ITS_TEST_EVT_CNT; evt++) {
			test_last_irq = -1;
			ret = its_test_send(dev, evt);
			if (ret == 0) {
				ret = its_test_wait_delivery(
					(int)test_irq[dev][evt]);
			}
			if (ret != 0) {
				its_log("its_test: re-armed dev %u event %u"
					" failed: %d\n",
					dev, evt, ret);
				return ret;
			}
			(*delivered)++;
		}
	}

	its_log("its_test: %u LPIs silent when disabled, all fired when"
		" re-armed\n",
		ITS_TEST_DEV_CNT * ITS_TEST_EVT_CNT);

	return 0;
}

static int its_test_interleaved_loop(uint32_t *delivered)
{
	const uint32_t total = ITS_TEST_DEV_CNT * ITS_TEST_EVT_CNT;
	uint32_t i;
	int ret;

	for (i = 0U; i < total; i++) {
		uint32_t n = (i * ITS_TEST_STRIDE) % total;
		uint32_t dev = n / ITS_TEST_EVT_CNT;
		uint32_t evt = n % ITS_TEST_EVT_CNT;

		test_last_irq = -1;
		ret = its_test_send(dev, evt);
		if (ret == 0) {
			ret = its_test_wait_delivery((int)test_irq[dev][evt]);
		}
		if (ret != 0) {
			its_log("its_test: interleaved dev %u event %u"
				" failed: %d\n",
				dev, evt, ret);
			return ret;
		}
		(*delivered)++;
	}

	its_log("its_test: %u interleaved INTs all delivered\n", total);

	return 0;
}

int its_selftest(uint32_t *delivered)
{
	uint32_t count = 0U;
	unsigned int dev;
	unsigned int evt;
	int ret;

	if ((its_init() != 0)) {
		its_log("its_test: init failed\n");
		return -1;
	}

	for (dev = 0U; dev < ITS_TEST_DEV_CNT; dev++) {
		for (evt = 0U; evt < ITS_TEST_EVT_CNT; evt++) {
			test_irq[dev][evt] = IRQ_LPI_INTID_FIRST +
					     ITS_LPI_QUANTITY;
		}
	}

	its_log("its_test: doorbell %#llx, %ux%u device/event grid\n",
		(unsigned long long)gic_its_trans_addr(), ITS_TEST_DEV_CNT,
		ITS_TEST_EVT_CNT);

	ret = its_test_map_grid();
	if (ret == 0) {
		ret = its_test_simple_int(&count);
	}
	if (ret == 0) {
		ret = its_test_disable_enable(&count);
	}
	if (ret == 0) {
		ret = its_test_grid_silent_then_refire(&count);
	}
	if (ret == 0) {
		ret = its_test_interleaved_loop(&count);
	}

	its_test_unmap_grid();

	if (ret != 0) {
		its_log("its_test: FAIL (%d)\n", ret);
		return ret;
	}

	if (delivered != NULL) {
		*delivered = count;
	}
	its_log("its_test: PASS\n");

	return 0;
}
