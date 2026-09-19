/*
 * @file   usb_stub.c
 * @brief  K4 null implementation of include/usb.h: the host stack never
 *         runs in the kernel-replacement build, linking is the result.
 *
 * @author zhugengyu
 * @date   19.09.2026
 */

#include "usb.h"

int usb_start(void)
{
	return -1;
}
