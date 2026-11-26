#!/bin/sh
# Service benchmark: the same ping/pong service, written three ways per language, compared.
#   C           libc2s2c directly (the best case: no language layer)
#   <lang> naive    written from scratch, without libc2s2c (what you write when you recode the protocol)
#   <lang> c2s2c   on top of the binding: "poll" = a loop over batches (replies sent in one call
#                   where the language makes calls costly), "dispatch" = the binding's handlers
# For each: throughput and latency under load (same C load generator), memory, how many lines the
# server takes, and how it behaves against hostile traffic (same C checker).
# Usage: bench_service.sh <build dir, release recommended> [seconds, default 5]
cd "$(dirname "$0")/../.." || exit 1
build=${1:?usage: bench_service.sh <build dir> [seconds]}
secs=${2:-5}
conns=64
inflight=4
case "$build" in *elease*) rust=Build/Rust/release ;; *) rust=Build/Rust/debug ;; esac
build=$(cd "$build" && pwd) # absolute paths: the language runtimes load files from other folders
lib=$(ls "$build"/libc2s2c.dylib "$build"/libc2s2c.so 2>/dev/null | head -1)
export PYTHONPATH="$build/PythonPackage" C2S2C_LIBRARY="$lib" C2S2C_ADDON="$build/Tests/c2s2c.node"
node="node --experimental-strip-types --no-warnings"

rss_mb() { kb=$(ps -o rss= -p "$1" 2>/dev/null | tr -d ' '); echo "$(( ${kb:-0} / 1024 ))"; }
loc() { cat "$@" | grep -cvE '^[[:space:]]*(#|//|/\*|\*|""")|^[[:space:]]*$'; }

printf '%-22s %12s %8s %8s %7s %6s  %s\n' implementation "round trips/s" "p50 us" "p99 us" "MB" lines hostile
run() { # name  source-files  command...
    name=$1; src=$2; shift 2
    out=$(mktemp)
    "$@" > "$out" 2>/dev/null &
    pid=$!
    tries=0
    until [ -s "$out" ] || [ $tries -ge 100 ]; do tries=$((tries + 1)); sleep 0.1; done
    port=$(head -n 1 "$out")
    if [ -z "$port" ]; then printf '%-22s did not start\n' "$name"; kill $pid 2>/dev/null; rm -f "$out"; return; fi
    # Three load generators at once (each with its own connections): one alone tops out near the
    # speed of the fastest server. Throughputs add up; the worst p99 is reported.
    for k in 1 2 3; do "$build"/Tests/loadgen "$port" $conns "$secs" $inflight > "$out.$k" & done
    wait $(jobs -p | grep -v "^$pid$") 2>/dev/null
    result=$(cat "$out".1 "$out".2 "$out".3 | awk '/throughput/{t+=$2} /^p50/{if($2>a)a=$2} /^p99/{if($2>b)b=$2} END{print "throughput " t " round_trips/s"; print "p50 " a " us"; print "p99 " b " us"}')
    rm -f "$out".1 "$out".2 "$out".3
    mb=$(rss_mb $pid)
    hostile=$("$build"/Tests/hostile "$port" | tail -1)
    alive=up
    kill -0 $pid 2>/dev/null || alive=DEAD
    printf '%-22s %12s %8s %8s %7s %6s  %s %s\n' "$name" \
        "$(echo "$result" | awk '/throughput/{print $2}')" "$(echo "$result" | awk '/^p50/{print $2}')" \
        "$(echo "$result" | awk '/^p99/{print $2}')" "$mb" "$(loc $src)" "$hostile" "$alive"
    kill $pid 2>/dev/null; wait $pid 2>/dev/null; rm -f "$out"
}

run "C (libc2s2c)"        Bench/Service/C/server.c                "$build"/Tests/bench_service_c
run "Rust naive"           Bench/Service/Rust/server_naive.rs      $rust/bench_service_naive
run "Rust c2s2c poll"     Bench/Service/Rust/server_c2s2c.rs     $rust/bench_service_c2s2c poll
run "Rust c2s2c dispatch" Bench/Service/Rust/server_c2s2c.rs     $rust/bench_service_c2s2c dispatch
run "Python naive"         Bench/Service/Python/server_naive.py    python3 Bench/Service/Python/server_naive.py
run "Python c2s2c poll"   Bench/Service/Python/server_c2s2c.py   python3 Bench/Service/Python/server_c2s2c.py poll
run "Python c2s2c dispatch" Bench/Service/Python/server_c2s2c.py python3 Bench/Service/Python/server_c2s2c.py dispatch
run "Node naive"           Bench/Service/Node/server_naive.ts      $node Bench/Service/Node/server_naive.ts
run "Node c2s2c poll"     Bench/Service/Node/server_c2s2c.ts     $node Bench/Service/Node/server_c2s2c.ts poll
run "Node c2s2c dispatch" Bench/Service/Node/server_c2s2c.ts     $node Bench/Service/Node/server_c2s2c.ts dispatch
