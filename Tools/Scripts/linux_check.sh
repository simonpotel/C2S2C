#!/bin/sh
# Runs the whole test suite on Linux inside an Ubuntu container: this is how the epoll backend
# and ThreadSanitizer (which cannot run on recent macOS) are checked from a Mac.
# (seccomp is relaxed because Docker's default profile blocks io_uring.) Needs Docker. Usage: linux_check.sh [preset...]   (default: debug asan tsan)
cd "$(dirname "$0")/../.." || exit 1
presets=${*:-debug asan tsan}

# The toolchain image is built once and cached by Docker, so later runs start immediately.
docker build -q -t duplex-linux-check - <<'DOCKERFILE' >/dev/null || exit 1
FROM ubuntu:24.04
RUN apt-get update -qq && apt-get install -y -qq build-essential cmake ninja-build python3 rustc cargo \
    && rm -rf /var/lib/apt/lists/*
DOCKERFILE

# The source is mounted read-only and copied, so Linux builds never touch the host's Build/.
docker run --rm --security-opt seccomp=unconfined -v "$PWD":/src:ro duplex-linux-check sh -c '
    mkdir /work
    tar -C /src --exclude=Build --exclude=.git -cf - . | tar -C /work -xf -
    cd /work
    for p in '"$presets"'; do
        echo "=== $p ==="
        cmake --preset $p >/dev/null
        cmake --build --preset $p 2>&1 | grep -E "error|warning" || true
        DX_STRESS_SECONDS=2 ctest --preset $p 2>&1 | grep -E "tests passed|Failed|Not Run"
    done
    # The runtime tests again on every other backend (1 = poll, 4 = io_uring; auto was epoll).
    for b in 1 4; do
        echo "=== debug, backend $b ==="
        DX_TEST_BACKEND=$b DX_STRESS_SECONDS=2 ctest --preset debug -L "Faults|Integration|Stress" 2>&1 | grep -E "tests passed|Failed|Not Run"
    done
'
