/*
 * @file   wlan_stub.c
 * @brief  wlan_start() for the kernel-replacement build (K4).
 *
 * Same story as net_stub.c: include/wlan.h is an interface app/ is allowed
 * to reach, so the no-kernel build needs something to link against. The
 * stub never runs and is never deployed.
 *
 * @author zhugengyu
 * @date   22.09.2026
 */

#include "wlan.h"

int wlan_start(void)
{
	return 0;
}
