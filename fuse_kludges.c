/*
 * Copyright (C) 2006-2008 Google. All Rights Reserved.
 */


#include "fuse_kludges.h"
#include <libkern/version.h>

#ifdef FUSE4X_ENABLE_EXCHANGE

/* The shop of horrors. */

/*
 * # 10.5        10.6-32     10.6-64     10.7-32     10.7-64
 *
 * # sizeof(struct vnode)
 *   144         148         248         152         256
 *
 * # offsetof(struct vnode, v_lflag)
 *   48          48          88          48          88
 *
 * # offsetof(struct vnode, v_name)
 *   112         116         184         116         184
 *
 * # offsetof(struct vnode, v_parent)
 *   116         120         192         120         192
 */

struct fuse_kludge_vnode_9 {
    char     dummy0[48];
    uint16_t v_lflag;
    char     dummy1[62];
    char    *v_name;
    vnode_t  v_parent;
    char     dummy2[24];
} __attribute__ ((packed));

#if __LP64__
struct fuse_kludge_vnode_10 {
    char     dummy0[88];
    uint16_t v_lflag;
    char     dummy1[94];
    char    *v_name;
    vnode_t  v_parent;
    char     dummy2[48];
} __attribute__ ((packed));
#else
struct fuse_kludge_vnode_10 {
    char     dummy0[48];
    uint16_t v_lflag;
    char     dummy1[66];
    char    *v_name;
    vnode_t  v_parent;
    char     dummy2[24];
} __attribute__ ((packed));
#endif

struct fuse_kludge_vnode_11 {
    void    *dummy0[10];
    char     dummy1[8];
    uint16_t v_lflag;
    void    *dummy2[7];
    char     dummy3[38];
    char    *v_name;
    vnode_t  v_parent;
    void    *dummy4[7];
} __attribute__ ((packed));

void
fuse_kludge_exchange(vnode_t v1, vnode_t v2)
{
    if (version_major > 10) {
        char *tmp_v_name = ((struct fuse_kludge_vnode_11 *)v1)->v_name;
        ((struct fuse_kludge_vnode_11 *)v1)->v_name =
            ((struct fuse_kludge_vnode_11 *)v2)->v_name;
        ((struct fuse_kludge_vnode_11 *)v2)->v_name = tmp_v_name;

        vnode_t tmp_v_parent = ((struct fuse_kludge_vnode_11 *)v1)->v_parent;
        ((struct fuse_kludge_vnode_11 *)v1)->v_parent =
            ((struct fuse_kludge_vnode_11 *)v2)->v_parent;
        ((struct fuse_kludge_vnode_11 *)v2)->v_parent = tmp_v_parent;
    }
    else if (version_major > 9) {
        char *tmp_v_name = ((struct fuse_kludge_vnode_10 *)v1)->v_name;
        ((struct fuse_kludge_vnode_10 *)v1)->v_name =
            ((struct fuse_kludge_vnode_10 *)v2)->v_name;
        ((struct fuse_kludge_vnode_10 *)v2)->v_name = tmp_v_name;

        vnode_t tmp_v_parent = ((struct fuse_kludge_vnode_10 *)v1)->v_parent;
        ((struct fuse_kludge_vnode_10 *)v1)->v_parent =
            ((struct fuse_kludge_vnode_10 *)v2)->v_parent;
        ((struct fuse_kludge_vnode_10 *)v2)->v_parent = tmp_v_parent;
    } else {
        char *tmp_v_name = ((struct fuse_kludge_vnode_9 *)v1)->v_name;
        ((struct fuse_kludge_vnode_9 *)v1)->v_name =
            ((struct fuse_kludge_vnode_9 *)v2)->v_name;
        ((struct fuse_kludge_vnode_9 *)v2)->v_name = tmp_v_name;

        vnode_t tmp_v_parent = ((struct fuse_kludge_vnode_9 *)v1)->v_parent;
        ((struct fuse_kludge_vnode_9 *)v1)->v_parent =
            ((struct fuse_kludge_vnode_9 *)v2)->v_parent;
        ((struct fuse_kludge_vnode_9 *)v2)->v_parent = tmp_v_parent;
    }
}

#endif /* FUSE4X_ENABLE_EXCHANGE */
