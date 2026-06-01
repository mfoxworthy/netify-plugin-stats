#!/bin/sh
# Cross-build the netify-plugin-stats .ipk using the OpenWRT SDK on the build
# host. Run this from the repo root on the mac:  scripts/sdk-build.sh
#
# It syncs the repo to the build host, stages the package into the SDK's
# package/ tree (Makefile + a "srctree" copy of the source the package builds
# from), and runs the SDK package compile. The resulting .ipk path is printed.
set -e

HOST="${NSP_BUILD_HOST:-mfoxworthy@10.0.4.220}"
REMOTE_DIR="${NSP_REMOTE_DIR:-/home/mfoxworthy/netify-plugin-stats-build}"
SDK_DIR="${NSP_SDK_DIR:-/home/mfoxworthy/openwrt}"
PKG_DIR="$SDK_DIR/package/netify-plugin-stats"
HERE="$(cd "$(dirname "$0")/.." && pwd)"

echo ">> syncing repo -> $HOST:$REMOTE_DIR"
rsync -az --delete \
    --exclude '.git/' --exclude 'automake/' --exclude 'm4/' \
    --exclude '*.o' --exclude '*.lo' --exclude '*.la' --exclude '.libs/' --exclude '.deps/' \
    --exclude 'autom4te.cache/' --exclude 'configure' --exclude 'Makefile.in' \
    "$HERE"/ "$HOST:$REMOTE_DIR"/

echo ">> staging package into SDK at $PKG_DIR"
ssh -o BatchMode=yes "$HOST" "
  set -e
  rm -rf '$PKG_DIR'
  mkdir -p '$PKG_DIR'
  cp '$REMOTE_DIR/deploy/openwrt/Makefile' '$PKG_DIR/Makefile'
  cp -r '$REMOTE_DIR/deploy/openwrt/files' '$PKG_DIR/files'
  # srctree = the buildable autotools source the package compiles.
  mkdir -p '$PKG_DIR/srctree'
  rsync -a --exclude '.git' --exclude 'deploy' --exclude 'scripts' \
        '$REMOTE_DIR'/ '$PKG_DIR/srctree'/
  cd '$SDK_DIR'
  echo '>> make package/netify-plugin-stats/compile'
  make package/netify-plugin-stats/{clean,compile} V=s ${NSP_MAKE_JOBS:+-j$NSP_MAKE_JOBS} 2>&1 | tail -60
  echo '>> built .ipk(s):'
  find '$SDK_DIR/bin' -name 'netify-plugin-stats*.ipk' 2>/dev/null
"
