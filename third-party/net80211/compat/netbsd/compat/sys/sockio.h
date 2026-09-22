/*
 * @file
 * @brief Forwarder: the imported ieee80211_ioctl.c includes the ioctl
 * commands as <compat/sys/sockio.h> (the NetBSD tree's own compat
 * layout); the shadow header lives one directory up.
 */

#include <sys/sockio.h>
