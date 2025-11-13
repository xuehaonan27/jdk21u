#!/bin/bash

bash configure \
    --with-debug-level=slowdebug \
    --with-boot-jdk=/home/xuehaonan/jdks/jdk-20.0.1 \
    --with-extra-cflags="-DXHN_THREAD_MAJFLT -DXHN_EVAC_RC" \
    --with-extra-cxxflags="-DXHN_THREAD_MAJFLT -DXHN_EVAC_RC"


make CONF=release images
# make CONF=slowdebug images
