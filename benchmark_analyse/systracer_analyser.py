import sys
import os
from collections import Counter

def analyse_systracer(file_path):
    try:
        with open(file_path, 'r') as f:
            lines = f.readlines()
    except FileNotFoundError:
        print(f"Error: File '{file_path}' not found.")
        return

    test_name = os.path.basename(file_path).replace('_systracer.txt', '')

    lost_events = "Unknown"
    
    # Counters and collections to store telemetry
    syscall_counter = Counter()
    layer_counter = Counter()
    unique_files = set()
    total_processed_lines = 0

    data_started = False

    for line in lines:
        line = line.strip()
        
        if not line:
            continue
            
        if line.startswith("Lost"):
            lost_events = line
            continue

        if line.startswith("pid") and "command" in line:
            continue
        if line.startswith("---"):
            data_started = True
            continue

        # Process data lines
        if data_started:
            parts = line.split()
            
            if len(parts) >= 5:
                operation = parts[2]
                layer = parts[3]
                file_name = parts[4]

                syscall_counter[operation] += 1
                layer_counter[layer] += 1
                unique_files.add(file_name)
                total_processed_lines += 1

    print(f"\n{'='*50}")
    print(f"TRACER X-RAY (eBPF): {test_name}")
    print(f"{'='*50}")

    print(f"Total events processed: {total_processed_lines:,}")
    print(f"Capture State: {lost_events}\n")

    print("KERNEL LAYER DISTRIBUTION:")
    for layer, count in layer_counter.most_common():
        percentage = (count / total_processed_lines) * 100 if total_processed_lines > 0 else 0
        print(f"   ┣ {layer:<10}: {count:>8,} events ({percentage:.1f}%)")

    print("\nTOP OPERATIONS / SYSCALLS:")
    for op, count in syscall_counter.most_common(5):
        print(f"   ┣ {op:<10}: {count:>8,} times")

    print("\nINTERCEPTED FILES (Unique Accesses):")
    file_list = list(unique_files)
    for file in file_list[:5]:
        print(f"   ┣ {file}")
    if len(file_list) > 5:
        print(f"   ┗ ... and {len(file_list) - 5} more hidden files.")
    elif len(file_list) > 0:
        print("   ┗ (End of list)")
    else:
        print("   ┗ No files detected (Strange!)")
        
    print("=" * 50 + "\n")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python3 systracer_analyser.py <path_to_systracer_file.txt>")
    else:
        analyse_systracer(sys.argv[1])
