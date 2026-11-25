#!/bin/sh
# Performance check: runs benchmark programs several times, keeps the best value of each
# measure (to ignore interruptions) and compares it with the recorded baseline of this machine.
# A throughput more than 5 % below its baseline fails the check. Latencies depend heavily on
# the scheduler (they vary by several times from run to run), so they only fail when they get
# more than twice as slow.
#
# Usage: bench_compare.sh [--record] <bench-program>...
#   --record   write the current best values as the new baseline of this machine
# Baselines live in Bench/Baselines/<os>-<arch>.txt (lower case) ("<name> <value>" per line). Names ending in
# "us" (latencies) are better when lower, all others (throughputs) when higher.
# With no baseline for this machine the check passes and says so.
cd "$(dirname "$0")/../.." || exit 1
record=0
[ "$1" = "--record" ] && { record=1; shift; }
baseline="Bench/Baselines/$(uname -s | tr A-Z a-z)-$(uname -m).txt"
runs=5

# Run every program `runs` times and keep the best value per measure.
results=$(for i in $(seq $runs); do for prog in "$@"; do "$prog"; done; done | grep -v '^#' |
    awk '{ name=$1; value=$2; unit=$3;
           lower = (unit == "us");
           if (!(name in best) || (lower && value < best[name]) || (!lower && value > best[name])) best[name]=value;
           u[name]=unit }
         END { for (n in best) print n, best[n], u[n] }' | sort)
echo "$results"

if [ $record = 1 ]; then
    echo "$results" | awk '{print $1, $2, $3}' > "$baseline"
    echo "baseline written to $baseline"
    exit 0
fi
if [ ! -f "$baseline" ]; then
    echo "no baseline for this machine ($baseline): nothing to compare, run with --record to create it"
    exit 0
fi

# Compare: fail on more than 5 % regression.
echo "$results" | awk -v base="$baseline" '
    BEGIN { while ((getline line < base) > 0) { split(line, f, " "); old[f[1]] = f[2]; unit[f[1]] = f[3] } }
    { name=$1; now=$2;
      if (!(name in old)) { printf "%-26s %10s  (new, no baseline)\n", name, now; next }
      lower = (unit[name] == "us");
      change = lower ? (now - old[name]) / old[name] : (old[name] - now) / old[name]; # >0 = worse
      limit = lower ? 1.0 : 0.05;
      status = change > limit ? "REGRESSION" : "ok";
      if (change > limit) bad = 1;
      printf "%-26s %10s vs %10s  %+6.1f%% worse  %s\n", name, now, old[name], change * 100, status }
    END { exit bad }'
