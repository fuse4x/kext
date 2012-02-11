/*
 * Copyright (C) 2006-2008 Google. All Rights Reserved.
 */

/*
 * Portions Copyright (c) 1999-2003 Apple Computer, Inc. All Rights Reserved.
 *
 * This file contains Original Code and/or Modifications of Original Code as
 * defined in and that are subject to the Apple Public Source License Version
 * 2.0 (the 'License'). You may not use this file except in compliance with
 * the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT. Please see
 * the License for the specific language governing rights and limitations
 * under the License.
 */

#include "fuse.h"
#include "fuse_nodehash.h"
#if M_FUSE4X_ENABLE_BIGLOCK
#include "fuse_biglock_vnops.h"
#include "fuse_ipc.h"
#endif
#include <fuse_param.h>

#include <sys/vnode.h>
#include <kern/assert.h>

/*
 * The HNode structure represents an entry in the VFS plug-ins hash table.
 * See the comments in the header file for a detailed description of the
 * relationship between this data structure and the vnodes that are maintained
 * by VFS itself.
 *
 * The HNode and the FSNode are intimately tied together. In fact, they're
 * allocated from the same memory block. When we allocate an HNode, we allocate
 * extra space (gFSNodeSize bytes) for the FSNode.
 *
 * This data structure is effectively reference counted by the forkVNodesCount
 * field. When the last vnode that references this HNode is reclaimed, the
 * HNode itself is reclaimed (along with the associated FSNode).
 */

struct HNode {
    /* [2] next pointer for hash chain */
    LIST_ENTRY(HNode) hashLink;

    /* [1] device number on which file system object (fsobj) resides */
    fuse_device_t dev;

    /* [1] inode number of fsobj resides */
    uint64_t ino;

    /* [2] [3] */
    bool attachOutstanding;

    /* [2] true if someone is waiting for attachOutstanding to go false */
    bool waiting;

    vnode_t vnode;
};
typedef struct HNode HNode;

/*
 * [HNode Notes]
 *
 * [1] This field is immutable. That is, it's set up as part of the process of
 *     creating an HNode, and is not modified after that. Thus, it doesn't need
 *     to be protected from concurrent access.
 *
 * [2] The gHashMutex lock protects this field in /all/ HNodes.
 *
 * [3] This is true if HNodeLookupCreatingIfNecessary has return success but
 *     with a NULL vnode.  In this case, we're expecting the client to call
 *     either HNodeAttachVNodeSucceeded or HNodeAttachVNodeFailed at some time
 *     in the future. While this is true, forkVNodesCount is incremented to
 *     prevent the HNode from going away.
 */

/*
 * The following client globals are set by the client when it calls HNodeInit.
 * See the header comments for HNodeInit for more details.
 */

static lck_grp_t   *gLockGroup;
static size_t       gFSNodeSize;
static OSMallocTag  gOSMallocTag;

/*
 * gHashMutex is a single mutex that protects all fields (except the immutable
 * ones) of all HNodes, the hash table itself (all elements of the gHashTable
 * array), and gHashNodeCount.
 */

static lck_mtx_t *gHashMutex;

/*
 * gHashNodeCount is a count of the number of HNodes in the hash table.
 * This is used solely for debugging (if it's non-zero when HNodeTerm is
 * called, the debug version of the code will panic).
 */

static size_t gHashNodeCount;

/*
 * gHashTable is a pointer to an array of HNodeHashHead structures that
 * represent the heads of all the hash chains. This structure, and the
 * associated gHashTableMask, are all a consequence of my use of hashinit.
 */

static LIST_HEAD(HNodeHashHead, HNode) *gHashTable;
typedef struct HNodeHashHead HNodeHashHead;

static u_long gHashTableMask;

/*
 * Given a device number and an inode number, return a pointer to the
 * hash chain head.
 */
static HNodeHashHead *
HNodeGetFirstFromHashTable(fuse_device_t dev, uint64_t ino)
{
    return (HNodeHashHead *)&gHashTable[((uint64_t)(u_long)dev + ino) & gHashTableMask];
}

extern errno_t
HNodeInit(lck_grp_t   *lockGroup,
          lck_attr_t  *lockAttr,
          OSMallocTag  mallocTag,
          size_t       fsNodeSize)
{
    errno_t     err;

    assert(lockGroup != NULL);
    // lockAttr may be NULL
    assert(mallocTag != NULL);
    assert(fsNodeSize != 0);

    gFSNodeSize  = fsNodeSize;
    gOSMallocTag = mallocTag;
    gLockGroup   = lockGroup;

    gHashMutex = lck_mtx_alloc_init(lockGroup, lockAttr);
    gHashTable = hashinit(desiredvnodes, M_TEMP, &gHashTableMask);
    err = 0;
    if ((gHashMutex == NULL) || (gHashTable == NULL)) {
        HNodeTerm(); /* Clean up any partial allocations */
        err = ENOMEM;
    }
    return err;
}

extern void
HNodeTerm(void)
{
    /*
     * Free the hash table. Also, if there are any hash nodes left, we
     * shouldn't be terminating.
     */

    if (gHashTable != NULL) {
        assert(gHashNodeCount == 0);
        #if MACH_ASSERT
            {
                u_long i;

                for (i = 0; i < (gHashTableMask + 1); i++) {
                    assert(gHashTable[i].lh_first == NULL);
                }
            }
        #endif
        FREE(gHashTable, M_TEMP);
        gHashTable = NULL;
    }

    if (gHashMutex != NULL) {
        assert(gLockGroup != NULL);

        lck_mtx_free(gHashMutex, gLockGroup);
        gHashMutex = NULL;
    }

    gLockGroup = NULL;
    gOSMallocTag = NULL;
    gFSNodeSize = 0;
}

extern void *
FSNodeGenericFromHNode(HNodeRef hnode)
{
    assert(hnode != NULL);
    return (void *) &hnode[1];
}

extern errno_t
HNodeLookupCreatingIfNecessary(fuse_device_t dev,
                               uint64_t      ino,
                               HNodeRef *hnodePtr,
                               vnode_t  *vnPtr)
{
    errno_t    err;
    HNodeRef   thisNode;
    HNodeRef   newNode;
    bool       needsUnlock;
    vnode_t    resultVN;
    uint32_t   vid;

    assert( hnodePtr != NULL);
    assert(*hnodePtr == NULL);
    assert( vnPtr    != NULL);
    assert(*vnPtr    == NULL);

    /*
     * If you forget to call HNodeInit, it's likely that the first call
     * you'll make is HNodeLookupCreatingIfNecessary (to set up your root
     * vnode), and this assert will fire (rather than you dying inside wit
     * a memory access exception inside lck_mtx_lock).
     */

    assert(gHashMutex != NULL);

    newNode = NULL;
    needsUnlock = true;
    resultVN = NULL;

    lck_mtx_lock(gHashMutex);

    do {
        lck_mtx_assert(gHashMutex, LCK_MTX_ASSERT_OWNED);

        err = EAGAIN;

        /* First look it up in the hash table. */

        thisNode = LIST_FIRST(HNodeGetFirstFromHashTable(dev, ino));
        while (thisNode != NULL) {

            if ((thisNode->dev == dev) && (thisNode->ino == ino)) {
                break;
            }

            thisNode = LIST_NEXT(thisNode, hashLink);
        }

        /*
         * If we didn't find it, we're creating a new HNode. If we haven't
         * already allocated newNode, we must do so. This drops the mutex,
         * so the hash table might have been changed by someone else, so we
         * have to loop.
         */

        /* If we do have a newNode at hand, use that. */

        if (thisNode == NULL) {
            if (newNode == NULL) {
                lck_mtx_unlock(gHashMutex);

                /* Allocate a new node. */

                newNode = FUSE_OSMalloc(sizeof(*newNode) + gFSNodeSize,
                                        gOSMallocTag);
                if (newNode == NULL) {
                    err = ENOMEM;
                } else {

                    /* Fill it in. */

                    memset(newNode, 0, sizeof(*newNode) + gFSNodeSize);

                    newNode->dev   = dev;
                    newNode->ino   = ino;
                    newNode->vnode = NULL;
                }

                lck_mtx_lock(gHashMutex);
            } else {
                LIST_INSERT_HEAD(HNodeGetFirstFromHashTable(dev, ino),
                                 newNode, hashLink);
                gHashNodeCount += 1;

                /*
                 * Set thisNode to the node that we inserted, and clear
                 * newNode so it doesn't get freed.
                 */

                thisNode = newNode;
                newNode = NULL;

                /*
                 * IMPORTANT:
                 * There's a /really/ subtle point here. Once we've inserted
                 * the new node into the hash table, it can be discovered by
                 * other threads. This would be bad, because it's only
                 * partially constructed at this point. We prevent this
                 * problem by not dropping gHashMutex from this point to
                 * the point that we're done. This only works because we
                 * allocate the new node with a fork buffer that's adequate
                 * to meet our needs.
                 */
            }
        }

        /*
         * If we found a hash node (including the case where we've used one
         * that we previously allocated), check its status.
         */

        if (thisNode != NULL) {
            if (thisNode->attachOutstanding) {
                /*
                 * If there are outstanding attaches, wait for them to
                 * complete. This means that there can be only one outstanding
                 * attach at a time, which is important because we don't want
                 * two threads trying to fill in the same fork's vnode entry.
                 *
                 * In theory we might keep an array of outstanding attach
                 * flags, one for each fork, but that's difficult and probably
                 * overkill.
                 */

                thisNode->waiting = true;

                (void)fuse_msleep(thisNode, gHashMutex, PINOD,
                                  "HNodeLookupCreatingIfNecessary", NULL);

                /*
                 * msleep drops and reacquires the mutex; the hash table may
                 * have changed, so we loop.
                 */
            } else if (thisNode->vnode == NULL) {
                /*
                 * If there's no existing vnode associated with this fork of
                 * the HNode, we're done. The caller is responsible for
                 * attaching a vnode for  this fork. Setting attachOutstanding
                 * will block any other threads from using the HNode until the
                 * caller is done attaching. Also, we artificially increment
                 * the reference count to prevent the HNode from being freed
                 * until the caller has finished with it.
                 */

                thisNode->attachOutstanding = true;

                /* Results for the caller. */

                assert(thisNode != NULL);
                assert(resultVN == NULL);

                err = 0;
            } else {

                vnode_t candidateVN;

                /*
                 * If there is an existing vnode, get a reference on it and
                 * return that to the caller. This vnode reference prevents
                 * the vnode from being reclaimed, which prevents the HNode
                 * from being freed.
                 */

                candidateVN = thisNode->vnode;
                assert(candidateVN != NULL);

                /*
                 * Check that our vnode hasn't been recycled. If this succeeds,
                 * it acquires a reference on the vnode, which is the one we
                 * return to our caller. We do this with gHashMutex unlocked
                 * to avoid any deadlock concerns.
                 */

                vid = vnode_vid(candidateVN);

                lck_mtx_unlock(gHashMutex);

#if M_FUSE4X_ENABLE_BIGLOCK
                struct fuse_data *data = dev->data;
                fuse_biglock_unlock(data->biglock);
#endif
                err = vnode_getwithvid(candidateVN, vid);
#if M_FUSE4X_ENABLE_BIGLOCK
                fuse_biglock_lock(data->biglock);
#endif

                if (err == 0) {
                    /* All ok; return the HNode/vnode to the caller. */
                    assert(thisNode != NULL);
                    assert(resultVN == NULL);
                    resultVN = candidateVN;
                    needsUnlock = false;
                } else {
                    /* We're going to loop and retry, so relock the mutex. */

                    lck_mtx_lock(gHashMutex);

                    err = EAGAIN;
                }
            }
        }
    } while (err == EAGAIN);

    /* On success, pass results back to the caller. */

    if (err == 0) {
        *hnodePtr = thisNode;
        *vnPtr    = resultVN;
    }

    /* Clean up. */

    if (needsUnlock) {
        lck_mtx_unlock(gHashMutex);
    }

    /* Free newNode if we allocated it but didn't put it into the table. */

    if (newNode != NULL) {
        FUSE_OSFree(newNode, sizeof(*newNode) + gFSNodeSize, gOSMallocTag);
    }

    assert( (err == 0) == (*hnodePtr != NULL) );

    return err;
}

/*
 * An attach operate has completed. If there is someone waiting for
 * the HNode, wake them up.
 */
static void
HNodeAttachComplete(HNodeRef hnode)
{
    assert(hnode != NULL);

    lck_mtx_assert(gHashMutex, LCK_MTX_ASSERT_OWNED);

    assert(hnode->attachOutstanding);
    hnode->attachOutstanding = false;

    if (hnode->waiting) {
        fuse_wakeup(hnode);
        hnode->waiting = false;
    }
}

/*
 * Decrement the number of fork vnodes for this HNode. If it hits zero,
 * the HNode is gone and we remove it from the hash table and return
 * true indicating to our caller that they need to clean it up.
 */
static bool
HNodeForkVNodeDecrement(HNodeRef hnode)
{
    bool scrubIt;

    assert(hnode != NULL);

    lck_mtx_assert(gHashMutex, LCK_MTX_ASSERT_OWNED);

    scrubIt = false;

    LIST_REMOVE(hnode, hashLink);

    /* We test for this case before decrementing it because it's unsigned */
    assert(gHashNodeCount > 0);

    gHashNodeCount -= 1;

    scrubIt = true;

    return scrubIt;
}

extern void
HNodeAttachVNodeSucceeded(HNodeRef hnode, vnode_t vn)
{
    errno_t junk;

    lck_mtx_lock(gHashMutex);

    assert(hnode != NULL);
    assert(vn != NULL);
    assert(vnode_fsnode(vn) == hnode);

    /*
     * If someone is waiting for the HNode, wake them up. They won't actually
     * start running until we drop gHashMutex.
     */

    HNodeAttachComplete(hnode);

    /* Record the vnode's association with this HNode. */
    hnode->vnode = vn;
    junk = vnode_addfsref(vn);
    assert(junk == 0);

    lck_mtx_unlock(gHashMutex);
}

extern bool
HNodeAttachVNodeFailed(HNodeRef hnode)
{
    bool   scrubIt;

    lck_mtx_lock(gHashMutex);

    assert(hnode != NULL);

    /*
     * If someone is waiting for the HNode, wake them up. They won't actually
     * start running until we drop gHashMutex.
     */

    HNodeAttachComplete(hnode);

    /*
     * Decrement the number of fork vnodes referencing the HNode, freeing
     * the HNode if it hits zero.
     */

    scrubIt = HNodeForkVNodeDecrement(hnode);

    lck_mtx_unlock(gHashMutex);

    return scrubIt;
}

extern bool
HNodeDetachVNode(HNodeRef hnode, vnode_t vn)
{
    errno_t   junk;
    bool scrubIt;

    lck_mtx_lock(gHashMutex);

    assert(hnode != NULL);
    assert(vn != NULL);

    /* Disassociate the vnode with this fork of the HNode. */

    hnode->vnode = NULL;
    junk = vnode_removefsref(vn);
    assert(junk == 0);
    vnode_clearfsnode(vn);

    /*
     * Decrement the number of fork vnodes referencing the HNode,
     * freeing the HNode if it hits zero.
     */

    scrubIt = HNodeForkVNodeDecrement(hnode);

    lck_mtx_unlock(gHashMutex);

    return scrubIt;
}

extern void
HNodeScrubDone(HNodeRef hnode)
{
    assert(hnode != NULL);

    /*
     * If anyone is waiting on this HNode, that would be bad.
     * It would be easy to fix this (we could wake them up at this
     * point) but, as I don't think it can actually happen, I'd rather
     * have this assert tell me whether the code is necessary than
     * just add it blindly.
     */

    assert(!hnode->waiting);
    FUSE_OSFree(hnode, sizeof(*hnode) + gFSNodeSize, gOSMallocTag);
}

