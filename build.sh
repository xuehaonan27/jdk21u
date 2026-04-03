#!/bin/bash
set -e

# === Clean environment ===
unset C_INCLUDE_PATH CPLUS_INCLUDE_PATH LD_LIBRARY_PATH LDFLAGS CFLAGS CXXFLAGS CC CXX

CC="$CUSTOM_PREFIX/bin/custom-gcc" CXX="$CUSTOM_PREFIX/bin/custom-g++"

# === Build LIBAPTH ===
cd /home/xuehaonan/libapth && make clean && make all || exit

# === Build OpenJDK 21 ===
cd /home/xuehaonan/jdks/jdk21u

BOOT_JDK=/home/xuehaonan/jdks/jdk-20.0.1
BUILD_JDK=/home/xuehaonan/jdks/jdk-21.0.6+7

# Use gcc-jdk/g++-jdk wrappers which bake in:
#   - System headers (-I/usr/include) for ALSA, X11, CUPS, etc.
#   - System libraries (-L/usr/lib/x86_64-linux-gnu) for linking
#   - Custom dynamic linker (GLIBC 2.43) for runtime
#   - rpath to custom GLIBC 2.43 for runtime
#
# We do NOT compile against GLIBC 2.43 headers — that causes
# __isoc23_sscanf link errors because GCC 14 + GLIBC 2.43 headers
# redirect libc calls to C23 symbols that the system GLIBC 2.27
# linker libraries don't have.
#
# Instead: compile against system GLIBC 2.27 → patchelf at the end
# makes everything run against GLIBC 2.43. This works because GLIBC
# is backward compatible.

export PKG_CONFIG_PATH="/usr/lib/x86_64-linux-gnu/pkgconfig:/usr/share/pkgconfig:/usr/lib/pkgconfig:$PKG_CONFIG_PATH"

bash configure \
  --x-includes=/usr/include \
  --x-libraries=/usr/lib/x86_64-linux-gnu \
  --with-boot-jdk="$BOOT_JDK" \
  --with-build-jdk="$BUILD_JDK" \
  --with-toolchain-type=gcc \
  --with-native-debug-symbols=none \
  --with-jvm-variants=server \
  --disable-warnings-as-errors \
  --enable-unlimited-crypto \
  --with-freetype=bundled \
  --with-zlib=bundled \
  --with-libapth=/home/xuehaonan/libapth

make images CONF=linux-x86_64-server-release JOBS=$(nproc)

# === Patchelf: point all binaries to custom GLIBC 2.43 ===
CUSTOM_DYNLINKER="$CUSTOM_SYSROOT/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2"
JDK_IMAGE="/home/xuehaonan/jdks/jdk21u/build/linux-x86_64-server-release/images/jdk"
LIBAPTH_DIR="/home/xuehaonan/libapth"

echo "Patching ELF binaries to use custom GLIBC 2.43..."
find "$JDK_IMAGE" "$LIBAPTH_DIR" -type f \( -name "java" -o -name "javac" -o -name "jar" -o -name "*.so" -o -name "*.so.*" \) | while read f; do
  if file "$f" | grep -q "ELF"; then
    OLD_RPATH=$(patchelf --print-rpath "$f" 2>/dev/null || echo "")
    NEW_RPATH="$CUSTOM_SYSROOT/lib/x86_64-linux-gnu:$CUSTOM_PREFIX/lib64:$CUSTOM_PREFIX/lib"
    if [ -n "$OLD_RPATH" ]; then
      NEW_RPATH="$NEW_RPATH:$OLD_RPATH"
    fi
    patchelf --set-interpreter "$CUSTOM_DYNLINKER" "$f" 2>/dev/null || true
    patchelf --set-rpath "$NEW_RPATH" "$f" 2>/dev/null || true
    echo "  Patched: $f"
  fi
done

# === CDS Archive Generation ===
# CDS is skipped during build (LIBAPTH JVM needs LD_PRELOAD at runtime).
# Generate the default shared archive post-build so -Xshare:off is not needed.
echo ""
echo "=== Generating CDS archive ==="
LD_PRELOAD=/home/xuehaonan/libapth/build/lib/libapth.so \
  "$JDK_IMAGE/bin/java" -Xshare:dump \
    -Xmx128M -Xms128M \
    -XX:SharedArchiveFile="$JDK_IMAGE/lib/server/classes.jsa" \
  && echo "CDS archive created successfully" \
  || echo "WARNING: CDS archive generation failed (non-fatal)"

echo ""
echo "=== Verification ==="
echo ""

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
  -Xint -Xmx128m -XX:+UnlockDiagnosticVMOptions \
  -XX:+G1SimulateRemoteEviction -Xlog:gc RemoteMemTest.java
