#!/usr/bin/env bash
# Uniflow receiver - Linux dev environment.
#
#   ./dev.sh            open a shell in the container, repo mounted at /workspace
#   ./dev.sh test       configure, build and run the tests, then exit
#   ./dev.sh <cmd...>   run an arbitrary command in the container
#
# The repo is BIND MOUNTED, so edits on the host are visible immediately and
# build output lands back on the host. Nothing is copied into the image.
#
# Tests need room for the receiver-owned SHM segment (~326 MB against a 64 MB
# container default) plus a writable tmp dir for fixture files, hence
# --shm-size and TMPDIR below. Mirrors sender/dev.sh.
set -euo pipefail

IMAGE="${IMAGE:-nexus-receiver-dev}"
cd "$(dirname "$0")"

docker build -t "$IMAGE" -f Dockerfile .

# -t only when stdin is a terminal, so this also works from a script or CI.
TTY_FLAGS=(-i)
[ -t 0 ] && TTY_FLAGS=(-i -t)

run() {
    docker run --rm "${TTY_FLAGS[@]}" \
        --shm-size=512m \
        -e TMPDIR=/tmp \
        -v "$PWD":/workspace \
        -w /workspace \
        "$IMAGE" "$@"
}

# build-linux/, NOT a shared directory. A CMake cache records absolute
# compiler paths, so caches from different platforms in the same directory
# will fight. Keep one build directory per platform.
case "${1:-shell}" in
    shell)
        run bash
        ;;
    test)
        run bash -lc '
            cmake -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Debug &&
            cmake --build build-linux &&
            ctest --test-dir build-linux --output-on-failure'
        ;;
    *)
        run "$@"
        ;;
esac
