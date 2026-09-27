/*
 * @file
 * @brief syslog(9) shell: kernel log emits go to the console sink.
 *
 * The audio(4) middle layer includes this for its log() calls; the port's
 * BSD-world printf already lands on the console gate (wlan_console.c), so
 * log() is the same sink at the same level vocabulary.
 */

#ifndef _COMPAT_SYS_SYSLOG_H_
#define _COMPAT_SYS_SYSLOG_H_

#include <sys/cdefs.h>

/* LOG_* levels (the imported set names them in log() calls) */
#define LOG_EMERG	0
#define LOG_ALERT	1
#define LOG_CRIT	2
#define LOG_ERR		3
#define LOG_WARNING	4
#define LOG_NOTICE	5
#define LOG_INFO	6
#define LOG_DEBUG	7

void log(int, const char *, ...) __printflike(2, 3);

#endif /* _COMPAT_SYS_SYSLOG_H_ */
