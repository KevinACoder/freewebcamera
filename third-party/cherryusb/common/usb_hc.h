/*
 * Copyright (c) 2022, sakumisu
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef USB_HC_H
#define USB_HC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*usbh_complete_callback_t)(void *arg, int nbytes);

struct usbh_bus;

/**
 * @brief USB Iso Configuration.
 *
 * Structure containing the USB Iso configuration.
 */
struct usbh_iso_frame_packet {
    uint8_t *transfer_buffer;
    uint32_t transfer_buffer_length;
    uint32_t actual_length;
    int errorcode;
};

/**
 * @brief USB Urb Configuration.
 *
 * Structure containing the USB Urb configuration.
 */
struct usbh_urb {
    usb_slist_t list;
    void *hcpriv;
    struct usbh_hubport *hport;
    struct usb_endpoint_descriptor *ep;
    uint8_t data_toggle;
    uint32_t interval;
    struct usb_setup_packet *setup;
    uint8_t *transfer_buffer;
    uint32_t transfer_buffer_length;
    int transfer_flags;
    uint32_t actual_length;
    uint32_t timeout;
    int errorcode;
    uint32_t num_of_iso_packets;
    uint32_t start_frame;
    usbh_complete_callback_t complete;
    void *arg;
#if defined(__ICCARM__) || defined(__ICCRISCV__) || defined(__ICCRX__)
    struct usbh_iso_frame_packet *iso_packet;
#else
    struct usbh_iso_frame_packet iso_packet[0];
#endif
};

/**
 * @brief USB host controller driver.
 *
 * Structure containing the USB host controller driver.
 */
struct usbh_hc_driver {
    const char *driver_name;
    const char *driver_desc;
    int (*init)(struct usbh_bus *bus);
    int (*deinit)(struct usbh_bus *bus);
    uint16_t (*get_frame_number)(struct usbh_bus *bus);
    int (*roothub_control)(struct usbh_bus *bus, struct usb_setup_packet *setup, uint8_t *buf);
    int (*submit_urb)(struct usbh_urb *urb);
    int (*kill_urb)(struct usbh_urb *urb);
    void (*irq_handler)(uint8_t busid);
};

/* transfer_flags: bulk OUT ends with an explicit zero-length packet when
 * the transfer length is an exact multiple of the endpoint max packet
 * size (USBD_FORCE_SHORT_XFER / URB_ZERO_PACKET semantics) */
#define USBH_URB_ZERO_PACKET (1u << 0)

/**
 * @brief USB host controller operations.
 *
 * With CONFIG_USBHOST_MULTI_HCD a controller port exposes one of these
 * tables and binds it per bus through usbh_hcd_register(); core then routes
 * the plain usb_hc_init / usbh_submit_urb / ... symbols per bus, so several
 * different HCD ports can link into one image. Without the macro the table
 * is unused and a port keeps defining those plain symbols directly (the
 * legacy single-HCD contract, unchanged).
 */
struct usbh_hcd_ops {
    const char *driver_name;
    int (*hc_init)(struct usbh_bus *bus);
    int (*hc_deinit)(struct usbh_bus *bus);
    uint16_t (*get_frame_number)(struct usbh_bus *bus);
    int (*roothub_control)(struct usbh_bus *bus, struct usb_setup_packet *setup, uint8_t *buf);
    int (*submit_urb)(struct usbh_urb *urb);
    int (*kill_urb)(struct usbh_urb *urb);
    void (*irq)(uint8_t busid);
};

/**
 * @brief Bind an HCD operations table to a bus (multi-HCD builds only).
 *
 * Must be called after usbh_initialize() and before the hub thread runs
 * usb_hc_init() on that bus.
 *
 * @param busid The bus to bind to.
 * @param ops The controller's operations table.
 */
void usbh_hcd_register(uint8_t busid, const struct usbh_hcd_ops *ops);

/**
 * @brief usb host controller hardware init.
 *
 * @return On success will return 0, and others indicate fail.
 */
int usb_hc_init(struct usbh_bus *bus);

/**
 * @brief usb host controller hardware deinit.
 *
 * @return On success will return 0, and others indicate fail.
 */
int usb_hc_deinit(struct usbh_bus *bus);

/**
 * @brief Get frame number.
 *
 * @return frame number.
 */
uint16_t usbh_get_frame_number(struct usbh_bus *bus);
/**
 * @brief control roothub.
 *
 * @param setup setup request buffer.
 * @param buf buf for reading response or write data.
 * @return On success will return 0, and others indicate fail.
 */
int usbh_roothub_control(struct usbh_bus *bus, struct usb_setup_packet *setup, uint8_t *buf);

/**
 * @brief Submit a usb transfer request to an endpoint.
 *
 * If timeout is not zero, this function will be in poll transfer mode,
 * otherwise will be in async transfer mode.
 *
 * @param urb Usb request block.
 * @return  On success will return 0, and others indicate fail.
 */
int usbh_submit_urb(struct usbh_urb *urb);

/**
 * @brief Cancel a transfer request.
 *
 * This function will call When calls usbh_submit_urb and return -USB_ERR_TIMEOUT or -USB_ERR_SHUTDOWN.
 *
 * @param urb Usb request block.
 * @return  On success will return 0, and others indicate fail.
 */
int usbh_kill_urb(struct usbh_urb *urb);

/* called by user */
void USBH_IRQHandler(uint8_t busid);

#ifdef __cplusplus
}
#endif

#endif /* USB_HC_H */
