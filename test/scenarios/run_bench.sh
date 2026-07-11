#!/bin/bash
# Run bench_ditto N times and extract key metrics.
# Usage: run_bench.sh <label> <N>
LABEL="${1:-baseline}"
N="${2:-3}"

cd "$(dirname "$0")"
echo "=== Running $N bench iterations with label=$LABEL ==="

for i in $(seq 1 $N); do
    echo ""
    echo "--- Run $i/$N ---"
    OUTPUT="/tmp/vfs_scenario_results/bench_${LABEL}_run${i}.json"
    python3 -u bench_ditto.py --label "${LABEL}_run${i}" --output "$(dirname $OUTPUT)" 2>&1 | tail -20
    # Move the actual output file
    if [ -f "/tmp/vfs_scenario_results/bench_${LABEL}_run${i}.json" ]; then
        echo "[saved to $OUTPUT]"
    fi
done

echo ""
echo "=== Summary for $LABEL ==="
python3 - << PYEOF
import json, glob
files = sorted(glob.glob("/tmp/vfs_scenario_results/bench_${LABEL}_run*.json"))
if not files:
    print("no files found"); exit()
for f in files:
    with open(f) as fp: data = json.load(fp)
    steps = {s["name"]: s["elapsed_ms"] for s in data["steps"]}
    val = data.get("validation", {})
    print(f"  {f.split('/')[-1]}: copy={steps.get('copy_in', 0):.0f}ms extract={steps.get('ditto_unzip', 0):.0f}ms common={val.get('common', 0)} only_vfs={val.get('only_vfs', 0)} only_host={val.get('only_host', 0)} md5_fail={val.get('mismatches', 0)}")
PYEOF
