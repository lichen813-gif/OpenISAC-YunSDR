# Published Windows and DGX benchmark snapshots

This directory contains the small, text-only result set used by the Windows
CPU, DGX Spark CPU, and DGX Spark CUDA stage reports dated 2026-08-27 and
2026-08-28.

- `platform-benchmark/` contains per-mode measurements and generated platform
  comparison tables.
- `ldpc-worker-sweep-*` contains the Windows and DGX LDPC worker-count sweeps.
- `cuda-boundary-optional-profile-20260828/` contains the CUDA boundary,
  profiling, cache-local FDM, and device-resident FDM comparisons.

The files are reproducibility artifacts, not runtime inputs. New benchmark
runs continue to write to the ignored or local working `out/` path selected by
the scripts; reviewed snapshots should be copied here only when publishing a
milestone. No video, IQ capture, executable, profiler cache, or machine-specific
binary is stored in this directory.
