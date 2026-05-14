#!/bin/bash
set -e

# ======================== OBSERVABILITY TOGGLES ========================
# Por defeito, tudo FALSE. Para ativar, corre: USE_PIDSTAT=true ./script.sh
USE_PIDSTAT=${USE_PIDSTAT:-false}

if [ "${USE_ALL:-false}" = "true" ]; then
    USE_PIDSTAT=true
fi

# ======================== CONFIGURATION ========================
PROJECT_ROOT=$(pwd)
MOUNTPOINT="/mnt/fs"
BACKEND="/backend"
RESULTS_DIR="$PROJECT_ROOT/benchmark_results/$(basename "$0" .sh)"
FUSE_BINARY_DEDUP="$PROJECT_ROOT/passthrough_dedup"

# Automatic cleanup function
limpar_no_fim() {
    echo ""
    echo "  [Shutdown] Unmounting FUSE and cleaning up..."
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
    cd "$PROJECT_ROOT/codededup"
    make clean
    make
    cp passthrough "$FUSE_BINARY_DEDUP"
    cd "$PROJECT_ROOT"
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

run_phase3_space_recycle() {
    echo ""
    echo "============================================="
    echo " PHASE 3: SPACE RECYCLE VALIDATION"
    echo "============================================="

    cleanup_env
    mount_fuse "$FUSE_BINARY_DEDUP"

    local MEM_OUT="$RESULTS_DIR/space_recycle_pidstat.txt"

    # Start Monitoring
    if [ "$USE_PIDSTAT" = "true" ]; then
        echo "  [Monitor] Starting pidstat..."
        pidstat -p "$FUSE_PID" -r 1 > "$MEM_OUT" 2>&1 &
        PS_PID=$!
    fi

    # Step 1: Initial Fill (Dataset A)
    echo "  [Step 1] Writing 500MB of unique data (Dataset A)..."
    fio --name=dataset_A --directory="$MOUNTPOINT" --size=500M --bs=4k --direct=1 --fallocate=none --rw=write --ioengine=psync --dedupe_percentage=0 --output=/dev/null
    
    local SIZE_BEFORE=$(sudo du -sb "$BACKEND" | awk '{print $1}')
    echo "  Physical size after Dataset A: $SIZE_BEFORE bytes"

    # Step 2: Delete all files
    echo "  [Step 2] Deleting all files to trigger space recycle..."
    rm -rf "$MOUNTPOINT"/*
    sync
    sleep 2

    # Step 3: Refill (Dataset B)
    echo "  [Step 3] Writing another 500MB of unique data (Dataset B)..."
    fio --name=dataset_B --directory="$MOUNTPOINT" --size=500M --bs=4k --direct=1 --fallocate=none --rw=write --ioengine=psync --dedupe_percentage=0 --output=/dev/null
    
    local SIZE_AFTER=$(sudo du -sb "$BACKEND" | awk '{print $1}')
    echo "  Physical size after Dataset B: $SIZE_AFTER bytes"

    # Step 4: Verification
    echo ""
    echo ">>> PHASE 3 RESULTS"
    echo "------------------------------------------------"
    echo "Size Before Delete: $SIZE_BEFORE bytes"
    echo "Size After Refill:  $SIZE_AFTER bytes"
    
    local DIFF=$(($SIZE_AFTER - $SIZE_BEFORE))
    [ $DIFF -lt 0 ] && DIFF=$(($DIFF * -1)) # Absolute value
    
    local MARGIN=10485760 # 10MB margin for metadata growth
    
    if [ $DIFF -lt $MARGIN ]; then
        echo "SUCCESS: Space was successfully recycled!"
        echo "The backend size did not double, proving that deleted blocks were reused."
    else
        echo "FAILURE: Space was NOT recycled!"
        echo "The backend size increased significantly ($DIFF bytes), indicating space leakage."
    fi
    echo "------------------------------------------------"
    
    # Stop Monitor
    if [ "$USE_PIDSTAT" = "true" ]; then 
        kill -INT "$PS_PID" 2>/dev/null || true
        wait "$PS_PID" 2>/dev/null || true
    fi

    cleanup_env
}

# ======================== EXECUTION ========================
sudo pkill -9 -f "[p]assthrough_dedup" || true
sudo fusermount3 -u "$MOUNTPOINT" 2>/dev/null || true

compile_dedup
run_phase3_space_recycle

echo "Space Recycle Test Complete."
