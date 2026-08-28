#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd -- "${script_dir}/.." && pwd)"
build_dir="${1:-${project_root}/build/cuda-release}"
cuda_root="${CUDA_ROOT:-/usr/local/cuda}"

export PATH="${cuda_root}/bin:${PATH}"
cmake -S "${script_dir}" -B "${build_dir}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DOPENISAC_ENABLE_CUDA=ON \
    -DCMAKE_CUDA_COMPILER="${cuda_root}/bin/nvcc" \
    -DCMAKE_CUDA_ARCHITECTURES=native
cmake --build "${build_dir}" --parallel "$(nproc)"
ctest --test-dir "${build_dir}" --output-on-failure

echo "DGX CUDA build and tests passed: ${build_dir}"
