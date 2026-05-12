#!/bin/bash
# =============================================================================
# PHASE 1 - Automated Benchmarking Pipeline (FIO + eBPF) - V2 (Full)
# =============================================================================
# This script automates:
# 1. Compilation of Baseline (Original) and Dedup versions.
# 2. FUSE Lifecycle (Mount/Unmount/Backend Cleanup).
# 3. Resource Monitoring (pidstat, perf record).
# 4. eBPF Observability (syscounter, systracer).
# 5. FIO Execution with --direct=1 (Sequential Write/Read, Multi-thread).
# 6. Automated Results Parsing to CSV.
# =============================================================================

set -e

# ======================== CONFIGURATION ========================
MOUNTPOINT="/mnt/fs"
BACKEND="/backend"
RESULTS_DIR="$(pwd)/benchmark_results"
FUSE_BINARY_DEDUP="$(pwd)/passthrough_dedup"
FUSE_BINARY_BASE="$(pwd)/passthrough_base"
WORKLOAD_FILE="$(pwd)/fileserver_append.f"

# Paths to tracers
SYSCOUNTER_BIN="$(pwd)/syscounter/syscounter"
SYSTRACER_BIN="$(pwd)/systracer/systracer"

# FIO Global Params
FIO_SIZE="128M"
FIO_BS="4k"
FIO_RUNTIME="30"

# ======================== PREPARATION ========================
mkdir -p "$RESULTS_DIR"
sudo mkdir -p "$BACKEND"
sudo chown $USER:$USER "$BACKEND"

# Ensure tracers are compiled
echo "--- Compiling eBPF tools ---"
(cd syscounter && make)
(cd systracer && make)

# ======================== HELPER FUNCTIONS ========================

compile_base() {
    echo "--- Compiling Original Passthrough (Baseline) ---"
    gcc codebase/skeleton/passthrough.c -o "$FUSE_BINARY_BASE" `pkg-config fuse3 --cflags --libs`
}

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
    echo "  [Mount] Starting FUSE: $BINARY"
    sudo "$BINARY" "$MOUNTPOINT" -omodules="subdir,subdir=$BACKEND" -oallow_other -f > "$RESULTS_DIR/fuse_log.txt" 2>&1 &
    sleep 2
    FUSE_PID=$(pgrep -f "$BINARY")
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
    local RW_TYPE="${5:-write}"
    local TEST_NAME="${VERSION_NAME}_T${TEST_ID}_D${DEDUP_PCT}_J${NUM_JOBS}_${RW_TYPE}"

    echo ""
    echo ">>> RUNNING TEST $TEST_ID: $TEST_NAME"

    # Files for results
    local FIO_OUT="$RESULTS_DIR/${TEST_NAME}_fio.json"
    local SYS_OUT="$RESULTS_DIR/${TEST_NAME}_syscounter.txt"
    local TRA_OUT="$RESULTS_DIR/${TEST_NAME}_systracer.txt"
    local PID_OUT="$RESULTS_DIR/${TEST_NAME}_pidstat.txt"
    local PERF_OUT="$RESULTS_DIR/${TEST_NAME}_perf.data"

    # 1. Start Monitors
    sudo "$SYSCOUNTER_BIN" "$FUSE_PID" > "$SYS_OUT" 2>&1 &
    local SC_PID=$!
    sudo "$SYSTRACER_BIN" "$FUSE_PID" > "$TRA_OUT" 2>&1 &
    local ST_PID=$!
    pidstat -p "$FUSE_PID" -u -r 1 > "$PID_OUT" 2>&1 &
    local PS_PID=$!

    # SPECIAL: perf record for Test 1.3 (100% dedup) to profile SHA-512
    local PERF_PID=""
    if [ "$TEST_ID" == "1.3" ]; then
        echo "  [Profiling] Starting perf record..."
        sudo perf record -p "$FUSE_PID" -g -o "$PERF_OUT" -- sleep "$FIO_RUNTIME" > /dev/null 2>&1 &
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
        --rw="$RW_TYPE" \
        --ioengine=psync \
        --dedupe_percentage="$DEDUP_PCT" \
        --numjobs="$NUM_JOBS" \
        --group_reporting \
        --output-format=json \
        --output="$FIO_OUT"

    # 3. Stop Monitors
    sudo kill -INT "$SC_PID" 2>/dev/null || true
    sudo kill -INT "$ST_PID" 2>/dev/null || true
    kill -INT "$PS_PID" 2>/dev/null || true
    if [ -n "$PERF_PID" ]; then wait "$PERF_PID" 2>/dev/null || true; fi
    wait "$SC_PID" 2>/dev/null || true
    wait "$ST_PID" 2>/dev/null || true

    echo "  Test $TEST_ID completed."
}

run_filebench_test() {
    local VERSION_NAME="$1"
    local TEST_ID="2.0"
    local TEST_NAME="${VERSION_NAME}_T${TEST_ID}_filebench"

    echo ""
    echo ">>> RUNNING PHASE 2 (FILEBENCH): $TEST_NAME"

    local FB_OUT="$RESULTS_DIR/${TEST_NAME}_output.txt"
    local MEM_OUT="$RESULTS_DIR/${TEST_NAME}_pidstat.txt"

    # Start Memory Monitoring
    pidstat -p "$FUSE_PID" -r 1 > "$MEM_OUT" 2>&1 &
    local PS_PID=$!

    # Run Filebench
    echo "  Executing Filebench..."
    sudo filebench -f "$WORKLOAD_FILE" > "$FB_OUT" 2>&1

    # Stop Monitor
    kill -INT "$PS_PID" 2>/dev/null || true
    wait "$PS_PID" 2>/dev/null || true

    echo "  Test $TEST_ID completed."
}

run_phase3_reclamation() {
    echo ""
    echo "============================================="
    echo " PHASE 3: SPACE RECLAMATION VALIDATION"
    echo "============================================="

    cleanup_env
    mount_fuse "$FUSE_BINARY_DEDUP"

    # Step 1: Initial Fill (Dataset A)
    echo "  [Step 1] Writing 500MB of unique data (Dataset A)..."
    fio --name=dataset_A --directory="$MOUNTPOINT" --size=500M --bs=4k --direct=1 --fallocate=none --rw=write --ioengine=psync --dedupe_percentage=0 --output=/dev/null
    
    local SIZE_BEFORE=$(sudo du -sb "$BACKEND" | awk '{print $1}')
    echo "  Physical size after Dataset A: $SIZE_BEFORE bytes"

    # Step 2: Delete all files
    echo "  [Step 2] Deleting all files to trigger reclamation..."
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
    
    cleanup_env
}

# ======================== PARSER FUNCTION ========================

parse_results() {
    echo ""
    echo ">>> PARSING ALL RESULTS TO CSV..."
    local CSV_OUT="$RESULTS_DIR/summary_final.csv"
    # New header with Phase 2 support
    echo "version,test_type,test_id,dedup_pct,jobs,rw,bw_kb,lat_ns_mean,lat_ns_p95,syscalls,ops_s,max_rss_kb" > "$CSV_OUT"

    # 1. Parse FIO Results
    for FIO_JSON in "$RESULTS_DIR"/*_fio.json; do
        [ -e "$FIO_JSON" ] || continue
        local BASE_FILE=$(basename "$FIO_JSON" "_fio.json")
        local SYS_FILE="$RESULTS_DIR/${BASE_FILE}_syscounter.txt"
        
        local VER=$(echo "$BASE_FILE" | cut -d'_' -f1)
        local TID=$(echo "$BASE_FILE" | cut -d'_' -f2 | sed 's/T//')
        local DPCT=$(echo "$BASE_FILE" | cut -d'_' -f3 | sed 's/D//')
        local JOBS=$(echo "$BASE_FILE" | cut -d'_' -f4 | sed 's/J//')
        local RW=$(echo "$BASE_FILE" | cut -d'_' -f5)

        local METRICS=$(python3 -c "
import json, sys
try:
    with open('$FIO_JSON') as f:
        data = json.load(f)
        job = data['jobs'][0]
        rw_key = 'read' if '$RW' == 'read' else 'write'
        bw = job.get(rw_key, {}).get('bw_mean', 0) or job.get(rw_key, {}).get('bw', 0)
        lat_mean = job.get(rw_key, {}).get('lat_ns', {}).get('mean', 0)
        lat_p95 = job.get(rw_key, {}).get('clat_ns', {}).get('percentile', {}).get('95.000000', 0)
        print(f'{bw},{lat_mean},{lat_p95}')
except: print('0,0,0')
")
        local TOTAL_SYSCALLS=$(grep -v "syscall" "$SYS_FILE" 2>/dev/null | awk '{sum += $2} END {print sum+0}')
        echo "$VER,fio,$TID,$DPCT,$JOBS,$RW,$METRICS,$TOTAL_SYSCALLS,0,0" >> "$CSV_OUT"
    done

    # 2. Parse Filebench Results
    for FB_FILE in "$RESULTS_DIR"/*_filebench_output.txt; do
        [ -e "$FB_FILE" ] || continue
        local BASE_FILE=$(basename "$FB_FILE" "_output.txt")
        local MEM_FILE="$RESULTS_DIR/${BASE_FILE}_pidstat.txt"
        
        local VER=$(echo "$BASE_FILE" | cut -d'_' -f1)
        local OPS=$(grep "Summary:" "$FB_FILE" | awk '{print $6}')
        local MAX_RSS=$(awk '{print $8}' "$MEM_FILE" | grep -E '^[0-9]+' | sort -rn | head -1)
        [ -z "$MAX_RSS" ] && MAX_RSS=0

        echo "$VER,filebench,2.0,50,8,mixed,0,0,0,0,$OPS,$MAX_RSS" >> "$CSV_OUT"
    done
    
    echo "Final CSV generated at: $CSV_OUT"
    column -t -s, "$CSV_OUT" 2>/dev/null || cat "$CSV_OUT"
}

# ======================== EXECUTION PHASE 1 ========================

# Ensure no old processes are running before compilation
echo "--- Cleaning up old processes ---"
sudo pkill -9 -f passthrough_dedup || true
sudo pkill -9 -f passthrough_base || true
sudo fusermount3 -u "$MOUNTPOINT" 2>/dev/null || true

# Compile both versions first
compile_base
compile_dedup

# --- TEST 1.0: TRUE BASELINE ---
# cleanup_env
# mount_fuse "$FUSE_BINARY_BASE"
# run_fio_test "BASE" "1.0" 0 1 "write"
# cleanup_env

# # --- TEST 1.1: 0% DEDUP ---
# mount_fuse "$FUSE_BINARY_DEDUP"
# run_fio_test "DEDUP" "1.1" 0 1 "write"
# cleanup_env

# # --- TEST 1.2: 50% DEDUP ---
# mount_fuse "$FUSE_BINARY_DEDUP"
# run_fio_test "DEDUP" "1.2" 50 1 "write"
# cleanup_env

# # --- TEST 1.3: 100% DEDUP (With PERF) ---
# mount_fuse "$FUSE_BINARY_DEDUP"
# run_fio_test "DEDUP" "1.3" 100 1 "write"
# cleanup_env

# # --- TEST 1.4: SEQUENTIAL READ (Cold Cache) ---
# mount_fuse "$FUSE_BINARY_DEDUP"
# echo "  [Pre-fill] Writing data for read test..."
# fio --name="prefill" --directory="$MOUNTPOINT" --size="$FIO_SIZE" --bs="$FIO_BS" --direct=1 --fallocate=none --rw=write --output=/dev/null
# echo "  [Cold Cache Cycle] Unmounting and remounting..."
# sudo fusermount3 -u "$MOUNTPOINT" 2>/dev/null || true
# # No rm -rf here! We want the data to stay in backend
# mount_fuse "$FUSE_BINARY_DEDUP"
# run_fio_test "DEDUP" "1.4" 0 1 "read"
# cleanup_env

# # --- TEST 1.5: MULTI-THREAD (BASE vs DEDUP) ---
# # BASE MT
# mount_fuse "$FUSE_BINARY_BASE"
# run_fio_test "BASE" "1.5" 50 4 "write"
# cleanup_env

# # DEDUP MT
# mount_fuse "$FUSE_BINARY_DEDUP"
# run_fio_test "DEDUP" "1.5" 50 4 "write"
# cleanup_env

# ======================== EXECUTION PHASE 2 ========================
echo ""
echo "============================================="
echo " PHASE 2: REAL-WORLD AGING BENCHMARK"
echo "============================================="

# BASE FILEBENCH
cleanup_env
mount_fuse "$FUSE_BINARY_BASE"
echo "  [Pre-fill] Aging baseline with 1GB unique data..."
fio --name="aging_prefill_base" --directory="$MOUNTPOINT" --size=1G --bs=4k --direct=1 --fallocate=none --rw=write --output=/dev/null
run_filebench_test "BASE"
cleanup_env

# DEDUP FILEBENCH
mount_fuse "$FUSE_BINARY_DEDUP"
echo "  [Pre-fill] Aging dedup system with 1GB unique data..."
fio --name="aging_prefill_dedup" --directory="$MOUNTPOINT" --size=1G --bs=4k --direct=1 --fallocate=none --rw=write --output=/dev/null
run_filebench_test "DEDUP"
cleanup_env

# ======================== EXECUTION PHASE 3 ========================

run_phase3_reclamation

# ======================== WRAP UP ========================
parse_results


echo "============================================="
echo " PHASE 1 COMPLETE!"
echo " Results available in: $RESULTS_DIR"
echo " Profiling Data: $RESULTS_DIR/DEDUP_T1.3_D100_J1_write_perf.data"
echo "============================================="
