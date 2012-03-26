/*
 * Copyright (C) 2006-2008 Google. All Rights Reserved.
 * Copyright (C) 2010 Tuxera. All Rights Reserved.
 * Copyright (C) 2011 Anatol Pomozov. All Rights Reserved.
 */

#ifndef _FUSE_IPC_H_
#define _FUSE_IPC_H_

#include "fuse.h"
#include "fuse_kernel.h"
#include "fuse_device.h"
#include "fuse_kludges.h"
#include "compat/tree.h"

#include <kern/assert.h>
#include <libkern/libkern.h>
#include <libkern/OSMalloc.h>
#include <libkern/locks.h>
#include <mach/mach_types.h>
#include <sys/errno.h>
#include <sys/mount.h>
#include <sys/dirent.h>
#include <sys/kernel_types.h>
#include <sys/stat.h>
#include <sys/proc.h>
#include <sys/fcntl.h>
#include <sys/queue.h>
#include <sys/select.h>
#include <sys/uio.h>
#include <sys/vm.h>
#include <sys/vnode.h>
#include <sys/vnode_if.h>

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

typedef int fuse_callback_t(struct fuse_ticket *ticket, uio_t uio);

struct fuse_ticket {
    uint64_t                     unique;
    struct fuse_data            *data;

    bool                         answered: 1; // request of ticket has already been answered
    bool                         invalid: 1; // ticket is invalidated
    bool                         dirty: 1; // ticket has been used
    bool                         killed: 1; // ticket has been marked for death (KILLL => KILL_LATER)

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
    fuse_callback_t             *aw_callback;
    TAILQ_ENTRY(fuse_ticket)     aw_link;
};

static __inline__
enum fuse_opcode
fuse_ticket_opcode(struct fuse_ticket *ticket)
{
    return (((struct fuse_in_header *)(ticket->ms_fiov.base))->opcode);
}


int fuse_ticket_pull(struct fuse_ticket *ticket, uio_t uio);

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
    bool                       destroyed: 1;
    bool                       dead: 1;

    lck_mtx_t                 *ms_mtx;
    STAILQ_HEAD(, fuse_ticket) ms_head;

    lck_mtx_t                 *aw_mtx;
    TAILQ_HEAD(, fuse_ticket)  aw_head;

    lck_mtx_t                 *ticket_mtx;
    STAILQ_HEAD(, fuse_ticket) freetickets_head; // protected by ticket_mtx
    TAILQ_HEAD(, fuse_ticket)  alltickets_head; // protected by ticket_mtx
    uint32_t                   freeticket_counter; // protected by ticket_mtx
    uint32_t                   deadticket_counter; // protected by ticket_mtx
    uint64_t                   ticketer; // protected by ticket_mtx

    uint32_t                   max_write;
    uint32_t                   max_read;
    uint32_t                   blocksize;
    uint32_t                   iosize;
    uint32_t                   userkernel_bufsize;
    uint32_t                   fssubtype;
    char                       volname[MAXPATHLEN];

    struct timespec            daemon_timeout;
    struct timespec           *daemon_timeout_p;

    lck_mtx_t                                *node_mtx;
    RB_HEAD(fuse_data_nodes, fuse_vnode_data) nodes_head; // map ino->vnode_data
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

struct fuse_ticket *fuse_ticket_fetch(struct fuse_data *data);
void fuse_ticket_drop(struct fuse_ticket *ticket);
void fuse_ticket_drop_invalid(struct fuse_ticket *ticket);
void fuse_ticket_kill(struct fuse_ticket *ticket);
void fuse_insert_callback(struct fuse_ticket *ticket, fuse_callback_t *callback);
void fuse_insert_message(struct fuse_ticket *ticket);

struct fuse_data *fuse_data_alloc(struct proc *p);
void fuse_data_destroy(struct fuse_data *data);
bool fuse_data_kill(struct fuse_data *data);

struct fuse_dispatcher {

    struct fuse_ticket    *ticket;
    struct fuse_in_header *finh;

    void    *indata;
    size_t   iosize;
    uint64_t nodeid;
    int      answer_errno;
    void    *answer;
};

static __inline__
void
fuse_dispatcher_init(struct fuse_dispatcher *dispatcher, size_t iosize)
{
    dispatcher->iosize = iosize;
    dispatcher->ticket = NULL;
}

void fuse_dispatcher_make(struct fuse_dispatcher *dispatcher, enum fuse_opcode op,
                mount_t mp, uint64_t nid, vfs_context_t context);

int  fuse_dispatcher_make_canfail(struct fuse_dispatcher *dispatcher, enum fuse_opcode op,
                        mount_t mp, uint64_t nid, vfs_context_t context);

void fuse_dispatcher_make_vp(struct fuse_dispatcher *dispatcher, enum fuse_opcode op,
                   vnode_t vp, vfs_context_t context);

int  fuse_dispatcher_make_vp_canfail(struct fuse_dispatcher *dispatcher, enum fuse_opcode op,
                           vnode_t vp, vfs_context_t context);

int  fuse_dispatcher_wait_answer(struct fuse_dispatcher *dispatcher);

static __inline__
int
fuse_dispatcher_simple_putget_vp(struct fuse_dispatcher *dispatcher, enum fuse_opcode op,
                       vnode_t vp, vfs_context_t context)
{
    fuse_dispatcher_init(dispatcher, 0);
    fuse_dispatcher_make_vp(dispatcher, op, vp, context);
    return fuse_dispatcher_wait_answer(dispatcher);
}

#endif /* _FUSE_IPC_H_ */
