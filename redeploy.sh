#!/bin/bash -e

CONFIGURATION=Debug
BUILD_PATH=build/$CONFIGURATION

# kext might not be loaded yet
set +e
sudo kextunload -b org.fuse4x.kext.fuse4x
set -e

xcodebuild -parallelizeTargets -configuration $CONFIGURATION -arch i386 -alltargets
sudo cp -R $BUILD_PATH/fuse4x.kext /tmp
sudo chown -R root:wheel /tmp/fuse4x.kext
sudo kextload /tmp/fuse4x.kext

sudo cp $BUILD_PATH/{load_fusefs,mount_fusefs} /opt/local/bin/
