#!/bin/bash
# ===========================================================================
# build.sh — Build & test JDK on the REMOTE experiment server (xhn28_tailscale)
# ===========================================================================
#
# Remote server: Ubuntu 18.04, GCC 7.5, GLIBC 2.27, custom GLIBC 2.43 toolchain
# SSH: ssh xhn28_tailscale
# $HOME: /home/xuehaonan
# JDK: /home/xuehaonan/jdks/jdk21u
# LIBAPTH: /home/xuehaonan/libapth
#
# Usage:
#   bash build.sh                          # Default: release, LIBAPTH+RDMA, build+test
#   REMOTE_BACKEND=TCP bash build.sh       # Use TCP backend instead of RDMA
#   REMOTE_BACKEND=SIM bash build.sh       # Use simulation (no network)
#   DEBUG_LEVEL=fastdebug bash build.sh    # Fastdebug build
#   JIT_LEVEL=0 bash build.sh             # Test interpreter-only
#   TAG_REFSITES=1 bash build.sh          # Enable bulk ref-site tagging
#   ACTION=test TEST_ROUNDS=100 bash build.sh  # 100-round stress test only
#
# Variables (same as build_local.sh):
#   USE_LIBAPTH     1 (default) | 0
#   REMOTE_BACKEND  RDMA (default) | TCP | SIM
#   DEBUG_LEVEL     release (default) | fastdebug | slowdebug
#   JIT_LEVEL       default | 0 | 1 | 3 | 4 | c2only
#   TAG_REFSITES    0 (default) | 1
#   ACTION          buildtest (default) | build | test
#   TEST_ROUNDS     1 (default)
#
# ===========================================================================

set -e

# ========================== Configurable Variables ==========================

USE_LIBAPTH=${USE_LIBAPTH:-1}
REMOTE_BACKEND=${REMOTE_BACKEND:-RDMA}
DEBUG_LEVEL=${DEBUG_LEVEL:-release}
JIT_LEVEL=${JIT_LEVEL:-default}
TAG_REFSITES=${TAG_REFSITES:-0}
ACTION=${ACTION:-buildtest}
TEST_ROUNDS=${TEST_ROUNDS:-1}

# ========================== Remote Server Paths ============================

BASEDIR="/home/xuehaonan"
JDKDIR="$BASEDIR/jdks/jdk21u"
LIBAPTH_DIR="$BASEDIR/libapth"
BOOT_JDK="$BASEDIR/jdks/jdk-20.0.1"
BUILD_JDK="$BASEDIR/jdks/jdk-21.0.6+7"
CONF="linux-x86_64-server-${DEBUG_LEVEL}"
JDK_IMAGE="$JDKDIR/build/$CONF/images/jdk"

# Custom toolchain (GCC 14 + GLIBC 2.43 for Ubuntu 18.04)
CUSTOM_PREFIX=${CUSTOM_PREFIX:-"$BASEDIR/custom-toolchain"}
CUSTOM_SYSROOT=${CUSTOM_SYSROOT:-"$BASEDIR/custom-sysroot"}

# Clean environment
unset C_INCLUDE_PATH CPLUS_INCLUDE_PATH LD_LIBRARY_PATH LDFLAGS CFLAGS CXXFLAGS CC CXX

# ========================== Build Phase ====================================

if [[ "$ACTION" == "build" || "$ACTION" == "buildtest" ]]; then
    echo "=== Build Configuration ==="
    echo "  DEBUG_LEVEL:    $DEBUG_LEVEL"
    echo "  USE_LIBAPTH:    $USE_LIBAPTH"
    echo "  REMOTE_BACKEND: $REMOTE_BACKEND"
    echo "  CONF:           $CONF"
    echo ""

    # Build LIBAPTH
    if [[ "$USE_LIBAPTH" == "1" ]]; then
        echo "=== Building LIBAPTH ==="
        cd "$LIBAPTH_DIR" && make clean && make all && make core || exit 1
    fi

    # Configure JDK
    echo "=== Configuring JDK ==="
    cd "$JDKDIR"

    export PKG_CONFIG_PATH="/usr/lib/x86_64-linux-gnu/pkgconfig:/usr/share/pkgconfig:$PKG_CONFIG_PATH"

    CONFIGURE_ARGS=(
        --x-includes=/usr/include
        --x-libraries=/usr/lib/x86_64-linux-gnu
        --with-boot-jdk="$BOOT_JDK"
        --with-build-jdk="$BUILD_JDK"
        --with-toolchain-type=gcc
        --with-native-debug-symbols=none
        --with-jvm-variants=server
        --disable-warnings-as-errors
        --enable-unlimited-crypto
        --with-freetype=bundled
        --with-zlib=bundled
        --enable-cds=no
    )

    if [[ "$DEBUG_LEVEL" != "release" ]]; then
        CONFIGURE_ARGS+=(--with-debug-level="$DEBUG_LEVEL")
    fi

    if [[ "$USE_LIBAPTH" == "1" ]]; then
        CONFIGURE_ARGS+=(--with-libapth="$LIBAPTH_DIR")
        CONFIGURE_ARGS+=(--with-remote="$REMOTE_BACKEND")
    fi

    bash configure "${CONFIGURE_ARGS[@]}"

    # Build
    echo "=== Building JDK ==="
    make images CONF="$CONF" JOBS=$(nproc)

    # Patchelf for custom GLIBC (remote server only)
    if [[ -f "$CUSTOM_SYSROOT/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2" ]]; then
        echo "=== Patching ELF binaries for custom GLIBC ==="
        CUSTOM_DYNLINKER="$CUSTOM_SYSROOT/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2"
        find "$JDK_IMAGE" "$LIBAPTH_DIR" -type f \( -name "java" -o -name "javac" -o -name "jar" -o -name "*.so" -o -name "*.so.*" \) | while read f; do
            if file "$f" | grep -q "ELF"; then
                OLD_RPATH=$(patchelf --print-rpath "$f" 2>/dev/null || echo "")
                NEW_RPATH="$CUSTOM_SYSROOT/lib/x86_64-linux-gnu:$CUSTOM_PREFIX/lib64:$CUSTOM_PREFIX/lib"
                [ -n "$OLD_RPATH" ] && NEW_RPATH="$NEW_RPATH:$OLD_RPATH"
                patchelf --set-interpreter "$CUSTOM_DYNLINKER" "$f" 2>/dev/null || true
                patchelf --set-rpath "$NEW_RPATH" "$f" 2>/dev/null || true
            fi
        done
    fi

    # Verification
    echo "=== Verification ==="
    if [[ "$USE_LIBAPTH" == "1" ]]; then
        LD_PRELOAD="$LIBAPTH_DIR/build/lib/libapth.so" \
            "$JDK_IMAGE/bin/java" -Xshare:off -XX:-UseCompressedOops -XX:-UseCompressedClassPointers -version
    else
        "$JDK_IMAGE/bin/java" -Xshare:off -XX:-UseCompressedOops -XX:-UseCompressedClassPointers -version
    fi

    echo "=== Build complete: $JDK_IMAGE ==="
fi

# ========================== Test Phase =====================================

if [[ "$ACTION" == "test" || "$ACTION" == "buildtest" ]]; then
    JAVA_CMD="$JDK_IMAGE/bin/java"
    JAVA_OPTS=(-Xshare:off -XX:-UseCompressedOops -XX:-UseCompressedClassPointers -Xmx256m)

    case "$JIT_LEVEL" in
        0)       JAVA_OPTS+=(-Xint); JIT_DESC="interpreter-only" ;;
        1)       JAVA_OPTS+=(-XX:TieredStopAtLevel=1); JIT_DESC="C1-only" ;;
        3)       JAVA_OPTS+=(-XX:TieredStopAtLevel=3); JIT_DESC="C1-full" ;;
        4)       JAVA_OPTS+=(-XX:TieredStopAtLevel=4); JIT_DESC="C1+C2" ;;
        c2only)  JAVA_OPTS+=(-XX:-TieredCompilation); JIT_DESC="C2-only" ;;
        default) JIT_DESC="full-tiered" ;;
    esac

    if [[ "$TAG_REFSITES" == "1" ]]; then
        JAVA_OPTS+=(-XX:+UnlockDiagnosticVMOptions -XX:+G1TagRefSites)
    fi

    # Compile StressTest
    if [[ -f "$JDKDIR/StressTest.java" ]]; then
        mkdir -p /tmp/stress
        javac -d /tmp/stress "$JDKDIR/StressTest.java" 2>/dev/null || true
    fi

    echo "=== Test: $JIT_DESC, libapth=$USE_LIBAPTH, tagging=$TAG_REFSITES, rounds=$TEST_ROUNDS ==="

    PASS=0; FAIL=0
    for i in $(seq 1 $TEST_ROUNDS); do
        if [[ "$USE_LIBAPTH" == "1" ]]; then
            LD_PRELOAD="$LIBAPTH_DIR/build/lib/libapth.so" \
                "$JAVA_CMD" "${JAVA_OPTS[@]}" -cp /tmp/stress StressTest > /tmp/stress_run_$i.txt 2>&1
        else
            "$JAVA_CMD" "${JAVA_OPTS[@]}" -cp /tmp/stress StressTest > /tmp/stress_run_$i.txt 2>&1
        fi
        RC=$?
        if [ $RC -eq 0 ]; then
            PASS=$((PASS+1))
            if [ $((i % 20)) -eq 0 ] || [ $i -eq $TEST_ROUNDS ]; then
                echo "Round $i/$TEST_ROUNDS: PASS ($PASS passed)"
            fi
        else
            FAIL=$((FAIL+1))
            echo "Round $i/$TEST_ROUNDS: FAIL (rc=$RC)"
        fi
    done

    echo "=== RESULT: $PASS/$TEST_ROUNDS passed ==="
    [ $FAIL -gt 0 ] && exit 1
fi
