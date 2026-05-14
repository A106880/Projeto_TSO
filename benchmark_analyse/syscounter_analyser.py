import sys
import os

def analisar_syscounter(caminho_ficheiro):
    try:
        with open(caminho_ficheiro, 'r') as f:
            linhas = f.readlines()
    except FileNotFoundError:
        print(f"Error: File '{caminho_ficheiro}' not found.")
        return

    cabecalhos = []
    totais = []

    # Search for header line and total line
    for linha in linhas:
        if linha.startswith("PID\\OP"):
            # split() cuts on whitespace. Ignore first element ('PID\\OP')
            cabecalhos = linha.split()[1:]
        elif linha.startswith("Total"):
            # Ignore the word 'Total'
            totais = linha.split()[1:]

    if not cabecalhos or not totais:
        print(f"Error: Could not detect table structure in file '{caminho_ficheiro}'.")
        print("Make sure the file contains 'PID\\OP' and 'Total' lines.")
        return

    nome_teste = os.path.basename(caminho_ficheiro).replace('_syscounter.txt', '')

    print(f"\n{'='*50}")
    print(f"🛡️  SYSCALL SUMMARY (eBPF): {nome_teste}")
    print(f"{'='*50}")

    total_mb_lidos = 0
    total_mb_escritos = 0

    # Iterate through operations (read, pwrite, openat, close)
    for index, op_string in enumerate(cabecalhos):
        # Clean operation name (e.g., "read(C/B)" -> "read")
        nome_operacao = op_string.split('(')[0].upper()
        
        # Get "Count/Bytes" values (e.g., "16218/66428928")
        valores = totais[index]
        
        if '/' in valores:
            count_str, bytes_str = valores.split('/')
            count = int(count_str)
            bytes_val = int(bytes_str)
            mb_val = bytes_val / (1024 * 1024)

            print(f"🔄 Operation: {nome_operacao}")
            print(f"   ┣ Executed Calls: {count:,}")
            
            # Only show bytes line if operation involves data movement
            if bytes_val > 0 or nome_operacao in ['READ', 'PWRITE']:
                print(f"   ┗ Transacted Data: {mb_val:.2f} MB ({bytes_val:,} bytes)\n")
                
                # Sum global counters for final summary
                if nome_operacao == 'READ':
                    total_mb_lidos += mb_val
                elif nome_operacao == 'PWRITE':
                    total_mb_escritos += mb_val
        else:
            # For future syscalls that might not have C/B format
            print(f"🔄 Operation: {nome_operacao}")
            print(f"   ┗ Executed Calls: {valores}\n")

    print("-" * 50)
    print("📈 ACTUAL BACKEND DATA BALANCE:")
    print(f"   Total Read:    {total_mb_lidos:.2f} MB")
    print(f"   Total Written: {total_mb_escritos:.2f} MB")
    print("=" * 50 + "\n")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python3 syscounter_analyser.py <path_to_syscounter_file.txt>")
    else:
        analisar_syscounter(sys.argv[1])