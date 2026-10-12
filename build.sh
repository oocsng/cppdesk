#!/usr/bin/env bash
set -euo pipefail

# Portable build script for macOS and Linux

DO_TEST=false
BUILD_TESTS=false

for arg in "$@"; do
    case "$arg" in
        --test)
            DO_TEST=true
            BUILD_TESTS=true
            ;;
        --build-tests)
            BUILD_TESTS=true
            ;;
        *)
            echo "Unknown argument: $arg"
            echo "Usage: ./build.sh [--build-tests] [--test]"
            exit 1
            ;;
    esac
done

if ! command -v cmake &> /dev/null; then
    echo "Error: cmake is required but not installed." >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"

mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

echo "Configuring build directory with CMake..."
cmake -DCMAKE_BUILD_TYPE=Release "${SCRIPT_DIR}"

NPROC=4
if command -v sysctl &> /dev/null; then
    NPROC=$(sysctl -n hw.ncpu 2>/dev/null || echo 4)
elif command -v nproc &> /dev/null; then
    NPROC=$(nproc 2>/dev/null || echo 4)
fi

echo "Compiling with ${NPROC} parallel jobs..."
if [ "$BUILD_TESTS" = true ]; then
    cmake --build . --config Release --target CppDeskTests -- -j"${NPROC}"
else
    cmake --build . --config Release -- -j"${NPROC}"
    if [ -d "./CppDesk.app" ]; then
        echo "Successfully built macOS Application Bundle: ${BUILD_DIR}/CppDesk.app"
        tar -czf "${SCRIPT_DIR}/CppDesk-macos.tar.gz" -C "${BUILD_DIR}" CppDesk.app
        echo "Created archive: ${SCRIPT_DIR}/CppDesk-macos.tar.gz"
    fi
fi

if [ "$DO_TEST" = true ]; then
    echo "Running automated test suite..."
    if [ -f "./CppDeskTests" ]; then
        ./CppDeskTests
    elif [ -f "./Release/CppDeskTests" ]; then
        ./Release/CppDeskTests
    else
        echo "Error: CppDeskTests binary not found." >&2
        exit 1
    fi
fi
