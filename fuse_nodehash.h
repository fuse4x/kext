/*
 * Copyright (C) 2006-2008 Google. All Rights Reserved.
 * Amit Singh <singh@>
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

#ifndef _FUSE_NODEHASH_H_
#define _FUSE_NODEHASH_H_

#include <stdint.h>
#include <stdbool.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <libkern/OSMalloc.h>

#include "fuse_device.h"

/*
    Theory of Operation
    ===================

    VNodes and FSNodes
    ------------------
    One of the hardest parts about writing a VFS plug-in is implementing the hash
    layer.  Both VFS and your plug-in have an idea of what file system objects (fsobjs)
    are currently cached in memory.  For VFS, fsobjs are cached as "vnodes".
    A VFS plug-in is free to represent the cached fsobjs however it sees
    fit.  For the sake of this discussion, the file system-specific fsobj
    cache items are called "FSNodes".  To make it easier to map from a vnode
    to an FSNode, VFS provides a per-file pointer for each vnode (accessed via
    vnode_fsnode).

    In a traditional UNIX system, there's a one-to-one correspondence between
    vnodes and fsnodes (excluding transition situations, where vnodes are being
    created or reclaimed).  However, Mac OS X's support for multi-fork files means that
    there can be more than one vnode for a given FSNode.  As vnodes are the hook on
    which file system operations like read and write are performed, each fork has
    its own vnode.  However, you don't want each of these vnodes to have its own FSNode,
    because that makes directory operations (like renaming or deleting a file) much harder.
    So, on Mac OS X there is a many-to-one relationship between vnodes and FSNodes.

    Also, the existance of hard links means that there's a one-to-many relationship
    between directory entries and fsobjs.  It's important to remember that a vnode
    represents an fsobj, not a directory entry.  Likewise for an FSNode.

    What makes this stuff tricky is that these two caches (vnodes and FSNodes) must
    be kept in sync, and this must be done in a multi-threaded environment.  Moreover,
    it's not possible for an FSNode to keep a strong reference to the vnode, because
    if it did it would be impossible to ever reclaim a vnode.

    The solution to this problem (as implemented by this module, the architecture of
    which was basically cribbed from the HFS Plus implementation) is for the VFS plug-in
    to maintain a hash table of the FSNodes that currently exist.  This is hashed by
    the device number on which the FSNode's fsobj resides, and the inode number of
    the fsobj.  This hashing scheme allows the VFS plug-in's implementation
    of VNOPLookup to quickly determine where an FSNode exists for the fsobj
    referenced by a given directory entry.

    The hash table in protected by a single (per VFS plug-in) lock.  For good performance,
    it's critical that the VFS plug-in not hold this lock for long.  Furthermore, to
    prevent deadlock the VFS plug-in should not call out to other parts of the system
    while holding this lock.  For example, a VFS plug-in /must/ drop its hash lock
    before allocating memory.

    This module implements the above recommendation exactly.  The locking is entirely
    internal to the module, and it will never return to you (or call out to the system)
    with a lock held.

    One key subtlety here is that, when VNOPLookup looks for an FSNode and fails to
    find one, it creates a dummy one and puts it in the hash table.  This dummy
    entry is in a state (the attaching state) that causes other threads that might
    also be looking up this FSNode to stall until the FSNode is fully constructed
    (that is, the vnode has been attached to it) or the original thread has failed
    to attach.  This presence of this dummy allows the thread to drop the hash table
    lock (in order to construct the vnode) without worrying about other threads stepping
    on its toes.

    As I mentioned previously, an entry in the FSNode hash table cannot keep
    a strong reference on its associated vnodes because that would prevent the
    vnodes from every being reclaimed.  Thus, every time you pull a vnode out of the
    FSNode hash table, you have to reconfirm that it's valid.  This is done using
    the vnode ID.  While holding the hash table lock (which holds up any threads
    reclaiming the vnode inside the VNOPReclaim) you get the vnode's ID.  You
    then drop the lock and get an I/O reference on the vnode using
    vnode_getwithvid.  This checks to see if the vnode ID has changed.  If so,
    the vnode has been recycled between you dropping your hash table lock and
    you getting the vnode I/O reference.  In that case, you can just retry
    and expect that the next time you will get consistent results (it's likely
    that, when you dropped the hash lock, the vnode was reclaimed and thus
    the vnode reference in the FSNode has been set to NULL).

        Note
        This architecture works because the system's vnode table grows
        (up to a limit), but it never shrinks.  Thus, vnodes are never freed,
        only recycled.  Therefore, once you've been given a vnode_t, you can
        rest assured that it will be valid for ever (well, until the system
        restarts).  It may, however, change what fsobj it refers to, but in
        that case its vnode ID will change.

    On the subject of vnode reclaiming, it's important to get three pieces of
    terminology correct:

      o When we say that a vnode has been recycled, it means that it has changed
        identity.  Remember, the system maintains a vnode table as a cache of
        the fsobjs on disk.  The system can pick and choose which fsobjs it
        wants to cache.  When it changes the identity of a vnode, the vnode
        has been recycled.

        A VFS plug-in can force a vnode to be recycled by calling vnode_recycle;
        typically you do this when you want to force the system to forget about
        some file (perhaps some internal B-tree file).

      o A vnode is reclaimed as part of the process of recycling it.  Once VFS
        has decided to change the identity of a vnode, it must first disconnect it
        from its previous identity.  It needs to tell the VFS plug-in about this,
        and this is the reclaim call (VNOPReclaim).

        It's vital to realise that a VFS plug-in cannot say "no" to a reclaim call
        (doing so will panic the system).  This is not an appropriate place to,
        for example, write the directory entry back to disk.  The primary goal of
        a reclaim call is to disconnect the vnode from the FSNode.  Secondary to
        that, if this is the last vnode associated with the FSNode, the reclaim
        call should, at its disgression, dispose of of the FSNode.

      o VFS makes a vnode inactive when the last significant reference to the
        vnode goes away.  For example, when the last file descriptor that uses
        the vnode is closed, VFS makes the inactive call to the VFS plug-in
        (VNOPInactive).  This is the point where the plug-in should do any clean
        up that might fail (for example, write to disk any changes made to the
        fsobj).

        It's important to realise that, just because a vnode has been made inactive,
        doesn't mean that it will be recycled (and, prior to that, reclaimed) soon.
        The vnodes act as a cache of directory entries, and if that cache is not
        under pressure, vnodes aren't recycled.  Also, if the vnode isn't recycled
        immediately, it's possible that someone will take a significant reference
        to the vnode (for example, open the file again) and that will make it
        active again.

    Module Usage
    ------------
    To start, you need to get the following terminology straight.

      o vnode -- This is VFS's handle to the cached file system object (fsobj).  Your
        VFS plug-in can create and manipulate vnodes using the VFS KPI.

      o HNode -- This is the file system specific data for a cached fsobj that is
        managed by this module.  This is not truly file system specific, in that
        the HNode data structure, like this module, is meant to be reusable, and would be
        the same for virtually any file system.

        Your VFS plug-in manipulates HNodes using the routines exported by this module.

      o FSNode -- This is the file system specific data for a cached fsobj that is
        not managed by this module.  Your VFS plug-in is responsible for this data.

    There is a one-to-many relationship between vnodes and HNodes.  The HNode maintains an
    array of vnodes for the file's forks.  The index into this array can be simple (for
    example, on a simple twin fork file system, like MFS, 0 is the data fork and 1 is the
    resource fork), or it can be complex (you might dynamically allocate fork indexes based
    on some criteria internal to your file system.  However, I strongly recommend that you
    follow these guidelines:

      o You should use fork 0 as the data fork, which is the fork seen by non-fork aware
        programs (<x-man-page://1/cat, for example).

      o For directory vnodes, you should always specify a fork index of 0.

    There is a strict one-to-one relationship between HNodes and FSNodes; in fact, they are
    both embedded in the same memory block.

    The routines exported by this module can be broken into 5 groups:

      o initialisations and termination -- Your VFS plug-in must call HNodeInit before
        calling any other routines in this module, and HNodeTerm before unloading.
        It is safe to call HNodeTerm even if HNodeInit fails, and it's safe to call
        HNodeTerm without ever calling HNodeInit.

        When you initialise this module, you give it the following information:

          o A lock group and lock attributes, so that the module can create locks.
          o A OS malloc tag, so that the module can allocate memory.
          o A magic number that the module uses to mark HNodes.
          o The size of your FSNode.

        The module will use this last piece of information to allocate your FSNode as part
        of the same memory block as it uses for the HNode.  It guarantees to clear the FSNode
        before you see it, so you can use an "initialised" field to determine whether you've
        constructed the FSNode yet.

      o transfer functions -- These routines allow you to get the HNode for a vnode
        (HNodeFromVNode), the FSNode for an HNode (FSNodeGenericFromHNode), and so on.

      o accessors -- The simple accessors let you access certain important fields of an
        including the device number (HNodeGetDevice) and inode number (HNodeGetInodeNumber).
        The complex accessors let you map a vnode to a fork index (HNodeGetForkIndexForVNode),
        and get the vnode for a given fork index (HNodeGetVNodeForForkAtIndex).

        IMPORTANT
        Because a vnode can be reclaimed at any time, and unless you know that the vnode
        can't be reclaimed, you should confirm that the vnode hasn't been reclaimed
        (using the vnode ID technique described above) before assuming that the vnode still
        refers to this FSNode.

      o vnode lifecycle -- These routines let you create and destroy FSNodes, and attach and
        detach vnodes from them.  Their usage is explained in more detail below.

      o debugging -- HNodePrintState lets you print the state of this module.

    The two most important places where your VFS plug-in interacts with this module are
    VNOPLookup and VNOPReclaim.  In VNOPLookup, you want to create a vnode for a given
    directory entry.  This module assumes that the file system object (fsobj) referenced
    by the directory entry is uniquely identified by its device number (dev_t) and inode
    number (ino_t).  These, along with the fork index, are the keys that you pass to
    HNodeLookupCreatingIfNecessary to see if the vnode already exists.  Once you've called
    HNodeLookupCreatingIfNecessary, you must interpret the results and then follow a strictly
    defined protocol to ensure that everything works properly.  The results of
    HNodeLookupCreatingIfNecessary are as follows:

      o failure -- If HNodeLookupCreatingIfNecessary returns an error, it has failed
        and you don't have to do anything more (other than handle the error, but that
        has no hash-layer implications).

      o success, no vnode -- If there is no vnode matching your request,
        HNodeLookupCreatingIfNecessary will return a valid HNode but a NULL vnode.
        In this case, you must either:

          o create a vnode and attach it to the HNode by calling HNodeAttachVNodeSucceeded
          o abort the attach by calling HNodeDetachVNode

        There are two substate here:

          o If the HNode is entirely new, the FSNode will have just been allocated and
            will be all zeros.  You should check for this, and initialise the FSNode
            (before creating the vnode).

          o If the HNode already exists, the FSNode will not be all zeros, and you can
            assume that you've initialised it already.

        Thus, you can use a simple "initialised" field in your FSNode to tell whether
        it's been initialised or not.

      o success, vnode -- If there is a vnode matching your request,
        HNodeLookupCreatingIfNecessary will return a valid HNode and a valid vnode.
        The FSNode associated with the HNode will be initialised in this case.  You have
        an I/O reference on the vnode, so you can guarantee that it won't go away
        until you call vnode_put (and neither will the HNode or FSNode, because the existance
        of the vnode will keep them from going away).  You /must/ call vnode_put on the vnode
        at some point in the future (or pass it to someone who will call vnode_put for you).

    So, here's a high-level outline of how to create a vnode using HNodeLookupCreatingIfNecessary.

        HNodeLookupCreatingIfNecessary(dev, ino, forkIndex) -> err, hn, vn

        if not err and vn is NULL then
            assert(hn != NULL);

            FSNodeGenericFromHNode(hn) -> fsn

            if fsn is not initialised then
                initialise fsn -> err
            end if

            if not err then
                vnode_create(hn) -> err, vn
            end if

            if err then
                HNodeAttachVNodeSucceeded(hn, forkIndex, vn)
            else
                HNodeAttachVNodeFailed(hn, forkIndex) -> see below
            end if
        end if

        if no err then
            -- vn is not NULL, and you have an I/O reference on it
            -- you must release that reference at some point by calling vnode_put
        else
            -- handle the error; with no further hash-layer implications
        end if

    The second important interaction between your VFS plug-in and this module is in
    VNOPReclaim.  In this case, you must call HNodeDetachVNode to detach the vnode from
    the HNode.  This is pretty simple, and the only complication is in FSNode
    scrubbing (described next).

    In all cases where you detach a reference on an HNode (HNodeDetachVNode, obviously, but
    also when you fail to create a vnode for an HNode and abort the operation by calling
    HNodeAttachVNodeFailed), the function returns a Boolean indicating whether this is
    the last vnode referencing the HNode.  If it is, you must scrub the FSNode and, when
    that's done, tell this module that you're done by calling HNodeScrubDone.  At this point,
    this module will free the memory used for the HNode and FSNode.

        IMPORTANT
        Your scrubbing code MUST NEVER FAIL.  This is a direct consequence of it running on
        the reclaim path.

    When your scrubbing code runs, it is guaranteed that no other threads will be
    scrubbing the FSNode, or attempting to resurrect the FSNode.  The HNode has been
    pulled out of the hash table.  Thus, once HNodeDetachVNode or HNodeAttachVNodeFailed
    returns true, any other threads that try to access this FSNode will end up creating a
    new FSNode instead.

    A typical scrubbing routine would just dispose of any data structures (like locks)
    that are owned by the FSNode.

    Here's a high-level outline of FSNode scrubbing.

        HNodeDetachVNode(hn, vn) -> scrubIt

        if scrubIt then
            FSNodeGenericFromHNode(hn) -> fsnode

            -- dispose of any data structures owned by fsnode

            HNodeScrubDone(hn)
        end if

        IMPORTANT
        Do not call HNodeFromVNode or FSNodeGenericFromVNode after HNodeDetachVNode has
        returned true.  By that point, the vnode no longer references the HNode/FSNode.

    Don't forget that you have to use the same logic for HNodeAttachVNodeFailed as well as
    HNodeDetachVNode.
*/

typedef struct HNode * HNodeRef;

// Initialises this module.  You must call this routine before calling any other
// routines exported by this module (except HNodeTerm).
//
// lockGroup must be a valid lock group, and that lock group must persist until
// HNodeTerm is called.  Any locks that this module creates will be in this group.
//
// lockAttr may be NULL, in which case locks with the default attributes are used.
// If lockAttr is not NULL, these attributes are used for all locks created by this
// module.  lockAttr may be destroyed after this routine has returned.
//
// mallocTag must be a valid OS malloc tag, and this must persist until HNodeTerm is
// called.
//
// magic is a magic number used for the first four bytes of any HNode created by this
// module; it's purely a debugging aid.  If you set it to something notable (like
// four ASCII characters), it's easy to see HNodes in the debugger.
//
// fsNodeSize is the amount of memory that this module should allocate for the FSNode
// when allocating an HNode.  That is, when you call FSNodeGenericFromHNode, the returned
// value will point to a block of memory that's at least this big.
//
// Returns an errno-style error.
//
// On success, you must call HNodeTerm before unloading this code.
// On error, you may call HNodeTerm before unloading this code (it will be a NOP).
extern errno_t HNodeInit(lck_grp_t   *lockGroup,
                         lck_attr_t  *lockAttr,
                         OSMallocTag  mallocTag,
                         uint32_t     magic,
                         size_t       fsNodeSize);

// Terminates this module.  Before calling this routine, you must ensure that all
// HNodes have been destroyed (by ensuring that all vnodes that reference these
// HNodes have been reclaimed).  Typically this happens automatically if you make
// sure that all of your VFS plug-in's volumes are unmounted before you call this
// routine.
//
// You must call this routine before unloading this code.
//
// You must not call any other routine in this module (except HNodeInit) after
// calling this routine.
//
// It is safe to call this routine even if you've never called HNodeInit, or if
// HNodeInit failed.
extern void HNodeTerm(void);

// Returns the FSNode associated with an hnode.  hnode must be a valid
// HNode.  This routine can never fail because the HNode and FSNode are allocated
// within the same memory block.
//
// This routine has the word "Generic" in it because it is expected
// that you'll write your own HNodeGetFSNode routine whose return value is
// cast to the type you're using for your FSNode.
extern void *    FSNodeGenericFromHNode(HNodeRef hnode);

// Returns the HNode associated a given FSNode.  fsnode must be a valid
// FSNode.  This routine can never fail because the HNode and FSNode are allocated
// within the same memory block.
//
// This routine has the word "Generic" in it because it is expected
// that you'll write your own FSNodeGetHNode routine whose input is of the
// appropriate type.
extern HNodeRef  HNodeFromFSNodeGeneric(void *fsNode);

// Returns the HNode for a given vnode.  This routine should never fail.
// The only circumstances in which it might fail are if vn is not valid,
// or its not a vnode for your file system, or it has somehow been created
// without an HNode.  All of these are panicworthy.
extern HNodeRef  HNodeFromVNode(vnode_t vn);

// Returns the FSNode for a given vnode.  As this is a composition of
// HNodeFromVNode and FSNodeGenericFromHNode, it shouldn't fail and any
// failures are panicworthy.
extern void *    FSNodeGenericFromVNode(vnode_t vn);


// Gets the device number associated with this HNode.  This is exactly what you passed
// in when you created the HNode using HNodeLookupCreatingIfNecessary.
extern fuse_device_t HNodeGetDevice(HNodeRef hnode);

// Gets the inode number associated with this HNode.  This is exactly what you passed
// in when you created the HNode using HNodeLookupCreatingIfNecessary.
extern uint64_t      HNodeGetInodeNumber(HNodeRef hnode);

// This returns the vnode for a given fork of the HNode, which may be NULL.  As this
// doesn't take any references on the vnode, the results can be stale.
//
// hnode must be a valid HNodeRef.
//
// forkIndex must not be greater than the highest fork index that you've passed to
// HNodeLookupCreatingIfNecessary for this HNode.
//
// Note:
// This routine is more expensive than you might think because it has to take the
// global hash table lock; if possible, it's better to remember the result from
// another routine (like HNodeLookupCreatingIfNecessary) rather than call this routine
// to recover the same information.
extern vnode_t       HNodeGetVNodeForForkAtIndex(HNodeRef hnode,
                                                 size_t forkIndex);

// This returns the fork index of the given vnode within the HNode.
//
// vn must be a valid vnode for a vnode on this file system.  To meet this precondition,
// the caller must hold a reference (typically an I/O reference) on the vnode when
// calling this routine.
//
// Note:
// This routine is more expensive than you might think because it has to take the
// global hash table lock; if possible, it's better to remember the result from
// another routine (like HNodeLookupCreatingIfNecessary) rather than call this routine
// to recover the same information.
extern size_t        HNodeGetForkIndexForVNode(vnode_t vn);

extern void          HNodeExchangeFromFSNode(void *fsnode1, void *fsnode2);

extern errno_t   HNodeLookupRealQuickIfExists(fuse_device_t dev,
                                              uint64_t      ino,
                                              size_t        forkIndex,
                                              HNodeRef     *hnodePtr,
                                              vnode_t      *vnPtr);

// Looks up the HNode in the hash table, and returns a reference to it and to the vnode for
// the specified fork (if one exists).
//
// dev and ino form the hash table key.
//
// forkIndex may be greater than any previous fork index used for this HNode, in which case
// the routine will silently and automatically expand the array used to track the fork vnodes.
//
// hnodePtr must not be NULL; *hnodePtr must be NULL.
//
// vnPtr must not be NULL; *vnPtr must be NULL.
//
// On error, *hnodePtr and *vnPtr will be NULL
//
// On success, *vnPtr will either be NULL or it will be a reference to a vnode.  In the first case,
// the caller must attach a vnode (by calling HNodeAttachVNodeSucceeded) or release the HNode
// (by calling HNodeAttachVNodeFailed).  In the second case, the caller must release the reference
// to the vnode by calling vnode_put.
//
// On success, *hnodePtr will be a reference to an HNode.  This reference will persist for one
// of two reasons:
//
//   o If *vnPtr is NULL, the reference to the HNode will persist because we've set internal state
//     that is cleared by this thread calling either HNodeAttachVNodeSucceeded or HNodeAttachVNodeFailed.
//
//   o If *vnPtr is not NULL, the reference will persist because the HNode is 'owned' by the vnode,
//     and the vnode will persist until the caller drops its reference by calling vnode_put.
//
// IMPORTANT
// The HNode returned by this routine might be newly allocated, in which case the associated
// FSNode will not be initialised.  It is your responsibility for checking whether the FSNode
// has been initialised, and initialising it if it hasn't already been.  This requirement is
// only relevant if the routine succeeds and *vnPtr is NULL.
//
// IMPORTANT
// In other hash layer implementations, the hash layer 'knows' whether an FSNode refers to a
// file system object that has been deleted.  This is not the case here.  If you have an
// FSNode "is deleted" flag, you are responsible for checking it upon return from this routine.
extern errno_t   HNodeLookupCreatingIfNecessary(fuse_device_t dev,
                                                uint64_t      ino,
                                                size_t        forkIndex,
                                                HNodeRef     *hnodePtr,
                                                vnode_t      *vnPtr);

// Attaches a vnode to an HNode.  You can only call this routine after calling
// HNodeLookupCreatingIfNecessary and having it succeed but not return a vnode.
// In that case, you must either call this routine or HNodeAttachVNodeFailed.
//
// hnode must be the HNodeRef returned by HNodeLookupCreatingIfNecessary.
//
// forkIndex must be the same as the forkIndex you passed to HNodeLookupCreatingIfNecessary.
//
// vn must be a valid vnode that you've created using vnode_create; the per-file system data
// for this vnode (that is, the value you passed in the vnfs_fsnode of the (struct vnode_fsparam)
// you passed to vnode_create) must be equal to hnode.
//
// Note
// This routine adds an FS reference to the vnode (it calls vnode_addfsref).
extern void      HNodeAttachVNodeSucceeded(HNodeRef hnode,
                                           size_t   forkIndex,
                                           vnode_t  vn);

// Indicates that an attempt to create a vnode to attach to to an HNode has failed.  You
// can only call this routine after calling HNodeLookupCreatingIfNecessary and having it
// succeed but not return a vnode.  In that case, you must either call this routine or
// HNodeAttachVNodeSucceeded.
//
// hnode must be the HNodeRef returned by HNodeLookupCreatingIfNecessary.
//
// forkIndex must be the same as the forkIndex you passed to HNodeLookupCreatingIfNecessary.
//
// If this routine returns true, you must scrub the FSNode associated with the HNode
// and then call HNodeScrubDone on the HNode.
extern bool HNodeAttachVNodeFailed(HNodeRef hnode, size_t forkIndex);

// Detaches a vnode from an HNode.  You must [should?] only call this from your
// VNOPReclaim routine.
//
// vn must be a valid vnode associated with this HNode
//
// If this routine returns true, you must scrub the FSNode associated with the HNode
// and then call HNodeScrubDone on the HNode.
//
// Note
// This routine removes the FS reference on the vnode (it calls vnode_removefsref).
extern bool HNodeDetachVNode(HNodeRef hnode, vnode_t vn);

// Deallocates an HNode.  You must call this routine on an HNode if either
// HNodeAttachVNodeFailed or HNodeDetachVNode returns true.
extern void      HNodeScrubDone(HNodeRef hnode);

// Prints the current state of this module using log().  This is a debugging aid
// only.  It makes a best attempt to be thead safe, but there are still race conditions.
void             HNodePrintState(void);

#endif /* _FUSE_NODEHASH_H_ */
