# libyunsdr-isac

[中文说明](README_zh.md)

Chinese standalone learning manual: [libyunsdr and YunSDR Y240 guide](docs/LIBYUNSDR_LEARNING_GUIDE_zh.md).
Fresh-clone Windows setup: [Y240 video, monitoring, and sensing quick start (Chinese)](docs/Y240_WINDOWS_QUICKSTART_zh.md).

`libyunsdr-isac` is the hardware-integration subproject for OpenISAC. It keeps
the vendor SDK, SDR device access, stream scheduling, loopback tools, and
hardware validation separate from the reusable OFDM/LDPC/MIMO algorithms.

The current revision contains a verified Y240 `pcies:0.0` backend, the formal
OpenISAC PHY codec, timestamped RF loopback tests, and a VLC/UDP hardware video
bridge for SISO, 2x2 two-layer spatial multiplexing, and 2x2 Alamouti STBC. The
video launcher also opens a live Python monitor for constellation, waveform,
channel/EVM/synchronization telemetry, range-Doppler sensing, and CFAR results.

## Boundaries

- OpenISAC owns the PHY algorithms and the vendor-neutral radio contract.
- This project implements that contract using a verified `libyunsdr` SDK.
- Vendor headers, import libraries, DLLs, drivers, and examples are staged
  separately and are not committed unless their license permits redistribution.
- The verified cabled-loopback modes are SISO, 2x2 two-layer spatial
  multiplexing, and 2x2 Alamouti STBC; their measurements and limits are
  recorded in `docs/Y240_HARDWARE_VIDEO.md`.

## Baseline build (Visual Studio 2019)

```powershell
.\build_vs2019.cmd
build\ninja-vs2019\yunsdr_probe.exe
```

This baseline command explicitly disables the hardware backend and therefore
does not require the YunSDR SDK. The script uses the CMake bundled with VS2019,
so a separate CMake installation or a global `PATH` change is not required.

Vendor files are not downloaded by `git clone`. Obtain libyunsdr 26-01-00.1
from YunSDR and the libusb 1.0.23 Windows binary package, then stage:

```text
import/
  libyunsdr-26-01-00.1.zip.zip   # a single-layer .zip is also accepted
  libusb-1.0.23/MS64/dll/libusb-1.0.lib
  libusb-1.0.23/MS64/dll/libusb-1.0.dll
```

The vendor build automatically expands either form of the libyunsdr archive
into the ignored `import/vendor-y240-26-01-00.1/source/` directory. It can also
be prepared explicitly with:

```powershell
.\prepare_vendor_y240_sdk.ps1
```

The verified Y240 vendor compatibility build (PCIES + USB3) is generated with:

```powershell
.\build_vendor_y240_pcies_vs2019.cmd
.\build_hardware_vs2019.cmd
```

Its DLL and vendor acceptance tools are staged under
`out/y240-sdk-26-01-00.1/bin/`. This is the only maintained vendor DLL variant;
`libusb-1.0.dll` must remain beside `libyunsdr_ss.dll`.

When this subproject is used inside OpenISAC-YunSDR, `OPENISAC_ROOT` defaults
to the parent repository (`..`). Override it only when the adapter is copied
or built as a standalone source tree:

```powershell
cmake -S . -B build -G "Visual Studio 16 2019" -A x64 `
  -DOPENISAC_ROOT="C:\path\to\OpenISAC-YunSDR"
```

`build_vendor_y240_pcies_vs2019.cmd` creates the staged
`out/y240-sdk-26-01-00.1` SDK, and `build_hardware_vs2019.cmd` then creates the
hardware-enabled OpenISAC adapter in a separate build directory. The vendor DLL
and `libusb-1.0.dll` are copied beside the hardware executables automatically.

## Hardware tools

1. `yunsdr_phy_loopback.exe`: deterministic PHY synchronization, decision-
   directed EVM, CRC, payload, clipping, and event-counter test.
2. `yunsdr_video_bridge.exe`: UDP fragmentation, batched timestamped RF
   bursts, retry/reassembly, and UDP forwarding for VLC.
3. `run_y240_video.cmd` / `.ps1`: foreground VLC receiver, hidden sender,
   live PHY/sensing monitor, hardware warmup/restart, log collection, queue
   drain, and automatic cleanup of every process started by the test.

Quick video test:

```powershell
.\run_y240_video.cmd "C:\path\to\video.mp4" siso 64qam fdm 1500 60 20 1000
```

See `docs/Y240_HARDWARE_VIDEO.md` for all modes, exact commands, gain/frequency
results, and the current hardware recovery gate.

Before adding the vendor backend, read:

- `docs/IMPORTED_LIBYUNSDR_ASSESSMENT.md` for the imported evidence and limits;
- `docs/SDR_HARDWARE_INTEGRATION_PLAN.md` for the staged implementation plan;
- `docs/ARCHITECTURE.md` for the component boundary;
- `docs/LIBYUNSDR_HANDOFF_CHECKLIST.md` for the remaining SDK/hardware inputs.
- `docs/Y240_PCIES_BUILD_ASSESSMENT.md` for the successful VS2019 `pcies:0.0`
  build, dependency findings, and optional USB3 variant.
- `docs/Y240_VENDOR_HARDWARE_TEST.md` for the generated `yunsdr_ss_rate` and
  `yunsdr_ss_txrx_multiport` hardware acceptance commands.
- `docs/CPP_HARDWARE_FLOW_INTERFACES.md` for the C++ SISO, 2x2 MIMO, 2x2 STBC
  and VLC/UDP hardware-flow interfaces.
- `docs/Y240_HARDWARE_VIDEO.md` for the implemented hardware video bridge and
  verified test results.

The imported handoff package is stored under the repository-level `import/`
directory and ignored by Git until
its upstream identity and redistribution license are confirmed.
