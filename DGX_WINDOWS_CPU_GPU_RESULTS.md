# Windows CPU and DGX Spark CPU/CUDA stage results

This document summarizes the cross-platform C++ PHY milestone completed on
2026-08-28. Detailed Chinese reports are available in
[`DGX_PHY_PORT_STATUS.md`](DGX_PHY_PORT_STATUS.md),
[`DGX_WINDOWS_PHY_PERFORMANCE.md`](DGX_WINDOWS_PHY_PERFORMANCE.md), and
[`DGX_Spark视频传输与CUDA移植方案.md`](DGX_Spark视频传输与CUDA移植方案.md).

## Implemented scope

- Windows x64/Visual Studio 2019 CPU and DGX Spark ARM64/GCC CPU builds share
  the same C++17 PHY source.
- The optional CUDA 13/cuFFT backend provides batched 1024-point OFDM and
  general 1/2/4/8-stream MMSE/ZF detection.
- The runtime selects `--backend cpu|cuda|auto`; CPU remains the numerical
  reference and fallback when CUDA is unavailable.
- SISO, 2x2 Rank-2, 2x2 Alamouti STBC, 4-port Rank-2, and 4x4 Rank-4 formal
  frame paths are covered. The 8x8 detector and algorithm chain are validated,
  but an 8x8 formal-frame/video mode is not claimed.
- Preamble synchronization, channel simulation, sparse SFO regression, and
  LDPC/CRC remain on CPU. CUDA acceleration is a hybrid full-frame backend,
  not an all-GPU implementation.

## Validation snapshot

- Windows CPU: CTest 10/10 passed.
- DGX Spark CPU: CTest 10/10 passed, including sanitizer validation.
- DGX Spark CUDA: CTest 12/12 passed with CPU/CUDA numerical equivalence.
- Three 50-run platform sweeps (150 runs total) recovered every UDP byte with
  FER 0 and no UDP packet loss.

## Current median frame latency

The measurements use Release builds, 1024 subcarriers, CP 128, 64QAM, fixed
channel parameters, and five runs. The common platform sweep uses 20 warm-up
and 200 measured packets. The two four-port FDM CUDA values below use the later
device-resident retest with 50 warm-up and 500 measured packets; the matching
raw snapshot is published with the other boundary measurements.

| Mode | Windows CPU ms/frame | DGX CPU ms/frame | DGX CUDA ms/frame |
| --- | ---: | ---: | ---: |
| SISO FDM | 0.1892 | 0.9313 | 0.3731 |
| SISO NR-DMRS | 0.2380 | 1.0342 | 0.4037 |
| 2x2 Rank-2 FDM | 0.3245 | 0.9482 | 0.5916 |
| 2x2 Rank-2 NR-DMRS | 0.3927 | 1.1696 | 0.8381 |
| 2x2 STBC FDM | 0.2340 | 0.9199 | 0.3453 |
| 2x2 STBC NR-DMRS | 0.3130 | 1.0289 | 0.4173 |
| 4-port Rank-2 FDM | 2.1445 | 2.7828 | 1.3956 |
| 4-port Rank-2 NR-DMRS | 3.1255 | 3.3444 | 1.9814 |
| 4x4 Rank-4 FDM | 2.4992 | 3.0754 | 1.7490 |
| 4x4 Rank-4 NR-DMRS | 3.4549 | 3.7084 | 2.3033 |

CUDA provides the most stable benefit on four-port modes. Windows CPU remains
the lowest-latency option for the smaller SISO, 2x2, and STBC batches.

## Reproduction and data

- Windows build and benchmark scripts are under [`cpp_phy/`](cpp_phy/README.md).
- DGX CPU/CUDA scripts include `build_linux_dgx.sh`, `build_linux_cuda.sh`,
  `benchmark_linux_modes.sh`, and `benchmark_ldpc_workers.sh`.
- The published raw CSV and comparison Markdown snapshots are under
  [`results/benchmarks/`](results/benchmarks/README.md).

Video files, build directories, executables, profiler caches, and vendor SDK
binaries are intentionally excluded from the repository.
