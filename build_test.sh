#!/bin/bash
set -e

# === Clean environment ===
unset C_INCLUDE_PATH CPLUS_INCLUDE_PATH LD_LIBRARY_PATH LDFLAGS CFLAGS CXXFLAGS CC CXX

# === Build OpenJDK 21 ===
cd /home/xuehaonan/jdks/jdk21u

JDK_IMAGE="/home/xuehaonan/jdks/jdk21u/build/linux-x86_64-server-release/images/jdk"

# Run java using the custom dynamic linker explicitly
LD_PRELOAD=/home/xuehaonan/libapth/build/lib/libapth.so \
  "$JDK_IMAGE/bin/java" \
  -Xshare:off -XX:-UseCompressedOops -XX:-UseCompressedClassPointers \
  -version

LD_PRELOAD=/home/xuehaonan/libapth/build/lib/libapth.so \
  "$JDK_IMAGE/bin/java" \
  -Xshare:off -XX:-UseCompressedOops -XX:-UseCompressedClassPointers \
  -Xint -Xmx64m GCStressTest.java

LD_PRELOAD=/home/xuehaonan/libapth/build/lib/libapth.so \
  "$JDK_IMAGE/bin/java" \
  -Xshare:off -XX:-UseCompressedOops -XX:-UseCompressedClassPointers \
  -Xint -Xmx128m -XX:+UnlockDiagnosticVMOptions
