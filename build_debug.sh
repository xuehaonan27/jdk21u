#!/bin/bash
set -e

# === Clean environment ===
unset C_INCLUDE_PATH CPLUS_INCLUDE_PATH LD_LIBRARY_PATH LDFLAGS CFLAGS CXXFLAGS CC CXX

CC="/usr/bin/gcc"
CXX="/usr/bin/g++"

# === Build LIBAPTH ===
cd /home/ubuntu/jvm_libapth/libapth && make clean && make all || exit

# === Build OpenJDK 21 (slowdebug) ===
cd /home/ubuntu/jvm_libapth/jdk21u

bash configure \
	--with-build-jdk=/usr/lib/jvm/java-21-openjdk-amd64 \
	--with-boot-jdk=/usr/lib/jvm/java-21-openjdk-amd64 \
	--enable-cds=no \
	--with-debug-level=fastdebug

make images CONF=linux-x86_64-server-fastdebug JOBS=$(nproc)

JDK_IMAGE="/home/ubuntu/jvm_libapth/jdk21u/build/linux-x86_64-server-fastdebug/images/jdk"

"$JDK_IMAGE/bin/java" \
    -Xshare:off \
    -XX:-UseCompressedOops \
    -XX:-UseCompressedClassPointers \
    -Xint \
    -version
