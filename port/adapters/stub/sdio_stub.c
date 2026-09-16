/*
 * @file   sdio_stub.c
 * @brief  K4 null implementation of include/sdio.h: the enumeration never
 *         runs in the kernel-replacement build, linking is the result.
 *
 * @author zhugengyu
 * @date   16.09.2026
 */

#include <stdbool.h>

#include "sdio.h"

int sdio_start(void)
{
	return -1;
}

bool sdio_card_up(void)
{
	return false;
}

int sdio_restart(void)
{
	return -1;
}
