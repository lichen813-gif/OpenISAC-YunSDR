#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd -- "${script_dir}/.." && pwd)"
build_dir="${1:-${project_root}/build/cpu-release}"

cmake -S "${script_dir}" -B "${build_dir}" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "${build_dir}" --parallel "$(nproc)"
ctest --test-dir "${build_dir}" --output-on-failure

echo "DGX/Linux CPU build and tests passed: ${build_dir}"
