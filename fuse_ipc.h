/*
 * Copyright (C) 2006-2008 Google. All Rights Reserved.
 * Copyright (C) 2010 Tuxera. All Rights Reserved.
 * Copyright (C) 2011 Anatol Pomozov. All Rights Reserved.
 */

#ifndef _FUSE_IPC_H_
#define _FUSE_IPC_H_

#include <mach/mach_types.h>
#include <sys/errno.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/vnode_if.h>
#include <sys/kernel_types.h>
#include <sys/stat.h>
#include <sys/dirent.h>
#include <sys/uio.h>
#include <sys/proc.h>
#include <sys/vm.h>
#include <sys/fcntl.h>
#include <sys/select.h>
#include <kern/assert.h>
#include <libkern/libkern.h>
#include <libkern/OSMalloc.h>
#include <libkern/locks.h>

#include "fuse.h"
#include "fuse_kernel.h"
#include "fuse_device.h"
#include "fuse_kludges.h"
#include "fuse_locking.h"

struct fuse_iov {
    void   *base;
    size_t  len;
    size_t  allocated_size;
    ssize_t credit;
};

void fiov_init(struct fuse_iov *fiov, size_t size);
void fiov_teardown(struct fuse_iov *fiov);
void fiov_refresh(struct fuse_iov *fiov);
void fiov_adjust(struct fuse_iov *fiov, size_t size);
int  fiov_adjust_canfail(struct fuse_iov *fiov, size_t size);

#define FUSE_DIMALLOC(fiov, spc1, spc2, amnt)          \
do {                                                   \
    fiov_adjust(fiov, (sizeof(*(spc1)) + (amnt)));     \
    (spc1) = (fiov)->base;                             \
    (spc2) = (char *)(fiov)->base + (sizeof(*(spc1))); \
} while (0)

#define FU_AT_LEAST(siz) max((size_t)(siz), (size_t)160)

struct fuse_ticket;
struct fuse_data;

typedef int fuse_handler_t(struct fuse_ticket *ftick, uio_t uio);

struct fuse_ticket {
    uint64_t                     unique;
    struct fuse_data            *data;
    int                          flag;

    STAILQ_ENTRY(fuse_ticket)    freetickets_link;
    TAILQ_ENTRY(fuse_ticket)     alltickets_link;

    struct fuse_iov              ms_fiov;
    void                        *ms_bufdata;
    size_t                       ms_bufsize;
    enum { FT_M_FIOV, FT_M_BUF } ms_type;
    STAILQ_ENTRY(fuse_ticket)    ms_link;

    struct fuse_iov              aw_fiov;
    void                        *aw_bufdata;
    size_t                       aw_bufsize;
    enum { FT_A_FIOV, FT_A_BUF } aw_type;

    struct fuse_out_header       aw_ohead;
    int                          aw_errno;
    lck_mtx_t                   *aw_mtx;
    fuse_handler_t              *aw_handler;
    TAILQ_ENTRY(fuse_ticket)     aw_link;
};

#define FT_ANSW  0x01  // request of ticket has already been answered
#define FT_INVAL 0x02  // ticket is invalidated
#define FT_DIRTY 0x04  // ticket has been used
#define FT_KILLL 0x08  // ticket has been marked for death (KILLL => KILL_LATER)

static __inline__
struct fuse_iov *
fticket_resp(struct fuse_ticket *ftick)
{
    return &ftick->aw_fiov;
}

static __inline__
int
fticket_answered(struct fuse_ticket *ftick)
{
    return (ftick->flag & FT_ANSW);
}

static __inline__
void
fticket_set_answered(struct fuse_ticket *ftick)
{
    ftick->flag |= FT_ANSW;
}

static __inline__
void
fticket_set_killl(struct fuse_ticket *ftick)
{
    ftick->flag |= FT_KILLL;
}

static __inline__
enum fuse_opcode
fticket_opcode(struct fuse_ticket *ftick)
{
    return (((struct fuse_in_header *)(ftick->ms_fiov.base))->opcode);
}

static __inline__
void
fticket_invalidate(struct fuse_ticket *ftick)
{
    ftick->flag |= FT_INVAL;
}

int fticket_pull(struct fuse_ticket *ftick, uio_t uio);

struct fuse_data {
    fuse_device_t              fdev;
    mount_t                    mp;
    vnode_t                    rootvp;
    kauth_cred_t               daemoncred;
    pid_t                      daemonpid;
    uint32_t                   dataflags;     /* effective fuse_data flags */
    uint64_t                   noimplflags;   /* not-implemented flags     */

    bool                       opened: 1;
    bool                       mounted: 1;
    bool                       inited: 1;
    bool                       dead: 1;

#if M_FUSE4X_ENABLE_DSELECT
    struct fuse_selinfo        d_rsel;
#endif /* M_FUSE4X_ENABLE_DSELECT */

    lck_mtx_t                 *ms_mtx;
    STAILQ_HEAD(, fuse_ticket) ms_head;

    lck_mtx_t                 *aw_mtx;
    TAILQ_HEAD(, fuse_ticket)  aw_head;

    lck_mtx_t                 *ticket_mtx;
    STAILQ_HEAD(, fuse_ticket) freetickets_head;
    TAILQ_HEAD(, fuse_ticket)  alltickets_head;
    uint32_t                   freeticket_counter;
    uint32_t                   deadticket_counter;
    uint64_t                   ticketer;

#if M_FUSE4X_EXPLICIT_RENAME_LOCK
    lck_rw_t                  *rename_lock;
#endif /* M_FUSE4X_EXPLICIT_RENAME_LOCK */

    uint32_t                   max_write;
    uint32_t                   max_read;
    uint32_t                   blocksize;
    uint32_t                   iosize;
    uint32_t                   userkernel_bufsize;
    uint32_t                   fssubtype;
    char                       volname[MAXPATHLEN];

    struct timespec            daemon_timeout;
    struct timespec           *daemon_timeout_p;
#if M_FUSE4X_ENABLE_BIGLOCK
    lck_mtx_t                 *biglock;
#endif
};

/* Not-Implemented Bits */
#define FSESS_NOIMPLBIT(MSG)      (1ULL << FUSE_##MSG)

enum {
    // These values repeat mount flags a lot
    // consider using mountflags + initflags
    FSESS_ALLOW_OTHER         = 1 << 0,
    FSESS_ALLOW_ROOT          = 1 << 1,
    FSESS_AUTO_XATTR          = 1 << 2,
    FSESS_DEFAULT_PERMISSIONS = 1 << 3,
    FSESS_DEFER_PERMISSIONS   = 1 << 4,
    FSESS_DIRECT_IO           = 1 << 5,
    FSESS_EXTENDED_SECURITY   = 1 << 6,
    FSESS_JAIL_SYMLINKS       = 1 << 7,
    FSESS_NEGATIVE_VNCACHE    = 1 << 8,
    FSESS_NO_APPLEDOUBLE      = 1 << 9,
    FSESS_NO_APPLEXATTR       = 1 << 10,
    FSESS_NO_ATTRCACHE        = 1 << 11,
    FSESS_NO_READAHEAD        = 1 << 12,
    FSESS_NO_SYNCONCLOSE      = 1 << 13,
    FSESS_NO_SYNCWRITES       = 1 << 14,
    FSESS_NO_UBC              = 1 << 15,
    FSESS_NO_VNCACHE          = 1 << 16,
    FSESS_CASE_INSENSITIVE    = 1 << 17,
    FSESS_VOL_RENAME          = 1 << 18,
    FSESS_XTIMES              = 1 << 19,
    FSESS_AUTO_CACHE          = 1 << 20,
    FSESS_NATIVE_XATTR        = 1 << 21,
    FSESS_SPARSE              = 1 << 22
};

static __inline__
struct fuse_data *
fuse_get_mpdata(mount_t mp)
{
    /*
     * data->mounted should be set for it to be valid
     */
    return (struct fuse_data *)vfs_fsprivate(mp);
}

static __inline__
void
fuse_ms_push(struct fuse_ticket *ftick)
{
    STAILQ_INSERT_TAIL(&ftick->data->ms_head, ftick, ms_link);
}

static __inline__
void
fuse_ms_push_head(struct fuse_ticket *ftick)
{
    STAILQ_INSERT_HEAD(&ftick->data->ms_head, ftick, ms_link);
}

static __inline__
struct fuse_ticket *
fuse_ms_pop(struct fuse_data *data)
{
    struct fuse_ticket *ftick = NULL;

    if ((ftick = STAILQ_FIRST(&data->ms_head))) {
        STAILQ_REMOVE_HEAD(&data->ms_head, ms_link);
    }

    return ftick;
}

static __inline__
void
fuse_aw_push(struct fuse_ticket *ftick)
{
    TAILQ_INSERT_TAIL(&ftick->data->aw_head, ftick, aw_link);
}

static __inline__
void
fuse_aw_remove(struct fuse_ticket *ftick)
{
    TAILQ_REMOVE(&ftick->data->aw_head, ftick, aw_link);
}

static __inline__
struct fuse_ticket *
fuse_aw_pop(struct fuse_data *data)
{
    struct fuse_ticket *ftick = NULL;

    if ((ftick = TAILQ_FIRST(&data->aw_head))) {
        fuse_aw_remove(ftick);
    }

    return ftick;
}

struct fuse_ticket *fuse_ticket_fetch(struct fuse_data *data);
void fuse_ticket_drop(struct fuse_ticket *ftick);
void fuse_ticket_drop_invalid(struct fuse_ticket *ftick);
void fuse_ticket_kill(struct fuse_ticket *ftick);
void fuse_insert_callback(struct fuse_ticket *ftick, fuse_handler_t *handler);
void fuse_insert_message(struct fuse_ticket *ftick);
void fuse_insert_message_head(struct fuse_ticket *ftick);

struct fuse_data *fdata_alloc(struct proc *p);
void fdata_destroy(struct fuse_data *data);
bool fdata_set_dead(struct fuse_data *data);

struct fuse_dispatcher {

    struct fuse_ticket    *tick;
    struct fuse_in_header *finh;

    void    *indata;
    size_t   iosize;
    uint64_t nodeid;
    int      answ_stat;
    void    *answ;
};

static __inline__
void
fdisp_init(struct fuse_dispatcher *fdisp, size_t iosize)
{
    fdisp->iosize = iosize;
    fdisp->tick = NULL;
}

void fdisp_make(struct fuse_dispatcher *fdip, enum fuse_opcode op,
                mount_t mp, uint64_t nid, vfs_context_t context);

int  fdisp_make_canfail(struct fuse_dispatcher *fdip, enum fuse_opcode op,
                        mount_t mp, uint64_t nid, vfs_context_t context);

void fdisp_make_vp(struct fuse_dispatcher *fdip, enum fuse_opcode op,
                   vnode_t vp, vfs_context_t context);

int  fdisp_make_vp_canfail(struct fuse_dispatcher *fdip, enum fuse_opcode op,
                           vnode_t vp, vfs_context_t context);

int  fdisp_wait_answ(struct fuse_dispatcher *fdip);

static __inline__
int
fdisp_simple_putget_vp(struct fuse_dispatcher *fdip, enum fuse_opcode op,
                       vnode_t vp, vfs_context_t context)
{
    fdisp_init(fdip, 0);
    fdisp_make_vp(fdip, op, vp, context);
    return fdisp_wait_answ(fdip);
}

#endif /* _FUSE_IPC_H_ */
