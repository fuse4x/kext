/*
 * Copyright (C) 2006-2008 Google. All Rights Reserved.
 * Copyright (C) 2010 Tuxera. All Rights Reserved.
 */

#include "fuse.h"
#include "fuse_internal.h"
#include "fuse_ipc.h"
#include "fuse_locking.h"
#include "fuse_node.h"
#include "fuse_sysctl.h"
#include <stdbool.h>

#ifdef FUSE4X_ENABLE_BIGLOCK
#include "fuse_biglock_vnops.h"
#endif

void
FSNodeScrub(struct fuse_vnode_data *fvdat)
{
    lck_mtx_free(fvdat->createlock, fuse_lock_group);
#ifdef FUSE4X_ENABLE_TSLOCKING
    lck_rw_free(fvdat->nodelock, fuse_lock_group);
    lck_rw_free(fvdat->truncatelock, fuse_lock_group);
#endif
}

errno_t
FSNodeGetOrCreateFileVNodeByID(vnode_t               *vnPtr,
                               uint32_t               flags,
                               struct fuse_entry_out *feo,
                               mount_t                mp,
                               vnode_t                dvp,
                               vfs_context_t          context,
                               uint32_t              *oflags)
{
    int   err;
    int   junk;

    vnode_t  vn    = NULLVP;
    HNodeRef hn    = NULL;
    vnode_t  dirVN = NULLVP;

    struct fuse_vnode_data *fvdat   = NULL;
    struct fuse_data       *mntdata = NULL;
    fuse_device_t           dummy_device;

    enum vtype vtyp = IFTOVT(feo->attr.mode);

    if ((vtyp >= VBAD) || (vtyp == VNON)) {
        return EINVAL;
    }

    int      markroot   = (flags & FN_IS_ROOT) ? 1 : 0;
    uint64_t size       = (flags & FN_IS_ROOT) ? 0 : feo->attr.size;
    uint64_t generation = feo->generation;

    mntdata = fuse_get_mpdata(mp);
    dummy_device = mntdata->fdev;

    err = HNodeLookupCreatingIfNecessary(dummy_device, feo->nodeid,
                                         &hn, &vn);
    if ((err == 0) && (vn == NULL)) {

        struct vnode_fsparam params;

        fvdat = (struct fuse_vnode_data *)FSNodeGenericFromHNode(hn);

        if (!fvdat->fInitialised) {

            fvdat->fInitialised = true;

            /* self */
            fvdat->vp           = NULLVP; /* hold on */
            fvdat->nodeid       = feo->nodeid;
            fvdat->generation   = generation;

            /* parent */
            fvdat->parentvp     = dvp;
            if (dvp) {
                fvdat->parent_nodeid = VTOI(dvp);
            } else {
                fvdat->parent_nodeid = 0;
            }

            /* I/O */
            for (int k = 0; k < FUFH_MAXTYPE; k++) {
                FUFH_USE_RESET(&(fvdat->fufh[k]));
            }

            /* flags */
            fvdat->flag         = flags;
            fvdat->c_flag       = 0;

            /* meta */

            /* XXX: truncation */
            fvdat->entry_valid.tv_sec  = (time_t)feo->entry_valid;

            fvdat->entry_valid.tv_nsec = feo->entry_valid_nsec;

            /* XXX: truncation */
            fvdat->attr_valid.tv_sec   = (time_t)feo->attr_valid;

            fvdat->attr_valid.tv_nsec  = feo->attr_valid_nsec;

            /* XXX: truncation */
            fvdat->modify_time.tv_sec  = (time_t)feo->attr.mtime;

            fvdat->modify_time.tv_nsec = feo->attr.mtimensec;

            fvdat->filesize            = size;
            fvdat->nlookup             = 0;
            fvdat->vtype               = vtyp;

            /* locking */
            fvdat->createlock = lck_mtx_alloc_init(fuse_lock_group,
                                                   fuse_lock_attr);
            fvdat->creator = current_thread();
#ifdef FUSE4X_ENABLE_TSLOCKING
            fvdat->nodelock = lck_rw_alloc_init(fuse_lock_group,
                                                fuse_lock_attr);
            fvdat->nodelockowner = NULL;
            fvdat->truncatelock  = lck_rw_alloc_init(fuse_lock_group,
                                                     fuse_lock_attr);
#endif
        }

        if (err == 0) {
            params.vnfs_mp     = mp;
            params.vnfs_vtype  = vtyp;
            params.vnfs_str    = NULL;
            params.vnfs_dvp    = dvp; /* NULLVP for the root vnode */
            params.vnfs_fsnode = hn;
            params.vnfs_vops = fuse_vnode_operations;
            params.vnfs_rdev = 0;
            params.vnfs_marksystem = 0;
            params.vnfs_cnp        = NULL;
            params.vnfs_flags      = VNFS_NOCACHE | VNFS_CANTCACHE;
            params.vnfs_filesize   = size;
            params.vnfs_markroot   = markroot;

#ifdef FUSE4X_ENABLE_BIGLOCK
            fuse_biglock_unlock(mntdata->biglock);
#endif
            err = vnode_create(VNCREATE_FLAVOR, (uint32_t)sizeof(params),
                               &params, &vn);
#ifdef FUSE4X_ENABLE_BIGLOCK
            fuse_biglock_lock(mntdata->biglock);
#endif
        }

        if (err == 0) {
            if (markroot) {
                fvdat->parentvp = vn;
            } else {
                fvdat->parentvp = dvp;
            }
            if (oflags) {
                *oflags |= MAKEENTRY;
            }
            HNodeAttachVNodeSucceeded(hn, vn);
            OSIncrementAtomic((SInt32 *)&fuse_vnodes_current);
        } else {
            if (HNodeAttachVNodeFailed(hn)) {
                FSNodeScrub(fvdat);
                HNodeScrubDone(hn);
            }
        }
    }

    if (err == 0) {
        if (vnode_vtype(vn) != vtyp) {
            log("fuse4x: vnode changed type behind us (old=%d, new=%d)\n",
                  vnode_vtype(vn), vtyp);
#ifdef FUSE4X_ENABLE_BIGLOCK
            fuse_biglock_unlock(mntdata->biglock);
#endif
            fuse_internal_vnode_disappear(vn, context, REVOKE_SOFT);
#ifdef FUSE4X_ENABLE_BIGLOCK
            fuse_biglock_lock(mntdata->biglock);
#endif
            vnode_put(vn);
            err = EIO;
        } else if (VTOFUD(vn)->generation != generation) {
            log("fuse4x: vnode changed generation\n");
#ifdef FUSE4X_ENABLE_BIGLOCK
            fuse_biglock_unlock(mntdata->biglock);
#endif
            fuse_internal_vnode_disappear(vn, context, REVOKE_SOFT);
#ifdef FUSE4X_ENABLE_BIGLOCK
            fuse_biglock_lock(mntdata->biglock);
#endif
            vnode_put(vn);
            err = ESTALE;
        }
    }

    if (err == 0) {
        *vnPtr = vn;
        /* Need VT_FUSE4X from xnu */
        vnode_settag(vn, VT_OTHER);
    }

    if (dirVN != NULL) {
        junk = vnode_put(dirVN);
        /* assert(junk == 0); */
    }

    /* assert((err == 0) == (*vnPtr != NULL); */

    return err;
}

int
fuse_vget_i(vnode_t               *vpp,
            uint32_t               flags,
            struct fuse_entry_out *feo,
            struct componentname  *cnp,
            vnode_t                dvp,
            mount_t                mp,
            vfs_context_t          context)
{
    int err = 0;

    if (!feo) {
        return EINVAL;
    }

    err = FSNodeGetOrCreateFileVNodeByID(vpp, flags, feo, mp, dvp,
                                         context, NULL);
    if (err) {
        return err;
    }

    if (!fuse_isnovncache_mp(mp) && (cnp->cn_flags & MAKEENTRY)) {
        fuse_vncache_enter(dvp, *vpp, cnp);
    }

/* found: */

    VTOFUD(*vpp)->nlookup++;

    return 0;
}
