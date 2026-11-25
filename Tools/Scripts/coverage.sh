#!/bin/sh
# Measures how much of Source/ the C tests execute (lines, branches) with clang's llvm-cov.
# Usage: coverage.sh        needs clang and llvm-cov/llvm-profdata of the same LLVM release
#   (on macOS: brew install llvm; the script finds it there).
cd "$(dirname "$0")/../.." || exit 1
llvm=${LLVM_BIN:-/opt/homebrew/opt/llvm/bin}
[ -x "$llvm/clang" ] || llvm=$(dirname "$(command -v clang)")
build=Build/coverage
profiles="$PWD/$build/profiles"
rm -rf "$build"

# Instrumented build of the library and every test (not the other languages' bindings).
CC="$llvm/clang" cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DDX_COVERAGE=ON >/dev/null || exit 1
cmake --build "$build" >/dev/null || exit 1

# Run the C tests; each process writes its own raw profile.
mkdir -p "$profiles"
LLVM_PROFILE_FILE="$profiles/%p.profraw" DX_STRESS_SECONDS=1 \
    ctest --test-dir "$build" -E "cross_|example_|ffi_|performance|node|python|rust|cpp|wasm|web|generated_|fuzz" >/dev/null
"$llvm/llvm-profdata" merge -sparse "$profiles"/*.profraw -o "$build/all.profdata"

# Report only Source/ (the library), using every test executable as the code to look at.
objects=""
for exe in "$build"/Tests/test_*; do [ -x "$exe" ] && [ -f "$exe" ] && objects="$objects -object $exe"; done
first=$(echo "$objects" | awk '{print $2}')
# shellcheck disable=SC2086
"$llvm/llvm-cov" report "$first" $(echo "$objects" | cut -d' ' -f4-) -instr-profile="$build/all.profdata" \
    -ignore-filename-regex='Tests/|Generated/|Bindings/|Examples/|Bench/' "$@"
