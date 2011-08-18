/*
 * Copyright (C) 2006 Google. All Rights Reserved.
 * Amit Singh <singh@>
 */

#ifndef _FUSE_BIGLOCK_VNOPS_H_
#define _FUSE_BIGLOCK_VNOPS_H_

#include <fuse_param.h>
#include "fuse.h"
#include "fuse_locking.h"

#if M_FUSE4X_ENABLE_INTERIM_FSNODE_LOCK

#if FUSE_TRACE_LK
#define biglock_log(fmt, ...)   log(fmt, ## __VA_ARGS__)
#else
#define biglock_log(fmt, ...)   {}
#endif

#if M_FUSE4X_ENABLE_HUGE_LOCK
#define fuse_hugelock_lock() \
    do { \
        biglock_log("0: fuse_hugelock_lock(%p): %s@%d by %d\n", fuse_huge_lock, __FUNCTION__, __LINE__, proc_selfpid());  \
        fusefs_recursive_lock_lock(fuse_huge_lock); \
        biglock_log("1: fuse_hugelock_lock(%p): %s@%d by %d\n", fuse_huge_lock, __FUNCTION__, __LINE__, proc_selfpid());  \
    } while(0)

#define fuse_hugelock_unlock() \
    do { \
        biglock_log("0: fuse_hugelock_unlock(%p): %s@%d by %d\n", fuse_huge_lock, __FUNCTION__, __LINE__, proc_selfpid());  \
        fusefs_recursive_lock_unlock(fuse_huge_lock); \
        biglock_log("1: fuse_hugelock_unlock(%p): %s@%d by %d\n", fuse_huge_lock, __FUNCTION__, __LINE__, proc_selfpid());  \
    } while(0)
#define fuse_biglock_lock(lock) fuse_hugelock_lock()
#define fuse_biglock_unlock(lock) fuse_hugelock_unlock()
#else
#define fuse_biglock lck_mtx_t
#define fuse_biglock_lock(lock)   fuse_lck_mtx_lock(lock)
#define fuse_biglock_unlock(lock) fuse_lck_mtx_unlock(lock)
#endif

#define fuse_nodelock_lock(node, type) \
    do { \
        int err; \
        biglock_log("0: fusefs_lock(%p,%d): %s@%d by %d\n", node, type, __FUNCTION__, __LINE__, proc_selfpid());  \
        err = fusefs_lock(node, type); \
        biglock_log("1: fusefs_lock(%p,%d): %s@%d by %d\n", node, type, __FUNCTION__, __LINE__, proc_selfpid());  \
        if(err) \
            return err; \
    } while(0)

#define fuse_nodelock_unlock(node) \
    do { \
        biglock_log("0: fusefs_unlock(%p): %s@%d by %d\n", node, __FUNCTION__, __LINE__, proc_selfpid());  \
        fusefs_unlock(node); \
        biglock_log("1: fusefs_unlock(%p): %s@%d by %d\n", node, __FUNCTION__, __LINE__, proc_selfpid());  \
    } while(0)

#define fuse_nodelock_lock_pair(node1, node2, type) \
    do { \
        int err; \
        biglock_log("0: fusefs_lockpair(%p,%p,%d): %s@%d by %d\n", node1, node2, type, __FUNCTION__, __LINE__, proc_selfpid());  \
        err = fusefs_lockpair(node1, node2, type); \
        biglock_log("1: fusefs_lockpair(%p,%p,%d): %s@%d by %d\n", node1, node2, type, __FUNCTION__, __LINE__, proc_selfpid());  \
        if(err) \
            return err; \
    } while(0)

#define fuse_nodelock_unlock_pair(node1, node2) \
    do { \
        biglock_log("0: fusefs_unlockpair(%p,%p): %s@%d by %d\n", node1, node2, __FUNCTION__, __LINE__, proc_selfpid());  \
        fusefs_unlockpair(node1, node2); \
        biglock_log("1: fusefs_unlockpair(%p,%p): %s@%d by %d\n", node1, node2, __FUNCTION__, __LINE__, proc_selfpid());  \
    } while(0)

#define fuse_nodelock_lock_four(node1, node2, node3, node4, type) \
    do { \
        int err; \
        biglock_log("0: fusefs_lockfour(%p,%p,%p,%p,%d): %s@%d by %d\n", node1, node2, node3, node4, type, __FUNCTION__, __LINE__, proc_selfpid());  \
        err = fusefs_lockfour(node1, node2, node3, node4, type); \
        biglock_log("1: fusefs_lockfour(%p,%p,%p,%p,%d): %s@%d by %d\n", node1, node2, node3, node4, type, __FUNCTION__, __LINE__, proc_selfpid());  \
        if(err) \
            return err; \
    } while(0)

#define fuse_nodelock_unlock_four(node1, node2, node3, node4) \
    do { \
        biglock_log("0: fusefs_unlockfour(%p,%p,%p,%p): %s@%d by %d\n", node1, node2, node3, node4, __FUNCTION__, __LINE__, proc_selfpid());  \
        fusefs_unlockfour(node1, node2, node3, node4); \
        biglock_log("1: fusefs_unlockfour(%p,%p,%p,%p): %s@%d by %d\n", node1, node2, node3, node4, __FUNCTION__, __LINE__, proc_selfpid());  \
    } while(0)

/** Wrapper that surrounds a vfsop call with biglock locking. */
#define locked_vfsop(mp, vfsop, args...) \
    do { \
        errno_t res; \
        struct fuse_data *data __unused = fuse_get_mpdata((mp)); \
        fuse_biglock_lock(data->biglock); \
        res = vfsop(mp, ##args); \
        fuse_biglock_unlock(data->biglock); \
        return res; \
    } while(0)

/** Wrapper that surrounds a vnop call with biglock locking. */
#define locked_vnop(vnode, vnop, args) \
    do { \
        int res; \
        vnode_t vp = (vnode); \
        struct fuse_data *data __unused = fuse_get_mpdata(vnode_mount(vp)); \
        fuse_biglock_lock(data->biglock); \
        res = vnop(args); \
        fuse_biglock_unlock(data->biglock); \
        return res; \
    } while(0)

/**
 * Wrapper that surrounds a vnop call with biglock locking and single-node
 * locking.
 */
#define nodelocked_vnop(vnode, vnop, args) \
    do { \
        int res; \
        vnode_t vp = (vnode); \
        struct fuse_data *data __unused = fuse_get_mpdata(vnode_mount(vp)); \
        struct fuse_vnode_data *node = VTOFUD(vp); \
        fuse_nodelock_lock(node, FUSEFS_EXCLUSIVE_LOCK); \
        fuse_biglock_lock(data->biglock); \
        res = vnop(args); \
        fuse_biglock_unlock(data->biglock); \
        fuse_nodelock_unlock(node); \
        return res; \
    } while(0)

/**
 * Wrapper that surrounds a vnop call with biglock locking and dual node
 * locking.
 */
#define nodelocked_pair_vnop(vnode1, vnode2, vnop, args) \
    do { \
        int res; \
        vnode_t vp1 = (vnode1), vp2 = (vnode2); \
        struct fuse_data *data __unused = fuse_get_mpdata(vnode_mount(vp1)); \
        struct fuse_vnode_data *node1 = vp1 ? VTOFUD(vp1) : NULL; \
        struct fuse_vnode_data *node2 = vp2 ? VTOFUD(vp2) : NULL; \
        fuse_nodelock_lock_pair(node1, node2, FUSEFS_EXCLUSIVE_LOCK); \
        fuse_biglock_lock(data->biglock); \
        res = vnop(args); \
        fuse_biglock_unlock(data->biglock); \
        fuse_nodelock_unlock_pair(node1, node2); \
        return res; \
    } while(0)

/**
 * Wrapper that surrounds a vnop call with biglock locking and four-node
 * locking.
 */
#define nodelocked_quad_vnop(vnode1, vnode2, vnode3, vnode4, vnop, args) \
    do { \
        int res; \
        vnode_t vp1 = (vnode1), vp2 = (vnode2), vp3 = (vnode3), vp4 = (vnode4); \
        struct fuse_data *data __unused = fuse_get_mpdata(vnode_mount(vp1)); \
        struct fuse_vnode_data *node1 = vp1 ? VTOFUD(vp1) : NULL; \
        struct fuse_vnode_data *node2 = vp2 ? VTOFUD(vp2) : NULL; \
        struct fuse_vnode_data *node3 = vp3 ? VTOFUD(vp3) : NULL; \
        struct fuse_vnode_data *node4 = vp4 ? VTOFUD(vp4) : NULL; \
        fuse_nodelock_lock_four(node1, node2, node3, node4, FUSEFS_EXCLUSIVE_LOCK); \
        fuse_biglock_lock(data->biglock); \
        res = vnop(args); \
        fuse_biglock_unlock(data->biglock); \
        fuse_nodelock_unlock_four(node1, node2, node3, node4); \
        return res; \
    } while(0)

typedef int (*fuse_biglock_vnode_op_t)(void *);

/*
 * VNOPs
 */

FUSE_VNOP_EXPORT int fuse_biglock_vnop_access(struct vnop_access_args *ap);

// FUSE_VNOP_EXPORT int fuse_biglock_vnop_advlock(struct vnop_advlock_args *ap);

// FUSE_VNOP_EXPORT int fuse_biglock_vnop_allocate(struct vnop_allocate_args *ap);

FUSE_VNOP_EXPORT int fuse_biglock_vnop_blktooff(struct vnop_blktooff_args *ap);

FUSE_VNOP_EXPORT int fuse_biglock_vnop_blockmap(struct vnop_blockmap_args *ap);

// FUSE_VNOP_EXPORT int fuse_biglock_vnop_bwrite(struct vnop_bwrite_args *ap);

FUSE_VNOP_EXPORT int fuse_biglock_vnop_close(struct vnop_close_args *ap);

// FUSE_VNOP_EXPORT int fuse_biglock_vnop_copyfile(struct vnop_copyfile_args *ap);

FUSE_VNOP_EXPORT int fuse_biglock_vnop_create(struct vnop_create_args *ap);

FUSE_VNOP_EXPORT int fuse_biglock_vnop_exchange(struct vnop_exchange_args *ap);

FUSE_VNOP_EXPORT int fuse_biglock_vnop_fsync(struct vnop_fsync_args *ap);

FUSE_VNOP_EXPORT int fuse_biglock_vnop_getattr(struct vnop_getattr_args *ap);

// FUSE_VNOP_EXPORT int fuse_biglock_vnop_getattrlist(struct vnop_getattrlist_args *ap);

// FUSE_VNOP_EXPORT int fuse_biglock_vnop_getnamedstream(struct vnop_getnamedstream_args *ap);

#if M_FUSE4X_ENABLE_XATTR
FUSE_VNOP_EXPORT int fuse_biglock_vnop_getxattr(struct vnop_getxattr_args *ap);
#endif

FUSE_VNOP_EXPORT int fuse_biglock_vnop_inactive(struct vnop_inactive_args *ap);

// FUSE_VNOP_EXPORT int fuse_biglock_vnop_ioctl(struct vnop_ioctl_args *ap);

#if M_FUSE4X_ENABLE_KQUEUE
FUSE_VNOP_EXPORT int fuse_biglock_vnop_kqfilt_add(struct vnop_kqfilt_add_args *ap);

FUSE_VNOP_EXPORT int fuse_biglock_vnop_kqfilt_remove(struct vnop_kqfilt_remove_args *ap);
#endif

FUSE_VNOP_EXPORT int fuse_biglock_vnop_link(struct vnop_link_args *ap);

#if M_FUSE4X_ENABLE_XATTR
FUSE_VNOP_EXPORT int fuse_biglock_vnop_listxattr(struct vnop_listxattr_args *ap);
#endif

FUSE_VNOP_EXPORT int fuse_biglock_vnop_lookup(struct vnop_lookup_args *ap);

// FUSE_VNOP_EXPORT int fuse_biglock_vnop_makenamedstream(struct fuse_makenamedstream_args *ap);

FUSE_VNOP_EXPORT int fuse_biglock_vnop_mkdir(struct vnop_mkdir_args *ap);

FUSE_VNOP_EXPORT int fuse_biglock_vnop_mknod(struct vnop_mknod_args *ap);

FUSE_VNOP_EXPORT int fuse_biglock_vnop_mmap(struct vnop_mmap_args *ap);

FUSE_VNOP_EXPORT int fuse_biglock_vnop_mnomap(struct vnop_mnomap_args *ap);

FUSE_VNOP_EXPORT int fuse_biglock_vnop_offtoblk(struct vnop_offtoblk_args *ap);

FUSE_VNOP_EXPORT int fuse_biglock_vnop_open(struct vnop_open_args *ap);

FUSE_VNOP_EXPORT int fuse_biglock_vnop_pagein(struct vnop_pagein_args *ap);

FUSE_VNOP_EXPORT int fuse_biglock_vnop_pageout(struct vnop_pageout_args *ap);

FUSE_VNOP_EXPORT int fuse_biglock_vnop_pathconf(struct vnop_pathconf_args *ap);

FUSE_VNOP_EXPORT int fuse_biglock_vnop_read(struct vnop_read_args *ap);

FUSE_VNOP_EXPORT int fuse_biglock_vnop_readdir(struct vnop_readdir_args *ap);

// FUSE_VNOP_EXPORT int fuse_biglock_vnop_readdirattr(struct vnop_readdirattr_args *ap);

FUSE_VNOP_EXPORT int fuse_biglock_vnop_readlink(struct vnop_readlink_args *ap);

FUSE_VNOP_EXPORT int fuse_biglock_vnop_reclaim(struct vnop_reclaim_args *ap);

FUSE_VNOP_EXPORT int fuse_biglock_vnop_remove(struct vnop_remove_args *ap);

// FUSE_VNOP_EXPORT int fuse_biglock_vnop_readnamedstream(struct vnop_readnamedstream_args *ap);

#if M_FUSE4X_ENABLE_XATTR
FUSE_VNOP_EXPORT int fuse_biglock_vnop_removexattr(struct vnop_removexattr_args *ap);
#endif

FUSE_VNOP_EXPORT int fuse_biglock_vnop_rename(struct vnop_rename_args *ap);

FUSE_VNOP_EXPORT int fuse_biglock_vnop_revoke(struct vnop_revoke_args *ap);

FUSE_VNOP_EXPORT int fuse_biglock_vnop_rmdir(struct vnop_rmdir_args *ap);

// FUSE_VNOP_EXPORT int fuse_biglock_vnop_searchfs(struct vnop_searchfs_args *ap);

FUSE_VNOP_EXPORT int fuse_biglock_vnop_select(struct vnop_select_args *ap);

FUSE_VNOP_EXPORT int fuse_biglock_vnop_setattr(struct vnop_setattr_args *ap);

// FUSE_VNOP_EXPORT int fuse_biglock_vnop_setlabel(struct vnop_setlabel_args *ap);

// FUSE_VNOP_EXPORT int fuse_biglock_vnop_setattrlist (struct vnop_setattrlist_args *ap);

#if M_FUSE4X_ENABLE_XATTR
FUSE_VNOP_EXPORT int fuse_biglock_vnop_setxattr(struct vnop_setxattr_args *ap);
#endif

FUSE_VNOP_EXPORT int fuse_biglock_vnop_strategy(struct vnop_strategy_args *ap);

FUSE_VNOP_EXPORT int fuse_biglock_vnop_symlink(struct vnop_symlink_args *ap);

// FUSE_VNOP_EXPORT int fuse_biglock_vnop_whiteout(struct vnop_whiteout_args *ap);

FUSE_VNOP_EXPORT int fuse_biglock_vnop_write(struct vnop_write_args *ap);

#endif /* M_FUSE4X_ENABLE_INTERIM_FSNODE_LOCK */

#endif /* _FUSE_BIGLOCK_VNOPS_H_ */
