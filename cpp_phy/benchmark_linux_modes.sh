#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd -- "${script_dir}/.." && pwd)"
binary="${1:-${project_root}/build/cpu-release/openisac_phy_video_bridge}"
output_dir="${2:-${project_root}/out/platform-benchmark/linux}"
packets="${3:-200}"
warmup="${4:-20}"
repeats="${5:-5}"
backend="${6:-cpu}"
metrics="${output_dir}/mode_performance.csv"

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

run_case() {
    local label="$1"
    shift
    for ((repeat = 1; repeat <= repeats; ++repeat)); do
        echo "[${label}] repeat ${repeat}/${repeats}"
        "${binary}" "${common[@]}" --benchmark-label "${label}" "$@"
    done
}

for pilot in fdm nr-dmrs; do
    run_case "siso_${pilot}" --rank 1 --tx-ports 1 --rx-ports 1 \
        --mimo-mode spatial --pilot-mode "${pilot}"
    run_case "mimo2_${pilot}" --rank 2 --tx-ports 2 --rx-ports 2 \
        --mimo-mode spatial --pilot-mode "${pilot}"
    run_case "stbc_${pilot}" --rank 1 --tx-ports 2 --rx-ports 2 \
        --mimo-mode stbc --pilot-mode "${pilot}"
    run_case "rank2_4port_${pilot}" --rank 2 --tx-ports 4 --rx-ports 4 \
        --mimo-mode spatial --pilot-mode "${pilot}"
    run_case "rank4_${pilot}" --rank 4 --tx-ports 4 --rx-ports 4 \
        --mimo-mode spatial --pilot-mode "${pilot}"
done

echo "Wrote ${metrics}"
