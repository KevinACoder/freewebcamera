/*
 * @file   video.h
 * @brief  UVC video capture interface - a CMSIS-Driver gap.
 *
 * CMSIS-Driver has no Video/imaging API at all (its 18 headers stop at
 * MCI/ETH/PHY/I2C/SPI/USART/USB*/WiFi/Flash/GPIO/NAND/SAI/CAN/Storage).
 * This fills the UVC gap while keeping the CMSIS ops-struct idiom.
 *
 * Scope at M0: interface FROZEN, no implementation. Lands with M8 (enumerate
 * and control) and M9 (streaming).
 *
 * DESIGN NOTE (important, it shapes this interface): the board does NOT
 * decode video. UVC cameras emit already-compressed MJPEG, so this layer only
 * moves frames - it never interprets their contents beyond locating JPEG
 * boundaries. Decoding happens in the browser. That is why the interface
 * exposes frames as opaque byte ranges rather than as pixels, and why there is
 * no format-conversion or scaling call.
 *
 * CONTRACT:
 *  - Calls are task-context except StreamEvent, which runs in ISR context.
 *  - Frame ownership: GetFrame returns a buffer owned by the driver. The
 *    caller must call ReleaseFrame before the driver can reuse it. If frames
 *    arrive faster than they are released the driver DROPS them rather than
 *    blocking the producer - the console and other tasks must not be starved.
 *  - The frame pool is fixed at Initialize time; the driver never allocates
 *    per frame.
 */

#ifndef FREEWEBCAMERA_VIDEO_H
#define FREEWEBCAMERA_VIDEO_H

#include <stdbool.h>
#include <stdint.h>

#include "Driver_Common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VIDEO_API_VERSION ARM_DRIVER_VERSION_MAJOR_MINOR(0, 1)

/* Pixel formats. UVC negotiates these over the wire; only MJPEG matters for
 * the streaming path, the uncompressed entries exist so a camera's full
 * format table can be reported faithfully. */
typedef enum {
    VIDEO_FORMAT_UNKNOWN = 0,
    VIDEO_FORMAT_MJPEG,
    VIDEO_FORMAT_YUY2,
    VIDEO_FORMAT_NV12,
    VIDEO_FORMAT_H264
} VIDEO_FORMAT;

#define VIDEO_EVENT_FRAME_READY  (1UL << 0)
#define VIDEO_EVENT_FRAME_DROPPED (1UL << 1)
#define VIDEO_EVENT_STREAM_ERROR (1UL << 2)
#define VIDEO_EVENT_DISCONNECTED (1UL << 3)

typedef void (*VIDEO_SignalEvent_t)(uint32_t event, uint32_t dev);

typedef struct _VIDEO_FORMAT_INFO {
    VIDEO_FORMAT format;
    uint16_t     width;
    uint16_t     height;
    uint32_t     fps;            /* max frame rate for this size */
} VIDEO_FORMAT_INFO;

typedef struct _VIDEO_CAPABILITIES {
    uint32_t format_count;       /* entries available via GetFormat */
    uint32_t max_frame_size;     /* largest single frame, in bytes */
    uint32_t frame_pool_count;   /* buffers in the fixed pool */
    uint8_t  supports_streaming; /* false for control-only cameras */
} VIDEO_CAPABILITIES;

typedef struct _VIDEO_FRAME {
    void        *data;           /* frame bytes (complete JPEG for MJPEG) */
    uint32_t     size;           /* valid bytes */
    uint32_t     index;          /* release token */
    uint32_t     sequence;       /* monotonic frame counter */
    uint64_t     timestamp_us;   /* capture time, driver clock */
} VIDEO_FRAME;

typedef struct _ARM_DRIVER_VIDEO {
    ARM_DRIVER_VERSION  (*GetVersion)      (void);
    VIDEO_CAPABILITIES  (*GetCapabilities)(uint32_t dev);
    int32_t             (*Initialize)      (uint32_t dev, VIDEO_SignalEvent_t cb_event);
    int32_t             (*Uninitialize)    (uint32_t dev);
    int32_t             (*PowerControl)    (uint32_t dev, uint32_t state);

    /* Format negotiation: enumerate what the camera offers, then select one.
     * This mirrors the UVC probe/commit exchange. */
    int32_t             (*GetFormatCount)  (uint32_t dev, uint32_t *count);
    int32_t             (*GetFormat)       (uint32_t dev, uint32_t index,
                                            VIDEO_FORMAT_INFO *format);
    int32_t             (*SetFormat)       (uint32_t dev, const VIDEO_FORMAT_INFO *format);

    /* Streaming. Start/Stop are idempotent: Start on a running stream and
     * Stop on a stopped one both return ARM_DRIVER_OK. */
    int32_t             (*StreamStart)     (uint32_t dev);
    int32_t             (*StreamStop)      (uint32_t dev);

    /* Frame retrieval. GetFrame is non-blocking: it returns
     * ARM_DRIVER_ERROR_BUSY when no frame is ready. A frame that is never
     * released is reclaimed only at Uninitialize, so release promptly. */
    int32_t             (*GetFrame)        (uint32_t dev, VIDEO_FRAME *frame);
    int32_t             (*ReleaseFrame)    (uint32_t dev, const VIDEO_FRAME *frame);

    /* Statistics, useful for the acceptance evidence (dropped-frame counts
     * are how the "drop rather than block" policy is verified). */
    int32_t             (*GetStats)        (uint32_t dev, uint32_t *captured,
                                            uint32_t *dropped, uint32_t *errors);
} ARM_DRIVER_VIDEO;

#ifdef __cplusplus
}
#endif

#endif /* FREEWEBCAMERA_VIDEO_H */
