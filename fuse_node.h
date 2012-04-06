/*
 * Copyright (C) 2006-2008 Google. All Rights Reserved.
 */

#ifndef _FUSE_NODE_H_
#define _FUSE_NODE_H_

#include "fuse.h"
#include "fuse_file.h"
#include "fuse_ipc.h"
#include "fuse_kernel.h"
#include "compat/tree.h"
#include <fuse_param.h>

#include <stdbool.h>

extern errno_t (**fuse_vnode_operations)(void *);

enum {
    kFSNodeMagic    = 'FUSE',
    kFSNodeBadMagic = 'FU**',
};

#define FN_ACCESS_NOOP       0x00000001
#define FN_CREATING          0x00000002
#define FN_DIRECT_IO         0x00000004
#define FN_HAS_ACL           0x00000008

#define C_NEED_RVNODE_PUT    0x000000001
#define C_NEED_DVNODE_PUT    0x000000002
#define C_ZFWANTSYNC         0x000000004
#define C_FROMSYNC           0x000000008
#define C_MODIFIED           0x000000010
#define C_NOEXISTS           0x000000020
#define C_DELETED            0x000000040
#define C_HARDLINK           0x000000080
#define C_FORCEUPDATE        0x000000100
#define C_HASXATTRS          0x000000200
#define C_NEED_DATA_SETSIZE  0x000001000
#define C_NEED_RSRC_SETSIZE  0x000002000
#define C_CREATING           0x000004000
#define C_ACCESS_NOOP        0x000008000
#define C_TOUCH_ACCTIME      0x000010000
#define C_TOUCH_CHGTIME      0x000020000
#define C_TOUCH_MODTIME      0x000040000
#define C_XTIMES_VALID       0x000080000

struct fuse_vnode_data {

    /** self **/
    vnode_t    vp;
    uint64_t   nodeid;
    uint32_t   vid; // id from vnode_vid()
    uint64_t   generation;
    RB_ENTRY(fuse_vnode_data)  nodes_link;

    /** parent **/
    vnode_t    parentvp;
    uint64_t   parent_nodeid;

    /** I/O **/
    struct     fuse_filehandle fufh[FUFH_MAXTYPE];

    /** flags **/
    uint32_t   flag;
    uint32_t   c_flag;

    /** meta **/
    struct timespec   modify_time;
    struct timespec   entry_valid;
    struct timespec   attr_valid;
    struct vnode_attr cached_attr;
    off_t             filesize;
    uint64_t          nlookup;
    enum vtype        vtype;

    /** locking **/

    lck_mtx_t *createlock;
    void      *creator;

#ifdef FUSE4X_ENABLE_TSLOCKING
    /*
     * The nodelock must be held when data in the FUSE node is accessed or
     * modified. Typically, we would take this lock at the beginning of a
     * vnop and drop it at the end of the vnop.
     */
    lck_rw_t  *nodelock;
    void      *nodelockowner;

    /*
     * The truncatelock guards against the EOF changing on us (that is, a
     * file resize) unexpectedly.
     */
    lck_rw_t  *truncatelock;
#endif
};
typedef struct fuse_vnode_data * fusenode_t;

void fuse_vnode_data_destroy(struct fuse_vnode_data *fvdat);

struct fuse_data_nodes;
RB_PROTOTYPE(fuse_data_nodes, fuse_vnode_data, nodes_link, x);

#define VTOFUD(vp) \
    ((struct fuse_vnode_data *)vnode_fsnode(vp))
#define VTOI(vp)    (VTOFUD(vp)->nodeid)
#define VTOVA(vp)   (&(VTOFUD(vp)->cached_attr))
#define VTOILLU(vp) ((uint64_t)(VTOFUD(vp) ? VTOI(vp) : 0))

#define FUSE_NULL_ID 0

static __inline__
void
fuse_invalidate_attr(vnode_t vp)
{
    if (VTOFUD(vp)) {
        bzero(&VTOFUD(vp)->attr_valid, sizeof(struct timespec));
        VTOFUD(vp)->c_flag &= ~C_XTIMES_VALID;
    }
}

void fuse_vnode_init(vnode_t vp, struct fuse_vnode_data *fvdat,
                     uint64_t nodeid, enum vtype vtyp, uint64_t parentid);
void fuse_vnode_ditch(vnode_t vp, vfs_context_t context);
void fuse_vnode_teardown(vnode_t vp, vfs_context_t context, enum vtype vtyp);

errno_t
FSNodeGetOrCreateFileVNodeByID(vnode_t               *vpp,
                               bool                   is_root,
                               struct fuse_entry_out *feo,
                               mount_t                mp,
                               vnode_t                dvp,
                               vfs_context_t          context,
                               uint32_t              *oflags);

int
fuse_vget_i(vnode_t               *vpp,
            struct fuse_entry_out *feo,
            struct componentname  *cnp,
            vnode_t                dvp,
            mount_t                mp,
            vfs_context_t          context);

/* Name cache wrappers */

static __inline__
void
fuse_vncache_enter(vnode_t dvp, vnode_t vp, struct componentname *cnp)
{
#ifdef FUSE4X_TRACE_VNCACHE
    log("fuse4x: cache enter dvp=%p, vp=%p, %s\n", dvp, vp, cnp->cn_nameptr);
#endif
    return cache_enter(dvp, vp, cnp);
}

static __inline__
void
fuse_vncache_purge(vnode_t vp)
{
#ifdef FUSE4X_TRACE_VNCACHE
    log("fuse4x: cache purge vp=%p\n", vp);
#endif
    return cache_purge(vp);
}

static __inline__
int
fuse_vncache_lookup(vnode_t dvp, vnode_t *vpp, struct componentname *cnp)
{
    int ret = cache_lookup(dvp, vpp, cnp);
#ifdef FUSE4X_TRACE_VNCACHE
    log("fuse4x: cache lookup ret=%d, dvp=%p, *vpp=%p, %s\n",
          ret, dvp, *vpp, cnp->cn_nameptr);
#endif
    return ret;
}

#endif /* _FUSE_NODE_H_ */
