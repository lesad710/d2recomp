#!/bin/sh
# Test run in a window on the desktop. ./run.sh [seconds] [game args...]; WIDE=1 for 16:9.
T=${1:-60}; shift
cd "$(dirname "$0")"
[ -n "$WIDE" ] && export D2WIDE=1068
timeout -k 5 "$T" ./build/d2recomp /home/lesad/project/wow/diablo_files run -- -3dfx "$@"
