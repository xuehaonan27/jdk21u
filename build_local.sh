#!/bin/bash
# ===========================================================================
# build_local.sh — Build & test JDK on the LOCAL development machine
# ===========================================================================
#
# Usage:
#   bash build_local.sh                    # Default: release, with LIBAPTH, build+test
#   USE_LIBAPTH=0 bash build_local.sh      # Without LIBAPTH
#   DEBUG_LEVEL=fastdebug bash build_local.sh  # Fastdebug build
#   JIT_LEVEL=0 bash build_local.sh        # Test interpreter-only
#   ACTION=build bash build_local.sh       # Build only, no test
#   ACTION=test bash build_local.sh        # Test only, no build
#   TEST_ROUNDS=100 bash build_local.sh    # 100-round stress test
#
# ===========================================================================

set -e

# ========================== Configurable Variables ==========================

# USE_LIBAPTH: Whether to link LIBAPTH (M:N threading library)
#   1 = link libapth (default) — enables M:N threading, USE_LIBAPTH macro,
#       apth_* TLS functions. Requires LD_PRELOAD at runtime.
#   0 = no libapth — standard pthreads. Useful for debugging signal handler
#       issues (LIBAPTH hooks sigaction/open/write).
USE_LIBAPTH=${USE_LIBAPTH:-1}

# DEBUG_LEVEL: JVM build optimization level
#   release   = -O2, no asserts, production performance (default)
#   fastdebug = -O1, asserts enabled, debug symbols. Best for debugging —
#               catches tagged-oop issues (check_oop, decode_not_null, last_sp)
#   slowdebug = -O0, full asserts, very slow. May trigger LIBAPTH TLS issues.
DEBUG_LEVEL=${DEBUG_LEVEL:-release}

# JIT_LEVEL: Which JIT compiler tiers to enable during testing
#   default = full tiered compilation (interpreter → C1 → C2)
#   0       = interpreter only (-Xint). Slowest but simplest — tests interpreter barrier.
#   1       = C1 only (-XX:TieredStopAtLevel=1). Tests C1 fused load+barrier.
#   3       = C1 full optimization (-XX:TieredStopAtLevel=3). No C2.
#   4       = C1+C2 full tiered (-XX:TieredStopAtLevel=4). Same as default.
#   c2only  = C2 only (-XX:-TieredCompilation). Skips C1, goes interpreter→C2.
JIT_LEVEL=${JIT_LEVEL:-default}

# TAG_REFSITES: Enable bulk ref-site tagging during classification
#   1 = enable G1TagRefSites — tags ~272K heap oop slots per GC (default for test)
#   0 = disable — only mark word bits, no heap slot tagging
TAG_REFSITES=${TAG_REFSITES:-1}

# ACTION: What to do
#   build     = configure + make only
#   test      = test only (assumes already built)
#   buildtest = build then test (default)
ACTION=${ACTION:-buildtest}

# TEST_ROUNDS: Number of StressTest repetitions
#   1   = single run (quick check)
#   20  = moderate confidence
#   100 = full confidence (default)
TEST_ROUNDS=${TEST_ROUNDS:-100}

# ========================== Derived Variables ==============================

BASEDIR="/home/ubuntu/jvm_libapth"
JDKDIR="$BASEDIR/jdk21u"
LIBAPTH_DIR="$BASEDIR/libapth"
BOOT_JDK="/usr/lib/jvm/java-21-openjdk-amd64"
CONF="linux-x86_64-server-${DEBUG_LEVEL}"
JDK_IMAGE="$JDKDIR/build/$CONF/images/jdk"

# Clean environment
unset C_INCLUDE_PATH CPLUS_INCLUDE_PATH LD_LIBRARY_PATH LDFLAGS CFLAGS CXXFLAGS CC CXX

# ========================== Build Phase ====================================

if [[ "$ACTION" == "build" || "$ACTION" == "buildtest" ]]; then
    echo "=== Build Configuration ==="
    echo "  DEBUG_LEVEL: $DEBUG_LEVEL"
    echo "  USE_LIBAPTH: $USE_LIBAPTH"
    echo "  CONF:        $CONF"
    echo ""

    # Build LIBAPTH if enabled
    if [[ "$USE_LIBAPTH" == "1" ]]; then
        echo "=== Building LIBAPTH ==="
        cd "$LIBAPTH_DIR" && make clean && make all || exit 1
    fi

    # Configure JDK
    echo "=== Configuring JDK ==="
    cd "$JDKDIR"

    CONFIGURE_ARGS=(
        --with-build-jdk="$BOOT_JDK"
        --with-boot-jdk="$BOOT_JDK"
        --enable-cds=no
    )

    if [[ "$DEBUG_LEVEL" != "release" ]]; then
        CONFIGURE_ARGS+=(--with-debug-level="$DEBUG_LEVEL")
    fi

    if [[ "$USE_LIBAPTH" == "1" ]]; then
        CONFIGURE_ARGS+=(--with-libapth="$LIBAPTH_DIR")
    fi

    bash configure "${CONFIGURE_ARGS[@]}"

    # Build
    echo "=== Building JDK ==="
    make images CONF="$CONF" JOBS=$(nproc)

    # Quick verification
    echo "=== Verification ==="
    JAVA_CMD="$JDK_IMAGE/bin/java"
    JAVA_OPTS=(-Xshare:off -XX:-UseCompressedOops -XX:-UseCompressedClassPointers)

    if [[ "$USE_LIBAPTH" == "1" ]]; then
        LD_PRELOAD="$LIBAPTH_DIR/build/lib/libapth.so" "$JAVA_CMD" "${JAVA_OPTS[@]}" -version
    else
        "$JAVA_CMD" "${JAVA_OPTS[@]}" -version
    fi

    echo ""
    echo "=== Build complete: $JDK_IMAGE ==="
fi

# ========================== Test Phase =====================================

if [[ "$ACTION" == "test" || "$ACTION" == "buildtest" ]]; then
    JAVA_CMD="$JDK_IMAGE/bin/java"
    JAVA_OPTS=(-Xshare:off -XX:-UseCompressedOops -XX:-UseCompressedClassPointers -Xmx256m)

    # JIT level flags
    case "$JIT_LEVEL" in
        0)       JAVA_OPTS+=(-Xint); JIT_DESC="interpreter-only" ;;
        1)       JAVA_OPTS+=(-XX:TieredStopAtLevel=1); JIT_DESC="C1-only" ;;
        3)       JAVA_OPTS+=(-XX:TieredStopAtLevel=3); JIT_DESC="C1-full" ;;
        4)       JAVA_OPTS+=(-XX:TieredStopAtLevel=4); JIT_DESC="C1+C2" ;;
        c2only)  JAVA_OPTS+=(-XX:-TieredCompilation); JIT_DESC="C2-only" ;;
        default) JIT_DESC="full-tiered" ;;
        *)       echo "Unknown JIT_LEVEL: $JIT_LEVEL"; exit 1 ;;
    esac

    # Tag ref-sites flag
    if [[ "$TAG_REFSITES" == "1" ]]; then
        JAVA_OPTS+=(-XX:+UnlockDiagnosticVMOptions -XX:+G1TagRefSites)
    fi

    # LD_PRELOAD for LIBAPTH
    PRELOAD_CMD=""
    if [[ "$USE_LIBAPTH" == "1" ]]; then
        PRELOAD_CMD="LD_PRELOAD=$LIBAPTH_DIR/build/lib/libapth.so"
    fi

    # Compile StressTest if needed
    if [[ ! -f /tmp/stress/StressTest.class ]] || \
       [[ "$JDKDIR/StressTest.java" -nt /tmp/stress/StressTest.class ]]; then
        echo "=== Compiling StressTest ==="
        javac -d /tmp/stress "$JDKDIR/StressTest.java"
    fi

    echo "=== Test Configuration ==="
    echo "  JIT_LEVEL:    $JIT_LEVEL ($JIT_DESC)"
    echo "  TAG_REFSITES: $TAG_REFSITES"
    echo "  USE_LIBAPTH:  $USE_LIBAPTH"
    echo "  TEST_ROUNDS:  $TEST_ROUNDS"
    echo "  DEBUG_LEVEL:  $DEBUG_LEVEL"
    echo ""

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
                echo "Round $i/$TEST_ROUNDS: PASS ($PASS passed so far)"
            fi
        else
            FAIL=$((FAIL+1))
            echo "Round $i/$TEST_ROUNDS: FAIL (rc=$RC)"
        fi
    done

    echo ""
    echo "=== RESULT: $PASS/$TEST_ROUNDS passed, $FAIL/$TEST_ROUNDS failed ==="
    echo "=== Config: $DEBUG_LEVEL, $JIT_DESC, libapth=$USE_LIBAPTH, tagging=$TAG_REFSITES ==="

    if [ $FAIL -gt 0 ]; then
        exit 1
    fi
fi
