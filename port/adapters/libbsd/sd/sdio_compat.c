/*
 * @file
 * @brief sdmmc(9) shim: the sdmmc_io_* register file the imported SDIO
 *        wlan drivers call, routed onto the claiming backend's
 *        per-device wlan_sdio_bus_ops (CMD52 one-byte, CMD53
 *        incremental; bursts over one 512-byte block go out in block
 *        mode, which requires the function's block length to be set).
 *
 * Same role as the usbdi shim for the USB bus; there is no worker
 * machinery here because the transport is synchronous from the
 * calling thread and the drivers bring their own kthread worker.
 *
 * @date 22.09.2026
 * @author zhugengyu
 */

#include <errno.h>

#include <dev/sdmmc/sdmmcvar.h>

static const struct wlan_sdio_bus_ops *
sf_ops(struct sdmmc_function *sf) {
	if (sf == NULL || sf->sf_port == NULL) {
		return NULL;
	}
	return sf->sf_port->ops;
}

uint8_t
sdmmc_io_read_1(struct sdmmc_function *sf, uint32_t addr)
{
	const struct wlan_sdio_bus_ops *ops = sf_ops(sf);

	if (ops == NULL || ops->read_1 == NULL) {
		return 0xff;
	}
	return ops->read_1(sf->sf_port, addr);
}

uint16_t
sdmmc_io_read_2(struct sdmmc_function *sf, uint32_t addr)
{
	const struct wlan_sdio_bus_ops *ops = sf_ops(sf);

	if (ops == NULL || ops->read_2 == NULL) {
		return 0xffff;
	}
	return ops->read_2(sf->sf_port, addr);
}

uint32_t
sdmmc_io_read_4(struct sdmmc_function *sf, uint32_t addr)
{
	const struct wlan_sdio_bus_ops *ops = sf_ops(sf);

	if (ops == NULL || ops->read_4 == NULL) {
		return 0xffffffffu;
	}
	return ops->read_4(sf->sf_port, addr);
}

int
sdmmc_io_write_1(struct sdmmc_function *sf, uint32_t addr, uint8_t val)
{
	const struct wlan_sdio_bus_ops *ops = sf_ops(sf);

	if (ops == NULL || ops->write_1 == NULL) {
		return ENXIO;
	}
	return ops->write_1(sf->sf_port, addr, val);
}

int
sdmmc_io_write_2(struct sdmmc_function *sf, uint32_t addr, uint16_t val)
{
	const struct wlan_sdio_bus_ops *ops = sf_ops(sf);

	if (ops == NULL || ops->write_2 == NULL) {
		return ENXIO;
	}
	return ops->write_2(sf->sf_port, addr, val);
}

int
sdmmc_io_write_4(struct sdmmc_function *sf, uint32_t addr, uint32_t val)
{
	const struct wlan_sdio_bus_ops *ops = sf_ops(sf);

	if (ops == NULL || ops->write_4 == NULL) {
		return ENXIO;
	}
	return ops->write_4(sf->sf_port, addr, val);
}

int
sdmmc_io_read_region_1(struct sdmmc_function *sf, uint32_t addr,
    u_char *buf, int len)
{
	const struct wlan_sdio_bus_ops *ops = sf_ops(sf);

	if (ops == NULL || ops->read_region == NULL) {
		return ENXIO;
	}
	return ops->read_region(sf->sf_port, addr, buf, len);
}

int
sdmmc_io_write_region_1(struct sdmmc_function *sf, uint32_t addr,
    const u_char *buf, int len)
{
	const struct wlan_sdio_bus_ops *ops = sf_ops(sf);

	if (ops == NULL || ops->write_region == NULL) {
		return ENXIO;
	}
	return ops->write_region(sf->sf_port, addr, buf, len);
}

int
sdmmc_io_set_blocklen(struct sdmmc_function *sf, int len)
{
	const struct wlan_sdio_bus_ops *ops = sf_ops(sf);

	if (ops == NULL || ops->set_blocklen == NULL) {
		return ENXIO;
	}
	return ops->set_blocklen(sf->sf_port, len);
}

int
sdmmc_io_function_enable(struct sdmmc_function *sf)
{
	const struct wlan_sdio_bus_ops *ops = sf_ops(sf);

	if (ops == NULL || ops->func_enable == NULL) {
		return ENXIO;
	}
	return ops->func_enable(sf->sf_port);
}

/* NET80211_PORT(L): SDIO card interrupt channel (M11 r4). NetBSD hangs
 * the handler off the host controller and re-arms internally; our line
 * is level-signalled and self-masked by the ISR, so the consumer acks
 * after draining. The cookie is the claimed wlan_sdio_dev. */
void *
sdmmc_intr_establish(struct sdmmc_function *sf, int (*fn_ih)(void *),
    void *arg)
{
	const struct wlan_sdio_bus_ops *ops = sf_ops(sf);

	if (ops == NULL || ops->intr_establish == NULL) {
		return NULL;
	}
	return ops->intr_establish(sf->sf_port, fn_ih, arg);
}

void
sdmmc_intr_ack(void *cookie)
{
	struct wlan_sdio_dev *dev = cookie;

	if (dev != NULL && dev->ops->intr_ack != NULL) {
		dev->ops->intr_ack(dev);
	}
}

void
sdmmc_intr_disestablish(void *cookie)
{
	struct wlan_sdio_dev *dev = cookie;

	if (dev != NULL && dev->ops->intr_release != NULL) {
		dev->ops->intr_release(dev);
	}
}
