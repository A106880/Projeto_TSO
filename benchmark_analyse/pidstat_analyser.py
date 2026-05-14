import sys
import os

def analisar_pidstat(caminho_ficheiro):
    try:
        with open(caminho_ficheiro, 'r') as f:
            linhas = f.readlines()
    except FileNotFoundError:
        print(f"Error: File '{caminho_ficheiro}' not found.")
        return

    nome_teste = os.path.basename(caminho_ficheiro).replace('_pidstat.txt', '')

    # Dictionary to store all measurements captured second by second
    metricas = {
        '%usr': [], '%system': [], '%CPU': [],
        'minflt/s': [], 'majflt/s': [], 'RSS': [], '%MEM': [],
        'cswch/s': [], 'nvcswch/s': []
    }

    cabecalho_ativo = []

    for linha in linhas:
        partes = linha.split()
        if not partes:
            continue

        # Ignore OS header (e.g., Linux 6.14.0...)
        if partes[0] == "Linux":
            continue

        # Ignore "Average:" lines pre-calculated by pidstat
        # (we calculate the average ourselves for precision)
        if partes[0] == "Average:":
            continue

        # Detect column header line
        if "UID" in partes and "PID" in partes:
            cabecalho_ativo = partes
            continue

        # Data line (same number of columns as header)
        if cabecalho_ativo and len(partes) == len(cabecalho_ativo):
            try:
                # Confirm PID column is a number to avoid errors
                idx_pid = cabecalho_ativo.index("PID")
                int(partes[idx_pid])
                
                # Extract values based on column name
                for metrica in metricas.keys():
                    if metrica in cabecalho_ativo:
                        idx = cabecalho_ativo.index(metrica)
                        # Replace commas with dots if system is in Portuguese
                        valor = float(partes[idx].replace(',', '.'))
                        metricas[metrica].append(valor)
            except ValueError:
                # Corrupted line or misread header, skip
                continue

    # Helper function to calculate averages safely
    def calcular_media(lista):
        if not lista: return 0.0
        return sum(lista) / len(lista)

    # Calculate final averages
    avg_usr = calcular_media(metricas['%usr'])
    avg_sys = calcular_media(metricas['%system'])
    avg_tot = calcular_media(metricas['%CPU'])
    
    avg_minflt = calcular_media(metricas['minflt/s'])
    
    # RSS is in Kilobytes. Convert to Megabytes for readability.
    avg_rss_kb = calcular_media(metricas['RSS'])
    avg_rss_mb = avg_rss_kb / 1024
    avg_mem_pct = calcular_media(metricas['%MEM'])

    avg_cswch = calcular_media(metricas['cswch/s'])
    avg_nvcswch = calcular_media(metricas['nvcswch/s'])

    # Total samples read (in seconds)
    segundos_gravados = len(metricas['%CPU'])

    print(f"\n{'='*50}")
    print(f"🖥️  HARDWARE USAGE (PIDSTAT): {nome_teste}")
    print(f"{'='*50}")
    print(f"Analyzed duration: ~{segundos_gravados} seconds\n")

    print("⚙️  PROCESSOR CONSUMPTION (CPU):")
    print(f"   ┣ User Space (%usr): {avg_usr:>6.2f} % (Your C code)")
    print(f"   ┣ Kernel Space (%system):  {avg_sys:>6.2f} % (OS working for you)")
    print(f"   ┗ Total CPU Consumed:      {avg_tot:>6.2f} %\n")

    print("🧠 MEMORY CONSUMPTION (RAM):")
    print(f"   ┣ Real Memory (RSS):   {avg_rss_mb:>6.2f} MB")
    print(f"   ┣ RAM Percentage:   {avg_mem_pct:>6.2f} %")
    print(f"   ┗ Page Faults (Minor): {avg_minflt:>6.2f} per second\n")

    if metricas['cswch/s']:
        print("🔄 CONTEXT SWITCHES:")
        print(f"   ┣ Voluntary (Wait): {avg_cswch:>6.2f} per second")
        print(f"   ┗ Forced (Full CPU): {avg_nvcswch:>6.2f} per second")
    
    print("=" * 50 + "\n")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python3 pidstat_analyser.py <path_to_pidstat_file.txt>")
    else:
        analisar_pidstat(sys.argv[1])