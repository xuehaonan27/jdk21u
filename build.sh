#!/bin/bash

bash configure \
    --with-boot-jdk=/home/xuehaonan/jdks/jdk-20.0.1 \
    --with-extra-cflags="-DXHN_THREAD_MAJFLT"

make CONF=release images
