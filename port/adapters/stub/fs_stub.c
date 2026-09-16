/*
 * @file   fs_stub.c
 * @brief  K4 null implementation of include/fs.h.
 *
 * K4 links app/ and drivers/ with no kernel and no shell, so every interface
 * entry point the application calls needs a definition that exists without the
 * adapter. This is the file-system one: without it, adding task_fs_start() to
 * app/main.c would break the kernel-replacement gate even though nothing in
 * drivers/ uses the file system.
 *
 * @author zhugengyu
 * @date   16.09.2026
 */

#include <stdbool.h>
#include <stdint.h>

#include "fs.h"

int fs_start(void)
{
	return 0;
}

uint32_t fs_volume_count(void)
{
	return 0;
}

int fs_mount(uint32_t volume)
{
	(void)volume;

	return -1;
}

int fs_umount(uint32_t volume)
{
	(void)volume;

	return -1;
}

bool fs_is_mounted(uint32_t volume)
{
	(void)volume;

	return false;
}