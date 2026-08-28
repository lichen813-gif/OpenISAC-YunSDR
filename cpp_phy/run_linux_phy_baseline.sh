#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd -- "${script_dir}/.." && pwd)"
build_dir="${1:-${project_root}/build/cpu-release}"
output_root="${2:-${project_root}/out/dgx-spark/baseline}"
frames="${3:-5}"
video_packets="${4:-20}"

mkdir -p "${output_root}/nxn" "${output_root}/formal" \
    "${output_root}/bench" "${output_root}/modes"

ctest --test-dir "${build_dir}" --output-on-failure \
    | tee "${output_root}/ctest.log"
"${build_dir}/openisac_phy_bench" \
    | tee "${output_root}/detector_fft_ldpc.log"

run_mode_baseline() {
    local name="$1"
    shift
    "${build_dir}/openisac_phy_video_bridge" \
        --self-test "${video_packets}" --modulation 64QAM --snr 45 \
        --sensing-coherent 0 "$@" \
        | tee "${output_root}/modes/${name}.log"
}

for pilot in fdm nr-dmrs; do
    run_mode_baseline "siso_${pilot}" \
        --rank 1 --tx-ports 1 --rx-ports 1 --mimo-mode spatial \
        --pilot-mode "${pilot}"
    run_mode_baseline "mimo2_${pilot}" \
        --rank 2 --tx-ports 2 --rx-ports 2 --mimo-mode spatial \
        --pilot-mode "${pilot}"
    run_mode_baseline "stbc_${pilot}" \
        --rank 1 --tx-ports 2 --rx-ports 2 --mimo-mode stbc \
        --pilot-mode "${pilot}"
done

for streams in 1 2 4 8; do
    "${build_dir}/openisac_phy_nxn_diagnostics" \
        --streams "${streams}" --frames "${frames}" \
        --modulation 64QAM --snr 50 \
        --tx-correlation 0.02 --rx-correlation 0.02 \
        --flat-channel --check \
        --output "${output_root}/nxn/${streams}x${streams}_flat" \
        | tee "${output_root}/nxn/${streams}x${streams}_flat.log"
    "${build_dir}/openisac_phy_nxn_diagnostics" \
        --streams "${streams}" --frames "${frames}" \
        --modulation 64QAM --snr 50 \
        --tx-correlation 0.02 --rx-correlation 0.02 \
        --output "${output_root}/nxn/${streams}x${streams}_tdl" \
        | tee "${output_root}/nxn/${streams}x${streams}_tdl.log"
done

"${build_dir}/openisac_phy_4x4_formal_diagnostics" \
    --frames "${frames}" --snr 50 --modulation 64QAM \
    --tx-correlation 0.02 --rx-correlation 0.02 \
    --output "${output_root}/formal/rank4_frequency" --check \
    | tee "${output_root}/formal/rank4_frequency.log"

for pilot in fdm nr-dmrs; do
    "${build_dir}/openisac_phy_4x4_time_diagnostics" \
        --frames "${frames}" --snr 50 --modulation 64QAM \
        --pilot-mode "${pilot}" --timing 20 --cfo 300 --sfo 20 \
        --tx-correlation 0.02 --rx-correlation 0.02 \
        --output "${output_root}/formal/rank4_time_${pilot}" --check \
        | tee "${output_root}/formal/rank4_time_${pilot}.log"
done

"${build_dir}/openisac_phy_full_pipeline_benchmark" \
    "${output_root}/bench/rank2_frames.csv" 50 \
    | tee "${output_root}/bench/rank2.log"
"${build_dir}/openisac_phy_4x4_time_pipeline_benchmark" \
    "${output_root}/bench/rank4_frames.csv" 20 12 \
    | tee "${output_root}/bench/rank4.log"
"${build_dir}/openisac_phy_sensing_benchmark" \
    | tee "${output_root}/bench/sensing.log"

echo "Complete CPU PHY baseline passed: ${output_root}"
