/*
 * @file   cherrysh_adapter.h
 * @brief  Board-side entry point for the CherrySH adapter.
 *
 * The shell is not a CMSIS component, so it has no standard header to be reached
 * through; this is the one declaration the application needs.
 *
 * MUST be called after the scheduler is running, from a task. Two reasons, both
 * about the console interrupt:
 *
 *  - The adapter's RX path ends in osThreadFlagsSetFromISR, which requires a
 *    running scheduler.
 *  - The adapter arms the console receiver as its last act, after which a byte
 *    can raise that interrupt at any moment.
 *
 * Calling it from board_main() before osKernelStart() would arm the receiver
 * with no scheduler behind it. A task that calls this and then terminates (or
 * simply continues) is the correct pattern.
 *
 * Returns 0 on success, non-zero if the shell could not be created - in which
 * case the console is left as a plain output path and the system still runs.
 */
#ifndef FREEWEBCAMERA_CHERRYSH_ADAPTER_H
#define FREEWEBCAMERA_CHERRYSH_ADAPTER_H

#ifdef __cplusplus
extern "C" {
#endif

int cherrysh_init(void);

#ifdef __cplusplus
}
#endif

#endif /* FREEWEBCAMERA_CHERRYSH_ADAPTER_H */
