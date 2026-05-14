#!/bin/bash
set -e

# ======================== CONFIGURATION ========================
PROJECT_ROOT=$(pwd)
MOUNTPOINT="/mnt/fs"
BACKEND="/backend"
RESULTS_DIR="$PROJECT_ROOT/benchmark_results/$(basename "$0" .sh)"
FUSE_BINARY_DEDUP="$PROJECT_ROOT/passthrough_dedup"

# Paths to tracers
SYSCOUNTER_BIN="$PROJECT_ROOT/syscounter/syscounter"
SYSTRACER_BIN="$PROJECT_ROOT/systracer/systracer"

# FIO Global Params
FIO_SIZE="128M"
FIO_BS="4k"
FIO_RUNTIME="30"

# Função de limpeza automática
limpar_no_fim() {
    echo ""
    echo "  [Encerramento] A desmontar o FUSE e a limpar o terminal..."
    sudo fusermount3 -u "$MOUNTPOINT" 2>/dev/null || true
    stty sane
}

trap limpar_no_fim EXIT SIGINT SIGTERM

# ======================== PREPARATION ========================
mkdir -p "$RESULTS_DIR"
sudo mkdir -p "$BACKEND"
sudo chown $USER:$USER "$BACKEND"

# ======================== HELPER FUNCTIONS ========================

compile_dedup() {
    echo "--- Compiling Deduplication Passthrough ---"
    make clean
    make
    cp passthrough "$FUSE_BINARY_DEDUP"
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
    
    # Tenta pidof primeiro (mais preciso para o binário real)
    FUSE_PID=$(pidof -s "$BIN_NAME")
    
    # Se falhar, usa pgrep -n (newest) para ignorar o processo sudo mais antigo
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
    local VERSION_NAME="$1" # "BASE" or "DEDUP"
    local TEST_ID="$2"      # e.g., "1.1"
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
    { sudo "$SYSCOUNTER_BIN" "$FUSE_PID" > "$SYS_OUT" 2>&1 & } 2>/dev/null
    local SC_PID=$!
    { sudo "$SYSTRACER_BIN" "$FUSE_PID" > "$TRA_OUT" 2>&1 & } 2>/dev/null
    local ST_PID=$!
    { pidstat -p "$FUSE_PID" -u -r 1 > "$PID_OUT" 2>&1 & } 2>/dev/null
    local PS_PID=$!

    local PERF_PID=""
    echo "  [Profiling] Starting perf record..."
    sudo perf record -F 99 -g -p "$FUSE_PID" -o "$PERF_OUT" -- sleep "$FIO_RUNTIME" > /dev/null 2>&1 &
    PERF_PID=$!

    sleep 1

    # 2. Run FIO
    echo "  Executing FIO ($RW_TYPE)..."
    fio --name="$TEST_NAME" \
        --directory="$MOUNTPOINT" \
        --size="$FIO_SIZE" \
        --bs="$FIO_BS" \
        --direct=1 \
        --fallocate=none \
        --rw="$RW_TYPE" \
        --ioengine=psync \
        --dedupe_percentage="$DEDUP_PCT" \
        --numjobs="$NUM_JOBS" \
        --group_reporting \
        --output-format=json \
        --output="$FIO_OUT" \
        --eta-interval=1

    # 3. Stop Monitors
    if [ -n "$PERF_PID" ]; then 
        wait "$PERF_PID" 2>/dev/null || true
    fi

    sudo kill -INT "$SC_PID" 2>/dev/null || true
    sudo kill -INT "$ST_PID" 2>/dev/null || true
    kill -INT "$PS_PID" 2>/dev/null || true
    wait "$SC_PID" 2>/dev/null || true
    wait "$ST_PID" 2>/dev/null || true

    echo "  Test $TEST_ID completed."
}

# ======================== EXECUTION ========================
sudo pkill -9 -f "[p]assthrough_dedup" || true
sudo fusermount3 -u "$MOUNTPOINT" 2>/dev/null || true

compile_dedup
cleanup_env
mount_fuse "$FUSE_BINARY_DEDUP"

echo "  [Pre-fill] Writing data for read test..."
fio --name="prefill" --directory="$MOUNTPOINT" --size="$FIO_SIZE" --bs="$FIO_BS" --direct=1 --fallocate=none --rw=write --output=/dev/null --status-interval=1 --eta-interval=1

echo "  [Cold Cache Cycle] Unmounting and remounting..."
sudo fusermount3 -u "$MOUNTPOINT" 2>/dev/null || true
# No rm -rf here! We want the data to stay in backend
mount_fuse "$FUSE_BINARY_DEDUP"

run_fio_test "DEDUP" "1.4" 0 1 "read"
cleanup_env

echo "Test 1.4 (DEDUP 0% Dedup Read) Complete."
