#!/bin/bash
set -e

# ======================== CONFIGURATION ========================
PROJECT_ROOT=$(pwd)
MOUNTPOINT="/mnt/fs"
BACKEND="/backend"
RESULTS_DIR="$PROJECT_ROOT/benchmark_results/$(basename "$0" .sh)"
FUSE_BINARY_DEDUP="$PROJECT_ROOT/passthrough_dedup"

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

run_aging_test() {
    local VERSION_NAME="$1"
    local TEST_NAME="${VERSION_NAME}_T2.0_aging"

    echo ""
    echo ">>> RUNNING PHASE 2 (AGING COM FIO - MODO FILE SERVER): $TEST_NAME"

    local OUT_FILE="$RESULTS_DIR/${TEST_NAME}_output.txt"
    local MEM_OUT="$RESULTS_DIR/${TEST_NAME}_pidstat.txt"
    local PERF_OUT="$RESULTS_DIR/${TEST_NAME}_perf.data"

    # Start Memory Monitoring
    pidstat -p "$FUSE_PID" -r 1 > "$MEM_OUT" 2>&1 &
    local PS_PID=$!

    local PERF_PID=""
    echo "  [Profiling] Starting perf record..."
    sudo perf record -F 99 -g -p "$FUSE_PID" -o "$PERF_OUT" -- sleep 60 > /dev/null 2>&1 &
    PERF_PID=$!

    # Run FIO em Modo File Server (Ciclo de vida completo durante 60 segundos)
    echo "  Executing FIO Aging Workload (Create -> Write -> Read -> Delete)..."
    fio --name="aging_fileserver" \
        --directory="$MOUNTPOINT" \
        --numjobs=4 \
        --ioengine=psync \
        --rw=randrw \
        --rwmixwrite=70 \
        --bs=4k \
        --direct=1 \
        --nrfiles=100 \
        --filesize=10m \
        --file_service_type=random \
        --unlink=1 \
        --unlink_each_loop=1 \
        --time_based \
        --runtime=60 \
        --group_reporting \
        --eta=never > "$OUT_FILE" 2>&1

    # Stop Monitor
    if [ -n "$PERF_PID" ]; then 
        wait "$PERF_PID" 2>/dev/null || true
    fi

    kill -INT "$PS_PID" 2>/dev/null || true
    wait "$PS_PID" 2>/dev/null || true

    echo "  Phase 2 completed for $VERSION_NAME."
}

# ======================== EXECUTION ========================
sudo pkill -9 -f "[p]assthrough_dedup" || true
sudo fusermount3 -u "$MOUNTPOINT" 2>/dev/null || true

compile_dedup
cleanup_env
mount_fuse "$FUSE_BINARY_DEDUP"
run_aging_test "DEDUP"
cleanup_env

echo "Aging Test (DEDUP) Complete."
