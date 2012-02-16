/*
 * Copyright (C) 2006-2008 Google. All Rights Reserved.
 */


#ifndef _FUSE_KLUDGES_H_
#define _FUSE_KLUDGES_H_

#include "fuse.h"
#include "fuse_sysctl.h"

#include <sys/cdefs.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/vnode.h>

#ifdef FUSE4X_ENABLE_DSELECT

/*
 * # 10.5        10.6-32        10.6-64
 *
 * # sizeof(struct selinfo)
 *   24          24             48
 *
 */

struct fuse_selinfo {
#if __LP64__
    unsigned char __data[48];
#else
    unsigned char __data[32];
#endif
};

#define POLLIN          0x0001          /* any readable data available */
#define POLLPRI         0x0002          /* OOB/Urgent readable data */
#define POLLOUT         0x0004          /* file descriptor is writeable */
#define POLLRDNORM      0x0040          /* non-OOB/URG data available */
#define POLLWRNORM      POLLOUT         /* no write type differentiation */
#define POLLRDBAND      0x0080          /* OOB/Urgent readable data */
#define POLLWRBAND      0x0100          /* OOB/Urgent data can be written */

#endif /* FUSE4X_ENABLE_DSELECT */

#ifdef FUSE4X_ENABLE_EXCHANGE

extern void fuse_kludge_exchange(vnode_t v1, vnode_t v2);

#endif /* FUSE4X_ENABLE_EXCHANGE */

#endif /* _FUSE_KLUDGES_H_ */
