/*
 * Copyright (C) fuse4x.org 2011 All Rights Reserved.
 */
#include <stdlib.h>
#include <sys/sysctl.h>
#include <sys/mount.h>
#include <grp.h>
#include <IOKit/kext/KextManager.h>

#include <fuse_param.h>
#include <fuse_version.h>

int
main(__unused int argc, __unused const char *argv[])
{
    struct vfsconf vfc;

    if (getvfsbyname(FUSE4X_FS_TYPE, &vfc) == 0) {
        /* Fuse4X is already loaded */
        return EXIT_SUCCESS;
    }

    CFStringRef kextPath = CFSTR(FUSE4X_KEXT_PATH);
    CFURLRef kextUrl = CFURLCreateWithFileSystemPath(
        kCFAllocatorDefault, kextPath, kCFURLPOSIXPathStyle, true);
    OSReturn result = KextManagerLoadKextWithURL(kextUrl, NULL);
    CFRelease(kextUrl);
    CFRelease(kextPath);
    if (result != kOSReturnSuccess) {
        fprintf(stderr, "Cannot load kext from " FUSE4X_KEXT_PATH "\n");
        return EXIT_FAILURE;
    }

    /* now do any kext-load-time settings we need to do as root */
    struct group *g = getgrnam(MACOSX_ADMIN_GROUP_NAME);
    if (g) {
        gid_t admin_gid = g->gr_gid;

        /* if this fails, we don't care */
        (void)sysctlbyname(SYSCTL_FUSE4X_TUNABLES_ADMIN, NULL, NULL, &admin_gid, sizeof(admin_gid));
    }

    return EXIT_SUCCESS;
}
