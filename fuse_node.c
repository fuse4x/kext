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
#include "compat/tree.h"

#include <stdbool.h>

#ifdef FUSE4X_ENABLE_BIGLOCK
#include "fuse_biglock_vnops.h"
#endif


static int fuse_vnode_compare(struct fuse_vnode_data *d1, struct fuse_vnode_data *d2)
{
    if (d1->nodeid > d2->nodeid) {
        return 1;
    } else if (d1->nodeid == d2->nodeid) {
        return 0;
    } else {
        return -1;
    }
}

RB_GENERATE(fuse_data_nodes, fuse_vnode_data, nodes_link, fuse_vnode_compare);

void
fuse_vnode_data_destroy(struct fuse_vnode_data *fvdat)
{
    lck_mtx_free(fvdat->createlock, fuse_lock_group);
#ifdef FUSE4X_ENABLE_TSLOCKING
    lck_rw_free(fvdat->nodelock, fuse_lock_group);
    lck_rw_free(fvdat->truncatelock, fuse_lock_group);
#endif

    FUSE_OSFree(fvdat, sizeof(*fvdat), fuse_malloc_tag);
}

errno_t
FSNodeGetOrCreateFileVNodeByID(vnode_t               *vnPtr,
                               bool                   is_root,
                               struct fuse_entry_out *feo,
                               mount_t                mp,
                               vnode_t                dvp,
                               vfs_context_t          context,
                               uint32_t              *oflags)
{
    int err = 0;
    vnode_t vn = NULLVP;
    uint32_t vid = 0;

    struct fuse_data       *mntdata = NULL;
    enum vtype vtyp = IFTOVT(feo->attr.mode);

    if ((vtyp >= VBAD) || (vtyp == VNON)) {
        return EINVAL;
    }

    uint64_t size       = is_root ? 0 : feo->attr.size;
    uint64_t generation = feo->generation;

    mntdata = fuse_get_mpdata(mp);

    struct fuse_vnode_data tt = {
        .nodeid = feo->nodeid
    };
    fuse_lck_mtx_lock(mntdata->node_mtx);
    struct fuse_vnode_data *fvdat = RB_FIND(fuse_data_nodes, &mntdata->nodes_head, &tt);
    if (fvdat) {
        vn = fvdat->vp;
        vid = vnode_vid(vn);
    }
    fuse_lck_mtx_unlock(mntdata->node_mtx);

    if (vn) {
        int geterr = vnode_getwithvid(vn, vid);
        if (geterr == ENOENT) {
            // What happened here is a race condition between this function and vnode reclaiming.
            // We do not increase a usage counter when we put nodes to rb-tree.
            // So vnode can be reclaimed by kernel at any time.
            // Let's think what heppens when this function is called at the very same time as reclaim.
            // T(this process), R(reclaim process)
            //   T - find vnode in rb-tree
            //   R - remove vnode from rb-tree
            //   R - free fuse_vnode_data structure
            //   R - reclaim vnode and let someone else use it, let's say process O
            //   O - call vnode_create() and reuse vnode reclaimed above
            //   T - call vnode_get() for vnode we got from rb-tree at the step one but now used by process O. bummer!!!
            // The problem is that vnode that we try to get() is completely different from the one that we
            // had in rb-tree at the beginning of the process.
            // To avoid this race condition we need to store and check vid. vid is some kind of vnode identifier -
            // once a vnode is reclaimend this id is changed. If vnode reclaim happened then vnode_getwithvid()
            // above fails with ENOENT error. In such case we just ignore the vnode and perform a new vnode creation.
            vn = NULLVP;
        }
    }

    if (!vn) {
        fvdat = FUSE_OSMalloc(sizeof(*fvdat), fuse_malloc_tag);
        bzero(fvdat, sizeof(*fvdat));

        struct vnode_fsparam params;

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
        fvdat->flag         = 0;
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

        params.vnfs_mp     = mp;
        params.vnfs_vtype  = vtyp;
        params.vnfs_str    = NULL;
        params.vnfs_dvp    = dvp; /* NULLVP for the root vnode */
        params.vnfs_fsnode = fvdat;
        params.vnfs_vops = fuse_vnode_operations;
        params.vnfs_rdev = 0;
        params.vnfs_marksystem = 0;
        params.vnfs_cnp        = NULL;
        params.vnfs_flags      = VNFS_NOCACHE | VNFS_CANTCACHE;
        params.vnfs_filesize   = size;
        params.vnfs_markroot   = is_root ? 1 : 0;

#ifdef FUSE4X_ENABLE_BIGLOCK
        fuse_biglock_unlock(mntdata->biglock);
#endif
        err = vnode_create(VNCREATE_FLAVOR, (uint32_t)sizeof(params),
                               &params, &vn);
#ifdef FUSE4X_ENABLE_BIGLOCK
        fuse_biglock_lock(mntdata->biglock);
#endif

        if (err == 0) {
            if (is_root) {
                fvdat->parentvp = vn;
            } else {
                fvdat->parentvp = dvp;
            }
            if (oflags) {
                *oflags |= MAKEENTRY;
            }

            fvdat->vp = vn;
            fvdat->vid = vnode_vid(vn);

            fuse_lck_mtx_lock(mntdata->node_mtx);
            RB_INSERT(fuse_data_nodes, &mntdata->nodes_head, fvdat);
            fuse_lck_mtx_unlock(mntdata->node_mtx);

            OSIncrementAtomic((SInt32 *)&fuse_vnodes_current);
        } else {
            log("fuse4x: vnode (ino=%llu) cannot be created, err=%d\n", feo->nodeid, err);
            fuse_vnode_data_destroy(fvdat);
        }
    }

    if (err == 0) {
        if (vnode_vtype(vn) != vtyp) {
            log("fuse4x: vnode changed type behind us (old=%d, new=%d)\n",
                  vnode_vtype(vn), vtyp);
#ifdef FUSE4X_ENABLE_BIGLOCK
            fuse_biglock_unlock(mntdata->biglock);
#endif
            fuse_internal_vnode_disappear(vn, context);
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
            fuse_internal_vnode_disappear(vn, context);
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

    return err;
}

int
fuse_vget_i(vnode_t               *vpp,
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

    err = FSNodeGetOrCreateFileVNodeByID(vpp, false, feo, mp, dvp,
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
