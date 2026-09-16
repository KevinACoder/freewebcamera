/*
 * @file   net_stub.c
 * @brief  net_start() for the kernel-replacement build (K4).
 *
 * The stub build links app/ and drivers/ with no kernel, no shell and no
 * components, to prove those layers only reach the interfaces. include/net.h is
 * one of those interfaces, so the build needs something to link against.
 *
 * The return value is 0 - "the network is up" - because the stub is checking
 * that the interface exists, not that anything works: nothing in this build is
 * ever deployed (see the Makefile's k4 target).
 *
 * @author zhugengyu
 * @date   16.09.2026
 */

#include "net.h"

int net_start(void)
{
	return 0;
}