#!/usr/bin/env ruby
# Possible flags are:
#   --release     build this module for final distribution
#   --root DIR    install the binary into this directory. If this flag is not set - the script
#                 redeploys kext to local machine and restarts it

CWD = File.dirname(__FILE__)
KEXT_DIR = '/System/Library/Extensions/'
Dir.chdir(CWD)

release = ARGV.include?('--release')
root_dir = ARGV.index('--root') ? ARGV[ARGV.index('--root') + 1] : nil

abort("root directory #{root_dir} does not exist") if ARGV.index('--root') and not File.exists?(root_dir)

system('git clean -xdf') if release

# Kext uses special configuration for final release.
# This configuration sets base SDK to 10.5 for i386 arch and 10.6 for x86_64, this
# is needed because kexts does not have forward compatibility and one have to compile a kext
# with SDK that matches target platform (in our case it is 10.5).
# load_fuse4x is a user-space program and still uses default SDK + target set to 10.5
configuration = release ? 'Distribution' : 'Debug'
flags = '-configuration ' + configuration

version = `git describe --tags --dirty`.chomp
flags += " GCC_PREPROCESSOR_DEFINITIONS=FUSE4X_KEXT_VERSION=#{version}"

system("xcodebuild SYMROOT=build SHARED_PRECOMPS_DIR=build -PBXBuildsContinueAfterErrors=0 -parallelizeTargets -alltargets #{flags}") or abort("cannot build kext")

unless root_dir
  # we need to reload the kext
  if system('kextstat | grep org.fuse4x.kext.fuse4x') then
    system('sudo kextunload -b org.fuse4x.kext.fuse4x') or abort('cannot unload the kext')
  end
end

install_path = root_dir ? File.join(root_dir, KEXT_DIR) : KEXT_DIR
system("sudo mkdir -p #{install_path}") if root_dir

system("sudo cp -R build/#{configuration}/fuse4x.kext #{install_path}") or abort
# Install load_fuse4x tool. The tool should have +s bit set to allow
# non-priveleged users mount fuse4x filesystem and init sysctl
system("sudo mkdir -p #{install_path}/fuse4x.kext/Support") or abort
system("sudo cp build/#{configuration}/load_fuse4x #{install_path}/fuse4x.kext/Support") or abort
system("sudo chmod +s #{install_path}/fuse4x.kext/Support/load_fuse4x")

system("sudo chown -R root:wheel #{install_path}/fuse4x.kext") or abort
