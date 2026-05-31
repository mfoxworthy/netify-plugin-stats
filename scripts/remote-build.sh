#!/bin/sh
# Build bridge: sync this repo to the build host and run a command there.
#
# The local mac lacks the toolchain (no autotools, libnetifyd, libuci); the
# build host mfoxworthy@10.0.4.220 has libnetifyd 5.2.7 + autotools natively
# and an OpenWRT SDK for cross-building the router .ipk. Key-based SSH is set
# up, so no password is needed.
#
# Usage:
#   scripts/remote-build.sh                 # default: autogen + configure + make check
#   scripts/remote-build.sh '<command>'     # run an arbitrary command in the remote tree
set -e

HOST="${NSP_BUILD_HOST:-mfoxworthy@10.0.4.220}"
REMOTE_DIR="${NSP_REMOTE_DIR:-/home/mfoxworthy/netify-plugin-stats-build}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"

CMD="${1:-./autogen.sh && ./configure && make check}"

echo ">> syncing $HERE -> $HOST:$REMOTE_DIR"
rsync -az --delete \
    --exclude '.git/' \
    --exclude 'automake/' --exclude 'm4/' \
    --exclude '*.o' --exclude '*.lo' --exclude '*.la' --exclude '.libs/' --exclude '.deps/' \
    --exclude 'autom4te.cache/' --exclude 'configure' --exclude 'Makefile.in' \
    "$HERE"/ "$HOST:$REMOTE_DIR"/

echo ">> running on $HOST: $CMD"
ssh -o BatchMode=yes "$HOST" "cd '$REMOTE_DIR' && $CMD"
