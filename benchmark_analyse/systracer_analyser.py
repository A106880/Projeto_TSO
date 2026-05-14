import sys
import os
from collections import Counter

def analisar_systracer(caminho_ficheiro):
    try:
        with open(caminho_ficheiro, 'r') as f:
            linhas = f.readlines()
    except FileNotFoundError:
        print(f"Error: File '{caminho_ficheiro}' not found.")
        return

    nome_teste = os.path.basename(caminho_ficheiro).replace('_systracer.txt', '')

    eventos_perdidos = "Unknown"
    
    # Counters and collections to store telemetry
    contador_syscalls = Counter()
    contador_camadas = Counter() # VFS, EXT4, BIO, syscall
    ficheiros_unicos = set()
    total_linhas_processadas = 0

    comecou_dados = False

    for linha in linhas:
        linha = linha.strip()
        
        # Ignore empty lines
        if not linha:
            continue
            
        # Detect end of file and if events were lost by eBPF
        if linha.startswith("Lost"):
            eventos_perdidos = linha
            continue

        # Detect when the data table actually starts
        if linha.startswith("pid") and "command" in linha:
            continue
        if linha.startswith("---"):
            comecou_dados = True
            continue

        # Process real data lines
        if comecou_dados:
            # split() divides by spaces (even multiple spaces)
            partes = linha.split()
            
            # A valid systracer line has at least 5 columns:
            # [0]pid, [1]command, [2]syscall, [3]type, [4]filename
            if len(partes) >= 5:
                # PID and Command are not needed as they are always your FUSE
                operacao = partes[2]
                camada = partes[3]
                nome_ficheiro = partes[4]

                contador_syscalls[operacao] += 1
                contador_camadas[camada] += 1
                ficheiros_unicos.add(nome_ficheiro)
                total_linhas_processadas += 1

    print(f"\n{'='*50}")
    print(f"🕵️  TRACER X-RAY (eBPF): {nome_teste}")
    print(f"{'='*50}")

    print(f"Total events processed: {total_linhas_processadas:,}")
    print(f"Capture State: {eventos_perdidos}\n")

    print("📚 KERNEL LAYER DISTRIBUTION:")
    for camada, count in contador_camadas.most_common():
        percentagem = (count / total_linhas_processadas) * 100 if total_linhas_processadas > 0 else 0
        print(f"   ┣ {camada:<10}: {count:>8,} events ({percentagem:.1f}%)")

    print("\n⚙️  TOP OPERATIONS / SYSCALLS:")
    # Show only top 5 to keep terminal clean
    for op, count in contador_syscalls.most_common(5):
        print(f"   ┣ {op:<10}: {count:>8,} times")

    print("\n📁 INTERCEPTED FILES (Unique Accesses):")
    # Show at most 5 files to summarize
    lista_ficheiros = list(ficheiros_unicos)
    for ficheiro in lista_ficheiros[:5]:
        print(f"   ┣ {ficheiro}")
    if len(lista_ficheiros) > 5:
        print(f"   ┗ ... and {len(lista_ficheiros) - 5} more hidden files.")
    elif len(lista_ficheiros) > 0:
        print("   ┗ (End of list)")
    else:
        print("   ┗ No files detected (Strange!)")
        
    print("=" * 50 + "\n")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python3 systracer_analyser.py <path_to_systracer_file.txt>")
    else:
        analisar_systracer(sys.argv[1])