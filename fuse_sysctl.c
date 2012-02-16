/*
 * Copyright (C) 2006-2008 Google. All Rights Reserved.
 * Copyright (C) 2011 Anatol Pomozov. All Rights Reserved.
 */

#include "fuse.h"
#include "fuse_kernel.h"
#include "fuse_device.h"
#include "fuse_sysctl.h"
#include <fuse_param.h>
#include <fuse_version.h>

#include <sys/systm.h>
#include <sys/types.h>
#include <sys/sysctl.h>

/* NB: none of these are bigger than unsigned 32-bit. */

int32_t  fuse_admin_group            = 0;                                  // rw
int32_t  fuse_allow_other            = 0;                                  // rw
uint32_t fuse_api_major              = FUSE_KERNEL_VERSION;                // r
uint32_t fuse_api_minor              = FUSE_KERNEL_MINOR_VERSION;          // r
int32_t  fuse_fh_current             = 0;                                  // r
uint32_t fuse_fh_reuse_count         = 0;                                  // r
uint32_t fuse_fh_upcall_count        = 0;                                  // r
uint32_t fuse_fh_zombies             = 0;                                  // r
int32_t  fuse_iov_credit             = FUSE_DEFAULT_IOV_CREDIT;            // rw
int32_t  fuse_iov_current            = 0;                                  // r
uint32_t fuse_iov_permanent_bufsize  = FUSE_DEFAULT_IOV_PERMANENT_BUFSIZE; // rw
int32_t  fuse_kill                   = -1;                                 // w
int32_t  fuse_print_vnodes           = -1;                                 // w
uint32_t fuse_lookup_cache_hits      = 0;                                  // r
uint32_t fuse_lookup_cache_misses    = 0;                                  // r
uint32_t fuse_lookup_cache_overrides = 0;                                  // r
uint32_t fuse_max_freetickets        = FUSE_DEFAULT_MAX_FREE_TICKETS;      // rw
uint32_t fuse_max_tickets            = 0;                                  // rw
int32_t  fuse_mount_count            = 0;                                  // r
int32_t  fuse_realloc_count          = 0;                                  // r
int32_t  fuse_tickets_current        = 0;                                  // r
uint32_t fuse_userkernel_bufsize     = FUSE_DEFAULT_USERKERNEL_BUFSIZE;    // rw
int32_t  fuse_vnodes_current         = 0;                                  // r
#ifndef FUSE4X_DISABLE_MACFUSE_MODE
int32_t  fuse_macfuse_mode           = 0;                                  // w
#endif
#ifdef FUSE4X_COUNT_MEMORY
int32_t  fuse_memory_allocated       = 0;                                  // r
#endif

SYSCTL_DECL(_vfs_generic);
SYSCTL_NODE(_vfs_generic, OID_AUTO, fuse4x, CTLFLAG_RW, 0,
            "fuse4x Sysctl Interface");
SYSCTL_NODE(_vfs_generic_fuse4x, OID_AUTO, control, CTLFLAG_RW, 0,
            "fuse4x Controls");
SYSCTL_NODE(_vfs_generic_fuse4x, OID_AUTO, counters, CTLFLAG_RW, 0,
            "fuse4x Monotonic Counters");
SYSCTL_NODE(_vfs_generic_fuse4x, OID_AUTO, resourceusage, CTLFLAG_RW, 0,
            "fuse4x Resource Usage");
SYSCTL_NODE(_vfs_generic_fuse4x, OID_AUTO, tunables, CTLFLAG_RW, 0,
            "fuse4x Tunables");
SYSCTL_NODE(_vfs_generic_fuse4x, OID_AUTO, version, CTLFLAG_RW, 0,
            "fuse4x Version Information");

/* fuse.control */

int sysctl_fuse4x_control_kill_handler SYSCTL_HANDLER_ARGS;
#ifndef FUSE4X_DISABLE_MACFUSE_MODE
int sysctl_fuse4x_control_macfuse_mode_handler SYSCTL_HANDLER_ARGS;
#endif
int sysctl_fuse4x_control_print_vnodes_handler SYSCTL_HANDLER_ARGS;
int sysctl_fuse4x_tunables_userkernel_bufsize_handler SYSCTL_HANDLER_ARGS;

int
sysctl_fuse4x_control_kill_handler SYSCTL_HANDLER_ARGS
{
    int error = 0;
    (void)oidp;

    if (arg1) {
        error = SYSCTL_OUT(req, arg1, sizeof(int));
    } else {
        error = SYSCTL_OUT(req, &arg2, sizeof(int));
    }

    if (error || !req->newptr) {
        return error;
    }

    if (!arg1) {
        error = EPERM;
    } else {
        error = SYSCTL_IN(req, arg1, sizeof(int));
        if (error == 0) {
            error = fuse_device_kill(*(int *)arg1, req->p);
        }
        fuse_kill = -1; /* set it back */
    }

    return error;
}

#ifndef FUSE4X_DISABLE_MACFUSE_MODE
// some applications (e.g. TrueCrypt) check what macfuse version is installed.
// ideally they should not hard-code such check as macfuse has several fuse provides.

SYSCTL_DECL(_macfuse);
SYSCTL_NODE(, OID_AUTO, macfuse, CTLFLAG_RW, 0,
            "MacFUSE Sysctl Interface");
SYSCTL_NODE(_macfuse, OID_AUTO, version, CTLFLAG_RW, 0,
            "MacFUSE Version Information");
SYSCTL_STRING(_macfuse_version, OID_AUTO, number, CTLFLAG_RD,
              "2.2.0", 0, ""); // this is for you TrueCrypt

static struct sysctl_oid *macfuse_sysctl_list[] = {
    &sysctl__macfuse_version,
    &sysctl__macfuse_version_number,
    NULL
};

static thread_t fuse4x_macfuse_mode_thread;

static void fuse4x_macfuse_mode_stop(void)
{
    int i;

    for (i = 0; macfuse_sysctl_list[i]; i++) {
        sysctl_unregister_oid(macfuse_sysctl_list[i]);
    }
    sysctl_unregister_oid(&sysctl__macfuse);
}

static void fuse4x_macfuse_mode_start(void)
{
    int i;

    sysctl_register_oid(&sysctl__macfuse);
    for (i = 0; macfuse_sysctl_list[i]; i++) {
        sysctl_register_oid(macfuse_sysctl_list[i]);
    }
}

static void fuse4x_macfuse_mode_handler(__unused void *param, __unused wait_result_t result)
{
    if (fuse_macfuse_mode) {
        fuse4x_macfuse_mode_start();
    } else {
        fuse4x_macfuse_mode_stop();
    }

    thread_terminate(current_thread());
}

int
sysctl_fuse4x_control_macfuse_mode_handler SYSCTL_HANDLER_ARGS
{
    int error = 0;

    if (arg1) {
        error = SYSCTL_OUT(req, arg1, sizeof(int));
    } else {
        error = SYSCTL_OUT(req, &arg2, sizeof(int));
    }

    if (error || !req->newptr) {
        goto exit;
    }
    if (!arg1) {
        error = EPERM;
        goto exit;
    } else {
        int value;
        error = SYSCTL_IN(req, &value, sizeof(value));
        if (error) {
            goto exit;
        }

        if (value != 0) {
            value = 1;
        }

        if (value != fuse_macfuse_mode) {
            fuse_macfuse_mode = value;
            // switch the comaptibility mode
            kern_return_t ret = kernel_thread_start(fuse4x_macfuse_mode_handler, NULL, &fuse4x_macfuse_mode_thread);
            if (ret != KERN_SUCCESS) {
                log("fuse4x: Cannot set macfuse compatibility mode");
                error = EPERM;
                goto exit;
            }

        }
    }

exit:
    return error;
}
#endif

int
sysctl_fuse4x_control_print_vnodes_handler SYSCTL_HANDLER_ARGS
{
    int error = 0;
    (void)oidp;

    if (arg1) {
        error = SYSCTL_OUT(req, arg1, sizeof(uint32_t));
    } else {
        error = SYSCTL_OUT(req, &arg2, sizeof(uint32_t));
    }

    if (error || !req->newptr) {
        return error;
    }

    if (!arg1) {
        error = EPERM;
    } else {
        error = SYSCTL_IN(req, arg1, sizeof(int));
        if (error == 0) {
            error = fuse_device_print_vnodes(*(int *)arg1, req->p);
        }
        fuse_print_vnodes = -1; /* set it back */
    }

    return error;
}

int
sysctl_fuse4x_tunables_userkernel_bufsize_handler SYSCTL_HANDLER_ARGS
{
    int error = 0;
    (void)oidp;

    if (arg1) {
        error = SYSCTL_OUT(req, arg1, sizeof(int));
    } else {
        error = SYSCTL_OUT(req, &arg2, sizeof(int));
    }

    if (error || !req->newptr) {
        return error;
    }

    if (!arg1) {
        error = EPERM;
    } else {
        error = SYSCTL_IN(req, arg1, sizeof(uint32_t));
        if (error == 0) {
            uint32_t incoming = *(uint32_t *)arg1;
            incoming = fuse_round_page_32(incoming);
            if (incoming > FUSE_MAX_USERKERNEL_BUFSIZE) {
                error = E2BIG;
            } else if (incoming < FUSE_MIN_USERKERNEL_BUFSIZE) {
                error = EINVAL;
            } else {
                fuse_userkernel_bufsize = incoming;
            }
        }
    }

    return error;
}

SYSCTL_PROC(_vfs_generic_fuse4x_control, // our parent
            OID_AUTO,         // automatically assign object ID
            kill,             // our name

            // type flag/access flag
            (CTLTYPE_INT | CTLFLAG_WR | CTLFLAG_ANYBODY),

            &fuse_kill,       // location of our data
            0,                // argument passed to our handler

            // our handler function
            sysctl_fuse4x_control_kill_handler,

            "I",              // our data type (integer)
            "fuse4x Controls: Kill the Given File System");

#ifndef FUSE4X_DISABLE_MACFUSE_MODE
SYSCTL_PROC(_vfs_generic_fuse4x_control, // our parent
            OID_AUTO,         // automatically assign object ID
            macfuse_mode,             // our name

            // type flag/access flag
            (CTLTYPE_INT | CTLFLAG_WR | CTLFLAG_ANYBODY),

            &fuse_macfuse_mode,       // location of our data
            0,                // argument passed to our handler

            // our handler function
            sysctl_fuse4x_control_macfuse_mode_handler,

            "I",              // our data type (integer)
            "fuse4x Controls: Enable Macfuse Compatibility Mode");
#endif

SYSCTL_PROC(_vfs_generic_fuse4x_control,   // our parent
            OID_AUTO,           // automatically assign object ID
            print_vnodes,       // our name

            // type flag/access flag
            (CTLTYPE_INT | CTLFLAG_WR),

            &fuse_print_vnodes, // location of our data
            0,                  // argument passed to our handler

            // our handler function
            sysctl_fuse4x_control_print_vnodes_handler,

            "I",                // our data type (integer)
            "fuse4x Controls: Print Vnodes for the Given File System");

/* fuse.counters */
SYSCTL_INT(_vfs_generic_fuse4x_counters, OID_AUTO, filehandle_reuse, CTLFLAG_RD,
           &fuse_fh_reuse_count, 0, "");
SYSCTL_INT(_vfs_generic_fuse4x_counters, OID_AUTO, filehandle_upcalls, CTLFLAG_RD,
           &fuse_fh_upcall_count, 0, "");
SYSCTL_INT(_vfs_generic_fuse4x_counters, OID_AUTO, lookup_cache_hits, CTLFLAG_RD,
           &fuse_lookup_cache_hits, 0, "");
SYSCTL_INT(_vfs_generic_fuse4x_counters, OID_AUTO, lookup_cache_misses, CTLFLAG_RD,
           &fuse_lookup_cache_misses, 0, "");
SYSCTL_INT(_vfs_generic_fuse4x_counters, OID_AUTO, lookup_cache_overrides,
           CTLFLAG_RD, &fuse_lookup_cache_overrides, 0, "");
SYSCTL_INT(_vfs_generic_fuse4x_counters, OID_AUTO, memory_reallocs, CTLFLAG_RD,
           &fuse_realloc_count, 0, "");

/* fuse.resourceusage */
SYSCTL_INT(_vfs_generic_fuse4x_resourceusage, OID_AUTO, filehandles, CTLFLAG_RD,
           &fuse_fh_current, 0, "");
SYSCTL_INT(_vfs_generic_fuse4x_resourceusage, OID_AUTO, filehandles_zombies, CTLFLAG_RD,
           &fuse_fh_zombies, 0, "");
SYSCTL_INT(_vfs_generic_fuse4x_resourceusage, OID_AUTO, ipc_iovs, CTLFLAG_RD,
           &fuse_iov_current, 0, "");
SYSCTL_INT(_vfs_generic_fuse4x_resourceusage, OID_AUTO, ipc_tickets, CTLFLAG_RD,
           &fuse_tickets_current, 0, "");
SYSCTL_INT(_vfs_generic_fuse4x_resourceusage, OID_AUTO, mounts, CTLFLAG_RD,
           &fuse_mount_count, 0, "");
SYSCTL_INT(_vfs_generic_fuse4x_resourceusage, OID_AUTO, vnodes, CTLFLAG_RD,
           &fuse_vnodes_current, 0, "");
#ifdef FUSE4X_COUNT_MEMORY
SYSCTL_INT(_vfs_generic_fuse4x_resourceusage, OID_AUTO, memory_bytes, CTLFLAG_RD,
           &fuse_memory_allocated, 0, "");
#endif

/* fuse.tunables */
SYSCTL_INT(_vfs_generic_fuse4x_tunables, OID_AUTO, admin_group, CTLFLAG_RW,
           &fuse_admin_group, 0, "");
SYSCTL_INT(_vfs_generic_fuse4x_tunables, OID_AUTO, allow_other, CTLFLAG_RW,
           &fuse_allow_other, 0, "");
SYSCTL_INT(_vfs_generic_fuse4x_tunables, OID_AUTO, iov_credit, CTLFLAG_RW,
           &fuse_iov_credit, 0, "");
SYSCTL_INT(_vfs_generic_fuse4x_tunables, OID_AUTO, iov_permanent_bufsize, CTLFLAG_RW,
           &fuse_iov_permanent_bufsize, 0, "");
SYSCTL_INT(_vfs_generic_fuse4x_tunables, OID_AUTO, max_freetickets, CTLFLAG_RW,
           &fuse_max_freetickets, 0, "");
SYSCTL_INT(_vfs_generic_fuse4x_tunables, OID_AUTO, max_tickets, CTLFLAG_RW,
           &fuse_max_tickets, 0, "");
SYSCTL_PROC(_vfs_generic_fuse4x_tunables,          // our parent
            OID_AUTO,                   // automatically assign object ID
            userkernel_bufsize,         // our name
            (CTLTYPE_INT | CTLFLAG_WR), // type flag/access flag
            &fuse_userkernel_bufsize,   // location of our data
            0,                          // argument passed to our handler
            sysctl_fuse4x_tunables_userkernel_bufsize_handler,
            "I",                        // our data type (integer)
            "fuse4x Tunables");        // our description

/* fuse.version */
SYSCTL_INT(_vfs_generic_fuse4x_version, OID_AUTO, api_major, CTLFLAG_RD,
           &fuse_api_major, 0, "");
SYSCTL_INT(_vfs_generic_fuse4x_version, OID_AUTO, api_minor, CTLFLAG_RD,
           &fuse_api_minor, 0, "");
SYSCTL_STRING(_vfs_generic_fuse4x_version, OID_AUTO, number, CTLFLAG_RD,
              FUSE4X_VERSION, 0, "");
SYSCTL_STRING(_vfs_generic_fuse4x_version, OID_AUTO, string, CTLFLAG_RD,
              FUSE4X_STRINGIFY(FUSE4X_KEXT_VERSION) ", " FUSE4X_TIMESTAMP, 0, "");

static struct sysctl_oid *fuse_sysctl_list[] =
{
    &sysctl__vfs_generic_fuse4x_control,
    &sysctl__vfs_generic_fuse4x_counters,
    &sysctl__vfs_generic_fuse4x_resourceusage,
    &sysctl__vfs_generic_fuse4x_tunables,
    &sysctl__vfs_generic_fuse4x_version,
    &sysctl__vfs_generic_fuse4x_control_kill,
#ifndef FUSE4X_DISABLE_MACFUSE_MODE
    &sysctl__vfs_generic_fuse4x_control_macfuse_mode,
#endif
    &sysctl__vfs_generic_fuse4x_control_print_vnodes,
    &sysctl__vfs_generic_fuse4x_counters_filehandle_reuse,
    &sysctl__vfs_generic_fuse4x_counters_filehandle_upcalls,
    &sysctl__vfs_generic_fuse4x_counters_lookup_cache_hits,
    &sysctl__vfs_generic_fuse4x_counters_lookup_cache_misses,
    &sysctl__vfs_generic_fuse4x_counters_lookup_cache_overrides,
    &sysctl__vfs_generic_fuse4x_counters_memory_reallocs,
    &sysctl__vfs_generic_fuse4x_resourceusage_filehandles,
    &sysctl__vfs_generic_fuse4x_resourceusage_filehandles_zombies,
    &sysctl__vfs_generic_fuse4x_resourceusage_ipc_iovs,
    &sysctl__vfs_generic_fuse4x_resourceusage_ipc_tickets,
#ifdef FUSE4X_COUNT_MEMORY
    &sysctl__vfs_generic_fuse4x_resourceusage_memory_bytes,
#endif
    &sysctl__vfs_generic_fuse4x_resourceusage_mounts,
    &sysctl__vfs_generic_fuse4x_resourceusage_vnodes,
    &sysctl__vfs_generic_fuse4x_tunables_admin_group,
    &sysctl__vfs_generic_fuse4x_tunables_allow_other,
    &sysctl__vfs_generic_fuse4x_tunables_iov_credit,
    &sysctl__vfs_generic_fuse4x_tunables_iov_permanent_bufsize,
    &sysctl__vfs_generic_fuse4x_tunables_max_freetickets,
    &sysctl__vfs_generic_fuse4x_tunables_max_tickets,
    &sysctl__vfs_generic_fuse4x_tunables_userkernel_bufsize,
    &sysctl__vfs_generic_fuse4x_version_api_major,
    &sysctl__vfs_generic_fuse4x_version_api_minor,
    &sysctl__vfs_generic_fuse4x_version_number,
    &sysctl__vfs_generic_fuse4x_version_string,
    NULL
};

void
fuse_sysctl_start(void)
{
    int i;

    sysctl_register_oid(&sysctl__vfs_generic_fuse4x);
    for (i = 0; fuse_sysctl_list[i]; i++) {
       sysctl_register_oid(fuse_sysctl_list[i]);
    }
}

void
fuse_sysctl_stop(void)
{
    int i;

    for (i = 0; fuse_sysctl_list[i]; i++) {
       sysctl_unregister_oid(fuse_sysctl_list[i]);
    }
    sysctl_unregister_oid(&sysctl__vfs_generic_fuse4x);

#ifndef FUSE4X_DISABLE_MACFUSE_MODE
    if (fuse_macfuse_mode) {
        fuse4x_macfuse_mode_stop();
    }
    thread_deallocate(fuse4x_macfuse_mode_thread);
#endif
}
