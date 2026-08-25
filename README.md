# OpenISAC-YunSDR: build and usage guide

[中文说明](README_zh.md) | [Design documents](docs/design/README.md) | [Changelog](CHANGELOG.md)

This repository contains the complete C++/Python source, configurations, and design documents for the current release. Video files are intentionally excluded.

> Current backend status: the main Linux `BS`/`UE` runtime remains simulation-only, and the former UHD/USRP implementation stays removed. The separate [`libyunsdr-isac/`](libyunsdr-isac/README.md) Windows subproject now implements the vendor-neutral `RadioBackend` contract for a verified YunSDR Y240 `pcies:0.0` path. Vendor SDK binaries, drivers, firmware, and videos are not redistributed in this repository.

## Components

| Component | Entry point | Purpose |
| --- | --- | --- |
| Channel model | `ChannelSimulator` | Shared-memory communication, sensing, noise, CFO/SRO, delay, and target simulation |
| Base station | `BS` | OFDM downlink, optional uplink receiver, monostatic sensing, UDP/ZMQ I/O |
| User equipment | `UE` | Downlink synchronization/demodulation, optional uplink transmitter, bistatic sensing |
| Runtime tools | `scripts/` | Sensing plots, diagnostics, configuration and control tools |
| Formal PHY | `cpp_phy/`, `python_phy/` | Cross-language frame, MIMO, modulation, LDPC, and regression validation |
| YunSDR hardware | `libyunsdr-isac/` | Y240 adapter, timestamped streams, RF loopback, VLC/UDP bridge, diagnostics, and hardware tests |

## Requirements

The main real-time programs target a C++17 Linux environment. Required libraries are Boost 1.66+, OpenMP, AFF3CT 3.0.2+, FFTW3f (including `fftw3f_threads`), yaml-cpp, libzmq, and cppzmq. DPDK is optional.

For Ubuntu/Debian, install the packaged dependencies first:

```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config \
  libboost-all-dev libfftw3-dev libyaml-cpp-dev \
  libzmq3-dev cppzmq-dev nlohmann-json3-dev python3-venv
```

Install AFF3CT 3.0.2 or newer separately and remember its installation prefix. No UHD package is required.

## Algorithm simulation

- Windows C++ PHY: [overview](cpp_phy/README.md), [formal frame](Windows_C++_PHY正式帧结构与使用说明.md), and [VLC video test](Windows_VLC视频信道仿真使用说明.md)
- Python golden model: [model and validation guide](python_phy/README.md)
- Design route: [Python MIMO to C++](Python_MIMO到C++实施路线.md) and [4x4/DM-RS validation](4x4_MIMO设计与第一阶段验证.md)

## YunSDR hardware integration

The Windows hardware adapter is documented in [`libyunsdr-isac/README.md`](libyunsdr-isac/README.md). Its source includes the Y240 `pcies:0.0` backend, formal-PHY codec bridge, timestamped SISO/2x2 streams, RF loopback and VLC/UDP tools. The verified vendor SDK must be staged locally and is intentionally excluded from Git.

```powershell
cd libyunsdr-isac
.\build_vs2019.cmd
```

## Build

```bash
git clone https://github.com/lichen813-gif/OpenISAC-YunSDR.git
cd OpenISAC-YunSDR
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAFF3CT_ROOT=/path/to/aff3ct/prefix
cmake --build build -j"$(nproc)"
```

If AFF3CT is installed in a standard prefix, omit `-DAFF3CT_ROOT`. The primary outputs are `build/ChannelSimulator`, `build/BS`, and `build/UE`.

## Repository layout

| Path | Description |
| :--- | :--- |
| `src/`, `include/` | Core C++ PHY, sensing, threading, and runtime logic |
| `config/` | Simulation presets for BS/UE roles |
| `scripts/` | Python frontends, web config console, and Linux performance helpers |
| `python_phy/` | Cross-platform Python PHY models, configurations, experiments, and tests |
| `cpp_phy/` | UHD-independent C++17 PHY core, VS2019 scripts, benchmarks, video bridge, and live monitor |
| `libyunsdr-isac/` | YunSDR Y240 hardware adapter, configuration, validation tools, and hardware documentation |
| `capture/` | Offline plotting helpers for saved sensing results |
| `docs/` | Static project site and architecture/signal-processing pages |

Python tools use:

```bash
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -r requirements.txt
```

## First simulation run

Use a matched BS/UE preset. From the repository root:

```bash
cp config/BS_Sim.yaml build/BS.yaml
cp config/UE_Sim.yaml build/UE.yaml
```

Open three terminals in `build/` and start them in this order:

```bash
# terminal 1
./ChannelSimulator BS.yaml

# terminal 2
./BS BS.yaml

# terminal 3
./UE UE.yaml
```

The simulator must start first because BS and UE attach to its shared-memory session. For uplink/duplex or eRTM tests, use the matching pairs `BS_Sim_Duplex.yaml` + `UE_Sim_Duplex.yaml` or `BS_Sim_eRTM.yaml` + `UE_Sim_eRTM.yaml`. Resource-map examples are also paired by name.

From the repository root, visualization examples include:

```bash
python scripts/plot_sensing_fast.py
python scripts/plot_bi_sensing_fast.py
```

## Runtime PHY parameters

The main `BS`/`UE` frame is controlled by YAML. Any TX/RX structural field must match on both sides.

| YAML section | Adjustable fields | Notes |
| --- | --- | --- |
| `rf_sampling` | `sample_rate`, `bandwidth` | Determines sample clock and occupied bandwidth |
| `ofdm_frame` | `fft_size`, `cp_length`, `num_symbols`, `sync_pos` | Basic OFDM frame structure |
| `ofdm_frame` | `enable_sec_sync_symbol`, `enable_cfo_training_sequence`, `cfo_training_period_samples` | Synchronization/CFO training layout |
| `ofdm_frame` | `zc_root`, `pilot_positions`, `midframe_pilot_symbols`, `midframe_pilot_seed` | Preamble and pilot layout |
| `downlink` | `center_freq`, `modulation` | Modulation: `qpsk`, `16qam`, `64qam`, or `256qam` |
| `uplink` | duplex enable/mode, `symbol_start`, `symbol_count`, `guard_symbols` | Selects TDD/FDD behavior and the TDD UL window in duplex presets |
| `sensing` | `rx_channel_count`, `range_fft_size`, `doppler_fft_size`, `symbol_stride`, `output_mode` | Sensing array and processing dimensions |
| `simulation` | `target_snr_db`, `noise_power_dbfs`, `cfo_hz`, `sample_rate_offset_ppm`, `timing_offset_samples` | Link impairment controls |
| `simulation` | `comm_multipath_taps`, `targets`, `bistatic_targets`, array spacing | Multipath and sensing scene |

Configuration rules:

- Keep BS and UE `sample_rate`, FFT/CP, frame-symbol count, synchronization layout, ZC root, pilot layout/seed, modulation, and duplex window identical.
- Every pilot or synchronization index must lie inside the configured FFT/frame; `sensing_symbol_num` must not exceed `num_symbols`.
- For TDD, `symbol_start + symbol_count` must not exceed `num_symbols`, and guard symbols consume part of the uplink window.
- Set CP longer than the largest channel delay that must remain free of inter-symbol interference.
- Use the same `simulation.session` for ChannelSimulator, BS, and UE. Do not reuse UDP/ZMQ ports across concurrent sessions.

The sample files in [`config/`](config/) are the authoritative runnable examples and contain field-level comments.

## Formal C++/Python PHY profile

The standalone `cpp_phy` compatibility profile is intentionally stricter than the runtime YAML frame. Its validated structure is FFT 1024, CP 128, one ZC symbol plus two data symbols, 128 control RE, two physical transmit ports, and LDPC(1008,504). Rank 1/2 and QPSK/16/64/256-QAM are selectable. The user payload capacities are:

| Rank | QPSK | 16-QAM | 64-QAM | 256-QAM |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 124 B | 250 B | 439 B | 565 B |
| 2 | 250 B | 565 B | 880 B | 1195 B |

In this formal path, FFT size and the 128-control-RE layout are compatibility constants, not runtime knobs. Changing them requires coordinated C++/Python implementation and test-vector updates. See [formal frame structure and usage](Windows_C++_PHY正式帧结构与使用说明.md), [C++ PHY guide](cpp_phy/README.md), and [Python PHY guide](python_phy/README.md).

## libyunsdr integration status

There is currently no `libyunsdr` source, SDK dependency, device configuration, or claimed API compatibility in this repository. A later hardware release should implement the existing stream/device boundary against the verified SDK, add hardware presets, and update this guide with real initialization, clocking, streaming, error handling, and test results. See [the current PHY and libyunsdr roadmap](最新物理层感知验证与libyunsdr路线.md).

## Tests

```bash
python -m pytest python_phy/tests -q
```

The C++ formal PHY has Windows helper scripts in `cpp_phy/`, including `build_windows_vs2019.cmd` and `run_cpp_tests.cmd`.

## License and upstream reference

This repository retains the applicable [license](LICENSE). The earlier open-source project description, authorship, community, and service information are not reproduced on this homepage; refer to the [original OpenISAC repository](https://github.com/zhouzhiwen2000/OpenISAC) and the [OpenISAC paper (arXiv:2601.03535)](https://arxiv.org/abs/2601.03535) for the original project and citation information.
