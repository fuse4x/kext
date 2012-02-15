/*
 * Copyright (C) 2006-2008 Google. All Rights Reserved.
 * Copyright (C) 2011 Anatol Pomozov. All Rights Reserved.
 */

#ifndef _FUSE4X_H_
#define _FUSE4X_H_

#include <fuse_param.h>
#include <fuse_version.h>

#include <IOKit/IOLib.h>
#include <kern/thread_call.h>
#include <libkern/OSMalloc.h>
#include <libkern/locks.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/event.h>
#include <sys/proc.h>

// #define FUSE_COUNT_MEMORY  1
// #define FUSE_DEBUG         1
// #define FUSE_TRACE         1
// #define FUSE_TRACE_LK      1
// #define FUSE_TRACE_MSLEEP  1
// #define FUSE_TRACE_OP      1
// #define FUSE_TRACE_VNCACHE 1

// #define M_FUSE4X_SERIALIZE_LOGGING 1

#define M_FUSE4X_ENABLE_INTERRUPT    1
#define M_FUSE4X_ENABLE_XATTR        1
#define M_FUSE4X_ENABLE_DSELECT      0
#define M_FUSE4X_ENABLE_EXCHANGE     1
#define M_FUSE4X_ENABLE_KQUEUE       0
#define M_FUSE4X_ENABLE_SIMPLE_LOCK  0
#define M_FUSE4X_ENABLE_TSLOCKING    !M_FUSE4X_ENABLE_SIMPLE_LOCK
#if __LP64__ && !M_FUSE4X_ENABLE_SIMPLE_LOCK
#define M_FUSE4X_ENABLE_BIGLOCK 1
#endif /* __LP64__ */

#if M_FUSE4X_ENABLE_BIGLOCK
#define FUSE_VNOP_EXPORT __private_extern__
#else
#define FUSE_VNOP_EXPORT static
#endif /* M_FUSE4X_ENABLE_BIGLOCK */


#if M_FUSE4X_SERIALIZE_LOGGING
extern lck_mtx_t *fuse_log_lock;

// In case if tracing (lock,sleep,operations,..) enabled it produces a lot of log output.
// Because these logs are written from multiple threads they interference with each other.
// To make log more readable we need to searialize the output. It is done in log() function
// in case if M_FUSE4X_SERIALIZE_LOGGING defined.
#define log(fmt, args...) \
    do { \
        lck_mtx_lock(fuse_log_lock); \
        IOLog(fmt, ##args); \
        lck_mtx_unlock(fuse_log_lock); \
    } while(0)

#else
#define log(fmt, args...) IOLog(fmt, ##args)
#endif /* M_FUSE4X_SERIALIZE_LOGGING */

#ifdef FUSE_TRACE
#define fuse_trace_printf(fmt, ...) log(fmt, ## __VA_ARGS__)
#define fuse_trace_printf_func()    log("%s by %d\n", __FUNCTION__, proc_selfpid())
#else
#define fuse_trace_printf(fmt, ...) {}
#define fuse_trace_printf_func()    {}
#endif

#ifdef FUSE_TRACE_OP
#define fuse_trace_printf_vfsop()     log("%s by %d\n", __FUNCTION__, proc_selfpid())
#define fuse_trace_printf_vnop_novp() log("%s by %d\n", __FUNCTION__, proc_selfpid())
#define fuse_trace_printf_vnop()      log("%s vp=%p by %d\n", __FUNCTION__, vp, proc_selfpid())
#else
#define fuse_trace_printf_vfsop()     {}
#define fuse_trace_printf_vnop()      {}
#define fuse_trace_printf_vnop_novp() {}
#endif

#ifdef FUSE_TRACE_MSLEEP

#define fuse_msleep(chan, mtx, pri, wmesg, ts)                                                        \
({                                                                                                    \
    log("0: msleep(%p, mtx=%p, mesg=%s): %s@%d by %d\n", (chan), (mtx), (wmesg), __FUNCTION__, __LINE__, proc_selfpid()); \
    int __FUNCTION__ ## ret = msleep((chan), (mtx), (pri), (wmesg), (ts));                            \
    log("1: msleep(%p, mtx=%p, mesg=%s)=%d: %s@%d by %d\n", (chan), (mtx), (wmesg), __FUNCTION__ ## ret, __FUNCTION__, __LINE__, proc_selfpid()); \
                                                                                                      \
    __FUNCTION__ ## ret;                                                                              \
})
#define fuse_wakeup(chan)                                                                \
{                                                                                        \
    log("0: wakeup(%p): %s@%d by %d\n", (chan), __FUNCTION__, __LINE__, proc_selfpid()); \
    wakeup((chan));                                                                      \
    log("1: wakeup(%p): %s@%d by %d\n", (chan), __FUNCTION__, __LINE__, proc_selfpid()); \
}
#define fuse_wakeup_one(chan)                                                                \
{                                                                                            \
    log("0: wakeup_one(%p): %s@%d by %d\n", (chan), __FUNCTION__, __LINE__, proc_selfpid()); \
    wakeup_one((chan));                                                                      \
    log("1: wakeup_one(%p): %s@%d by %d\n", (chan), __FUNCTION__, __LINE__, proc_selfpid()); \
}
#else
#define fuse_msleep(chan, mtx, pri, wmesg, ts) msleep((chan), (mtx), (pri), (wmesg), (ts))
#define fuse_wakeup(chan)                      wakeup((chan))
#define fuse_wakeup_one(chan)                  wakeup_one((chan))
#endif

#define fuse_round_page_32(x) \
    (((uint32_t)(x) + 0x1000 - 1) & ~(0x1000 - 1))

#define FUSE_ZERO_SIZE 0x0000000000000000ULL
#define FUSE_ROOT_SIZE 0xFFFFFFFFFFFFFFFFULL

extern OSMallocTag fuse_malloc_tag;

#ifdef FUSE_COUNT_MEMORY

extern int32_t fuse_memory_allocated;

static __inline__
void *
FUSE_OSMalloc(size_t size, OSMallocTag tag)
{
    void *addr = OSMalloc((uint32_t)size, tag);

    if (!addr) {
        panic("fuse4x: memory allocation failed (size=%lu)", size);
    }

    OSAddAtomic((UInt32)size, (SInt32 *)&fuse_memory_allocated);

    return addr;
}

static __inline__
void
FUSE_OSFree(void *addr, size_t size, OSMallocTag tag)
{
    OSFree(addr, (uint32_t)size, tag);

    OSAddAtomic(-(UInt32)(size), (SInt32 *)&fuse_memory_allocated);
}

#else

#define FUSE_OSMalloc(size, tag)           OSMalloc((uint32_t)(size), (tag))
#define FUSE_OSFree(addr, size, tag)       OSFree((addr), (uint32_t)(size), (tag))

#endif /* FUSE_COUNT_MEMORY */

#ifndef FUSE4X_KEXT_VERSION
#define FUSE4X_KEXT_VERSION FUSE4X_VERSION_LITERAL
#endif

#define FUSE4X_TIMESTAMP __DATE__ ", " __TIME__

#endif /* _FUSE4X_H_ */
