/*
 * Copyright (C) 2006-2008 Google. All Rights Reserved.
 * Copyright (C) 2010 Tuxera. All Rights Reserved.
 * Copyright (C) 2011 Anatol Pomozov. All Rights Reserved.
 */

#ifndef _FUSE_LOCKING_H_
#define _FUSE_LOCKING_H_

#include "fuse.h"
#include "fuse_node.h"

#include <IOKit/IOLocks.h>

enum fusefslocktype {
    FUSEFS_SHARED_LOCK    = 1,
    FUSEFS_EXCLUSIVE_LOCK = 2,
    FUSEFS_FORCE_LOCK     = 3
};

#define FUSEFS_SHARED_OWNER (void *)0xffffffff

/* Locking */
extern int fusefs_lock(fusenode_t, enum fusefslocktype);
extern int fusefs_lockpair(fusenode_t, fusenode_t, enum fusefslocktype);
extern int fusefs_lockfour(fusenode_t, fusenode_t, fusenode_t, fusenode_t,
                           enum fusefslocktype);
extern void fusefs_lock_truncate(fusenode_t, lck_rw_type_t);

/* Unlocking */
extern void fusefs_unlock(fusenode_t);
extern void fusefs_unlockpair(fusenode_t, fusenode_t);
extern void fusefs_unlockfour(fusenode_t, fusenode_t, fusenode_t, fusenode_t);
extern void fusefs_unlock_truncate(fusenode_t);

/* Wish the kernel exported lck_rw_done()... */
extern void fusefs_lck_rw_done(lck_rw_t *);

extern lck_attr_t     *fuse_lock_attr;
extern lck_grp_attr_t *fuse_group_attr;
extern lck_grp_t      *fuse_lock_group;
extern lck_mtx_t      *fuse_device_mutex;

#ifdef FUSE_TRACE_LK

#define fuse_lck_mtx_lock(m)                                                                 \
{                                                                                            \
    log("0: lck_mtx_lock(%p): %s@%d by %d\n", (m), __FUNCTION__, __LINE__, proc_selfpid());  \
    lck_mtx_lock((m));                                                                       \
    log("1: lck_mtx_lock(%p): %s@%d by %d\n", (m), __FUNCTION__, __LINE__, proc_selfpid());  \
}

#define fuse_lck_mtx_unlock(m)                                                                 \
{                                                                                              \
    log("0: lck_mtx_unlock(%p): %s@%d by %d\n", (m), __FUNCTION__, __LINE__, proc_selfpid());  \
    lck_mtx_unlock((m));                                                                       \
    log("1: lck_mtx_unlock(%p): %s@%d by %d\n", (m), __FUNCTION__, __LINE__, proc_selfpid());  \
}

#else /* !FUSE_TRACE_LK */

#define fuse_lck_mtx_lock(m)            lck_mtx_lock((m))
#define fuse_lck_mtx_unlock(m)          lck_mtx_unlock((m))

#endif /* FUSE_TRACE_LK */

#define fuse_lck_rw_lock_shared(l)      lck_rw_lock_shared((l))
#define fuse_lck_rw_lock_exclusive(l)   lck_rw_lock_exclusive((l))
#define fuse_lck_rw_unlock_shared(l)    lck_rw_unlock_shared((l))
#define fuse_lck_rw_unlock_exclusive(l) lck_rw_unlock_exclusive((l))
#define fuse_lck_mtx_try_lock(l)        IOLockTryLock((IOLock *)l)

#if M_FUSE4X_ENABLE_BIGLOCK

typedef struct _fusefs_recursive_lock fusefs_recursive_lock;

extern fusefs_recursive_lock* fusefs_recursive_lock_alloc(void);
extern void fusefs_recursive_lock_free(fusefs_recursive_lock* lock);
extern void fusefs_recursive_lock_lock(fusefs_recursive_lock *lock);
extern void fusefs_recursive_lock_unlock(fusefs_recursive_lock *lock);

#endif /* M_FUSE4X_ENABLE_BIGLOCK */

#endif /* _FUSE_LOCKING_H_ */
