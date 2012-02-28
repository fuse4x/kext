/*
 * Copyright (C) 2011 Anatol Pomozov. All Rights Reserved.
 */

#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/mount.h>
#include <grp.h>
#include <AvailabilityMacros.h>

#if MAC_OS_X_VERSION_MIN_REQUIRED >= 1060
#include <IOKit/kext/KextManager.h>
#endif

#include <fuse_param.h>
#include <fuse_version.h>

#define KEXTLOAD_PROGRAM "/sbin/kextload"

#ifdef FUSE4X_ENABLE_MACFUSE_MODE
static bool is_macfuse_mode(void)
{
    struct stat macfuse_file, fuse4x_file;
    if (stat("/usr/local/lib/libfuse_ino64.dylib", &macfuse_file) < 0)
        return false;
    if (stat("/usr/local/lib/libfuse4x.dylib", &fuse4x_file) < 0)
        return false;

    // if macfuse file points to fuse4x file then we assume that fuse4x is in compatibility mode
    return (macfuse_file.st_dev == fuse4x_file.st_dev) && (macfuse_file.st_ino == fuse4x_file.st_ino);
}
#endif

int
main(__unused int argc, __unused const char *argv[])
{
    struct vfsconf vfc;

    if (getvfsbyname(FUSE4X_FS_TYPE, &vfc) == 0) {
        /* Fuse4X is already loaded */
        return EXIT_SUCCESS;
    }

#if MAC_OS_X_VERSION_MIN_REQUIRED >= 1060
    CFStringRef kextPath = CFSTR(FUSE4X_KEXT_PATH);
    CFURLRef kextUrl = CFURLCreateWithFileSystemPath(kCFAllocatorDefault, kextPath, kCFURLPOSIXPathStyle, true);
    OSReturn result = KextManagerLoadKextWithURL(kextUrl, NULL);
    CFRelease(kextUrl);
    CFRelease(kextPath);
    if (result != kOSReturnSuccess) {
        fprintf(stderr, "Cannot load kext from " FUSE4X_KEXT_PATH "\n");
        return EXIT_FAILURE;
    }
#else
    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        return EXIT_FAILURE;
    }

    if (pid == 0) {
        if (execl(KEXTLOAD_PROGRAM, KEXTLOAD_PROGRAM, FUSE4X_KEXT_PATH, NULL) < 0) {
            perror("execl");
            _exit(EXIT_FAILURE);
        }
    } else {
        int status;
        if (waitpid(pid, &status, 0) < 0) {
            perror("waitpid");
            return EXIT_FAILURE;
        };

        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            return EXIT_FAILURE;
        }
    }
#endif

    /* now do any kext-load-time settings we need to do as root */
    struct group *g = getgrnam(MACOSX_ADMIN_GROUP_NAME);
    if (g) {
        gid_t admin_gid = g->gr_gid;

        /* if this fails, we don't care */
        (void)sysctlbyname(SYSCTL_FUSE4X_TUNABLES_ADMIN, NULL, NULL, &admin_gid, sizeof(admin_gid));
    }

#ifdef FUSE4X_ENABLE_MACFUSE_MODE
    if (is_macfuse_mode()) {
        int macfuse_mode = 1;
        (void)sysctlbyname("vfs.generic.fuse4x.control.macfuse_mode", NULL, NULL, &macfuse_mode, sizeof(macfuse_mode));
    }
#endif

    return EXIT_SUCCESS;
}
