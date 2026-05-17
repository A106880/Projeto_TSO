#!/bin/bash

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_ROOT"

echo "=========================================================="
echo " STARTING ALL BENCHMARKS"
echo " Project Root: $PROJECT_ROOT"
echo "=========================================================="

SCRIPTS=$(find script-teste -name "*.sh" -not -name "run_all.sh" -type f | sort)

if [ -z "$SCRIPTS" ]; then
    echo "No test scripts found in script-teste/"
    exit 1
fi

for s in $SCRIPTS; do
    echo ""
    echo "############################################################"
    echo " RUNNING: $s"
    echo "############################################################"
    
    bash "$s"
    
    echo "############################################################"
    echo " COMPLETED: $s"
    echo "############################################################"
done

echo ""
echo "=========================================================="
echo " ALL TESTS COMPLETED!"
echo " Check results in: $PROJECT_ROOT/benchmark_results/"
echo "=========================================================="
