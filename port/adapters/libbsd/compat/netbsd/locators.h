/*
 * @file
 * @brief Locator indices: the part of config(8)'s generated locators.h
 * the compiled usb set references. Values follow the NetBSD sys/dev/usb
 * files.usb locator declarations.
 */

#ifndef _LOCATORS_H_
#define _LOCATORS_H_

/* usbdevif */
#define USBDEVIFCF_VENDOR		0
#define USBDEVIFCF_PRODUCT		1
#define USBDEVIFCF_RELEASE		2
#define USBDEVIFCF_CONFIGURATION	3
#define USBDEVIFCF_INTERFACE		4
#define USBDEVIFCF_PORT			5
#define USBDEVIFCF_NLOCS		6

/* usbifif */
#define USBIFIFCF_VENDOR		0
#define USBIFIFCF_PRODUCT		1
#define USBIFIFCF_RELEASE		2
#define USBIFIFCF_CONFIGURATION		3
#define USBIFIFCF_INTERFACE		4
#define USBIFIFCF_PORT			5
#define USBIFIFCF_NLOCS			6

#define USBIFIFCF_VENDOR_DEFAULT	-1
#define USBIFIFCF_PRODUCT_DEFAULT	-1
#define USBIFIFCF_RELEASE_DEFAULT	-1
#define USBIFIFCF_CONFIGURATION_DEFAULT	-1
#define USBIFIFCF_INTERFACE_DEFAULT	-1
#define USBIFIFCF_PORT_DEFAULT		-1

/* ucombus */
#define UCOMBUSCF_PORTNO		0
#define UCOMBUSCF_PORTNO_DEFAULT	-1

/* uhidbus */
#define UHIDBUSCF_REPORTID		0
#define UHIDBUSCF_NLOCS			1

#endif /* _LOCATORS_H_ */
