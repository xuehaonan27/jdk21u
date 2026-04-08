#!/bin/bash
set -e
unset C_INCLUDE_PATH CPLUS_INCLUDE_PATH LD_LIBRARY_PATH LDFLAGS CFLAGS CXXFLAGS CC CXX

cd /home/ubuntu/jvm_libapth/jdk21u

bash configure \
	--with-build-jdk=/usr/lib/jvm/java-21-openjdk-amd64 \
	--with-boot-jdk=/usr/lib/jvm/java-21-openjdk-amd64 \
	--enable-cds=no

make images CONF=linux-x86_64-server-release JOBS=$(nproc)

JDK_IMAGE="build/linux-x86_64-server-release/images/jdk"
"$JDK_IMAGE/bin/java" -Xshare:off -XX:-UseCompressedOops -XX:-UseCompressedClassPointers -version
