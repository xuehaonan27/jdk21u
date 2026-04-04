#!/bin/bash
set -e

# === Clean environment ===
unset C_INCLUDE_PATH CPLUS_INCLUDE_PATH LD_LIBRARY_PATH LDFLAGS CFLAGS CXXFLAGS CC CXX

CC="/usr/bin/gcc"
CXX="/usr/bin/g++"

# === Build LIBAPTH ===
cd /home/ubuntu/jvm_libapth/libapth && make clean && make all || exit

# === Build OpenJDK 21 ===
cd /home/ubuntu/jvm_libapth/jdk21u
JDK_IMAGE="/home/ubuntu/jvm_libapth/jdk21u/build/linux-x86_64-server-release/images/jdk"

# No Boot JDK or Build JDK needed, because we have apt installed a JDK21 to system on local machine

bash configure --with-libapth=/home/ubuntu/jvm_libapth/libapth \
	--with-build-jdk=/usr/lib/jvm/java-21-openjdk-amd64 \
	--with-boot-jdk=/usr/lib/jvm/java-21-openjdk-amd64 \
	--enable-cds=no

make images CONF=linux-x86_64-server-release JOBS=$(nproc)

# Run java using the custom dynamic linker explicitly
LD_PRELOAD=/home/ubuntu/jvm_libapth/libapth/build/lib/libapth.so \
    "$JDK_IMAGE/bin/java" \
    -Xshare:off \
    -XX:-UseCompressedOops \
    -XX:-UseCompressedClassPointers \
    -version

LD_PRELOAD=/home/ubuntu/jvm_libapth/libapth/build/lib/libapth.so \
    "$JDK_IMAGE/bin/java" \
    -Xshare:off \
    -XX:-UseCompressedOops \
    -XX:-UseCompressedClassPointers \
    -Xint \
    HelloWorld.java
