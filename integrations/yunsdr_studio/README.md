# YunSDR Studio OpenISAC integration

[中文](README_zh.md) · [Frame layout](FRAME_V2.md) · [Hardware validation guide](Y240_LIVE_VALIDATION.md)

The Studio adapter uses 816 active carriers, indexed −408…−1 and +1…+408, in a 1024-point FFT with a 128-sample prefix. The nominal span is 817/1024 of the IQ sample rate. At 122.88 MSa/s the span is 98.04 MHz. SISO, 2/4/8-layer spatial multiplexing and two-port Alamouti STBC support FDM and front-loaded DM-RS. The transmit preamble shares the same carrier allocation. QPSK filling reduces coherent peaks, and EVM and constellation plots use the LDPC-coded resource positions.

The following Windows x64 commands build and validate the adapter from the repository root. The CTest executable checks all 40 topology, modulation and pilot combinations without a Studio installation.

```powershell
cmake -S integrations/yunsdr_studio/native -B build/studio-phy -G "Visual Studio 16 2019" -A x64
cmake --build build/studio-phy --config Release --parallel
ctest --test-dir build/studio-phy -C Release --output-on-failure
cmake --install build/studio-phy --config Release --prefix build/studio-runtime
```

The optional `overlay/yunsdr_studio` sources are intended for a YunSDR Studio 0.16 workspace with the OpenISAC workbench entry points already integrated. The overlay contains its Python binding, sensing, clip loopback, live video and OpenISAC panel. It requires the Studio host's IQ utilities, device session and PySide6 widgets. Overlay the files under the Studio package and point `YUNSDR_OPENISAC_RUNTIME` to the installed runtime directory or install into Studio's `output/openisac_runtime`. The runtime ABI is 7. The native standalone test requires no vendor SDK.

Studio live video uses two-port STBC, 64-QAM/FDM and 15.36 MSa/s. A 1,316-byte video datagram occupies five frames. Both ends of a two-station Studio link must use the 816-carrier integration. The standalone Y240/VLC and CPU/CUDA baselines retain their published layouts.

The Studio offline suite passed 72 tests. At 30 dB AWGN, 30-second tests at 1, 1.5 and 2 Mbit/s recovered every transmitted datagram. New RF video and sensing still require hardware verification. [Benchmark CSV](../../results/benchmarks/studio_816_live_windows.csv)
