#!/bin/bash

# bash configure \
#     --with-boot-jdk=/home/xuehaonan/jdks/jdk-20.0.1 \
#     --with-extra-cflags="-DXHN_THREAD_MAJFLT -DXHN_EVAC_RC -DXHN_COUNT_RC" \
#     --with-extra-cxxflags="-DXHN_THREAD_MAJFLT -DXHN_EVAC_RC -DXHN_COUNT_RC"

# bash configure \
#     --with-boot-jdk=/home/xuehaonan/jdks/jdk-20.0.1 \
#     --with-extra-cflags="-DXHN_THREAD_MAJFLT -DXHN_EVAC_RC" \
#     --with-extra-cxxflags="-DXHN_THREAD_MAJFLT -DXHN_EVAC_RC"


# Pth:
# ----------------------------------------------------------------------
# Libraries have been installed in:
#    /usr/local/lib

# If you ever happen to want to link against installed libraries
# in a given directory, LIBDIR, you must either use libtool, and
# specify the full pathname of the library, or use the `-LLIBDIR'
# flag during linking and do at least one of the following:
#    - add LIBDIR to the `LD_LIBRARY_PATH' environment variable
#      during execution
#    - add LIBDIR to the `LD_RUN_PATH' environment variable
#      during linking
#    - use the `-Wl,--rpath -Wl,LIBDIR' linker flag
#    - have your system administrator add LIBDIR to `/etc/ld.so.conf'

# See any operating system documentation about shared libraries for
# more information, such as the ld(1) and ld.so(8) manual pages.
# ----------------------------------------------------------------------

# PTH_INCLUDE="/usr/local/include"
# PTH_LIB="/usr/local/lib"

# bash configure \
#     --with-boot-jdk=/home/xuehaonan/jdks/jdk-20.0.1 \
#     --with-extra-cflags="-include ${PTH_INCLUDE}/pthread.h -I${PTH_INCLUDE} -DXHN_THREAD_MAJFLT -DXHN_EVAC_RC -DXHN_COUNT_RC -DXHN_BARRIER" \
#     --with-extra-cxxflags="-include ${PTH_INCLUDE}/pthread.h -I${PTH_INCLUDE} -DXHN_THREAD_MAJFLT -DXHN_EVAC_RC -DXHN_COUNT_RC -DXHN_BARRIER" \
#     --with-extra-ldflags="-L${PTH_LIB} -Wl,--rpath -Wl,${PTH_LIB}" \
#     --disable-warnings-as-errors

    # --with-extra-cflags="-include ${PTH_INCLUDE}/pthread.h -I${PTH_INCLUDE} -DXHN_THREAD_MAJFLT -DXHN_EVAC_RC -DXHN_COUNT_RC -DXHN_BARRIER" \
    # --with-extra-cxxflags="-include ${PTH_INCLUDE}/pthread.h -I${PTH_INCLUDE} -DXHN_THREAD_MAJFLT -DXHN_EVAC_RC -DXHN_COUNT_RC -DXHN_BARRIER" \

bash configure \
    --with-boot-jdk=/home/xuehaonan/jdks/jdk-20.0.1 \
    --with-extra-cflags="-DXHN_THREAD_MAJFLT -DXHN_EVAC_RC -DXHN_COUNT_RC -DXHN_BARRIER" \
    --with-extra-cxxflags="-DXHN_THREAD_MAJFLT -DXHN_EVAC_RC -DXHN_COUNT_RC -DXHN_BARRIER"
    
    # --with-debug-level=slowdebug

make CONF=release images
# make CONF=slowdebug images
