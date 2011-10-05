/*
 * Copyright (C) 2006-2008 Google. All Rights Reserved.
 * Copyright (C) 2011 Anatol Pomozov. All Rights Reserved.
 */


#ifndef _FUSE_DEVICE_H_
#define _FUSE_DEVICE_H_

#include <sys/conf.h>
#include <miscfs/devfs/devfs.h>

struct fuse_data;

/* softc */

struct fuse_device {
    lck_mtx_t        *mtx;
    int               usecount;
    pid_t             pid;
    dev_t             dev;
    void             *cdev;
    struct fuse_data *data;
};
typedef struct fuse_device * fuse_device_t;

/* Global */

int fuse_devices_start(void);
int fuse_devices_stop(void);

/* Per Device */

fuse_device_t     fuse_device_get(dev_t dev);
void              fuse_device_close_final(fuse_device_t fdev);

/* Control/Debug Utilities */

int fuse_device_kill(int unit, struct proc *p);
int fuse_device_print_vnodes(int unit_flags, struct proc *p);

#endif /* _FUSE_DEVICE_H_ */
