/*
 * Copyright (C) 2006-2008 Google. All Rights Reserved.
 * Copyright (C) 2010 Tuxera. All Rights Reserved.
 * Copyright (C) 2011 Anatol Pomozov. All Rights Reserved.
 */

#include "fuse.h"
#include "fuse_file.h"
#include "fuse_internal.h"
#include "fuse_ipc.h"
#include "fuse_locking.h"
#include "fuse_node.h"
#include "fuse_file.h"
#include "fuse_nodehash.h"
#include "fuse_sysctl.h"
#include "fuse_kludges.h"

#include <AvailabilityMacros.h>
#include <kern/assert.h>
#include <libkern/libkern.h>
#include <libkern/OSMalloc.h>
#include <libkern/locks.h>
#include <mach/mach_types.h>
#include <sys/dirent.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/kernel_types.h>
#include <sys/mount.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/ubc.h>
#include <sys/unistd.h>
#include <sys/vnode.h>
#include <sys/vnode_if.h>
#include <sys/xattr.h>
#include <sys/buf.h>
#include <sys/namei.h>
#include <sys/mman.h>
#include <sys/param.h>

#ifdef FUSE4X_ENABLE_BIGLOCK
#include "fuse_biglock_vnops.h"
#endif

/* access */

__private_extern__
int
fuse_internal_access(vnode_t                   vp,
                     int                       action,
                     vfs_context_t             context,
                     struct fuse_access_param *facp)
{
    int err = 0;
    uint32_t mask = 0;
    int dataflags;
    mount_t mp;
    struct fuse_dispatcher fdi;
    struct fuse_access_in *fai;
    struct fuse_data      *data;

    fuse_trace_printf_func();

    mp = vnode_mount(vp);

    data = fuse_get_mpdata(mp);
    dataflags = data->dataflags;

    /* Allow for now; let checks be handled inline later. */
    if (fuse_isdeferpermissions_mp(mp)) {
        return 0;
    }

    /*
     * (action & KAUTH_VNODE_GENERIC_WRITE_BITS) on a read-only file system
     * would have been handled by higher layers.
     */

    if (!fuse_implemented(data, FSESS_NOIMPLBIT(ACCESS))) {
        return ENOTSUP;
    }

    if (!vnode_isvroot(vp) && !(facp->facc_flags & FACCESS_NOCHECKSPY)) {
        CHECK_BLANKET_DENIAL(vp, context, EPERM);

        facp->facc_flags |= FACCESS_NOCHECKSPY;
    }

    if (!(facp->facc_flags & FACCESS_DO_ACCESS)) {
        return ENOTSUP;
    }

    if (vnode_isdir(vp)) {
        if (action & (KAUTH_VNODE_LIST_DIRECTORY   |
                      KAUTH_VNODE_READ_EXTATTRIBUTES)) {
            mask |= R_OK;
        }
        if (action & (KAUTH_VNODE_ADD_FILE         |
                      KAUTH_VNODE_ADD_SUBDIRECTORY |
                      KAUTH_VNODE_DELETE_CHILD)) {
            mask |= W_OK;
        }
        if (action & KAUTH_VNODE_SEARCH) {
            mask |= X_OK;
        }
    } else {
        if (action & (KAUTH_VNODE_READ_DATA | KAUTH_VNODE_READ_EXTATTRIBUTES)) {
            mask |= R_OK;
        }
        if (action & (KAUTH_VNODE_WRITE_DATA | KAUTH_VNODE_APPEND_DATA)) {
            mask |= W_OK;
        }
        if (action & KAUTH_VNODE_EXECUTE) {
            mask |= X_OK;
        }
    }

    if (action & (KAUTH_VNODE_WRITE_ATTRIBUTES    |
                  KAUTH_VNODE_WRITE_EXTATTRIBUTES |
                  KAUTH_VNODE_WRITE_SECURITY)) {
        mask |= W_OK;
    }

    bzero(&fdi, sizeof(fdi));

    fuse_dispatcher_init(&fdi, sizeof(*fai));
    fuse_dispatcher_make_vp(&fdi, FUSE_ACCESS, vp, context);

    fai = fdi.indata;
    fai->mask = F_OK;
    fai->mask |= mask;

    if (!(err = fuse_dispatcher_wait_answer(&fdi))) {
        fuse_ticket_drop(fdi.ticket);
    }

    if (err == ENOSYS) {
        /*
         * Make sure we don't come in here again.
         */
        vfs_clearauthopaque(mp);
        fuse_clear_implemented(data, FSESS_NOIMPLBIT(ACCESS));
        err = ENOTSUP;
    }

    if (err == ENOENT) {
        const char *vname = vnode_getname(vp);

        log("fuse4x: disappearing vnode %p (name=%s type=%d action=%x)\n",
            vp, (vname) ? vname : "?", vnode_vtype(vp), action);

        if (vname) {
            vnode_putname(vname);
        }

#ifdef FUSE4X_ENABLE_BIGLOCK
        fuse_biglock_unlock(data->biglock);
#endif
        fuse_internal_vnode_disappear(vp, context, REVOKE_SOFT);
#ifdef FUSE4X_ENABLE_BIGLOCK
        fuse_biglock_lock(data->biglock);
#endif
    }

    return err;
}

#ifdef FUSE4X_ENABLE_EXCHANGE

/* exchange */

__private_extern__
int
fuse_internal_exchange(vnode_t       fvp,
                       const char   *fname,
                       size_t        flen,
                       vnode_t       tvp,
                       const char   *tname,
                       size_t        tlen,
                       int           options,
                       vfs_context_t context)
{
    struct fuse_dispatcher fdi;
    struct fuse_exchange_in *fei;
    struct fuse_vnode_data *ffud = VTOFUD(fvp);
    struct fuse_vnode_data *tfud = VTOFUD(tvp);
    vnode_t fdvp = ffud->parentvp;
    vnode_t tdvp = tfud->parentvp;
    int err = 0;

    fuse_dispatcher_init(&fdi, sizeof(*fei) + flen + tlen + 2);
    fuse_dispatcher_make_vp(&fdi, FUSE_EXCHANGE, fvp, context);

    fei = fdi.indata;
    fei->olddir = VTOI(fdvp);
    fei->newdir = VTOI(tdvp);
    fei->options = (uint64_t)options;

    memcpy((char *)fdi.indata + sizeof(*fei), fname, flen);
    ((char *)fdi.indata)[sizeof(*fei) + flen] = '\0';

    memcpy((char *)fdi.indata + sizeof(*fei) + flen + 1, tname, tlen);
    ((char *)fdi.indata)[sizeof(*fei) + flen + tlen + 1] = '\0';

    ubc_msync(fvp, (off_t)0, (off_t)ffud->filesize, NULL,
              UBC_PUSHALL | UBC_INVALIDATE | UBC_SYNC);
    ubc_msync(tvp, (off_t)0, (off_t)tfud->filesize, NULL,
              UBC_PUSHALL | UBC_INVALIDATE | UBC_SYNC);

    if (!(err = fuse_dispatcher_wait_answer(&fdi))) {
        fuse_ticket_drop(fdi.ticket);
    }

    if (err == 0) {
        if (fdvp) {
            fuse_invalidate_attr(fdvp);
        }
        if (tdvp != fdvp) {
            if (tdvp) {
                fuse_invalidate_attr(tdvp);
            }
        }

        fuse_invalidate_attr(fvp);
        fuse_invalidate_attr(tvp);

        cache_purge(fvp);
        cache_purge(tvp);

        /* Swap sizes */
        off_t tmpfilesize = ffud->filesize;
        ffud->filesize = tfud->filesize;
        tfud->filesize = tmpfilesize;
        ubc_setsize(fvp, (off_t)ffud->filesize);
        ubc_setsize(tvp, (off_t)tfud->filesize);

        fuse_kludge_exchange(fvp, tvp);

        /*
         * Another approach (will need additional kernel support to work):
         *
        vnode_t tmpvp = ffud->vp;
        ffud->vp = tfud->vp;
        tfud->vp = tmpvp;

        vnode_t tmpparentvp = ffud->parentvp;
        ffud->parentvp = tfud->parentvp;
        tfud->parentvp = tmpparentvp;

        off_t tmpfilesize = ffud->filesize;
        ffud->filesize = tfud->filesize;
        tfud->filesize = tmpfilesize;

        struct fuse_vnode_data tmpfud;
        memcpy(&tmpfud, ffud, sizeof(struct fuse_vnode_data));
        memcpy(ffud, tfud, sizeof(struct fuse_vnode_data));
        memcpy(tfud, &tmpfud, sizeof(struct fuse_vnode_data));

        HNodeExchangeFromFSNode(ffud, tfud);
        *
        */
    }

    return err;
}

#endif /* FUSE4X_ENABLE_EXCHANGE */

/* fsync */

__private_extern__
int
fuse_internal_fsync_callback(struct fuse_ticket *ticket, __unused uio_t uio)
{
    fuse_trace_printf_func();

    if (ticket->aw_ohead.error == ENOSYS) {
        if (fuse_ticket_opcode(ticket) == FUSE_FSYNC) {
            fuse_clear_implemented(ticket->data, FSESS_NOIMPLBIT(FSYNC));
        } else if (fuse_ticket_opcode(ticket) == FUSE_FSYNCDIR) {
            fuse_clear_implemented(ticket->data, FSESS_NOIMPLBIT(FSYNCDIR));
        } else {
            log("fuse4x: unexpected opcode in sync handling\n");
        }
    }

    fuse_ticket_drop(ticket);

    return 0;
}

__private_extern__
int
fuse_internal_fsync(vnode_t                 vp,
                    vfs_context_t           context,
                    struct fuse_filehandle *fufh,
                    void                   *param)
{
    int err = 0;
    int op = FUSE_FSYNC;
    struct fuse_fsync_in *ffsi;
    struct fuse_dispatcher *dispatcher = param;

    const bool wait_for_completion = true;

    fuse_trace_printf_func();

    dispatcher->iosize = sizeof(*ffsi);
    dispatcher->ticket = NULL;
    if (vnode_isdir(vp)) {
        op = FUSE_FSYNCDIR;
    }

    fuse_dispatcher_make_vp(dispatcher, op, vp, context);
    ffsi = dispatcher->indata;
    ffsi->fh = fufh->fh_id;

    ffsi->fsync_flags = 1; /* datasync */

    if (wait_for_completion) {
        if ((err = fuse_dispatcher_wait_answer(dispatcher))) {
            if (err == ENOSYS) {
                if (op == FUSE_FSYNC) {
                    fuse_clear_implemented(dispatcher->ticket->data,
                                           FSESS_NOIMPLBIT(FSYNC));
                } else if (op == FUSE_FSYNCDIR) {
                    fuse_clear_implemented(dispatcher->ticket->data,
                                           FSESS_NOIMPLBIT(FSYNCDIR));
                }
            }
            goto out;
        } else {
            fuse_ticket_drop(dispatcher->ticket);
        }
    } else {
        fuse_insert_callback(dispatcher->ticket, fuse_internal_fsync_callback);
        fuse_insert_message(dispatcher->ticket);
    }

out:
    return err;
}

/* getattr sidekicks */
__private_extern__
int
fuse_internal_loadxtimes(vnode_t vp, struct vnode_attr *out_vap,
                         vfs_context_t context)
{
    struct vnode_attr *in_vap = VTOVA(vp);
    struct fuse_data *data = fuse_get_mpdata(vnode_mount(vp));
    struct fuse_dispatcher fdi;
    struct fuse_getxtimes_out *fgxo = NULL;
    int isvroot = vnode_isvroot(vp);
    struct timespec t = { 0, 0 };
    const struct timespec kZeroTime = { 0, 0 };
    int err = 0;

    if (!(data->dataflags & FSESS_XTIMES)) {
        /* We don't return anything. */
        goto out;
    }

    if (VTOFUD(vp)->c_flag & C_XTIMES_VALID) {
        VATTR_RETURN(out_vap, va_backup_time, in_vap->va_backup_time);
        VATTR_RETURN(out_vap, va_create_time, in_vap->va_create_time);
        goto out;
    }

    if (!fuse_implemented(data, FSESS_NOIMPLBIT(GETXTIMES))) {
        goto fake;
    }

    if (fuse_isdeadfs(vp) && isvroot) {
        goto fake;
    }

    if (!data->inited && isvroot) {
        goto fake;
    }

    err = fuse_dispatcher_simple_putget_vp(&fdi, FUSE_GETXTIMES, vp, context);
    if (err) {
        /* We don't ever treat this as a hard error. */
        err = 0;
        goto fake;
    }

    fgxo = (struct fuse_getxtimes_out *)fdi.answer;

    t.tv_sec = (time_t)fgxo->bkuptime; /* XXX: truncation */
    t.tv_nsec = fgxo->bkuptimensec;
    VATTR_RETURN(in_vap, va_backup_time, t);
    VATTR_RETURN(out_vap, va_backup_time, t);

    t.tv_sec = (time_t)fgxo->crtime; /* XXX: truncation */
    t.tv_nsec = fgxo->crtimensec;
    VATTR_RETURN(in_vap, va_create_time, t);
    VATTR_RETURN(out_vap, va_create_time, t);

    fuse_ticket_drop(fdi.ticket);

    VTOFUD(vp)->c_flag |= C_XTIMES_VALID;

    goto out;

fake:
    VATTR_RETURN(out_vap, va_backup_time, kZeroTime);
    VATTR_RETURN(out_vap, va_create_time, kZeroTime);

out:
    return err;
}

/* setattr sidekicks */
__private_extern__
int
fuse_internal_attr_vat2fsai(mount_t                 mp,
                            vnode_t                 vp,
                            struct vnode_attr      *vap,
                            struct fuse_setattr_in *fsai,
                            uint64_t               *newsize)
{
    /*
     * XXX: Locking
     *
     * We need to worry about the file size changing in setattr(). If the call
     * is indeed altering the size, then:
     *
     * lock_exclusive(truncatelock)
     *   lock(nodelock)
     *     set the new size
     *   unlock(nodelock)
     *   adjust ubc
     *   lock(nodelock)
     *     do cleanup
     *   unlock(nodelock)
     * unlock(truncatelock)
     * ...
     */

    int sizechanged = 0;
    uid_t nuid;
    gid_t ngid;

    fsai->valid = 0;

    if (newsize) {
        *newsize = 0;
    }

    nuid = VATTR_IS_ACTIVE(vap, va_uid) ? vap->va_uid : (uid_t)VNOVAL;
    if (nuid != (uid_t)VNOVAL) {
        fsai->uid = nuid;
        fsai->valid |= FATTR_UID;
    }
    VATTR_SET_SUPPORTED(vap, va_uid);

    ngid = VATTR_IS_ACTIVE(vap, va_gid) ? vap->va_gid : (gid_t)VNOVAL;
    if (ngid != (gid_t)VNOVAL) {
        fsai->gid = ngid;
        fsai->valid |= FATTR_GID;
    }
    VATTR_SET_SUPPORTED(vap, va_gid);

    if (VATTR_IS_ACTIVE(vap, va_data_size)) {

        // Truncate to a new value.
        fsai->size = vap->va_data_size;
        sizechanged = 1;
        if (newsize) {
            *newsize = vap->va_data_size;
        }
        fsai->valid |= FATTR_SIZE;

        if (vp) {
            struct fuse_filehandle *fufh = NULL;
            fufh_type_t fufh_type = FUFH_WRONLY;
            struct fuse_vnode_data *fvdat = VTOFUD(vp);

            fufh = &(fvdat->fufh[fufh_type]);

            if (!FUFH_IS_VALID(fufh)) {
                fufh_type = FUFH_RDWR;
                fufh = &(fvdat->fufh[fufh_type]);
                if (!FUFH_IS_VALID(fufh)) {
                    fufh = NULL;
                }
            }

            if (fufh) {
                fsai->fh = fufh->fh_id;
                fsai->valid |= FATTR_FH;
            }
        }
    }
    VATTR_SET_SUPPORTED(vap, va_data_size);

    /*
     * Possible timestamps:
     *
     * Mac OS X                                          Linux  FUSE API
     *
     * va_access_time    last access time                atime  atime
     * va_backup_time    last backup time                -      -
     * va_change_time    last metadata change time       ctime* -
     * va_create_time    creation time                   -      -
     * va_modify_time    last data modification time     mtime  mtime
     *
     */

    if (VATTR_IS_ACTIVE(vap, va_access_time)) {
        fsai->atime = vap->va_access_time.tv_sec;
        /* XXX: truncation */
        fsai->atimensec = (uint32_t)vap->va_access_time.tv_nsec;
        fsai->valid |=  FATTR_ATIME;
    }
    VATTR_SET_SUPPORTED(vap, va_access_time);

    if (VATTR_IS_ACTIVE(vap, va_modify_time)) {
        fsai->mtime = vap->va_modify_time.tv_sec;
        /* XXX: truncation */
        fsai->mtimensec = (uint32_t)vap->va_modify_time.tv_nsec;
        fsai->valid |=  FATTR_MTIME;
    }
    VATTR_SET_SUPPORTED(vap, va_modify_time);

    if (VATTR_IS_ACTIVE(vap, va_backup_time) && fuse_isxtimes_mp(mp)) {
        fsai->bkuptime = vap->va_backup_time.tv_sec;
        /* XXX: truncation */
        fsai->bkuptimensec = (uint32_t)vap->va_backup_time.tv_nsec;
        fsai->valid |= FATTR_BKUPTIME;
        VATTR_SET_SUPPORTED(vap, va_backup_time);
    }

    if (VATTR_IS_ACTIVE(vap, va_change_time)) {
        if (fuse_isxtimes_mp(mp)) {
            fsai->chgtime = vap->va_change_time.tv_sec;
            /* XXX: truncation */
            fsai->chgtimensec = (uint32_t)vap->va_change_time.tv_nsec;
            fsai->valid |=  FATTR_CHGTIME;
            VATTR_SET_SUPPORTED(vap, va_change_time);
        }
    }

    if (VATTR_IS_ACTIVE(vap, va_create_time) && fuse_isxtimes_mp(mp)) {
        fsai->crtime = vap->va_create_time.tv_sec;
        /* XXX: truncation */
        fsai->crtimensec = (uint32_t)vap->va_create_time.tv_nsec;
        fsai->valid |= FATTR_CRTIME;
        VATTR_SET_SUPPORTED(vap, va_create_time);
    }

    if (VATTR_IS_ACTIVE(vap, va_mode)) {
        fsai->mode = vap->va_mode & ALLPERMS;
        fsai->mode |= VTTOIF(vnode_vtype(vp));
        fsai->valid |= FATTR_MODE;
    }
    VATTR_SET_SUPPORTED(vap, va_mode);

    if (VATTR_IS_ACTIVE(vap, va_flags)) {
        fsai->flags = vap->va_flags;
        fsai->valid |= FATTR_FLAGS;
    }
    VATTR_SET_SUPPORTED(vap, va_flags);

    /*
     * We /are/ OK with va_acl, va_guuid, and va_uuuid passing through here.
     */

    return sizechanged;
}


/* readdir */

__private_extern__
int
fuse_internal_readdir(vnode_t                 vp,
                      uio_t                   uio,
                      vfs_context_t           context,
                      struct fuse_filehandle *fufh,
                      struct fuse_iov        *cookediov,
                      int                    *numdirent)
{
    int err = 0;
    struct fuse_dispatcher fdi;
    struct fuse_read_in   *fri;
    struct fuse_data      *data;

    if (uio_resid(uio) == 0) {
        return 0;
    }

    fuse_dispatcher_init(&fdi, 0);

    /* Note that we DO NOT have a UIO_SYSSPACE here (so no need for p2p I/O). */

    while (uio_resid(uio) > 0) {

        fdi.iosize = sizeof(*fri);
        fuse_dispatcher_make_vp(&fdi, FUSE_READDIR, vp, context);

        fri = fdi.indata;
        fri->fh = fufh->fh_id;
        fri->offset = uio_offset(uio);
        data = fuse_get_mpdata(vnode_mount(vp));
        fri->size = (typeof(fri->size))min((size_t)uio_resid(uio), data->iosize);

        if ((err = fuse_dispatcher_wait_answer(&fdi))) {
            goto out;
        }

        if ((err = fuse_internal_readdir_processdata(vp,
                                                     uio,
                                                     fri->size,
                                                     fdi.answer,
                                                     fdi.iosize,
                                                     cookediov,
                                                     numdirent))) {
            break;
        }
    }

/* done: */

    fuse_ticket_drop(fdi.ticket);

out:
    return ((err == -1) ? 0 : err);
}

__private_extern__
int
fuse_internal_readdir_processdata(vnode_t          vp,
                                  uio_t            uio,
                         __unused size_t           reqsize,
                                  void            *buf,
                                  size_t           bufsize,
                                  struct fuse_iov *cookediov,
                                  int             *numdirent)
{
    int err = 0;
    int cou = 0;
    int n   = 0;
    size_t bytesavail;
    size_t freclen;

    struct dirent      *de;
    struct fuse_dirent *fudge;

    if (bufsize < FUSE_NAME_OFFSET) {
        return -1;
    }

    for (;;) {

        if (bufsize < FUSE_NAME_OFFSET) {
            err = -1;
            break;
        }

        fudge = (struct fuse_dirent *)buf;
        freclen = FUSE_DIRENT_SIZE(fudge);

        cou++;

        if (bufsize < freclen) {
            err = ((cou == 1) ? -1 : 0);
            break;
        }

        /*
         * if (isbzero(buf, FUSE_NAME_OFFSET)) {
         *     // zero-pad incomplete buffer
         *     ...
         *     err = -1;
         *     break;
         * }
         */

        if (!fudge->namelen) {
            err = EINVAL;
            break;
        }

        if (fudge->namelen > FUSE_MAXNAMLEN) {
            err = EIO;
            break;
        }

#define GENERIC_DIRSIZ(dp) \
  ((sizeof(struct dirent) - (FUSE_MAXNAMLEN + 1)) + \
   (((dp)->d_namlen + 1 + 3) & ~3))

        bytesavail = GENERIC_DIRSIZ((struct pseudo_dirent *)&fudge->namelen);

        if (bytesavail > (size_t)uio_resid(uio)) {
            err = -1;
            break;
        }

        fiov_refresh(cookediov);
        fiov_adjust(cookediov, bytesavail);

        de = (struct dirent *)cookediov->base;
#ifdef _DARWIN_FEATURE_64_BIT_INODE
        de->d_ino = fudge->ino;
#else
        de->d_ino = (ino_t)fudge->ino; /* XXX: truncation */
#endif /* _DARWIN_FEATURE_64_BIT_INODE */
        de->d_reclen = bytesavail;
        de->d_type   = fudge->type;
        de->d_namlen = fudge->namelen;

        /* Filter out any ._* files if the mount is configured as such. */
        if (fuse_skip_apple_double_mp(vnode_mount(vp),
                                      fudge->name, fudge->namelen)) {
            de->d_fileno = 0;
            de->d_type = DT_WHT;
        }

        memcpy((char *)cookediov->base +
               sizeof(struct dirent) - FUSE_MAXNAMLEN - 1,
               (char *)buf + FUSE_NAME_OFFSET, fudge->namelen);
        ((char *)cookediov->base)[bytesavail] = '\0';

        err = uiomove(cookediov->base, (int)cookediov->len, uio);
        if (err) {
            break;
        }

        n++;

        buf = (char *)buf + freclen;
        bufsize -= freclen;
        uio_setoffset(uio, fudge->off);
    }

    if (!err && numdirent) {
        *numdirent = n;
    }

    return err;
}

/* remove */

static int
fuse_internal_remove_callback(vnode_t vp, void *cargs)
{
    struct vnode_attr *vap;
    uint64_t target_nlink;

    vap = VTOVA(vp);

    target_nlink = *(uint64_t *)cargs;

    /* somewhat lame "heuristics", but you got better ideas? */
    if ((vap->va_nlink == target_nlink) && vnode_isreg(vp)) {
        fuse_invalidate_attr(vp);
    }

    return VNODE_RETURNED;
}

__private_extern__
int
fuse_internal_remove(vnode_t               dvp,
                     vnode_t               vp,
                     struct componentname *cnp,
                     enum fuse_opcode      op,
                     vfs_context_t         context)
{
    struct fuse_dispatcher fdi;

    struct vnode_attr *vap = VTOVA(vp);
    int need_invalidate = 0;
    uint64_t target_nlink = 0;
    mount_t mp = vnode_mount(vp);

    int err = 0;

    fuse_dispatcher_init(&fdi, cnp->cn_namelen + 1);
    fuse_dispatcher_make_vp(&fdi, op, dvp, context);

    memcpy(fdi.indata, cnp->cn_nameptr, cnp->cn_namelen);
    ((char *)fdi.indata)[cnp->cn_namelen] = '\0';

    if ((vap->va_nlink > 1) && vnode_isreg(vp)) {
        need_invalidate = 1;
        target_nlink = vap->va_nlink;
    }

    if (!(err = fuse_dispatcher_wait_answer(&fdi))) {
        fuse_ticket_drop(fdi.ticket);
    }

    fuse_invalidate_attr(dvp);
    fuse_invalidate_attr(vp);

    /*
     * XXX: M_FUSE4X_INVALIDATE_CACHED_VATTRS_UPON_UNLINK
     *
     * Consider the case where vap->va_nlink > 1 for the entity being
     * removed. In our world, other in-memory vnodes that share a link
     * count each with this one may not know right way that this one just
     * got deleted. We should let them know, say, through a vnode_iterate()
     * here and a callback that does fuse_invalidate_attr(vp) on each
     * relevant vnode.
     */
    if (need_invalidate && !err) {
        if (!vfs_busy(mp, LK_NOWAIT)) {
#ifdef FUSE4X_ENABLE_BIGLOCK
            struct fuse_data *data = fuse_get_mpdata(mp);
            fuse_biglock_unlock(data->biglock);
#endif
            vnode_iterate(mp, 0, fuse_internal_remove_callback,
                          (void *)&target_nlink);
#ifdef FUSE4X_ENABLE_BIGLOCK
            fuse_biglock_lock(data->biglock);
#endif
            vfs_unbusy(mp);
        } else {
            log("fuse4x: skipping link count fixup upon remove\n");
        }
    }

    return err;
}

/* rename */

__private_extern__
int
fuse_internal_rename(vnode_t               fdvp,
            __unused vnode_t               fvp,
                     struct componentname *fcnp,
                     vnode_t               tdvp,
            __unused vnode_t               tvp,
                     struct componentname *tcnp,
                     vfs_context_t         context)
{
    struct fuse_dispatcher fdi;
    struct fuse_rename_in *fri;
    int err = 0;

    fuse_dispatcher_init(&fdi, sizeof(*fri) + fcnp->cn_namelen + tcnp->cn_namelen + 2);
    fuse_dispatcher_make_vp(&fdi, FUSE_RENAME, fdvp, context);

    fri = fdi.indata;
    fri->newdir = VTOI(tdvp);
    memcpy((char *)fdi.indata + sizeof(*fri), fcnp->cn_nameptr,
           fcnp->cn_namelen);
    ((char *)fdi.indata)[sizeof(*fri) + fcnp->cn_namelen] = '\0';
    memcpy((char *)fdi.indata + sizeof(*fri) + fcnp->cn_namelen + 1,
           tcnp->cn_nameptr, tcnp->cn_namelen);
    ((char *)fdi.indata)[sizeof(*fri) + fcnp->cn_namelen +
                         tcnp->cn_namelen + 1] = '\0';

    if (!(err = fuse_dispatcher_wait_answer(&fdi))) {
        fuse_ticket_drop(fdi.ticket);
    }

    if (err == 0) {
        fuse_invalidate_attr(fdvp);
        if (tdvp != fdvp) {
            fuse_invalidate_attr(tdvp);
        }
    }

    return err;
}

/* revoke */

__private_extern__
int
fuse_internal_revoke(vnode_t vp, int flags, vfs_context_t context, int how)
{
    int ret = 0;
    struct fuse_vnode_data *fvdat = VTOFUD(vp);

    fvdat->flag |= FN_REVOKED;

    if (how == REVOKE_HARD) {
        ret = vn_revoke(vp, flags, context);
    }

    return ret;
}

/* strategy */

__private_extern__
int
fuse_internal_strategy(vnode_t vp, buf_t bp)
{
    size_t biosize;
    size_t chunksize;
    size_t respsize;

    bool mapped = false;
    int mode;
    int op;
    int vtype = vnode_vtype(vp);

    int err = 0;

    caddr_t bufdat;
    off_t   left;
    off_t   offset;
    int32_t bflags = buf_flags(bp);

    fufh_type_t             fufh_type;
    struct fuse_dispatcher  fdi;
    struct fuse_data       *data;
    struct fuse_vnode_data *fvdat = VTOFUD(vp);
    struct fuse_filehandle *fufh = NULL;
    mount_t mp = vnode_mount(vp);

    data = fuse_get_mpdata(mp);

    biosize = data->blocksize;

    if (!(vtype == VREG || vtype == VDIR)) {
        return ENOTSUP;
    }

    if (bflags & B_READ) {
        mode = FREAD;
        fufh_type = FUFH_RDONLY; /* FUFH_RDWR will also do */
    } else {
        mode = FWRITE;
        fufh_type = FUFH_WRONLY; /* FUFH_RDWR will also do */
    }

    if (fvdat->flag & FN_CREATING) {
        fuse_lck_mtx_lock(fvdat->createlock);
        if (fvdat->flag & FN_CREATING) {
            (void)fuse_msleep(fvdat->creator, fvdat->createlock,
                              PDROP | PINOD | PCATCH, "fuse_internal_strategy",
                              NULL);
        } else {
            fuse_lck_mtx_unlock(fvdat->createlock);
        }
    }

    fufh = &(fvdat->fufh[fufh_type]);

    if (!FUFH_IS_VALID(fufh)) {
        fufh_type = FUFH_RDWR;
        fufh = &(fvdat->fufh[fufh_type]);
        if (!FUFH_IS_VALID(fufh)) {
            fufh = NULL;
        } else {
            /* We've successfully fallen back to FUFH_RDWR. */
        }
    }

    if (!fufh) {

        if (mode == FREAD) {
            fufh_type = FUFH_RDONLY;
        } else {
            fufh_type = FUFH_RDWR;
        }

        /*
         * Lets NOT do the filehandle preflight check here.
         */

        err = fuse_filehandle_get(vp, NULL, fufh_type, 0 /* mode */);

        if (!err) {
            fufh = &(fvdat->fufh[fufh_type]);
            FUFH_AUX_INC(fufh);
            /* We've created a NEW fufh of type fufh_type. open_count is 1. */
        }

    } else { /* good fufh */

        OSIncrementAtomic((SInt32 *)&fuse_fh_reuse_count);

        /* We're using an existing fufh of type fufh_type. */
    }

    if (err) {

         /* A more typical error case. */
         if ((err == ENOTCONN) || fuse_isdeadfs(vp)) {
             buf_seterror(bp, EIO);
             buf_biodone(bp);
             return EIO;
         }

         log("fuse4x: strategy failed to get fh "
               "(vtype=%d, fufh_type=%d, err=%d)\n", vtype, fufh_type, err);

         if (!vfs_issynchronous(mp)) {
             log("fuse4x: asynchronous write failed!\n");
         }

         buf_seterror(bp, EIO);
         buf_biodone(bp);
         return EIO;
    }

    if (!fufh) {
        panic("fuse4x: tried everything but still no fufh");
        /* NOTREACHED */
    }

#define B_INVAL 0x00040000 /* Does not contain valid info. */
#define B_ERROR 0x00080000 /* I/O error occurred. */

    if (bflags & B_INVAL) {
        log("fuse4x: buffer does not contain valid information\n");
    }

    if (bflags & B_ERROR) {
        log("fuse4x: an I/O error has occured\n");
    }

    if (buf_count(bp) == 0) {
        return 0;
    }

    fuse_dispatcher_init(&fdi, 0);

    if (mode == FREAD) {

        struct fuse_read_in *fri;

        buf_setresid(bp, buf_count(bp));
        offset = (off_t)((off_t)buf_blkno(bp) * biosize);

        if (offset >= fvdat->filesize) {
            /* Trying to read at/after EOF? */
            if (offset != fvdat->filesize) {
                /* Trying to read after EOF? */
                buf_seterror(bp, EINVAL);
            }
            buf_biodone(bp);
            return 0;
        }

        /* Note that we just made sure that offset < fvdat->filesize. */
        if ((offset + buf_count(bp)) > fvdat->filesize) {
            /* Trimming read */
            buf_setcount(bp, (uint32_t)(fvdat->filesize - offset));
        }

        if (buf_map(bp, &bufdat)) {
            log("fuse4x: failed to map buffer in strategy\n");
            return EFAULT;
        } else {
            mapped = true;
        }

        while (buf_resid(bp) > 0) {

            chunksize = min((size_t)buf_resid(bp), data->iosize);

            fdi.iosize = sizeof(*fri);

            op = FUSE_READ;
            if (vtype == VDIR) {
                op = FUSE_READDIR;
            }
            fuse_dispatcher_make_vp(&fdi, op, vp, NULL);

            fri = fdi.indata;
            fri->fh = fufh->fh_id;

            /*
             * Historical note:
             *
             * fri->offset = ((off_t)(buf_blkno(bp))) * biosize;
             *
             * This wasn't being incremented!?
             */

            fri->offset = offset;
            fri->size = (typeof(fri->size))chunksize;
            fdi.ticket->aw_type = FT_A_BUF;
            fdi.ticket->aw_bufdata = bufdat;

            if ((err = fuse_dispatcher_wait_answer(&fdi))) {
                /* There was a problem with reading. */
                goto out;
            }

            respsize = fdi.ticket->aw_bufsize;

            buf_setresid(bp, (uint32_t)(buf_resid(bp) - respsize));
            bufdat += respsize;
            offset += respsize;

            /* Did we hit EOF before being done? */
            if ((respsize == 0) && (buf_resid(bp) > 0)) {
                 /*
                  * Historical note:
                  * If we don't get enough data, just fill the rest with zeros.
                  * In NFS context, this would mean a hole in the file.
                  */

                 /* Zero-pad the incomplete buffer. */
                 bzero(bufdat, buf_resid(bp));
                 buf_setresid(bp, 0);
                 break;
            }
        } /* while (buf_resid(bp) > 0) */
    } else {
        /* write */
        struct fuse_write_in  *fwi;
        struct fuse_write_out *fwo;
        int merr = 0;
        off_t diff;

        if (buf_map(bp, &bufdat)) {
            log("fuse4x: failed to map buffer in strategy\n");
            return EFAULT;
        } else {
            mapped = true;
        }

        /* Write begin */

        buf_setresid(bp, buf_count(bp));
        offset = (off_t)((off_t)buf_blkno(bp) * biosize);

        /* XXX: TBD -- Check here for extension (writing past end) */

        left = buf_count(bp);

        while (left) {

            fdi.iosize = sizeof(*fwi);
            op = FUSE_WRITE;

            fuse_dispatcher_make_vp(&fdi, op, vp, NULL);
            chunksize = min((size_t)left, data->iosize);

            fwi = fdi.indata;
            fwi->fh = fufh->fh_id;
            fwi->offset = offset;
            fwi->size = (typeof(fwi->size))chunksize;

            fdi.ticket->ms_type = FT_M_BUF;
            fdi.ticket->ms_bufdata = bufdat;
            fdi.ticket->ms_bufsize = chunksize;

            /* About to write <chunksize> at <offset> */

            if ((err = fuse_dispatcher_wait_answer(&fdi))) {
                merr = 1;
                break;
            }

            fwo = fdi.answer;
            diff = chunksize - fwo->size;
            if (diff < 0) {
                err = EINVAL;
                break;
            }

            left -= fwo->size;
            bufdat += fwo->size;
            offset += fwo->size;
            buf_setresid(bp, buf_resid(bp) - fwo->size);
        }

        if (merr) {
            goto out;
        }
    }

    if (fdi.ticket) {
        fuse_ticket_drop(fdi.ticket);
    } else {
        /* No ticket upon leaving */
    }

out:

    if (err) {
        buf_seterror(bp, err);
    }

    if (mapped) {
        buf_unmap(bp);
    }

    buf_biodone(bp);

    return err;
}

__private_extern__
errno_t
fuse_internal_strategy_buf(struct vnop_strategy_args *ap)
{
    int32_t   bflags;
    upl_t     bupl;
    daddr64_t blkno, lblkno;
    int       bmap_flags;
    buf_t     bp    = ap->a_bp;
    vnode_t   vp    = buf_vnode(bp);
    int       vtype = vnode_vtype(vp);

    struct fuse_data *data;

    if (!vp || vtype == VCHR || vtype == VBLK) {
        panic("fuse4x: buf_strategy: b_vp == NULL || vtype == VCHR | VBLK\n");
    }

    bflags = buf_flags(bp);

    if (bflags & B_READ) {
        bmap_flags = VNODE_READ;
    } else {
        bmap_flags = VNODE_WRITE;
    }

    bupl = buf_upl(bp);
    blkno = buf_blkno(bp);
    lblkno = buf_lblkno(bp);

    if (!(bflags & B_CLUSTER)) {

        if (bupl) {
            return cluster_bp(bp);
        }

        if (blkno == lblkno) {
            off_t  f_offset;
            size_t contig_bytes;

            data = fuse_get_mpdata(vnode_mount(vp));

            // Still think this is a kludge?
            f_offset = lblkno * data->blocksize;
            blkno = f_offset / data->blocksize;

            buf_setblkno(bp, blkno);

            contig_bytes = buf_count(bp);

            if (blkno == -1) {
                buf_clear(bp);
            }

            /*
             * Our "device" is always /all contiguous/. We don't wanna be
             * doing things like:
             *
             * ...
             *     else if ((long)contig_bytes < buf_count(bp)) {
             *         ret = buf_strategy_fragmented(devvp, bp, f_offset,
             *                                       contig_bytes));
             *         return ret;
             *      }
             */
        }

        if (blkno == -1) {
            buf_biodone(bp);
            return 0;
        }
    }

    // Issue the I/O

    return fuse_internal_strategy(vp, bp);
}

/* entity creation */

__private_extern__
void
fuse_internal_newentry_makerequest(mount_t                 mp,
                                   uint64_t                dnid,
                                   struct componentname   *cnp,
                                   enum fuse_opcode        op,
                                   void                   *buf,
                                   size_t                  bufsize,
                                   struct fuse_dispatcher *dispatcher,
                                   vfs_context_t           context)
{
    fuse_dispatcher_init(dispatcher, bufsize + cnp->cn_namelen + 1);

    fuse_dispatcher_make(dispatcher, op, mp, dnid, context);
    memcpy(dispatcher->indata, buf, bufsize);
    memcpy((char *)dispatcher->indata + bufsize, cnp->cn_nameptr, cnp->cn_namelen);
    ((char *)dispatcher->indata)[bufsize + cnp->cn_namelen] = '\0';
}

__private_extern__
int
fuse_internal_newentry_core(vnode_t                 dvp,
                            vnode_t                *vpp,
                            struct componentname   *cnp,
                            enum vtype              vtyp,
                            struct fuse_dispatcher *dispatcher,
                            vfs_context_t           context)
{
    int err = 0;
    struct fuse_entry_out *feo;
    mount_t mp = vnode_mount(dvp);

    if ((err = fuse_dispatcher_wait_answer(dispatcher))) {
        return err;
    }

    feo = dispatcher->answer;

    if ((err = fuse_internal_checkentry(feo, vtyp))) {
        goto out;
    }

    err = fuse_vget_i(vpp, 0 /* flags */, feo, cnp, dvp, mp, context);
    if (err) {
        fuse_internal_forget_send(mp, context, feo->nodeid, 1, dispatcher);
        return err;
    }

    cache_attrs(*vpp, feo);

out:
    fuse_ticket_drop(dispatcher->ticket);

    return err;
}

__private_extern__
int
fuse_internal_newentry(vnode_t               dvp,
                       vnode_t              *vpp,
                       struct componentname *cnp,
                       enum fuse_opcode      op,
                       void                 *buf,
                       size_t                bufsize,
                       enum vtype            vtype,
                       vfs_context_t         context)
{
    int err;
    struct fuse_dispatcher fdi;
    mount_t mp = vnode_mount(dvp);

    if (fuse_skip_apple_double_mp(mp, cnp->cn_nameptr, cnp->cn_namelen)) {
        return EACCES;
    }

    fuse_dispatcher_init(&fdi, 0);
    fuse_internal_newentry_makerequest(mp, VTOI(dvp), cnp, op, buf,
                                       bufsize, &fdi, context);
    err = fuse_internal_newentry_core(dvp, vpp, cnp, vtype, &fdi, context);
    fuse_invalidate_attr(dvp);

    return err;
}

/* entity destruction */

__private_extern__
int
fuse_internal_forget_callback(struct fuse_ticket *ticket, __unused uio_t uio)
{
    struct fuse_dispatcher fdi;

    fdi.ticket = ticket;

    fuse_internal_forget_send(ticket->data->mp, NULL,
        ((struct fuse_in_header *)ticket->ms_fiov.base)->nodeid, 1, &fdi);

    return 0;
}

__private_extern__
void
fuse_internal_forget_send(mount_t                 mp,
                          vfs_context_t           context,
                          uint64_t                nodeid,
                          uint64_t                nlookup,
                          struct fuse_dispatcher *dispatcher)
{
    struct fuse_forget_in *ffi;

    /*
     * KASSERT(nlookup > 0, ("zero-times forget for vp #%llu",
     *         (long long unsigned) nodeid));
     */

    fuse_dispatcher_init(dispatcher, sizeof(*ffi));
    fuse_dispatcher_make(dispatcher, FUSE_FORGET, mp, nodeid, context);

    ffi = dispatcher->indata;
    ffi->nlookup = nlookup;

    dispatcher->ticket->invalid = true;
    fuse_insert_message(dispatcher->ticket);
}

__private_extern__
void
fuse_internal_interrupt_send(struct fuse_ticket *ticket)
{
    struct fuse_dispatcher fdi;
    struct fuse_interrupt_in *fii;

    fdi.ticket = ticket;
    fuse_dispatcher_init(&fdi, sizeof(*fii));
    fuse_dispatcher_make(&fdi, FUSE_INTERRUPT, ticket->data->mp, (uint64_t)0, NULL);
    fii = fdi.indata;
    fii->unique = ticket->unique;
    fdi.ticket->invalid = true;
    fuse_insert_message(fdi.ticket);
}

__private_extern__
void
fuse_internal_vnode_disappear(vnode_t vp, vfs_context_t context, int how)
{
    int err = 0;

    fuse_vncache_purge(vp);

    if (how != REVOKE_NONE) {
        err = fuse_internal_revoke(vp, REVOKEALL, context, how);
        if (err) {
            log("fuse4x: disappearing act: revoke failed (%d)\n", err);
        }

#if MAC_OS_X_VERSION_MIN_REQUIRED >= 1060
        /* Checking whether the vnode is in the process of being recycled
         * to avoid the 'vnode reclaim in progress' kernel panic.
         *
         * Obviously this is a quick fix done without much understanding of
         * the code flow of a recycle operation, but it seems that we
         * shouldn't call this again if a recycle operation was the reason
         * that we got here.
         */
        if(!vnode_isrecycled(vp)) {
#endif
            err = vnode_recycle(vp);
            if (err) {
                log("fuse4x: disappearing act: recycle failed (%d)\n", err);
            }
#if MAC_OS_X_VERSION_MIN_REQUIRED >= 1060
        } else {
                log("fuse4x: Avoided 'vnode reclaim in progress' kernel "
                        "panic. What now?\n");
        }
#endif
    }
}

/* fuse start/stop */

__private_extern__
int
fuse_internal_init_callback(struct fuse_ticket *ticket, __unused uio_t uio)
{
    int err = 0;
    struct fuse_init_out *fiio;
    struct fuse_data *data = ticket->data;

    fuse_trace_printf_func();

    if ((err = ticket->aw_ohead.error)) {
        log("fuse4x: user-space initialization failed (%d)\n", err);
        goto out;
    }

    err = fuse_ticket_pull(ticket, uio);
    if (err) {
        log("fuse4x: cannot pull ticket\n");
        goto out;
    }

    fiio = ticket->aw_fiov.base;

    if ((fiio->major < FUSE_KERNEL_VERSION) ||
        (fiio->minor < FUSE_KERNEL_MINOR_VERSION)) {
        log("fuse4x: user-space library has outdated protocol version. Required(%d.%d), user returned (%d.%d)\n",
              FUSE_KERNEL_VERSION, FUSE_KERNEL_MINOR_VERSION,
              fiio->major, fiio->minor);
        err = EPROTONOSUPPORT;
        goto out;
    }

    if (ticket->aw_fiov.len == sizeof(struct fuse_init_out)) {
        data->max_write = fiio->max_write;
    } else {
        err = EINVAL;
    }

    if (fiio->flags & FUSE_CASE_INSENSITIVE) {
        data->dataflags |= FSESS_CASE_INSENSITIVE;
    }

    if (fiio->flags & FUSE_VOL_RENAME) {
        data->dataflags |= FSESS_VOL_RENAME;
    }

    if (fiio->flags & FUSE_XTIMES) {
        data->dataflags |= FSESS_XTIMES;
    }

out:
    fuse_ticket_drop(ticket);

    if (err) {
        fuse_data_kill(data);
    }

    fuse_lck_mtx_lock(data->ticket_mtx);
    data->inited = true;
    fuse_wakeup(&data->ticketer);
    fuse_lck_mtx_unlock(data->ticket_mtx);

    return 0;
}

__private_extern__
int
fuse_send_init(struct fuse_data *data, vfs_context_t context)
{
    struct fuse_init_in   *fiii;
    struct fuse_dispatcher fdi;

    fuse_dispatcher_init(&fdi, sizeof(*fiii));
    fuse_dispatcher_make(&fdi, FUSE_INIT, data->mp, 0, context);
    fiii = fdi.indata;
    fiii->major = FUSE_KERNEL_VERSION;
    fiii->minor = FUSE_KERNEL_MINOR_VERSION;
    fiii->max_readahead = data->iosize * 16;
    fiii->flags = 0;

    fuse_insert_callback(fdi.ticket, fuse_internal_init_callback);
    fuse_insert_message(fdi.ticket);

    return 0;
}

/* other */

static int
fuse_internal_print_vnodes_callback(vnode_t vp, __unused void *cargs)
{
    struct fuse_vnode_data *fvdat = VTOFUD(vp);
    const char *vname = vnode_getname(vp);

    if (vname) {
        log("fuse4x: vp=%p ino=%lld parent=%lld inuse=%d %s\n",
              vp, fvdat->nodeid, fvdat->parent_nodeid,
              vnode_isinuse(vp, 0), vname);
    } else {
        if (fvdat->nodeid == FUSE_ROOT_ID) {
            log("fuse4x: vp=%p ino=%lld parent=%lld inuse=%d /\n",
                  vp, fvdat->nodeid, fvdat->parent_nodeid,
                  vnode_isinuse(vp, 0));
        } else {
            log("fuse4x: vp=%p ino=%lld parent=%lld inuse=%d\n",
                  vp, fvdat->nodeid, fvdat->parent_nodeid,
                  vnode_isinuse(vp, 0));
        }
    }

    if (vname) {
        vnode_putname(vname);
    }

    return VNODE_RETURNED;
}

__private_extern__
void
fuse_internal_print_vnodes(mount_t mp)
{
    vnode_iterate(mp, VNODE_ITERATE_ALL,
                  fuse_internal_print_vnodes_callback, NULL);
}

__private_extern__
void
fuse_preflight_log(vnode_t vp, fufh_type_t fufh_type, int err, char *message)
{
    const char *vname = vnode_getname(vp);

    if (vname) {
        log("fuse4x: file handle preflight (caller=%s, type=%d, err=%d, name=%s)\n",
            message, fufh_type, err, vname);
    } else {
        log("fuse4x: file handle preflight (caller=%s, type=%d, err=%d)\n",
            message, fufh_type, err);
    }

    if (vname) {
        vnode_putname(vname);
    }
}
