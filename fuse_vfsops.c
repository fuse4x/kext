/*
 * Copyright (C) 2006-2008 Google. All Rights Reserved.
 * Copyright (C) 2010 Tuxera. All Rights Reserved.
 * Copyright (C) 2011 Anatol Pomozov. All Rights Reserved.
 */

#include "fuse.h"
#include "fuse_device.h"
#include "fuse_internal.h"
#include "fuse_ipc.h"
#include "fuse_kludges.h"
#include "fuse_locking.h"
#include "fuse_node.h"
#include "fuse_sysctl.h"
#include "fuse_vfsops.h"

#include <fuse_mount.h>

#ifdef FUSE4X_ENABLE_BIGLOCK
#include <fuse_biglock_vnops.h>
#endif

#define FUSE_MAKEDEV(x, y)     ((dev_t)(((x) << 24) | (y)))
#define FUSEFS_SIGNATURE       0x55464553 // 'FUSE'
#define FUSE_CUSTOM_FSID_VAL1  FUSEFS_SIGNATURE

#include <libkern/version.h>

static const struct timespec kZeroTime = { 0, 0 };

vfstable_t fuse_vfs_table_ref = NULL;

errno_t (**fuse_vnode_operations)(void *);

static struct vnodeopv_desc fuse_vnode_operation_vector_desc = {
    &fuse_vnode_operations,              // opv_desc_vector_p
#ifdef FUSE4X_ENABLE_BIGLOCK
    fuse_biglock_vnode_operation_entries // opv_desc_ops
#else
    fuse_vnode_operation_entries         // opv_desc_ops
#endif /* FUSE4X_ENABLE_BIGLOCK */
};

static struct vnodeopv_desc *fuse_vnode_operation_vector_desc_list[] =
{
    &fuse_vnode_operation_vector_desc,
};

#ifdef FUSE4X_ENABLE_BIGLOCK

static errno_t
fuse_vfsop_biglock_root(mount_t mp, struct vnode **vpp, vfs_context_t context);

static errno_t
fuse_vfsop_biglock_getattr(mount_t mp, struct vfs_attr *attr, vfs_context_t context);

static errno_t
fuse_vfsop_biglock_sync(mount_t mp, int waitfor, vfs_context_t context);

static errno_t
fuse_vfsop_biglock_setattr(mount_t mp, struct vfs_attr *fsap, vfs_context_t context);

#endif

static struct vfsops fuse_vfs_ops = {
#ifdef FUSE4X_ENABLE_BIGLOCK
    fuse_vfsop_mount,           // vfs_mount
    NULL,                       // vfs_start
    fuse_vfsop_unmount,         // vfs_unmount
    fuse_vfsop_biglock_root,    // vfs_root
    NULL,                       // vfs_quotactl
    fuse_vfsop_biglock_getattr, // vfs_getattr
    fuse_vfsop_biglock_sync,    // vfs_sync
    NULL,                       // vfs_vget
    NULL,                       // vfs_fhtovp
    NULL,                       // vfs_vptofh
    NULL,                       // vfs_init
    NULL,                       // vfs_sysctl
    fuse_vfsop_biglock_setattr, // vfs_setattr
#else
    fuse_vfsop_mount,   // vfs_mount
    NULL,               // vfs_start
    fuse_vfsop_unmount, // vfs_unmount
    fuse_vfsop_root,    // vfs_root
    NULL,               // vfs_quotactl
    fuse_vfsop_getattr, // vfs_getattr
    fuse_vfsop_sync,    // vfs_sync
    NULL,               // vfs_vget
    NULL,               // vfs_fhtovp
    NULL,               // vfs_vptofh
    NULL,               // vfs_init
    NULL,               // vfs_sysctl
    fuse_vfsop_setattr, // vfs_setattr
#endif
    { NULL, NULL, NULL, NULL, NULL, NULL, NULL } // vfs_reserved[]
};

struct vfs_fsentry fuse_vfs_entry = {

    // VFS operations
    &fuse_vfs_ops,

    // Number of vnodeopv_desc being registered
    (int)(sizeof(fuse_vnode_operation_vector_desc_list) /\
          sizeof(*fuse_vnode_operation_vector_desc_list)),

    // The vnodeopv_desc's
    fuse_vnode_operation_vector_desc_list,

    // File system type number
    0,

    // File system type name
    FUSE4X_FS_TYPE,

    // Flags specifying file system capabilities
#if defined(FUSE4X_ENABLE_BIGLOCK) || defined(FUSE4X_ENABLE_SIMPLE_LOCK)
    VFS_TBLTHREADSAFE |
#endif
    VFS_TBL64BITREADY | VFS_TBLNOTYPENUM,

    // Reserved for future use
    { NULL, NULL }
};

static errno_t
fuse_vfsop_mount(mount_t mp, __unused vnode_t devvp, user_addr_t udata,
                 vfs_context_t context)
{
    int err      = 0;
    int mntopts  = 0;
    bool mounted = false;

    uint32_t max_read = ~0;

    size_t len;

    fuse_device_t      fdev = NULL;
    struct fuse_data  *data = NULL;
    fuse_mount_args    fusefs_args;
    struct vfsstatfs  *vfsstatfsp = vfs_statfs(mp);

#ifdef FUSE4X_ENABLE_BIGLOCK
    lck_mtx_t         *biglock;
#endif

    fuse_trace_printf_vfsop();

    if (vfs_isupdate(mp)) {
        return ENOTSUP;
    }

    err = copyin(udata, &fusefs_args, sizeof(fusefs_args));
    if (err) {
        return EINVAL;
    }

    /*
     * Interesting flags that we can receive from mount or may want to
     * otherwise forcibly set include:
     *
     *     MNT_ASYNC
     *     MNT_AUTOMOUNTED
     *     MNT_DEFWRITE
     *     MNT_DONTBROWSE
     *     MNT_IGNORE_OWNERSHIP
     *     MNT_JOURNALED
     *     MNT_NODEV
     *     MNT_NOEXEC
     *     MNT_NOSUID
     *     MNT_NOUSERXATTR
     *     MNT_RDONLY
     *     MNT_SYNCHRONOUS
     *     MNT_UNION
     */

    err = ENOTSUP;

    vfs_setlocklocal(mp);

    /** Option Processing. **/

    if (*fusefs_args.fstypename) {
        size_t typenamelen = strlen(fusefs_args.fstypename);
        if (typenamelen > FUSE_FSTYPENAME_MAXLEN) {
            return EINVAL;
        }
        snprintf(vfsstatfsp->f_fstypename, MFSTYPENAMELEN, "%s%s",
                 FUSE_FSTYPENAME_PREFIX, fusefs_args.fstypename);
    }

    if (!*fusefs_args.fsname)
        return EINVAL;

    if ((fusefs_args.daemon_timeout > FUSE_MAX_DAEMON_TIMEOUT) ||
        (fusefs_args.daemon_timeout < FUSE_MIN_DAEMON_TIMEOUT)) {
        return EINVAL;
    }

    if (fusefs_args.altflags & FUSE_MOPT_SPARSE) {
        mntopts |= FSESS_SPARSE;
    }

    if (fusefs_args.altflags & FUSE_MOPT_AUTO_CACHE) {
        mntopts |= FSESS_AUTO_CACHE;
    }

    if (fusefs_args.altflags & FUSE_MOPT_AUTO_XATTR) {
        if (fusefs_args.altflags & FUSE_MOPT_NATIVE_XATTR) {
            return EINVAL;
        }
        mntopts |= FSESS_AUTO_XATTR;
    } else if (fusefs_args.altflags & FUSE_MOPT_NATIVE_XATTR) {
        mntopts |= FSESS_NATIVE_XATTR;
    }

    if (fusefs_args.altflags & FUSE_MOPT_JAIL_SYMLINKS) {
        mntopts |= FSESS_JAIL_SYMLINKS;
    }

    /*
     * Note that unlike Linux, which keeps allow_root in user-space and
     * passes allow_other in that case to the kernel, we let allow_root
     * reach the kernel. The 'if' ordering is important here.
     */
    if (fusefs_args.altflags & FUSE_MOPT_ALLOW_ROOT) {
        int is_member = 0;
        if ((kauth_cred_ismember_gid(kauth_cred_get(), fuse_admin_group, &is_member) != 0) || !is_member) {
            log("fuse4x: caller is not a member of fuse4x admin group. "
                  "Either add user (id=%d) to group (id=%d), "
                  "or set correct '" SYSCTL_FUSE4X_TUNABLES_ADMIN "' sysctl value.\n",
                  kauth_cred_getuid(kauth_cred_get()), fuse_admin_group);
            return EPERM;
        }
        mntopts |= FSESS_ALLOW_ROOT;
    } else if (fusefs_args.altflags & FUSE_MOPT_ALLOW_OTHER) {
        if (!fuse_allow_other && !fuse_vfs_context_issuser(context)) {
            int is_member = 0;
            if ((kauth_cred_ismember_gid(kauth_cred_get(), fuse_admin_group, &is_member) != 0) || !is_member) {
                log("fuse4x: caller is not a member of fuse4x admin group. "
                      "Either add user (id=%d) to group (id=%d), "
                      "or set correct '" SYSCTL_FUSE4X_TUNABLES_ADMIN "' sysctl value.\n",
                      kauth_cred_getuid(kauth_cred_get()), fuse_admin_group);
                return EPERM;
            }
        }
        mntopts |= FSESS_ALLOW_OTHER;
    }

    if (fusefs_args.altflags & FUSE_MOPT_NO_APPLEDOUBLE) {
        mntopts |= FSESS_NO_APPLEDOUBLE;
    }

    if (fusefs_args.altflags & FUSE_MOPT_NO_APPLEXATTR) {
        mntopts |= FSESS_NO_APPLEXATTR;
    }

    if ((fusefs_args.altflags & FUSE_MOPT_FSID) && (fusefs_args.fsid != 0)) {
        fsid_t   fsid;
        mount_t  other_mp;
        uint32_t target_dev;

        target_dev = FUSE_MAKEDEV(FUSE_CUSTOM_FSID_DEVICE_MAJOR,
                                  fusefs_args.fsid);

        fsid.val[0] = target_dev;
        fsid.val[1] = FUSE_CUSTOM_FSID_VAL1;

        other_mp = vfs_getvfs(&fsid);
        if (other_mp != NULL) {
            return EPERM;
        }

        vfsstatfsp->f_fsid.val[0] = target_dev;
        vfsstatfsp->f_fsid.val[1] = FUSE_CUSTOM_FSID_VAL1;

    } else {
        vfs_getnewfsid(mp);
    }

    if (fusefs_args.altflags & FUSE_MOPT_NO_ATTRCACHE) {
        mntopts |= FSESS_NO_ATTRCACHE;
    }

    if (fusefs_args.altflags & FUSE_MOPT_NO_READAHEAD) {
        mntopts |= FSESS_NO_READAHEAD;
    }

    if (fusefs_args.altflags & (FUSE_MOPT_NO_UBC | FUSE_MOPT_DIRECT_IO)) {
        mntopts |= FSESS_NO_UBC;
    }

    if (fusefs_args.altflags & FUSE_MOPT_NO_VNCACHE) {
        mntopts |= FSESS_NO_VNCACHE;
    }

    if (fusefs_args.altflags & FUSE_MOPT_NEGATIVE_VNCACHE) {
        if (mntopts & FSESS_NO_VNCACHE) {
            return EINVAL;
        }
        mntopts |= FSESS_NEGATIVE_VNCACHE;
    }

    if (fusefs_args.altflags & FUSE_MOPT_NO_SYNCWRITES) {

        /* Cannot mix 'nosyncwrites' with 'noubc' or 'noreadahead'. */
        if (mntopts & (FSESS_NO_READAHEAD | FSESS_NO_UBC)) {
            log("fuse4x: cannot mix 'nosyncwrites' with 'noubc' or 'noreadahead'\n");
            return EINVAL;
        }

        mntopts |= FSESS_NO_SYNCWRITES;
        vfs_clearflags(mp, MNT_SYNCHRONOUS);
        vfs_setflags(mp, MNT_ASYNC);

        /* We check for this only if we have nosyncwrites in the first place. */
        if (fusefs_args.altflags & FUSE_MOPT_NO_SYNCONCLOSE) {
            mntopts |= FSESS_NO_SYNCONCLOSE;
        }

    } else {
        vfs_clearflags(mp, MNT_ASYNC);
        vfs_setflags(mp, MNT_SYNCHRONOUS);
    }

    if (mntopts & FSESS_NO_UBC) {
        /* If no buffer cache, disallow exec from file system. */
        vfs_setflags(mp, MNT_NOEXEC);
    }

    vfs_setauthopaque(mp);
    vfs_setauthopaqueaccess(mp);

    if ((fusefs_args.altflags & FUSE_MOPT_DEFAULT_PERMISSIONS) &&
        (fusefs_args.altflags & FUSE_MOPT_DEFER_PERMISSIONS)) {
        return EINVAL;
    }

    if (fusefs_args.altflags & FUSE_MOPT_DEFAULT_PERMISSIONS) {
        mntopts |= FSESS_DEFAULT_PERMISSIONS;
        vfs_clearauthopaque(mp);
    }

    if (fusefs_args.altflags & FUSE_MOPT_DEFER_PERMISSIONS) {
        mntopts |= FSESS_DEFER_PERMISSIONS;
    }

    if (fusefs_args.altflags & FUSE_MOPT_EXTENDED_SECURITY) {
        mntopts |= FSESS_EXTENDED_SECURITY;
        vfs_setextendedsecurity(mp);
    }

    if (fusefs_args.altflags & FUSE_MOPT_LOCALVOL) {
        vfs_setflags(mp, MNT_LOCAL);
    }
    /* done checking incoming option bits */

    err = 0;

    vfs_setfsprivate(mp, NULL);

    fdev = fuse_device_get(fusefs_args.rdev);
    if (!fdev) {
        log("fuse4x: invalid device file (number=%d)\n", fusefs_args.rdev);
        return EINVAL;
    }

    fuse_lck_mtx_lock(fdev->mtx);

    data = fdev->data;

    if (!data) {
        fuse_lck_mtx_unlock(fdev->mtx);
        return ENXIO;
    }

#ifdef FUSE4X_ENABLE_BIGLOCK
    biglock = data->biglock;
    fuse_biglock_lock(biglock);
#endif

    if (data->mounted) {
#ifdef FUSE4X_ENABLE_BIGLOCK
        fuse_biglock_unlock(biglock);
#endif
        fuse_lck_mtx_unlock(fdev->mtx);
        return EALREADY;
    }

    if (!data->opened) {
        fuse_lck_mtx_unlock(fdev->mtx);
        err = ENXIO;
        goto out;
    }

    data->mounted = true;
    OSIncrementAtomic((SInt32 *)&fuse_mount_count);
    mounted = true;

    if (data->dead) {
        fuse_lck_mtx_unlock(fdev->mtx);
        err = ENOTCONN;
        goto out;
    }

    if (!data->daemoncred) {
        panic("fuse4x: daemon found but identity unknown");
    }

    if (fuse_vfs_context_issuser(context) &&
        kauth_cred_getuid(vfs_context_ucred(context)) != kauth_cred_getuid(data->daemoncred)) {
        fuse_lck_mtx_unlock(fdev->mtx);
        err = EPERM;
        log("fuse4x: fuse daemon running by user_id=%d does not have privileges to mount on directory %s owned by user_id=%d\n",
              kauth_cred_getuid(data->daemoncred), vfsstatfsp->f_mntonname, kauth_cred_getuid(vfs_context_ucred(context)));
        goto out;
    }

    data->mp = mp;
    data->fdev = fdev;
    data->dataflags |= mntopts;

    data->daemon_timeout.tv_sec =  fusefs_args.daemon_timeout;
    data->daemon_timeout.tv_nsec = 0;
    if (data->daemon_timeout.tv_sec) {
        data->daemon_timeout_p = &(data->daemon_timeout);
    } else {
        data->daemon_timeout_p = NULL;
    }

    data->max_read = max_read;
    data->fssubtype = fusefs_args.fssubtype;
    data->noimplflags = (uint64_t)0;

    data->blocksize = fuse_round_size(fusefs_args.blocksize,
                                      FUSE_MIN_BLOCKSIZE, FUSE_MAX_BLOCKSIZE);

    data->iosize = fuse_round_size(fusefs_args.iosize,
                                   FUSE_MIN_IOSIZE, FUSE_MAX_IOSIZE);

    if (data->iosize < data->blocksize) {
        data->iosize = data->blocksize;
    }

    data->userkernel_bufsize = FUSE_DEFAULT_USERKERNEL_BUFSIZE;

    copystr(fusefs_args.fsname, vfsstatfsp->f_mntfromname,
            MNAMELEN - 1, &len);
    bzero(vfsstatfsp->f_mntfromname + len, MNAMELEN - len);

    copystr(fusefs_args.volname, data->volname, MAXPATHLEN - 1, &len);
    bzero(data->volname + len, MAXPATHLEN - len);

    /* previous location of vfs_setioattr() */

    vfs_setfsprivate(mp, data);

    fuse_lck_mtx_unlock(fdev->mtx);

    /* Send a handshake message to the daemon. */
    fuse_send_init(data, context);

    struct vfs_attr vfs_attr;
    VFSATTR_INIT(&vfs_attr);
    // Our vfs_getattr() doesn't look at most *_IS_ACTIVE()'s
    err = fuse_vfsop_getattr(mp, &vfs_attr, context);
    if (!err) {
        vfsstatfsp->f_bsize  = vfs_attr.f_bsize;
        vfsstatfsp->f_iosize = vfs_attr.f_iosize;
        vfsstatfsp->f_blocks = vfs_attr.f_blocks;
        vfsstatfsp->f_bfree  = vfs_attr.f_bfree;
        vfsstatfsp->f_bavail = vfs_attr.f_bavail;
        vfsstatfsp->f_bused  = vfs_attr.f_bused;
        vfsstatfsp->f_files  = vfs_attr.f_files;
        vfsstatfsp->f_ffree  = vfs_attr.f_ffree;
        // vfsstatfsp->f_fsid already handled above
        vfsstatfsp->f_owner  = kauth_cred_getuid(data->daemoncred);
        vfsstatfsp->f_flags  = vfs_flags(mp);
        // vfsstatfsp->f_fstypename already handled above
        // vfsstatfsp->f_mntonname handled elsewhere
        // vfsstatfsp->f_mnfromname already handled above
        vfsstatfsp->f_fssubtype = data->fssubtype;
    }
    if (fusefs_args.altflags & FUSE_MOPT_BLOCKSIZE) {
        vfsstatfsp->f_bsize = data->blocksize;
    } else {
        //data->blocksize = vfsstatfsp->f_bsize;
    }
    if (fusefs_args.altflags & FUSE_MOPT_IOSIZE) {
        vfsstatfsp->f_iosize = data->iosize;
    } else {
        //data->iosize = (uint32_t)vfsstatfsp->f_iosize;
        vfsstatfsp->f_iosize = data->iosize;
    }

out:
    if (err) {
        vfs_setfsprivate(mp, NULL);

        fuse_lck_mtx_lock(fdev->mtx);
        data = fdev->data; /* again */
        if (mounted) {
            OSDecrementAtomic((SInt32 *)&fuse_mount_count);
        }
        if (data) {
            data->mounted = false;
            if (!data->opened) {
#ifdef FUSE4X_ENABLE_BIGLOCK
                assert(biglock == data->biglock);
                fuse_biglock_unlock(biglock);
#endif
                fuse_device_close_final(fdev);
                /* data is gone now */
            }
        }
        fuse_lck_mtx_unlock(fdev->mtx);
    } else {
        vnode_t fuse_rootvp = NULLVP;
        err = fuse_vfsop_root(mp, &fuse_rootvp, context);
        if (err) {
            goto out; /* go back and follow error path */
        }
        err = vnode_ref(fuse_rootvp);
        (void)vnode_put(fuse_rootvp);
        if (err) {
            goto out; /* go back and follow error path */
        } else {
            struct vfsioattr ioattr;

            vfs_ioattr(mp, &ioattr);
            ioattr.io_devblocksize = data->blocksize;
            ioattr.io_maxsegreadsize = ioattr.io_maxsegwritesize =
              ioattr.io_maxreadcnt = ioattr.io_maxwritecnt = data->iosize;
            ioattr.io_segreadcnt = ioattr.io_segwritecnt = data->iosize / PAGE_SIZE;
            vfs_setioattr(mp, &ioattr);
        }
    }

#ifdef FUSE4X_ENABLE_BIGLOCK
    fuse_lck_mtx_lock(fdev->mtx);
    data = fdev->data; /* ...and again */
    if(data) {
        assert(data->biglock == biglock);
        fuse_biglock_unlock(biglock);
    }
    fuse_lck_mtx_unlock(fdev->mtx);
#endif

    return err;
}

static errno_t
fuse_vfsop_unmount(mount_t mp, int mntflags, vfs_context_t context)
{
    int err = 0;
    bool force = false;

    fuse_device_t          fdev;
    struct fuse_data      *data;
    struct fuse_dispatcher fdi;

    vnode_t fuse_rootvp = NULLVP;

    fuse_trace_printf_vfsop();

    if (mntflags & MNT_FORCE) {
        force = true;
    }

    data = fuse_get_mpdata(mp);
    if (!data) {
        panic("fuse4x: no mount private data in vfs_unmount");
    }

#ifdef FUSE4X_ENABLE_BIGLOCK
    fuse_biglock_lock(data->biglock);
#endif

    fdev = data->fdev;

    if (data->dead) {

        /*
         * If the file system daemon is dead, it's pointless to try to do
         * any unmount-time operations that go out to user space. Therefore,
         * we pretend that this is a force unmount. However, this isn't of much
         * use. That's because if any non-root vnode is in use, the vflush()
         * that the kernel does before calling our VFS_UNMOUNT will fail
         * if the original unmount wasn't forcible already. That earlier
         * vflush is called with SKIPROOT though, so it wouldn't bail out
         * on the root vnode being in use.
         *
         * If we want, we could set FORCECLOSE here so that a non-forced
         * unmount will be "upgraded" to a forced unmount if the root vnode
         * is busy (you are cd'd to the mount point, for example). It's not
         * quite pure to do that though.
         *
         *    flags |= FORCECLOSE;
         *    log("fuse4x: forcing unmount on a dead file system\n");
         */

    } else if (!data->inited) {
        force = true;
        log("fuse4x: forcing unmount on not-yet-alive file system\n");
        fuse_data_kill(data);
    }

    fuse_rootvp = data->rootvp;

    fuse_trace_printf("%s: Calling vflush(mp, fuse_rootvp, flags=0x%X);\n", __FUNCTION__, force ? FORCECLOSE : 0);
#ifdef FUSE4X_ENABLE_BIGLOCK
    fuse_biglock_unlock(data->biglock);
#endif
    err = vflush(mp, fuse_rootvp, force ? FORCECLOSE : 0);
#ifdef FUSE4X_ENABLE_BIGLOCK
    fuse_biglock_lock(data->biglock);
#endif
    fuse_trace_printf("%s:   Done.\n", __FUNCTION__);
    if (err) {
#ifdef FUSE4X_ENABLE_BIGLOCK
        fuse_biglock_unlock(data->biglock);
#endif
        return err;
    }

    if (vnode_isinuse(fuse_rootvp, 1) && !force) {
#ifdef FUSE4X_ENABLE_BIGLOCK
        fuse_biglock_unlock(data->biglock);
#endif
        return EBUSY;
    }

    if (data->dead) {
        goto alreadydead;
    }

    fuse_dispatcher_init(&fdi, 0 /* no data to send along */);
    fuse_dispatcher_make(&fdi, FUSE_DESTROY, mp, FUSE_ROOT_ID, context);

    fuse_trace_printf("%s: Waiting for reply from FUSE_DESTROY.\n", __FUNCTION__);
    err = fuse_dispatcher_wait_answer(&fdi);
    fuse_trace_printf("%s:   Reply received.\n", __FUNCTION__);
    if (!err) {
        fuse_ticket_drop(fdi.ticket);
    }

    fuse_data_kill(data);

alreadydead:

    fuse_trace_printf("%s: Calling vnode_rele(fuse_rootp);\n", __FUNCTION__);
#ifdef FUSE4X_ENABLE_BIGLOCK
    fuse_biglock_unlock(data->biglock);
#endif
    vnode_rele(fuse_rootvp); /* We got this reference in fuse_vfsop_mount(). */
#ifdef FUSE4X_ENABLE_BIGLOCK
    fuse_biglock_lock(data->biglock);
#endif
    fuse_trace_printf("%s:   Done.\n", __FUNCTION__);

    data->rootvp = NULLVP;

    fuse_trace_printf("%s: Calling vflush(mp, NULLVP, FORCECLOSE);\n", __FUNCTION__);
#ifdef FUSE4X_ENABLE_BIGLOCK
    fuse_biglock_unlock(data->biglock);
#endif
    (void)vflush(mp, NULLVP, FORCECLOSE);
#ifdef FUSE4X_ENABLE_BIGLOCK
    fuse_biglock_lock(data->biglock);
#endif
    fuse_trace_printf("%s:   Done.\n", __FUNCTION__);

    fuse_lck_mtx_lock(fdev->mtx);

    vfs_setfsprivate(mp, NULL);
    data->mounted = false;
    OSDecrementAtomic((SInt32 *)&fuse_mount_count);

#ifdef FUSE4X_ENABLE_BIGLOCK
    fuse_biglock_unlock(data->biglock);
#endif

    if (!data->opened) {

        /* fdev->data was left for us to clean up */

        fuse_device_close_final(fdev);

        /* fdev->data is gone now */
    }

    fuse_lck_mtx_unlock(fdev->mtx);

    return 0;
}

static errno_t
fuse_vfsop_root(mount_t mp, struct vnode **vpp, vfs_context_t context)
{
    int err = 0;
    vnode_t vp = NULLVP;
    struct fuse_entry_out feo_root;
    struct fuse_data *data = fuse_get_mpdata(mp);

    fuse_trace_printf_vfsop();

    if (data->rootvp != NULLVP) {
        *vpp = data->rootvp;
        return vnode_get(*vpp);
    }

    bzero(&feo_root, sizeof(feo_root));
    feo_root.nodeid      = FUSE_ROOT_ID;
    feo_root.generation  = 0;
    feo_root.attr.ino    = FUSE_ROOT_ID;
    feo_root.attr.size   = FUSE_ROOT_SIZE;
    feo_root.attr.mode   = VTTOIF(VDIR);

    err = FSNodeGetOrCreateFileVNodeByID(&vp, FN_IS_ROOT, &feo_root, mp,
                                         NULLVP /* dvp */, context,
                                         NULL /* oflags */);
    *vpp = vp;

    if (!err) {
        data->rootvp = *vpp;
    }

    return err;
}

static void
handle_capabilities_and_attributes(mount_t mp, struct vfs_attr *attr)
{

    struct fuse_data *data = fuse_get_mpdata(mp);
    if (!data) {
        panic("fuse4x: no private data for mount point?");
    }

    attr->f_capabilities.capabilities[VOL_CAPABILITIES_FORMAT] = 0
//      | VOL_CAP_FMT_PERSISTENTOBJECTIDS
        | VOL_CAP_FMT_SYMBOLICLINKS

        /*
         * Note that we don't really have hard links in a fuse4x file system
         * unless the user file system daemon provides persistent/consistent
         * inode numbers. Maybe instead of returning the "wrong" answer here
         * we should just deny knowledge of this capability in the valid bits
         * below.
         */
        | VOL_CAP_FMT_HARDLINKS
//      | VOL_CAP_FMT_JOURNAL
//      | VOL_CAP_FMT_JOURNAL_ACTIVE
        | VOL_CAP_FMT_NO_ROOT_TIMES
        | VOL_CAP_FMT_SPARSE_FILES
//      | VOL_CAP_FMT_ZERO_RUNS
        | VOL_CAP_FMT_CASE_SENSITIVE
        | VOL_CAP_FMT_CASE_PRESERVING
        | VOL_CAP_FMT_FAST_STATFS
        | VOL_CAP_FMT_2TB_FILESIZE
//      | VOL_CAP_FMT_OPENDENYMODES
        | VOL_CAP_FMT_HIDDEN_FILES
//      | VOL_CAP_FMT_PATH_FROM_ID
#if VERSION_MAJOR >= 10
//      | VOL_CAP_FMT_NO_VOLUME_SIZES
//      | VOL_CAP_FMT_DECMPFS_COMPRESSION
#endif
#if VERSION_MAJOR >= 11
//      | VOL_CAP_FMT_64BIT_OBJECT_IDS
#endif
        ;

    attr->f_capabilities.valid[VOL_CAPABILITIES_FORMAT] = 0
        | VOL_CAP_FMT_PERSISTENTOBJECTIDS
        | VOL_CAP_FMT_SYMBOLICLINKS
        | VOL_CAP_FMT_HARDLINKS
        | VOL_CAP_FMT_JOURNAL
        | VOL_CAP_FMT_JOURNAL_ACTIVE
        | VOL_CAP_FMT_NO_ROOT_TIMES
        | VOL_CAP_FMT_SPARSE_FILES
        | VOL_CAP_FMT_ZERO_RUNS
        | VOL_CAP_FMT_CASE_SENSITIVE
        | VOL_CAP_FMT_CASE_PRESERVING
        | VOL_CAP_FMT_FAST_STATFS
        | VOL_CAP_FMT_2TB_FILESIZE
        | VOL_CAP_FMT_OPENDENYMODES
        | VOL_CAP_FMT_HIDDEN_FILES
        | VOL_CAP_FMT_PATH_FROM_ID
#if VERSION_MAJOR >= 10
        | VOL_CAP_FMT_NO_VOLUME_SIZES
        | VOL_CAP_FMT_DECMPFS_COMPRESSION
#endif
#if VERSION_MAJOR >= 11
        | VOL_CAP_FMT_64BIT_OBJECT_IDS
#endif
        ;

    attr->f_capabilities.capabilities[VOL_CAPABILITIES_INTERFACES] = 0
//      | VOL_CAP_INT_SEARCHFS
        | VOL_CAP_INT_ATTRLIST
//      | VOL_CAP_INT_NFSEXPORT
//      | VOL_CAP_INT_READDIRATTR
//      | VOL_CAP_INT_EXCHANGEDATA
//      | VOL_CAP_INT_COPYFILE
//      | VOL_CAP_INT_ALLOCATE
//      | VOL_CAP_INT_VOL_RENAME
        | VOL_CAP_INT_ADVLOCK
        | VOL_CAP_INT_FLOCK
        | VOL_CAP_INT_EXTENDED_SECURITY
//      | VOL_CAP_INT_USERACCESS
//      | VOL_CAP_INT_MANLOCK
//      | VOL_CAP_INT_NAMEDSTREAMS
//      | VOL_CAP_INT_EXTENDED_ATTR
        ;

    if (data->dataflags & FSESS_NATIVE_XATTR) {
        attr->f_capabilities.capabilities[VOL_CAPABILITIES_INTERFACES] |=
            VOL_CAP_INT_EXTENDED_ATTR;
    }

    /* Don't set the EXCHANGEDATA capability if it's known not to be
     * implemented in the FUSE daemon. */
    if (fuse_implemented(data, FSESS_NOIMPLBIT(EXCHANGE))) {
        attr->f_capabilities.capabilities[VOL_CAPABILITIES_INTERFACES] |=
            VOL_CAP_INT_EXCHANGEDATA;
    }

    attr->f_capabilities.valid[VOL_CAPABILITIES_INTERFACES] = 0
        | VOL_CAP_INT_SEARCHFS
        | VOL_CAP_INT_ATTRLIST
        | VOL_CAP_INT_NFSEXPORT
        | VOL_CAP_INT_READDIRATTR
        | VOL_CAP_INT_EXCHANGEDATA
        | VOL_CAP_INT_COPYFILE
        | VOL_CAP_INT_ALLOCATE
        | VOL_CAP_INT_VOL_RENAME
        | VOL_CAP_INT_ADVLOCK
        | VOL_CAP_INT_FLOCK
        | VOL_CAP_INT_EXTENDED_SECURITY
        | VOL_CAP_INT_USERACCESS
        | VOL_CAP_INT_MANLOCK
        | VOL_CAP_INT_NAMEDSTREAMS
        | VOL_CAP_INT_EXTENDED_ATTR
        ;

    attr->f_capabilities.capabilities[VOL_CAPABILITIES_RESERVED1] = 0;
    attr->f_capabilities.valid[VOL_CAPABILITIES_RESERVED1] = 0;
    attr->f_capabilities.capabilities[VOL_CAPABILITIES_RESERVED2] = 0;
    attr->f_capabilities.valid[VOL_CAPABILITIES_RESERVED2] = 0;
    VFSATTR_SET_SUPPORTED(attr, f_capabilities);

    attr->f_attributes.validattr.commonattr = 0
        | ATTR_CMN_NAME
        | ATTR_CMN_DEVID
        | ATTR_CMN_FSID
        | ATTR_CMN_OBJTYPE
//      | ATTR_CMN_OBJTAG
        | ATTR_CMN_OBJID
//      | ATTR_CMN_OBJPERMANENTID
        | ATTR_CMN_PAROBJID
//      | ATTR_CMN_SCRIPT
//      | ATTR_CMN_CRTIME
        | ATTR_CMN_MODTIME
//      | ATTR_CMN_CHGTIME
//      | ATTR_CMN_ACCTIME
//      | ATTR_CMN_BKUPTIME
//      | ATTR_CMN_FNDRINFO
        | ATTR_CMN_OWNERID
        | ATTR_CMN_GRPID
        | ATTR_CMN_ACCESSMASK
        | ATTR_CMN_FLAGS
//      | ATTR_CMN_USERACCESS
        | ATTR_CMN_EXTENDED_SECURITY
//      | ATTR_CMN_UUID
//      | ATTR_CMN_GRPUUID
//      | ATTR_CMN_FILEID
//      | ATTR_CMN_PARENTID
#if VERSION_MAJOR >= 10
//      | ATTR_CMN_FULLPATH
#endif
#if VERSION_MAJOR >= 11
//      | ATTR_CMN_ADDEDTIME
#endif
        ;

    attr->f_attributes.validattr.volattr = 0
        | ATTR_VOL_FSTYPE
        | ATTR_VOL_SIGNATURE
        | ATTR_VOL_SIZE
        | ATTR_VOL_SPACEFREE
        | ATTR_VOL_SPACEAVAIL
//      | ATTR_VOL_MINALLOCATION
//      | ATTR_VOL_ALLOCATIONCLUMP
        | ATTR_VOL_IOBLOCKSIZE
//      | ATTR_VOL_OBJCOUNT
        | ATTR_VOL_FILECOUNT
//      | ATTR_VOL_DIRCOUNT
//      | ATTR_VOL_MAXOBJCOUNT
        | ATTR_VOL_MOUNTPOINT
        | ATTR_VOL_NAME
        | ATTR_VOL_MOUNTFLAGS
        | ATTR_VOL_MOUNTEDDEVICE
//      | ATTR_VOL_ENCODINGSUSED
        | ATTR_VOL_CAPABILITIES
        | ATTR_VOL_ATTRIBUTES
//      | ATTR_VOL_INFO
        ;
    attr->f_attributes.validattr.dirattr = 0
        | ATTR_DIR_LINKCOUNT
//      | ATTR_DIR_ENTRYCOUNT
//      | ATTR_DIR_MOUNTSTATUS
        ;
    attr->f_attributes.validattr.fileattr = 0
        | ATTR_FILE_LINKCOUNT
        | ATTR_FILE_TOTALSIZE
        | ATTR_FILE_ALLOCSIZE
        | ATTR_FILE_IOBLOCKSIZE
        | ATTR_FILE_DEVTYPE
//      | ATTR_FILE_FORKCOUNT
//      | ATTR_FILE_FORKLIST
        | ATTR_FILE_DATALENGTH
        | ATTR_FILE_DATAALLOCSIZE
//      | ATTR_FILE_RSRCLENGTH
//      | ATTR_FILE_RSRCALLOCSIZE
        ;

    attr->f_attributes.validattr.forkattr = 0;
//      | ATTR_FORK_TOTALSIZE
//      | ATTR_FORK_ALLOCSIZE
        ;

    // Handle some special cases

    if (!(data->dataflags & FSESS_CASE_INSENSITIVE)) {
        attr->f_capabilities.capabilities[VOL_CAPABILITIES_FORMAT] |=
            VOL_CAP_FMT_CASE_SENSITIVE;
    }

    /*
    if (data->dataflags & FSESS_VOL_RENAME) {
        attr->f_capabilities.capabilities[VOL_CAPABILITIES_INTERFACES] |=
            VOL_CAP_INT_VOL_RENAME;
    } else {
        fuse_clear_implemented(data, FSESS_NOIMPLBIT(SETVOLNAME));
    }
    */
    /* Not yet. */
    fuse_clear_implemented(data, FSESS_NOIMPLBIT(SETVOLNAME));

    if (data->dataflags & FSESS_XTIMES) {
        attr->f_attributes.validattr.commonattr |=
            (ATTR_CMN_BKUPTIME | ATTR_CMN_CHGTIME | ATTR_CMN_CRTIME);
    } else {
        fuse_clear_implemented(data, FSESS_NOIMPLBIT(GETXTIMES));
    }

    // All attributes that we do support, we support natively.

    attr->f_attributes.nativeattr.commonattr = \
        attr->f_attributes.validattr.commonattr;
    attr->f_attributes.nativeattr.volattr    = \
        attr->f_attributes.validattr.volattr;
    attr->f_attributes.nativeattr.dirattr    = \
        attr->f_attributes.validattr.dirattr;
    attr->f_attributes.nativeattr.fileattr   = \
        attr->f_attributes.validattr.fileattr;
    attr->f_attributes.nativeattr.forkattr   = \
        attr->f_attributes.validattr.forkattr;

    VFSATTR_SET_SUPPORTED(attr, f_attributes);
}

static errno_t
fuse_vfsop_getattr(mount_t mp, struct vfs_attr *attr, vfs_context_t context)
{
    int err     = 0;
    bool deading = false, faking = false;

    struct fuse_dispatcher  fdi;
    struct fuse_statfs_out *fsfo;
    struct fuse_statfs_out  faked;
    struct fuse_data       *data;

    fuse_trace_printf_vfsop();

    data = fuse_get_mpdata(mp);
    if (!data) {
        panic("fuse4x: no private data for mount point?");
    }

    if (!data->inited) {
        // coreservices process requests ATTR_VOL_CAPABILITIES on the mountpoint right before
        // returning from mount() syscall. We need to fake the output because daemon might
        // not be ready to response yet (and deadlock will happen).
        faking = true;
        goto dostatfs;
    }

    fuse_dispatcher_init(&fdi, 0);
    fuse_dispatcher_make(&fdi, FUSE_STATFS, mp, FUSE_ROOT_ID, context);
    if ((err = fuse_dispatcher_wait_answer(&fdi))) {
        // If we cannot communicate with the daemon (most likely because
        // it's dead), we still want to portray that we are a bonafide
        // file system so that we can be gracefully unmounted.

        if (err == ENOTCONN) {
            deading = faking = true;
            goto dostatfs;
        }

        return err;
    }

dostatfs:
    if (faking) {
        bzero(&faked, sizeof(faked));
        fsfo = &faked;
    } else {
        fsfo = fdi.answer;
    }

    if (fsfo->st.bsize == 0) {
        fsfo->st.bsize = FUSE_DEFAULT_IOSIZE;
    }

    if (fsfo->st.frsize == 0) {
        fsfo->st.frsize = FUSE_DEFAULT_BLOCKSIZE;
    }

    /* optimal transfer block size; will go into f_iosize in the kernel */
    fsfo->st.bsize = fuse_round_size(fsfo->st.bsize,
                                     FUSE_MIN_IOSIZE, FUSE_MAX_IOSIZE);

    /* file system fragment size; will go into f_bsize in the kernel */
    fsfo->st.frsize  = fuse_round_size(fsfo->st.frsize,
                                       FUSE_MIN_BLOCKSIZE, FUSE_MAX_BLOCKSIZE);

    /* We must have: f_iosize >= f_bsize (fsfo->st.bsize >= fsfo->st_frsize) */
    if (fsfo->st.bsize < fsfo->st.frsize) {
        fsfo->st.bsize = fsfo->st.frsize;
    }

    /*
     * TBD: Possibility:
     *
     * For actual I/O to fuse4x's "virtual" storage device, we use
     * data->blocksize and data->iosize. These are really meant to be
     * constant across the lifetime of a single mount. If necessary, we
     * can experiment by updating the mount point's stat with the frsize
     * and bsize values we come across here.
     */

    /*
     * FUSE user daemon will (might) give us this:
     *
     * __u64   blocks;  // total data blocks in the file system
     * __u64   bfree;   // free blocks in the file system
     * __u64   bavail;  // free blocks available to non-superuser
     * __u64   files;   // total file nodes in the file system
     * __u64   ffree;   // free file nodes in the file system
     * __u32   bsize;   // preferred/optimal file system block size
     * __u32   namelen; // maximum length of filenames
     * __u32   frsize;  // fundamental file system block size
     *
     * On Mac OS X, we will map this data to struct vfs_attr as follows:
     *
     *  Mac OS X                     FUSE
     *  --------                     ----
     *  uint64_t f_supported   <-    // handled here
     *  uint64_t f_active      <-    // handled here
     *  uint64_t f_objcount    <-    -
     *  uint64_t f_filecount   <-    files
     *  uint64_t f_dircount    <-    -
     *  uint32_t f_bsize       <-    frsize
     *  size_t   f_iosize      <-    bsize
     *  uint64_t f_blocks      <-    blocks
     *  uint64_t f_bfree       <-    bfree
     *  uint64_t f_bavail      <-    bavail
     *  uint64_t f_bused       <-    blocks - bfree
     *  uint64_t f_files       <-    files
     *  uint64_t f_ffree       <-    ffree
     *  fsid_t   f_fsid        <-    // handled elsewhere
     *  uid_t    f_owner       <-    // handled elsewhere
     *  ... capabilities       <-    // handled here
     *  ... attributes         <-    // handled here
     *  f_create_time          <-    -
     *  f_modify_time          <-    -
     *  f_access_time          <-    -
     *  f_backup_time          <-    -
     *  uint32_t f_fssubtype   <-    // daemon provides
     *  char *f_vol_name       <-    // handled here
     *  uint16_t f_signature   <-    // handled here
     *  uint16_t f_carbon_fsid <-    // handled here
     */

    VFSATTR_RETURN(attr, f_filecount, fsfo->st.files);
    VFSATTR_RETURN(attr, f_bsize, fsfo->st.frsize);
    VFSATTR_RETURN(attr, f_iosize, fsfo->st.bsize);
    VFSATTR_RETURN(attr, f_blocks, fsfo->st.blocks);
    VFSATTR_RETURN(attr, f_bfree, fsfo->st.bfree);
    VFSATTR_RETURN(attr, f_bavail, fsfo->st.bavail);
    VFSATTR_RETURN(attr, f_bused, (fsfo->st.blocks - fsfo->st.bfree));
    VFSATTR_RETURN(attr, f_files, fsfo->st.files);
    VFSATTR_RETURN(attr, f_ffree, fsfo->st.ffree);

    /* f_fsid and f_owner handled elsewhere. */

    /* Handle capabilities and attributes. */
    handle_capabilities_and_attributes(mp, attr);

    VFSATTR_RETURN(attr, f_create_time, kZeroTime);
    VFSATTR_RETURN(attr, f_modify_time, kZeroTime);
    VFSATTR_RETURN(attr, f_access_time, kZeroTime);
    VFSATTR_RETURN(attr, f_backup_time, kZeroTime);

    VFSATTR_RETURN(attr, f_fssubtype, data->fssubtype);

    /* Daemon needs to pass this. */
    if (VFSATTR_IS_ACTIVE(attr, f_vol_name)) {
        if (data->volname[0] != 0) {
            strncpy(attr->f_vol_name, data->volname, MAXPATHLEN);
            attr->f_vol_name[MAXPATHLEN - 1] = 0;
            VFSATTR_SET_SUPPORTED(attr, f_vol_name);
        }
    }

    VFSATTR_RETURN(attr, f_signature, OSSwapBigToHostInt16(FUSEFS_SIGNATURE));
    VFSATTR_RETURN(attr, f_carbon_fsid, 0);

    if (!faking)
        fuse_ticket_drop(fdi.ticket);

    return 0;
}

struct fuse_sync_cargs {
    vfs_context_t context;
    int waitfor;
    int error;
};

static int
fuse_sync_callback(vnode_t vp, void *cargs)
{
    int type;
    struct fuse_sync_cargs *args;
    struct fuse_vnode_data *fvdat;
    struct fuse_dispatcher  fdi;
    struct fuse_filehandle *fufh;
    struct fuse_data       *data;
    mount_t mp;

    if (!vnode_hasdirtyblks(vp)) {
        return VNODE_RETURNED;
    }

    mp = vnode_mount(vp);

    if (fuse_isdeadfs_mp(mp)) {
        return VNODE_RETURNED_DONE;
    }

    data = fuse_get_mpdata(mp);

    if (!fuse_implemented(data, (vnode_isdir(vp)) ?
        FSESS_NOIMPLBIT(FSYNCDIR) : FSESS_NOIMPLBIT(FSYNC))) {
        return VNODE_RETURNED;
    }

    args = (struct fuse_sync_cargs *)cargs;
    fvdat = VTOFUD(vp);

    cluster_push(vp, 0);

    fuse_dispatcher_init(&fdi, 0);
    for (type = 0; type < FUFH_MAXTYPE; type++) {
        fufh = &(fvdat->fufh[type]);
        if (FUFH_IS_VALID(fufh)) {
            (void)fuse_internal_fsync(vp, args->context, fufh, &fdi);
        }
    }

    /*
     * In general:
     *
     * - can use vnode_isinuse() if the need be
     * - vnode and UBC are in lock-step
     * - note that umount will call ubc_sync_range()
     */

    return VNODE_RETURNED;
}

static errno_t
fuse_vfsop_sync(mount_t mp, int waitfor, vfs_context_t context)
{
    uint64_t mntflags;
    struct fuse_sync_cargs args;
    int allerror = 0;

    fuse_trace_printf_vfsop();

    mntflags = vfs_flags(mp);

    if (fuse_isdeadfs_mp(mp)) {
        return 0;
    }

    if (vfs_isupdate(mp)) {
        return 0;
    }

    if (vfs_isrdonly(mp)) {
        return EROFS; // should panic!?
    }

    /*
     * Write back each (modified) fuse node.
     */
    args.context = context;
    args.waitfor = waitfor;
    args.error = 0;

#ifdef FUSE4X_ENABLE_BIGLOCK
    struct fuse_data *data = fuse_get_mpdata(mp);
    fuse_biglock_unlock(data->biglock);
#endif
    vnode_iterate(mp, 0, fuse_sync_callback, (void *)&args);
#ifdef FUSE4X_ENABLE_BIGLOCK
    fuse_biglock_lock(data->biglock);
#endif

    if (args.error) {
        allerror = args.error;
    }

    /*
     * For other types of stale file system information, such as:
     *
     * - fs control info
     * - quota information
     * - modified superblock
     */

    return allerror;
}

static errno_t
fuse_vfsop_setattr(mount_t mp, struct vfs_attr *fsap, vfs_context_t context)
{
    int error = 0;

    fuse_trace_printf_vfsop();

    kauth_cred_t cred = vfs_context_ucred(context);

    if (!fuse_vfs_context_issuser(context) &&
        (kauth_cred_getuid(cred) != vfs_statfs(mp)->f_owner)) {
        return EACCES;
    }

    struct fuse_data *data = fuse_get_mpdata(mp);

    if (VFSATTR_IS_ACTIVE(fsap, f_vol_name)) {

        if (!fuse_implemented(data, FSESS_NOIMPLBIT(SETVOLNAME))) {
            error = ENOTSUP;
            goto out;
        }

        if (fsap->f_vol_name[0] == 0) {
            error = EINVAL;
            goto out;
        }

        size_t namelen = strlen(fsap->f_vol_name);
        if (namelen >= MAXPATHLEN) {
            error = ENAMETOOLONG;
            goto out;
        }

        vnode_t root_vp;

        error = fuse_vfsop_root(mp, &root_vp, context);
        if (error) {
            goto out;
        }

        struct fuse_dispatcher fdi;
        fuse_dispatcher_init(&fdi, namelen + 1);
        fuse_dispatcher_make_vp(&fdi, FUSE_SETVOLNAME, root_vp, context);
        memcpy((char *)fdi.indata, fsap->f_vol_name, namelen);
        ((char *)fdi.indata)[namelen] = '\0';

        if (!(error = fuse_dispatcher_wait_answer(&fdi))) {
            fuse_ticket_drop(fdi.ticket);
        }

        (void)vnode_put(root_vp);

        if (error) {
            if (error == ENOSYS) {
                error = ENOTSUP;
                fuse_clear_implemented(data, FSESS_NOIMPLBIT(SETVOLNAME));
            }
            goto out;
        }

        copystr(fsap->f_vol_name, data->volname, MAXPATHLEN - 1, &namelen);
        bzero(data->volname + namelen, MAXPATHLEN - namelen);

        VFSATTR_SET_SUPPORTED(fsap, f_vol_name);
    }

out:
    return error;
}
#ifdef FUSE4X_ENABLE_BIGLOCK

static errno_t
fuse_vfsop_biglock_root(mount_t mp, struct vnode **vpp, vfs_context_t context)
{
    locked_vfsop(mp, fuse_vfsop_root, vpp, context);
}

static errno_t
fuse_vfsop_biglock_getattr(mount_t mp, struct vfs_attr *attr, vfs_context_t context)
{
    locked_vfsop(mp, fuse_vfsop_getattr, attr, context);
}

static errno_t
fuse_vfsop_biglock_sync(mount_t mp, int waitfor, vfs_context_t context)
{
    locked_vfsop(mp, fuse_vfsop_sync, waitfor, context);
}

static errno_t
fuse_vfsop_biglock_setattr(mount_t mp, struct vfs_attr *fsap, vfs_context_t context)
{
    locked_vfsop(mp, fuse_vfsop_setattr, fsap, context);
}

#endif
