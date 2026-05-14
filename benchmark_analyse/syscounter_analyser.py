import sys
import os

def analyse_syscounter(file_path):
    try:
        with open(file_path, 'r') as f:
            lines = f.readlines()
    except FileNotFoundError:
        print(f"Error: File '{file_path}' not found.")
        return

    headers = []
    totals = []

    # Search for header line and total line
    for line in lines:
        if line.startswith("PID\\OP"):
            # split() cuts on whitespace. Ignore first element ('PID\\OP')
            headers = line.split()[1:]
        elif line.startswith("Total"):
            # Ignore the word 'Total'
            totals = line.split()[1:]

    if not headers or not totals:
        print(f"Error: Could not detect table structure in file '{file_path}'.")
        print("Make sure the file contains 'PID\\OP' and 'Total' lines.")
        return

    test_name = os.path.basename(file_path).replace('_syscounter.txt', '')

    print(f"\n{'='*50}")
    print(f"🛡️  SYSCALL SUMMARY (eBPF): {test_name}")
    print(f"{'='*50}")

    total_mb_read = 0
    total_mb_written = 0

    # Iterate through operations (read, pwrite, openat, close)
    for index, op_string in enumerate(headers):
        # Clean operation name (e.g., "read(C/B)" -> "read")
        operation_name = op_string.split('(')[0].upper()
        
        # Get "Count/Bytes" values (e.g., "16218/66428928")
        values = totals[index]
        
        if '/' in values:
            count_str, bytes_str = values.split('/')
            count = int(count_str)
            bytes_val = int(bytes_str)
            mb_val = bytes_val / (1024 * 1024)

            print(f"🔄 Operation: {operation_name}")
            print(f"   ┣ Executed Calls: {count:,}")
            
            # Only show bytes line if operation involves data movement
            if bytes_val > 0 or operation_name in ['READ', 'PWRITE']:
                print(f"   ┗ Transacted Data: {mb_val:.2f} MB ({bytes_val:,} bytes)\n")
                
                # Sum global counters for final summary
                if operation_name == 'READ':
                    total_mb_read += mb_val
                elif operation_name == 'PWRITE':
                    total_mb_written += mb_val
        else:
            # For future syscalls that might not have C/B format
            print(f"🔄 Operation: {operation_name}")
            print(f"   ┗ Executed Calls: {values}\n")

    print("-" * 50)
    print("📈 ACTUAL BACKEND DATA BALANCE:")
    print(f"   Total Read:    {total_mb_read:.2f} MB")
    print(f"   Total Written: {total_mb_written:.2f} MB")
    print("=" * 50 + "\n")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python3 syscounter_analyser.py <path_to_syscounter_file.txt>")
    else:
        analyse_syscounter(sys.argv[1])
