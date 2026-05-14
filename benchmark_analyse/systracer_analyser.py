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
    layer_counter = Counter() # VFS, EXT4, BIO, syscall
    unique_files = set()
    total_processed_lines = 0

    data_started = False

    for line in lines:
        line = line.strip()
        
        # Ignore empty lines
        if not line:
            continue
            
        # Detect end of file and if events were lost by eBPF
        if line.startswith("Lost"):
            lost_events = line
            continue

        # Detect when the data table actually starts
        if line.startswith("pid") and "command" in line:
            continue
        if line.startswith("---"):
            data_started = True
            continue

        # Process real data lines
        if data_started:
            # split() divides by spaces (even multiple spaces)
            parts = line.split()
            
            # A valid systracer line has at least 5 columns:
            # [0]pid, [1]command, [2]syscall, [3]type, [4]filename
            if len(parts) >= 5:
                # PID and Command are not needed as they are always your FUSE
                operation = parts[2]
                layer = parts[3]
                file_name = parts[4]

                syscall_counter[operation] += 1
                layer_counter[layer] += 1
                unique_files.add(file_name)
                total_processed_lines += 1

    print(f"\n{'='*50}")
    print(f"🕵️  TRACER X-RAY (eBPF): {test_name}")
    print(f"{'='*50}")

    print(f"Total events processed: {total_processed_lines:,}")
    print(f"Capture State: {lost_events}\n")

    print("📚 KERNEL LAYER DISTRIBUTION:")
    for layer, count in layer_counter.most_common():
        percentage = (count / total_processed_lines) * 100 if total_processed_lines > 0 else 0
        print(f"   ┣ {layer:<10}: {count:>8,} events ({percentage:.1f}%)")

    print("\n⚙️  TOP OPERATIONS / SYSCALLS:")
    # Show only top 5 to keep terminal clean
    for op, count in syscall_counter.most_common(5):
        print(f"   ┣ {op:<10}: {count:>8,} times")

    print("\n📁 INTERCEPTED FILES (Unique Accesses):")
    # Show at most 5 files to summarize
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
