import os
import subprocess

# Directory configuration
RESULTS_DIR = "benchmark_results"
ANALYSE_DIR = "benchmark_analyse"

# Mapping file suffixes to their respective analysers
ANALYSERS = {
    "_fio.json": "fio_analyser.py",
    "_pidstat.txt": "pidstat_analyser.py",
    "_syscounter.txt": "syscounter_analyser.py",
    "_systracer.txt": "systracer_analyser.py"
}

def run_analysis():
    if not os.path.exists(RESULTS_DIR):
        print(f"Error: Directory {RESULTS_DIR} not found.")
        return

    # Get list of subdirectories (test categories)
    categories = [d for d in os.listdir(RESULTS_DIR) if os.path.isdir(os.path.join(RESULTS_DIR, d))]
    
    if not categories:
        print("No results subdirectories found.")
        return

    print(f"Found {len(categories)} test categories. Starting analysis...")

    for test_category in sorted(categories):
        category_path = os.path.join(RESULTS_DIR, test_category)
        resume_file = f"resume_{test_category}.txt"
        
        print(f" -> Analyzing {test_category}...", end=" ", flush=True)

        with open(resume_file, "w", encoding="utf-8") as f_out:
            f_out.write(f"{'='*70}\n")
            f_out.write(f" GLOBAL SUMMARY: {test_category}\n")
            f_out.write(f"{'='*70}\n")

            all_files = sorted(os.listdir(category_path))
            
            prefixes = set()
            for f in all_files:
                for suffix in ANALYSERS.keys():
                    if f.endswith(suffix):
                        prefixes.add(f.replace(suffix, ""))
            
            if not prefixes:
                f_out.write("\nNo monitoring data found in this directory.\n")
                print("Skipped (no data).")
                continue

            for prefix in sorted(list(prefixes)):
                f_out.write(f"\n\n{'#'*60}\n")
                f_out.write(f"### TEST DATA: {prefix}\n")
                f_out.write(f"{'#'*60}\n")

                for suffix, analyser_script in ANALYSERS.items():
                    file_to_analyse = os.path.join(category_path, prefix + suffix)
                    
                    if os.path.exists(file_to_analyse):
                        analyser_path = os.path.join(ANALYSE_DIR, analyser_script)
                        
                        try:
                            result = subprocess.run(
                                ["python3", analyser_path, file_to_analyse],
                                capture_output=True,
                                text=True,
                                encoding="utf-8"
                            )
                            
                            if result.stdout:
                                f_out.write(result.stdout)
                            
                            if result.stderr:
                                f_out.write(f"\n[ERROR IN {analyser_script}]:\n{result.stderr}\n")
                                
                        except Exception as e:
                            f_out.write(f"\n[FAILURE EXECUTING {analyser_script}]: {str(e)}\n")
            
            f_out.write(f"\n\n{'='*70}\n")
            f_out.write(f" END OF SUMMARY: {test_category}\n")
            f_out.write(f"{'='*70}\n")

        print(f"OK! (Generated: {resume_file})")

    print("\nProcessing completed.")

if __name__ == "__main__":
    run_analysis()
