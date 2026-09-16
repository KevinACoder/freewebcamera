/*
 * @file   fsl_common.h
 * @brief  Shadow of the fsl_common.h the vendored fsl_sdmmc stack includes.
 *
 * The standalone line's copy of this header is entangled with its SDK
 * (fdebug.h, fkernel.h, the FT_DEBUG_PRINT family); this shadow provides the
 * same surface over this project's primitives instead, so the vendored stack
 * itself stays byte-identical (decision D27). The logging macros go to
 * board_log like every other driver-level trace.
 *
 * Not an interface: nothing outside the sdmmc adapter includes this.
 */

#ifndef _FSL_COMMON_H_
#define _FSL_COMMON_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "board.h"

/*
 * The vendored files call assert() as a NULL-guard. There is no
 * __assert_func to back the toolchain's assert.h in this freestanding image,
 * and the conditions it guards (NULL handle derefs) would fault anyway, so
 * it becomes a no-op for the translation units that include this header.
 */
#define assert(x) ((void)0)

typedef int32_t status_t;

#define MAKE_STATUS(group, code) \
	((status_t)((((uint32_t)(group)) << 8U) | ((uint32_t)(code) & 0xFFU)))

#define MAKE_VERSION(major, minor, bugfix) \
	((((uint32_t)(major)) << 16U) | (((uint32_t)(minor)) << 8U) | ((uint32_t)(bugfix)))

/* Size constants the vendored code uses (SDK fsl_common.h vocabulary). */
#define SZ_1K (1024U)
#define SZ_2K (2048U)
#define SZ_4K (4096U)
#define SZ_64K (65536U)
#define SZ_1M (1024U * 1024U)
#define SZ_2M (2U * 1024U * 1024U)
#define SZ_512M (512UL * 1024UL * 1024UL)
#define SZ_1G (1024UL * 1024UL * 1024UL)
#define SZ_2G (2UL * 1024UL * 1024UL)

/* __REV: the GCC built-in the SDK's version maps to (byte reverse). */
#define __REV(v) __builtin_bswap32(v)

enum _sdmmc_status_group
{
	kStatusGroup_SDMMC = 1U,
};

enum _generic_status
{
	kStatus_Success         = 0U,
	kStatus_Fail            = MAKE_STATUS(0U, 1U),
	kStatus_InvalidArgument = MAKE_STATUS(0U, 4U),
	kStatus_OutOfRange      = MAKE_STATUS(0U, 5U),
	kStatus_Timeout         = MAKE_STATUS(0U, 6U),
};

/* Logging: the stack's SDMMC_LOG* family, on board_log. The tag prefix of
 * the original is folded into the line. Error traces always print; info and
 * debug traces only with SDMMC_VERBOSE (boot debugging). */
#ifdef SDMMC_VERBOSE
#define SDMMC_LOGD(tag, format, ...) board_log("sdmmc: " format "\n", ##__VA_ARGS__)
#define SDMMC_LOGI(tag, format, ...) board_log("sdmmc: " format "\n", ##__VA_ARGS__)
#else
#define SDMMC_LOGD(tag, format, ...) ((void)0)
#define SDMMC_LOGI(tag, format, ...) ((void)0)
#endif
#define SDMMC_LOGE(tag, format, ...) board_log("sdmmc: " format "\n", ##__VA_ARGS__)
#define SDMMC_LOG(format, ...) board_log("sdmmc: " format "\n", ##__VA_ARGS__)

#endif /* _FSL_COMMON_H_ */
