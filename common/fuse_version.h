/*
 * 'rebel' branch modifications:
 *     Copyright (C) 2010 Tuxera. All Rights Reserved.
 */

/*
 * Copyright (C) 2006-2008 Google. All Rights Reserved.
 * Amit Singh <singh@>
 */

#ifndef _FUSE_VERSION_H_
#define _FUSE_VERSION_H_

#define FUSE4X_STRINGIFY(s)         FUSE4X_STRINGIFY_BACKEND(s)
#define FUSE4X_STRINGIFY_BACKEND(s) #s

/* Add things here. */

#define FUSE4X_FS_TYPE_LITERAL fusefs
#define FUSE4X_FS_TYPE         FUSE4X_STRINGIFY(FUSE4X_FS_TYPE_LITERAL)

#define FUSE4X_BUNDLE_IDENTIFIER_LITERAL \
        com.google.filesystems.FUSE4X_FS_TYPE_LITERAL
#define FUSE4X_BUNDLE_IDENTIFIER \
        FUSE4X_STRINGIFY(FUSE4X_BUNDLE_IDENTIFIER_LITERAL)

#define FUSE4X_BUNDLE_IDENTIFIER_TRUNK_LITERAL  fusefs
#define FUSE4X_BUNDLE_IDENTIFIER_TRUNK \
        FUSE4X_STRINGIFY(FUSE4X_BUNDLE_IDENTIFIER_TRUNK_LITERAL)

#define FUSE4X_TIMESTAMP __DATE__ ", " __TIME__

#define FUSE4X_VERSION_LITERAL 0.8.5
#define FUSE4X_VERSION         FUSE4X_STRINGIFY(FUSE4X_VERSION_LITERAL)

#endif /* _FUSE_VERSION_H_ */
