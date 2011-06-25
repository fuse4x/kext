#!/usr/bin/env ruby
# Possible flags are:
#   --debug       this builds distribuition with debug flags enabled
#   --root DIR    install the binary into this directory. If this flag is not set - the script
#                 redeploys kext to local machine and restarts it
#   --clean       clean before build

CWD = File.dirname(__FILE__)
KEXT_DIR = '/System/Library/Extensions/'
Dir.chdir(CWD)

debug = ARGV.include?('--debug')
clean = ARGV.include?('--clean')
root_dir = ARGV.index('--root') ? ARGV[ARGV.index('--root') + 1] : nil

abort("root directory #{root_dir} does not exist") if ARGV.index('--root') and not File.exists?(root_dir)

system('git clean -xdf') if clean

configuration = debug ? 'Debug' : 'Release'
system("xcodebuild -parallelizeTargets -configuration #{configuration} -alltargets") or abort("cannot build kext")

unless root_dir
  # we need to reload the kext
  if system('kextstat | grep org.fuse4x.kext.fuse4x') then
    system('sudo kextunload -b org.fuse4x.kext.fuse4x') or abort('cannot unload the kext')
  end
end

install_path = root_dir ? File.join(root_dir, KEXT_DIR) : KEXT_DIR
system("sudo mkdir -p #{install_path}") if root_dir

system("sudo cp -R build/#{configuration}/fuse4x.kext #{install_path}") or abort
system("sudo chown -R root:wheel #{install_path}/fuse4x.kext") or abort
