import json
import sys
import os

def analyse_fio_json(filepath):
    try:
        with open(filepath, 'r') as f:
            data = json.load(f)
    except FileNotFoundError:
        print(f"Error: File '{filepath}' not found.")
        return
    except json.JSONDecodeError:
        print(f"Error: File '{filepath}' is not a valid JSON.")
        return

    if 'jobs' not in data or len(data['jobs']) == 0:
        print("Error: Unexpected JSON format (missing 'jobs' key).")
        return

    job = data['jobs'][0]
    test_name = job.get('jobname', 'Unknown')

    print(f"\n{'='*50}")
    print(f"TEST SUMMARY: {test_name}")
    print(f"{'='*50}")

    # FIO's own CPU Usage
    usr_cpu = job.get('usr_cpu', 0)
    sys_cpu = job.get('sys_cpu', 0)
    print(f"CPU Overhead (FIO): {usr_cpu:.2f}% User | {sys_cpu:.2f}% System\n")

    # Internal function to avoid code repetition between read and write
    def extract_metrics(operation_type):
        op_data = job.get(operation_type, {})
        iops = op_data.get('iops', 0)

        # In case of failed operation
        if iops == 0:
            return

        # Bandwidth (Modern FIO uses bw_bytes)
        bw_bytes = op_data.get('bw_bytes', 0)
        bw_mb = bw_bytes / (1024 * 1024) # Convert to MB/s

        # Total Latency (lat_ns) and Completion Latency (clat_ns) are in nanoseconds
        lat_ns = op_data.get('lat_ns', {})
        lat_mean_ms = lat_ns.get('mean', 0) / 1000000

        clat_ns = op_data.get('clat_ns', {})
        percentiles = clat_ns.get('percentile', {})
        
        # P95 and P99 are the Tail Latency (worst 5% and 1% cases)
        p95_ms = percentiles.get('95.000000', 0) / 1000000
        p99_ms = percentiles.get('99.000000', 0) / 1000000

        print(f"OPERATION: {operation_type.upper()}")
        print(f"   ┣ Throughput (BW): {bw_mb:.2f} MB/s")
        print(f"   ┣ IOPS:            {iops:.2f}")
        print(f"   ┣ Average Latency:  {lat_mean_ms:.3f} ms")
        print(f"   ┗ Tail Latency:    p95 = {p95_ms:.3f} ms | p99 = {p99_ms:.3f} ms\n")

    extract_metrics('read')
    extract_metrics('write')

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python3 fio_analyser.py <path_to_json_file>")
    else:
        analyse_fio_json(sys.argv[1])
