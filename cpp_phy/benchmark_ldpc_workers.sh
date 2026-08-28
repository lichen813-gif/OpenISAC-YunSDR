#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd -- "${script_dir}/.." && pwd)"
binary="${1:-${project_root}/build/cuda-release/openisac_phy_video_bridge}"
output_dir="${2:-${project_root}/out/ldpc-worker-sweep}"
packets="${3:-500}"
warmup="${4:-50}"
repeats="${5:-3}"
backend="${6:-cuda}"
worker_list="${7:-1 2 4 6 8}"
profile_backend="${8:-0}"
metrics="${output_dir}/ldpc_worker_performance.csv"

mkdir -p "${output_dir}"
if [[ -e "${metrics}" ]]; then
    echo "Refusing to append to existing benchmark file: ${metrics}" >&2
    exit 2
fi

common=(
    --self-test "${packets}"
    --benchmark-warmup "${warmup}"
    --metrics-csv "${metrics}"
    --modulation 64QAM
    --snr 45
    --cfo 300
    --sfo 20
    --timing 20
    --tx-correlation 0.02
    --rx-correlation 0.02
    --seed 49239
    --spatial-seed 49239
    --sensing-coherent 0
    --backend "${backend}"
)
if [[ "${profile_backend}" == "1" ]]; then
    common+=(--profile-backend)
fi

run_case() {
    local label="$1"
    local workers="$2"
    shift 2
    for ((repeat = 1; repeat <= repeats; ++repeat)); do
        echo "[${label}_w${workers}] repeat ${repeat}/${repeats}"
        "${binary}" "${common[@]}" --workers "${workers}" \
            --benchmark-label "${label}_w${workers}" "$@"
    done
}

for workers in ${worker_list}; do
    for pilot in fdm nr-dmrs; do
        run_case "rank2_4port_${pilot}" "${workers}" \
            --rank 2 --tx-ports 4 --rx-ports 4 \
            --mimo-mode spatial --pilot-mode "${pilot}"
        run_case "rank4_${pilot}" "${workers}" \
            --rank 4 --tx-ports 4 --rx-ports 4 \
            --mimo-mode spatial --pilot-mode "${pilot}"
    done
done

echo "Wrote ${metrics}"
