/*
 * Copyright (C) 2012 Anatol Pomozov. All Rights Reserved.
 */


#ifndef _FUSE_COMPAT_EXCHANGE_H_
#define _FUSE_COMPAT_EXCHANGE_H_

#include "fuse.h"

#include <sys/kernel_types.h>

#ifdef FUSE4X_ENABLE_EXCHANGE

void fuse_compat_exchange(vnode_t v1, vnode_t v2);

#endif /* FUSE4X_ENABLE_EXCHANGE */

#endif /* _FUSE_COMPAT_EXCHANGE_H_ */
