/*
 * Copyright (C) 2006-2008 Google. All Rights Reserved.
 * Copyright (C) 2010 Tuxera. All Rights Reserved.
 * Copyright (C) 2011 Anatol Pomozov. All Rights Reserved.
 */

#include "fuse.h"
#include "fuse_internal.h"
#include "fuse_ipc.h"
#include "fuse_locking.h"
#include "fuse_node.h"
#include "fuse_sysctl.h"

#include <sys/types.h>
#include <sys/malloc.h>
#include <sys/queue.h>

#ifdef FUSE4X_ENABLE_BIGLOCK
#include "fuse_biglock_vnops.h"
#endif

static struct fuse_ticket *fuse_ticket_alloc(struct fuse_data *data);
static void                fuse_ticket_refresh(struct fuse_ticket *ticket);
static void                fuse_ticket_destroy(struct fuse_ticket *ticket);
static int                 fuse_ticket_wait_answer(struct fuse_ticket *ticket);
static __inline__ int      fuse_ticket_aw_pull_uio(struct fuse_ticket *ticket,
                                               uio_t uio);
static __inline__ void     fuse_push_freeticks(struct fuse_ticket *ticket);

static __inline__ struct fuse_ticket *fuse_pop_freeticks(struct fuse_data *data);

static __inline__ void     fuse_push_allticks(struct fuse_ticket *ticket);
static __inline__ void     fuse_remove_allticks(struct fuse_ticket *ticket);
static struct fuse_ticket *fuse_pop_allticks(struct fuse_data *data);

static int             fuse_body_audit(struct fuse_ticket *ticket, size_t blen);
static __inline__ void fuse_setup_ihead(struct fuse_in_header *ihead,
                                        struct fuse_ticket    *ticket,
                                        uint64_t               nid,
                                        enum fuse_opcode       op,
                                        size_t                 blen,
                                        vfs_context_t          context);

static fuse_callback_t  fuse_standard_callback;


static __inline__
void *
FUSE_OSRealloc_nocopy(void *oldptr, size_t oldsize, size_t newsize)
{
    void *data;

    data = FUSE_OSMalloc(newsize, fuse_malloc_tag);
    if (!data) {
        panic("fuse4x: OSMalloc failed in realloc");
    }

    FUSE_OSFree(oldptr, oldsize, fuse_malloc_tag);
    OSIncrementAtomic((SInt32 *)&fuse_realloc_count);

    return data;
}

static __inline__
void *
FUSE_OSRealloc_nocopy_canfail(void *oldptr, size_t oldsize, size_t newsize)
{
    void *data;

    data = FUSE_OSMalloc(newsize, fuse_malloc_tag);
    if (!data) {
        goto out;
    } else {
        FUSE_OSFree(oldptr, oldsize, fuse_malloc_tag);
        OSIncrementAtomic((SInt32 *)&fuse_realloc_count);
    }

out:
    return data;
}

void
fiov_init(struct fuse_iov *fiov, size_t size)
{
    size_t msize = FU_AT_LEAST(size);

    fiov->len = 0;

    fiov->base = FUSE_OSMalloc(msize, fuse_malloc_tag);
    if (!fiov->base) {
        panic("fuse4x: OSMalloc failed in fiov_init");
    }

    OSIncrementAtomic((SInt32 *)&fuse_iov_current);

    bzero(fiov->base, msize);

    fiov->allocated_size = msize;
    fiov->credit = fuse_iov_credit;
}

void
fiov_teardown(struct fuse_iov *fiov)
{
    FUSE_OSFree(fiov->base, fiov->allocated_size, fuse_malloc_tag);
    fiov->allocated_size = 0;

    OSDecrementAtomic((SInt32 *)&fuse_iov_current);
}

void
fiov_adjust(struct fuse_iov *fiov, size_t size)
{
    if (fiov->allocated_size < size ||
        (fiov->allocated_size - size > fuse_iov_permanent_bufsize &&
             --fiov->credit < 0)) {

        fiov->base = FUSE_OSRealloc_nocopy(fiov->base, fiov->allocated_size,
                                           FU_AT_LEAST(size));
        if (!fiov->base) {
            panic("fuse4x: realloc failed");
        }

        fiov->allocated_size = FU_AT_LEAST(size);
        fiov->credit = fuse_iov_credit;
    }

    fiov->len = size;
}

int
fiov_adjust_canfail(struct fuse_iov *fiov, size_t size)
{
    if (fiov->allocated_size < size ||
        (fiov->allocated_size - size > fuse_iov_permanent_bufsize &&
             --fiov->credit < 0)) {

        void *tmpbase = NULL;

        tmpbase = FUSE_OSRealloc_nocopy_canfail(fiov->base,
                                                fiov->allocated_size,
                                                FU_AT_LEAST(size));
        if (!tmpbase) {
            return ENOMEM;
        }

        fiov->base = tmpbase;
        fiov->allocated_size = FU_AT_LEAST(size);
        fiov->credit = fuse_iov_credit;
    }

    fiov->len = size;

    return 0;
}

void
fiov_refresh(struct fuse_iov *fiov)
{
    bzero(fiov->base, fiov->len);
    fiov_adjust(fiov, 0);
}

static struct fuse_ticket *
fuse_ticket_alloc(struct fuse_data *data)
{
    struct fuse_ticket *ticket;

    ticket = (struct fuse_ticket *)FUSE_OSMalloc(sizeof(struct fuse_ticket),
                                                fuse_malloc_tag);
    if (!ticket) {
        panic("fuse4x: OSMalloc failed in " __FUNCTION__);
    }

    OSIncrementAtomic((SInt32 *)&fuse_tickets_current);

    bzero(ticket, sizeof(struct fuse_ticket));

    fuse_lck_mtx_lock(data->ticket_mtx);
    ticket->unique = data->ticketer++;
    fuse_lck_mtx_unlock(data->ticket_mtx);

    ticket->data = data;

    fiov_init(&ticket->ms_fiov, sizeof(struct fuse_in_header));
    ticket->ms_type = FT_M_FIOV;

    ticket->aw_mtx = lck_mtx_alloc_init(fuse_lock_group, fuse_lock_attr);
    fiov_init(&ticket->aw_fiov, 0);
    ticket->aw_type = FT_A_FIOV;

    return ticket;
}

static __inline__
void
fuse_ticket_refresh(struct fuse_ticket *ticket)
{
    fiov_refresh(&ticket->ms_fiov);
    ticket->ms_bufdata = NULL;
    ticket->ms_bufsize = 0;
    ticket->ms_type = FT_M_FIOV;

    bzero(&ticket->aw_ohead, sizeof(struct fuse_out_header));

    fiov_refresh(&ticket->aw_fiov);
    ticket->aw_errno = 0;
    ticket->aw_bufdata = NULL;
    ticket->aw_bufsize = 0;
    ticket->aw_type = FT_A_FIOV;

    ticket->answered = false;
    ticket->invalid = false;
    ticket->dirty = false;
    ticket->killed = false;
}

static void
fuse_ticket_destroy(struct fuse_ticket *ticket)
{
    fiov_teardown(&ticket->ms_fiov);

    lck_mtx_free(ticket->aw_mtx, fuse_lock_group);
    ticket->aw_mtx = NULL;
    fiov_teardown(&ticket->aw_fiov);

    FUSE_OSFree(ticket, sizeof(struct fuse_ticket), fuse_malloc_tag);

    OSDecrementAtomic((SInt32 *)&fuse_tickets_current);
}

static int
fuse_ticket_wait_answer(struct fuse_ticket *ticket)
{
    int err = 0;
    struct fuse_data *data = ticket->data;

    fuse_lck_mtx_lock(ticket->aw_mtx);

    if (ticket->answered) {
        goto out;
    }

    if (data->dead || data->destroyed) {
        err = ENOTCONN;
        ticket->answered = true;
        goto out;
    }
    if (fuse_ticket_opcode(ticket) == FUSE_DESTROY)
        data->destroyed = true;

#ifdef FUSE4X_ENABLE_BIGLOCK
    // release biglock before going to sleep:
    // 1) it reduces biglock contention - we really have no reason to keep the lock and prevent other requests from
    //    processing, the biglock protects vnode operations only.
    // 2) in case if a fuse daemon performs some non-fuse filesystem operations it may lead to fsync on the *fuse*
    //    filesystem. And this leads to deadlock. See https://trac.macports.org/ticket/30129 (the second, deadlock issue).
    fuse_biglock_unlock(data->biglock);
#endif

    err = fuse_msleep(ticket, ticket->aw_mtx, PCATCH, "fu_ans", data->daemon_timeout_p);

#ifdef FUSE4X_ENABLE_BIGLOCK
    fuse_biglock_lock(data->biglock);
#endif

    if (err == EAGAIN) { /* same as EWOULDBLOCK */
        if (fuse_data_kill(data)) {
            struct vfsstatfs *statfs = vfs_statfs(data->mp);
            log("fuse4x: daemon (pid=%d, mountpoint=%s) did not respond in %ld seconds. Mark the filesystem as dead.\n",
                    data->daemonpid, statfs->f_mntonname, data->daemon_timeout.tv_sec);
        }

        err = ENOTCONN;
        ticket->answered = true;

        goto out;
    }

#ifdef FUSE4X_ENABLE_INTERRUPT
    else if (err == EINTR) {
       /*
        * XXX: Stop gap! I really need to finish interruption plumbing.
        */
       fuse_internal_interrupt_send(ticket);
    }
#endif

out:
    fuse_lck_mtx_unlock(ticket->aw_mtx);

    if (!(err || ticket->answered)) {
        log("fuse4x: requester was woken up but still no answer");
        err = ENXIO;
    }

    return err;
}

static __inline__
int
fuse_ticket_aw_pull_uio(struct fuse_ticket *ticket, uio_t uio)
{
    int err = 0;
    size_t len = (size_t)uio_resid(uio);

    if (len) {
        switch (ticket->aw_type) {
        case FT_A_FIOV:
            err = fiov_adjust_canfail(&ticket->aw_fiov, len);
            if (err) {
                ticket->killed = true;
                log("fuse4x: failed to pull uio (error=%d)\n", err);
                break;
            }
            err = uiomove(ticket->aw_fiov.base, (int)len, uio);
            if (err) {
                log("fuse4x: FT_A_FIOV error is %d (%p, %ld, %p)\n",
                      err, ticket->aw_fiov.base, len, uio);
            }
            break;

        case FT_A_BUF:
            ticket->aw_bufsize = len;
            err = uiomove(ticket->aw_bufdata, (int)len, uio);
            if (err) {
                log("fuse4x: FT_A_BUF error is %d (%p, %ld, %p)\n",
                      err, ticket->aw_bufdata, len, uio);
            }
            break;

        default:
            panic("fuse4x: unknown answer type for ticket %p", ticket);
        }
    }

    return err;
}

int
fuse_ticket_pull(struct fuse_ticket *ticket, uio_t uio)
{
    int err = 0;

    if (ticket->aw_ohead.error) {
        return 0;
    }

    err = fuse_body_audit(ticket, (size_t)uio_resid(uio));
    if (!err) {
        err = fuse_ticket_aw_pull_uio(ticket, uio);
    }

    return err;
}

struct fuse_data *
fuse_data_alloc(struct proc *p)
{
    struct fuse_data *data;

    data = (struct fuse_data *)FUSE_OSMalloc(sizeof(struct fuse_data),
                                             fuse_malloc_tag);
    if (!data) {
        panic("fuse4x: OSMalloc failed in " __FUNCTION__);
    }

    bzero(data, sizeof(struct fuse_data));

    data->mp            = NULL;
    data->rootvp        = NULLVP;
    data->daemoncred    = kauth_cred_proc_ref(p);
    data->daemonpid     = proc_pid(p);
    data->dataflags     = 0;
    data->noimplflags   = 0ULL;

    data->opened        = false;
    data->mounted       = false;
    data->inited        = false;
    data->destroyed     = false;
    data->dead          = false;

    data->ms_mtx        = lck_mtx_alloc_init(fuse_lock_group, fuse_lock_attr);
    data->aw_mtx        = lck_mtx_alloc_init(fuse_lock_group, fuse_lock_attr);
    data->ticket_mtx    = lck_mtx_alloc_init(fuse_lock_group, fuse_lock_attr);

    STAILQ_INIT(&data->ms_head);
    TAILQ_INIT(&data->aw_head);
    STAILQ_INIT(&data->freetickets_head);
    TAILQ_INIT(&data->alltickets_head);

    data->freeticket_counter = 0;
    data->deadticket_counter = 0;
    data->ticketer           = 0;

#ifdef FUSE4X_ENABLE_BIGLOCK
    data->biglock        = lck_mtx_alloc_init(fuse_lock_group, fuse_lock_attr);
#endif

    return data;
}

void
fuse_data_destroy(struct fuse_data *data)
{
    struct fuse_ticket *ticket;

    lck_mtx_free(data->ms_mtx, fuse_lock_group);
    data->ms_mtx = NULL;

    lck_mtx_free(data->aw_mtx, fuse_lock_group);
    data->aw_mtx = NULL;

    lck_mtx_free(data->ticket_mtx, fuse_lock_group);
    data->ticket_mtx = NULL;

#ifdef FUSE4X_ENABLE_BIGLOCK
    lck_mtx_free(data->biglock, fuse_lock_group);
    data->biglock = NULL;
#endif

    while ((ticket = fuse_pop_allticks(data))) {
        fuse_ticket_destroy(ticket);
    }

    kauth_cred_unref(&(data->daemoncred));

    FUSE_OSFree(data, sizeof(struct fuse_data), fuse_malloc_tag);
}


bool
fuse_data_kill(struct fuse_data *data)
{
    fuse_trace_printf_func();

    fuse_lck_mtx_lock(data->ms_mtx);
    if (data->dead) {
        fuse_lck_mtx_unlock(data->ms_mtx);
        return false;
    }

    data->dead = true;
    fuse_wakeup_one((caddr_t)data);
#ifdef FUSE4X_ENABLE_DSELECT
    selwakeup((struct selinfo*)&data->d_rsel);
#endif /* FUSE4X_ENABLE_DSELECT */
    fuse_lck_mtx_unlock(data->ms_mtx);

    fuse_lck_mtx_lock(data->ticket_mtx);
    fuse_wakeup(&data->ticketer);
    fuse_lck_mtx_unlock(data->ticket_mtx);

    return true;
}

static __inline__
void
fuse_push_freeticks(struct fuse_ticket *ticket)
{
    STAILQ_INSERT_TAIL(&ticket->data->freetickets_head, ticket,
                       freetickets_link);
    ticket->data->freeticket_counter++;
}

static __inline__
struct fuse_ticket *
fuse_pop_freeticks(struct fuse_data *data)
{
    struct fuse_ticket *ticket;

    if ((ticket = STAILQ_FIRST(&data->freetickets_head))) {
        STAILQ_REMOVE_HEAD(&data->freetickets_head, freetickets_link);
        data->freeticket_counter--;
    }

    if (STAILQ_EMPTY(&data->freetickets_head) &&
        (data->freeticket_counter != 0)) {
        panic("fuse4x: ticket count mismatch!");
    }

    return ticket;
}

static __inline__
void
fuse_push_allticks(struct fuse_ticket *ticket)
{
    TAILQ_INSERT_TAIL(&ticket->data->alltickets_head, ticket,
                      alltickets_link);
}

static __inline__
void
fuse_remove_allticks(struct fuse_ticket *ticket)
{
    ticket->data->deadticket_counter++;
    TAILQ_REMOVE(&ticket->data->alltickets_head, ticket, alltickets_link);
}

static struct fuse_ticket *
fuse_pop_allticks(struct fuse_data *data)
{
    struct fuse_ticket *ticket;

    if ((ticket = TAILQ_FIRST(&data->alltickets_head))) {
        fuse_remove_allticks(ticket);
    }

    return ticket;
}

struct fuse_ticket *
fuse_ticket_fetch(struct fuse_data *data)
{
    int err = 0;
    struct fuse_ticket *ticket;

    fuse_lck_mtx_lock(data->ticket_mtx);

    if (data->freeticket_counter == 0) {
        fuse_lck_mtx_unlock(data->ticket_mtx);
        ticket = fuse_ticket_alloc(data);
        if (!ticket) {
            panic("fuse4x: ticket allocation failed");
        }
        fuse_lck_mtx_lock(data->ticket_mtx);
        fuse_push_allticks(ticket);
    } else {
        /* locked here */
        ticket = fuse_pop_freeticks(data);
        if (!ticket) {
            panic("fuse4x: no free ticket despite the counter's value");
        }
    }

    if (!data->inited && data->ticketer > 1) {
        err = fuse_msleep(&data->ticketer, data->ticket_mtx, PCATCH | PDROP,
                          "fu_ini", 0);
    } else {
        if ((fuse_max_tickets != 0) &&
            ((data->ticketer - data->deadticket_counter) > fuse_max_tickets)) {
            err = 1;
        }
        fuse_lck_mtx_unlock(data->ticket_mtx);
    }

    if (err) {
        fuse_data_kill(data);
    }

    return ticket;
}

void
fuse_ticket_drop(struct fuse_ticket *ticket)
{
    struct fuse_data *data = ticket->data;

    fuse_lck_mtx_lock(data->ticket_mtx);

    if ((fuse_max_freetickets <= data->freeticket_counter) ||
        ticket->killed) {
        fuse_remove_allticks(ticket);
        fuse_lck_mtx_unlock(data->ticket_mtx);
        fuse_ticket_destroy(ticket);
    } else {
        fuse_lck_mtx_unlock(data->ticket_mtx);
        fuse_ticket_refresh(ticket);
        fuse_lck_mtx_lock(data->ticket_mtx);
        fuse_push_freeticks(ticket);
        fuse_lck_mtx_unlock(data->ticket_mtx);
    }
}

void
fuse_ticket_kill(struct fuse_ticket *ticket)
{
    struct fuse_data *data = ticket->data;
    fuse_lck_mtx_lock(data->ticket_mtx);
    fuse_remove_allticks(ticket);
    fuse_lck_mtx_unlock(data->ticket_mtx);
    fuse_ticket_destroy(ticket);
}

void
fuse_ticket_drop_invalid(struct fuse_ticket *ticket)
{
    if (ticket->invalid) {
        fuse_ticket_drop(ticket);
    }
}

void
fuse_insert_callback(struct fuse_ticket *ticket, fuse_callback_t *callback)
{
    struct fuse_data *data = ticket->data;

    if (data->dead || data->destroyed) {
        return;
    }

    ticket->aw_callback = callback;

    fuse_lck_mtx_lock(data->aw_mtx);
    TAILQ_INSERT_TAIL(&data->aw_head, ticket, aw_link);
    fuse_lck_mtx_unlock(data->aw_mtx);
}

void
fuse_insert_message(struct fuse_ticket *ticket)
{
    struct fuse_data *data = ticket->data;

    if (ticket->dirty) {
        panic("fuse4x: ticket reused without being refreshed");
    }

    ticket->dirty = true;

    if (data->dead || data->destroyed) {
        return;
    }

    fuse_lck_mtx_lock(data->ms_mtx);
    STAILQ_INSERT_TAIL(&data->ms_head, ticket, ms_link);
    fuse_wakeup_one((caddr_t)data);
#ifdef FUSE4X_ENABLE_DSELECT
    selwakeup((struct selinfo*)&data->d_rsel);
#endif /* FUSE4X_ENABLE_DSELECT */
    fuse_lck_mtx_unlock(data->ms_mtx);
}

static int
fuse_body_audit(struct fuse_ticket *ticket, size_t blen)
{
    int err = 0;
    enum fuse_opcode opcode;

    if (ticket->data->dead) {
        return ENOTCONN;
    }

    opcode = fuse_ticket_opcode(ticket);

    switch (opcode) {
    case FUSE_LOOKUP:
        err = (blen == sizeof(struct fuse_entry_out)) ? 0 : EINVAL;
        break;

    case FUSE_FORGET:
        panic("fuse4x: a callback has been intalled for FUSE_FORGET");
        break;

    case FUSE_GETATTR:
        err = (blen == sizeof(struct fuse_attr_out)) ? 0 : EINVAL;
        break;

    case FUSE_SETATTR:
        err = (blen == sizeof(struct fuse_attr_out)) ? 0 : EINVAL;
        break;

    case FUSE_GETXTIMES:
        err = (blen == sizeof(struct fuse_getxtimes_out)) ? 0 : EINVAL;
        break;

    case FUSE_READLINK:
        err = (PAGE_SIZE >= blen) ? 0 : EINVAL;
        break;

    case FUSE_SYMLINK:
        err = (blen == sizeof(struct fuse_entry_out)) ? 0 : EINVAL;
        break;

    case FUSE_MKNOD:
        err = (blen == sizeof(struct fuse_entry_out)) ? 0 : EINVAL;
        break;

    case FUSE_MKDIR:
        err = (blen == sizeof(struct fuse_entry_out)) ? 0 : EINVAL;
        break;

    case FUSE_UNLINK:
        err = (blen == 0) ? 0 : EINVAL;
        break;

    case FUSE_RMDIR:
        err = (blen == 0) ? 0 : EINVAL;
        break;

    case FUSE_RENAME:
        err = (blen == 0) ? 0 : EINVAL;
        break;

    case FUSE_LINK:
        err = (blen == sizeof(struct fuse_entry_out)) ? 0 : EINVAL;
        break;

    case FUSE_OPEN:
        err = (blen == sizeof(struct fuse_open_out)) ? 0 : EINVAL;
        break;

    case FUSE_READ:
        err = (((struct fuse_read_in *)(
                (char *)ticket->ms_fiov.base +
                        sizeof(struct fuse_in_header)
                  ))->size >= blen) ? 0 : EINVAL;
        break;

    case FUSE_WRITE:
        err = (blen == sizeof(struct fuse_write_out)) ? 0 : EINVAL;
        break;

    case FUSE_STATFS:
        err = (blen == sizeof(struct fuse_statfs_out)) ? 0 : EINVAL;
        break;

    case FUSE_RELEASE:
        err = (blen == 0) ? 0 : EINVAL;
        break;

    case FUSE_FSYNC:
        err = (blen == 0) ? 0 : EINVAL;
        break;

    case FUSE_SETXATTR:
        /* TBD */
        break;

    case FUSE_GETXATTR:
        /* TBD */
        break;

    case FUSE_LISTXATTR:
        /* TBD */
        break;

    case FUSE_REMOVEXATTR:
        /* TBD */
        break;

    case FUSE_FLUSH:
        err = (blen == 0) ? 0 : EINVAL;
        break;

    case FUSE_INIT:
        if (blen == sizeof(struct fuse_init_out) || blen == 8) {
            err = 0;
        } else {
            err = EINVAL;
        }
        break;

    case FUSE_OPENDIR:
        err = (blen == sizeof(struct fuse_open_out)) ? 0 : EINVAL;
        break;

    case FUSE_READDIR:
        err = (((struct fuse_read_in *)(
                (char *)ticket->ms_fiov.base +
                        sizeof(struct fuse_in_header)
                  ))->size >= blen) ? 0 : EINVAL;
        break;

    case FUSE_RELEASEDIR:
        err = (blen == 0) ? 0 : EINVAL;
        break;

    case FUSE_FSYNCDIR:
        err = (blen == 0) ? 0 : EINVAL;
        break;

    case FUSE_GETLK:
        panic("fuse4x: no response body format check for FUSE_GETLK");
        break;

    case FUSE_SETLK:
        panic("fuse4x: no response body format check for FUSE_SETLK");
        break;

    case FUSE_SETLKW:
        panic("fuse4x: no response body format check for FUSE_SETLKW");
        break;

    case FUSE_ACCESS:
        err = (blen == 0) ? 0 : EINVAL;
        break;

    case FUSE_CREATE:
        err = (blen == sizeof(struct fuse_entry_out) +
                           sizeof(struct fuse_open_out)) ? 0 : EINVAL;
        break;

    case FUSE_INTERRUPT:
        /* TBD */
        break;

    case FUSE_BMAP:
        /* TBD */
        break;

    case FUSE_IOCTL:
        /* TBD */
        break;

    case FUSE_DESTROY:
        err = (blen == 0) ? 0 : EINVAL;
        break;

    case FUSE_EXCHANGE:
        err = (blen == 0) ? 0 : EINVAL;
        break;

    case FUSE_SETVOLNAME:
        err = (blen == 0) ? 0 : EINVAL;
        break;

    default:
        log("fuse4x: opcodes out of sync (%d)\n", opcode);
        panic("fuse4x: opcodes out of sync (%d)", opcode);
    }

    return err;
}

static void
fuse_setup_ihead(struct fuse_in_header *ihead,
                 struct fuse_ticket    *ticket,
                 uint64_t               nid,
                 enum fuse_opcode       op,
                 size_t                 blen,
                 vfs_context_t          context)
{
    ihead->len = (uint32_t)(sizeof(*ihead) + blen);
    ihead->unique = ticket->unique;
    ihead->nodeid = nid;
    ihead->opcode = op;

    if (context) {
        ihead->pid = vfs_context_pid(context);
        ihead->uid = kauth_cred_getuid(vfs_context_ucred(context));
        ihead->gid = kauth_cred_getgid(vfs_context_ucred(context));
    } else {
        /* XXX: could use more thought */
        ihead->pid = proc_selfpid();
        ihead->uid = kauth_getuid();
        ihead->gid = kauth_getgid();
    }
}

static int
fuse_standard_callback(struct fuse_ticket *ticket, uio_t uio)
{
    int err = 0;
    bool dropflag = false;

    err = fuse_ticket_pull(ticket, uio);

    fuse_lck_mtx_lock(ticket->aw_mtx);

    if (ticket->answered) {
        dropflag = true;
    } else {
        ticket->answered = true;
        ticket->aw_errno = err;
        fuse_wakeup(ticket);
    }

    fuse_lck_mtx_unlock(ticket->aw_mtx);

    if (dropflag) {
        fuse_ticket_drop(ticket);
    }

    return err;
}

void
fuse_dispatcher_make(struct fuse_dispatcher *dispatcher,
           enum fuse_opcode        op,
           mount_t                 mp,
           uint64_t                nid,
           vfs_context_t           context)
{
    struct fuse_data *data = fuse_get_mpdata(mp);

    if (dispatcher->ticket) {
        fuse_ticket_refresh(dispatcher->ticket);
    } else {
        dispatcher->ticket = fuse_ticket_fetch(data);
    }

    if (!dispatcher->ticket) {
        panic("fuse4x: fuse_ticket_fetch() failed");
    }

    FUSE_DIMALLOC(&dispatcher->ticket->ms_fiov, dispatcher->finh,
                  dispatcher->indata, dispatcher->iosize);

    fuse_setup_ihead(dispatcher->finh, dispatcher->ticket, nid, op, dispatcher->iosize, context);
}

int
fuse_dispatcher_make_canfail(struct fuse_dispatcher *dispatcher,
                   enum fuse_opcode        op,
                   mount_t                 mp,
                   uint64_t                nid,
                   vfs_context_t           context)
{
    int failed = 0;
    struct fuse_iov *fiov = NULL;

    struct fuse_data *data = fuse_get_mpdata(mp);

    if (dispatcher->ticket) {
        fuse_ticket_refresh(dispatcher->ticket);
    } else {
        dispatcher->ticket = fuse_ticket_fetch(data);
    }

    if (dispatcher->ticket == 0) {
        panic("fuse4x: fuse_ticket_fetch() failed");
    }

    fiov = &dispatcher->ticket->ms_fiov;

    failed = fiov_adjust_canfail(fiov,
                                 sizeof(struct fuse_in_header) + dispatcher->iosize);

    if (failed) {
        fuse_ticket_kill(dispatcher->ticket);
        return failed;
    }

    dispatcher->finh = fiov->base;
    dispatcher->indata = (char *)(fiov->base) + sizeof(struct fuse_in_header);

    fuse_setup_ihead(dispatcher->finh, dispatcher->ticket, nid, op, dispatcher->iosize, context);

    return 0;
}

void
fuse_dispatcher_make_vp(struct fuse_dispatcher *dispatcher,
              enum fuse_opcode        op,
              vnode_t                 vp,
              vfs_context_t           context)
{
    return fuse_dispatcher_make(dispatcher, op, vnode_mount(vp), VTOI(vp), context);
}

int
fuse_dispatcher_make_vp_canfail(struct fuse_dispatcher *dispatcher,
                      enum fuse_opcode        op,
                      vnode_t                 vp,
                      vfs_context_t           context)
{
    return fuse_dispatcher_make_canfail(dispatcher, op, vnode_mount(vp), VTOI(vp), context);
}

/* The function returns 0 in case of success and errorcode in case of error */
int
fuse_dispatcher_wait_answer(struct fuse_dispatcher *dispatcher)
{
    int err = 0;
    struct fuse_ticket *ticket = dispatcher->ticket;

    dispatcher->answer_errno = 0;
    fuse_insert_callback(ticket, fuse_standard_callback);
    fuse_insert_message(ticket);

    if ((err = fuse_ticket_wait_answer(ticket))) { /* interrupted */
        fuse_lck_mtx_lock(ticket->aw_mtx);

        if (ticket->answered) {
            /* IPC: already answered */
            fuse_lck_mtx_unlock(ticket->aw_mtx);
            goto out;
        } else {
            /* IPC: explicitly setting to answered */
            ticket->answered = true;
            fuse_lck_mtx_unlock(ticket->aw_mtx);
            return err;
        }
    }

    /* IPC was NOT interrupt */

    if (ticket->aw_errno) {

        /* Explicitly EIO-ing */

        err = EIO;
        goto out;
    }

    if ((err = ticket->aw_ohead.error)) {

        /* Explicitly setting status */

        dispatcher->answer_errno = err;
        goto out;
    }

    dispatcher->answer = ticket->aw_fiov.base;
    dispatcher->iosize = ticket->aw_fiov.len;

    return 0;

out:
    fuse_ticket_drop(ticket);

    return err;
}
