#!/bin/bash

bash configure \
    --with-boot-jdk=/home/xuehaonan/jdks/jdk-20.0.1 \
    --with-extra-cflags="-DXHN_THREAD_MAJFLT" \
    --with-extra-cflags="-DXHN_REBUILD_RC" \
    --with-extra-cflags="-DXHN_EVAC_RC" \
    --with-extra-cflags="-DXHN_EVAC_ROOTS"

make CONF=release images
