/*
 * Copyright (C) 2006-2008 Google. All Rights Reserved.
 * Copyright (C) 2010 Tuxera. All Rights Reserved.
 * Copyright (C) 2011 Anatol Pomozov. All Rights Reserved.
 */

#ifndef _FUSE_VFSOPS_H_
#define _FUSE_VFSOPS_H_

#include "fuse.h"
#include <fuse_param.h>

#include <sys/kernel_types.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/vnode.h>

struct fuse_data;
struct fuse_ticket;

#if M_FUSE4X_ENABLE_BIGLOCK
extern struct vnodeopv_entry_desc fuse_biglock_vnode_operation_entries[];
#else
extern struct vnodeopv_entry_desc fuse_vnode_operation_entries[];
#endif

/* VFS operations */

static errno_t
fuse_vfsop_mount(mount_t mp, vnode_t devvp, user_addr_t data,
                 vfs_context_t context);

static errno_t
fuse_vfsop_unmount(mount_t mp, int mntflags, vfs_context_t context);

static errno_t
fuse_vfsop_root(mount_t mp, vnode_t *vpp, vfs_context_t context);

static errno_t
fuse_vfsop_getattr(mount_t mp, struct vfs_attr *attr, vfs_context_t context);

static errno_t
fuse_vfsop_sync(mount_t mp, int waitfor, vfs_context_t context);

static errno_t
fuse_vfsop_setattr(mount_t mp, struct vfs_attr *fsap,
                   __unused vfs_context_t context);

/* Other VFS operations */

extern int
fuse_setextendedsecurity(mount_t mp, int state);

#endif /* _FUSE_VFSOPS_H_ */
