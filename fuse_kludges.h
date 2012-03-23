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

#ifdef FUSE4X_ENABLE_EXCHANGE

extern void fuse_kludge_exchange(vnode_t v1, vnode_t v2);

#endif /* FUSE4X_ENABLE_EXCHANGE */

#endif /* _FUSE_KLUDGES_H_ */
