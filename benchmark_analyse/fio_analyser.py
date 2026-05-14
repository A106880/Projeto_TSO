import json
import sys
import os

def analisar_fio_json(caminho_ficheiro):
    try:
        with open(caminho_ficheiro, 'r') as f:
            dados = json.load(f)
    except FileNotFoundError:
        print(f"Error: File '{caminho_ficheiro}' not found.")
        return
    except json.JSONDecodeError:
        print(f"Error: File '{caminho_ficheiro}' is not a valid JSON.")
        return

    # FIO aggregate data is in the first "job" when using --group_reporting
    if 'jobs' not in dados or len(dados['jobs']) == 0:
        print("Error: Unexpected JSON format (missing 'jobs' key).")
        return

    job = dados['jobs'][0]
    nome_teste = job.get('jobname', 'Unknown')

    print(f"\n{'='*50}")
    print(f"📊 TEST SUMMARY: {nome_teste}")
    print(f"{'='*50}")

    # FIO's own CPU Usage
    usr_cpu = job.get('usr_cpu', 0)
    sys_cpu = job.get('sys_cpu', 0)
    print(f"💻 CPU Overhead (FIO): {usr_cpu:.2f}% User | {sys_cpu:.2f}% System\n")

    # Internal function to avoid code repetition between read and write
    def extrair_metricas(tipo_operacao):
        op_data = job.get(tipo_operacao, {})
        iops = op_data.get('iops', 0)

        # If IOPS is 0, it means this operation did not occur (e.g., 100% write test)
        if iops == 0:
            return

        # Bandwidth (Modern FIO uses bw_bytes)
        bw_bytes = op_data.get('bw_bytes', 0)
        bw_mb = bw_bytes / (1024 * 1024) # Convert to MB/s

        # Total Latency (lat_ns) and Completion Latency (clat_ns) are in nanoseconds
        lat_ns = op_data.get('lat_ns', {})
        lat_mean_ms = lat_ns.get('mean', 0) / 1000000 # Convert ns to milliseconds

        clat_ns = op_data.get('clat_ns', {})
        percentis = clat_ns.get('percentile', {})
        
        # P95 and P99 are the Tail Latency (worst 5% and 1% cases)
        p95_ms = percentis.get('95.000000', 0) / 1000000
        p99_ms = percentis.get('99.000000', 0) / 1000000

        print(f"🔄 OPERATION: {tipo_operacao.upper()}")
        print(f"   ┣ Throughput (BW): {bw_mb:.2f} MB/s")
        print(f"   ┣ IOPS:            {iops:.2f}")
        print(f"   ┣ Average Latency:  {lat_mean_ms:.3f} ms")
        print(f"   ┗ Tail Latency:    p95 = {p95_ms:.3f} ms | p99 = {p99_ms:.3f} ms\n")

    extrair_metricas('read')
    extrair_metricas('write')

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python3 fio_analyser.py <path_to_json_file>")
    else:
        analisar_fio_json(sys.argv[1])