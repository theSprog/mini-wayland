#!/usr/bin/env bash
set -euo pipefail

rm -f ./mini-wayland.tar

tar --exclude='.git' \
    --exclude='*.tar' \
    --exclude='*.o' \
    -cf mini-wayland.tar *
