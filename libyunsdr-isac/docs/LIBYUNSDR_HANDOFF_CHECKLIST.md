# libyunsdr handoff checklist

Before implementing the real backend, import and record all of the following:

- hardware product name, RF port count, serial/firmware versions;
- libyunsdr repository URL, tag/branch, and exact commit;
- Windows driver, header, import-library, DLL, and runtime dependencies;
- Visual Studio architecture and runtime-library requirements;
- verified open/close and device-discovery example;
- RX/TX sampling rate, LO, RF bandwidth, and gain APIs;
- simultaneous TX/RX and FDD/TDD behavior;
- single-port and multi-port read/write APIs;
- channel-mask numbering and physical-port mapping;
- IQ sample format, scaling, alignment, byte order, and count units;
- timestamp tick rate, epoch, rollover, and scheduled-TX flags;
- timeout, overflow, underflow, late-command, and short-transfer semantics;
- thread-safety and whether one device handle supports concurrent streams;
- measured continuous throughput at 15.36 Msps per enabled channel;
- vendor examples and logs from a successful known-waveform loopback.

Do not import passwords, access tokens, private keys, device credentials, or
unredacted serial numbers. Review the SDK license before committing binaries.
