#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
terrain_tests=OFF
if [[ -f "$root/tests/native_abi_test.cpp" ]]; then terrain_tests=ON; fi
terrain_tests="${BUILD_TESTING:-$terrain_tests}"
cmake -S "$root/native" -B "$root/build/native" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING="$terrain_tests"
cmake --build "$root/build/native" --parallel "${JOBS:-4}"
if [[ "$terrain_tests" == ON ]]; then ctest --test-dir "$root/build/native" --output-on-failure; fi
cmake --install "$root/build/native" --prefix "$root"
