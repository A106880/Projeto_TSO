#!/bin/bash
set -e
USE_FIO=${USE_FIO:-false}
USE_SYSCOUNTER=${USE_SYSCOUNTER:-false}
USE_SYSTRACER=${USE_SYSTRACER:-false}
USE_PIDSTAT=${USE_PIDSTAT:-false}
USE_PERF=${USE_PERF:-false}
   
if [ "${USE_ALL:-false}" = "true" ]; then
    USE_FIO=true; USE_SYSCOUNTER=true; USE_SYSTRACER=true; USE_PIDSTAT=true; USE_PERF=true
fi

# CONFIGURATION
PROJECT_ROOT=$(pwd)
MOUNTPOINT="/mnt/fs"
BACKEND="/backend"
RESULTS_DIR="$PROJECT_ROOT/benchmark_results/$(basename "$0" .sh)"
FUSE_BINARY_BASE="$PROJECT_ROOT/passthrough_base"

# Paths to tracers
SYSCOUNTER_BIN="$PROJECT_ROOT/syscounter/syscounter"
SYSTRACER_BIN="$PROJECT_ROOT/systracer/systracer"

# FIO Global Params
FIO_SIZE="256M"
FIO_BS="4k"
FIO_RUNTIME="60"

# Em caso de encerramento forçado
force_cleanup() {
    echo ""
    echo "  [Shutdown] Unmounting FUSE and cleaning up..."
    sudo fusermount3 -u "$MOUNTPOINT" 2>/dev/null || true
    stty sane
}

trap force_cleanup EXIT SIGINT SIGTERM

# PREPARATION
mkdir -p "$RESULTS_DIR"
sudo mkdir -p "$BACKEND"
sudo chown $USER:$USER "$BACKEND"

# HELPER FUNCTIONS

compile_base() {
    echo "--- Compiling Original Passthrough (Baseline) ---"
    gcc codebase/skeleton/passthrough.c -o "$FUSE_BINARY_BASE" `pkg-config fuse3 --cflags --libs`
}

cleanup_env() {
    echo "  [Cleanup] Unmounting and wiping backend..."
    sudo fusermount3 -u "$MOUNTPOINT" 2>/dev/null || true
    sudo rm -rf "$BACKEND"/*
    sudo rm "$BACKEND"/.metadata || true
    sudo rm "$BACKEND"/.sysdata || true
    sudo rm -rf "$MOUNTPOINT"/* 2>/dev/null || true
}

mount_fuse() {
    local BINARY="$1"
    local BIN_NAME=$(basename "$BINARY")
    echo "  [Mount] Starting FUSE: $BINARY"
    { sudo "$BINARY" "$MOUNTPOINT" -omodules="subdir,subdir=$BACKEND" -oallow_other -f > "$RESULTS_DIR/fuse_log.txt" 2>&1 & } 2>/dev/null
    sleep 2
    
    FUSE_PID=$(pidof -s "$BIN_NAME")
    
    if [ -z "$FUSE_PID" ]; then
        FUSE_PID=$(pgrep -n -f "$BINARY")
    fi

    if [ -z "$FUSE_PID" ]; then
        echo "ERROR: FUSE failed to start!"
        exit 1
    fi
    echo "  [Mount] FUSE PID: $FUSE_PID"
}

run_fio_test() {
    local VERSION_NAME="$1"
    local TEST_ID="$2"
    local DEDUP_PCT="$3"
    local NUM_JOBS="${4:-1}"
    local RW_TYPE="${5:-read}"
    local TEST_NAME="${VERSION_NAME}_T${TEST_ID}_D${DEDUP_PCT}_J${NUM_JOBS}_${RW_TYPE}"

    echo ""
    echo ">>> RUNNING TEST $TEST_ID: $TEST_NAME"

    local FIO_OUT="$RESULTS_DIR/${TEST_NAME}_fio.json"
    local SYS_OUT="$RESULTS_DIR/${TEST_NAME}_syscounter.txt"
    local TRA_OUT="$RESULTS_DIR/${TEST_NAME}_systracer.txt"
    local PID_OUT="$RESULTS_DIR/${TEST_NAME}_pidstat.txt"
    local PERF_OUT="$RESULTS_DIR/${TEST_NAME}_perf.data"

    # 1. Start Monitors
    if [ "$USE_SYSCOUNTER" = "true" ]; then
        { sudo "$SYSCOUNTER_BIN" "$FUSE_PID" > "$SYS_OUT" 2>&1 & } 2>/dev/null
        SC_PID=$!
    fi

    if [ "$USE_SYSTRACER" = "true" ]; then
        { sudo "$SYSTRACER_BIN" "$FUSE_PID" > "$TRA_OUT" 2>&1 & } 2>/dev/null
        ST_PID=$!
    fi

    if [ "$USE_PIDSTAT" = "true" ]; then
        { pidstat -p "$FUSE_PID" -u -r 1 > "$PID_OUT" 2>&1 & } 2>/dev/null
        PS_PID=$!
    fi

    if [ "$USE_PERF" = "true" ]; then
        echo "  [Profiling] Starting perf record..."
        sudo perf record -F 99 -g -p "$FUSE_PID" -o "$PERF_OUT" -- sleep "$FIO_RUNTIME" > /dev/null 2>&1 &
        PERF_PID=$!
    fi

    sleep 1

    # 2. Run FIO
    echo "  Executing FIO ($RW_TYPE)..."
    fio --name="$TEST_NAME" \
        --directory="$MOUNTPOINT" \
        --size="$FIO_SIZE" \
        --bs="$FIO_BS" \
        --direct=1 \
        --fallocate=none \
        --time_based \
        --runtime="$FIO_RUNTIME" \
        --rw="$RW_TYPE" \
        --ioengine=psync \
        --dedupe_percentage="$DEDUP_PCT" \
        --numjobs="$NUM_JOBS" \
        --group_reporting \
        --output-format=json \
        --output="$FIO_OUT" \
        --eta-interval=1

    # 3. Stop Monitors
    [ -n "$SC_PID" ] && sudo kill -INT "$SC_PID" 2>/dev/null || true
    [ -n "$ST_PID" ] && sudo kill -INT "$ST_PID" 2>/dev/null || true
    [ -n "$PS_PID" ] && kill -INT "$PS_PID" 2>/dev/null || true
    [ -n "$PERF_PID" ] && wait "$PERF_PID" 2>/dev/null || true

    wait "$SC_PID" 2>/dev/null || true
    wait "$ST_PID" 2>/dev/null || true

    echo "  Test $TEST_ID completed."
}

# EXECUTION
sudo pkill -9 -f "[p]assthrough_base" || true
sudo fusermount3 -u "$MOUNTPOINT" 2>/dev/null || true

compile_base
cleanup_env
mount_fuse "$FUSE_BINARY_BASE"

echo "  [Pre-fill] Writing data for read test..."
fio --name="prefill" --directory="$MOUNTPOINT" --size="$FIO_SIZE" --bs="$FIO_BS" --direct=1 --fallocate=none --rw=write --output=/dev/null --status-interval=1 --eta-interval=1

echo "  [Cold Cache Cycle] Unmounting and remounting..."
sudo fusermount3 -u "$MOUNTPOINT" 2>/dev/null || true

mount_fuse "$FUSE_BINARY_BASE"

run_fio_test "BASE" "1.4" 0 1 "read"
cleanup_env

echo "Test (BASE 0% Dedup Read) Complete."
