import sys
import os

def pidstat_analyse(filepath):
    try:
        with open(filepath, 'r') as file:
            lines = file.readlines()
    except FileNotFoundError:
        print(f"Error: File '{filepath}' not found.")
        return

    test_name = os.path.basename(filepath).replace('_pidstat.txt', '')

    # Dictionary to store all measurements captured second by second
    metrics = {
        '%usr': [], '%system': [], '%CPU': [],
        'minflt/s': [], 'majflt/s': [], 'RSS': [], '%MEM': [],
        'cswch/s': [], 'nvcswch/s': []
    }

    active_header = []

    for line in lines:
        parts = line.split()
        if not parts:
            continue

        # Ignore OS header (e.g., Linux 6.14.0...)
        if parts[0] == "Linux":
            continue

        # Ignore "Average:" lines pre-calculated by pidstat
        # (we calculate the average ourselves for precision)
        if parts[0] == "Average:":
            continue

        # Detect column header line
        if "UID" in parts and "PID" in parts:
            active_header = parts
            continue

        # Data line (same number of columns as header)
        if active_header and len(parts) == len(active_header):
            try:
                # Confirm PID column is a number to avoid errors
                pid_idx = active_header.index("PID")
                int(parts[pid_idx])
                
                # Extract values based on column name
                for metric in metrics.keys():
                    if metric in active_header:
                        idx = active_header.index(metric)
                        # Replace commas with dots if system is in Portuguese
                        value = float(parts[idx].replace(',', '.'))
                        metrics[metric].append(value)
            except ValueError:
                # Corrupted line or misread header, skip
                continue

    # Helper function to calculate averages safely
    def calculate_average(data_list):
        if not data_list: return 0.0
        return sum(data_list) / len(data_list)

    # Calculate final averages
    avg_usr = calculate_average(metrics['%usr'])
    avg_sys = calculate_average(metrics['%system'])
    avg_tot = calculate_average(metrics['%CPU'])
    
    avg_minflt = calculate_average(metrics['minflt/s'])
    
    # RSS is in Kilobytes. Convert to Megabytes for readability.
    avg_rss_kb = calculate_average(metrics['RSS'])
    avg_rss_mb = avg_rss_kb / 1024
    avg_mem_pct = calculate_average(metrics['%MEM'])

    avg_cswch = calculate_average(metrics['cswch/s'])
    avg_nvcswch = calculate_average(metrics['nvcswch/s'])

    # Total samples read (in seconds)
    recorded_seconds = len(metrics['%CPU'])

    print(f"\n{'='*50}")
    print(f"HARDWARE USAGE (PIDSTAT): {test_name}")
    print(f"{'='*50}")
    print(f"analysed duration: ~{recorded_seconds} seconds\n")

    print("PROCESSOR CONSUMPTION (CPU):")
    print(f"   ┣ User Space (%usr): {avg_usr:>6.2f} % (Your C code)")
    print(f"   ┣ Kernel Space (%system):  {avg_sys:>6.2f} % (OS working for you)")
    print(f"   ┗ Total CPU Consumed:      {avg_tot:>6.2f} %\n")

    print("MEMORY CONSUMPTION (RAM):")
    print(f"   ┣ Real Memory (RSS):   {avg_rss_mb:>6.2f} MB")
    print(f"   ┣ RAM Percentage:   {avg_mem_pct:>6.2f} %")
    print(f"   ┗ Page Faults (Minor): {avg_minflt:>6.2f} per second\n")

    if metrics['cswch/s']:
        print("CONTEXT SWITCHES:")
        print(f"   ┣ Voluntary (Wait): {avg_cswch:>6.2f} per second")
        print(f"   ┗ Forced (Full CPU): {avg_nvcswch:>6.2f} per second")
    
    print("=" * 50 + "\n")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python3 pidstat_analyser.py <path_to_pidstat_file.txt>")
    else:
        pidstat_analyse(sys.argv[1])
