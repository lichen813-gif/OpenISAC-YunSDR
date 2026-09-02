# Local vendor dependencies

This directory is intentionally excluded from the public source release except
for this file. YunSDR SDK files, libusb binaries, drivers, firmware, generated
libraries, and device captures must not be committed unless their licenses
explicitly permit redistribution.

For the verified Windows Y240 build, place the locally obtained files here:

```text
import/
  libyunsdr-26-01-00.1.zip.zip
  libusb-1.0.23/MS64/dll/libusb-1.0.lib
  libusb-1.0.23/MS64/dll/libusb-1.0.dll
```

A single-layer `libyunsdr-26-01-00.1.zip` is also accepted. From
`libyunsdr-isac`, run:

```powershell
.\prepare_vendor_y240_sdk.ps1
.\build_vendor_y240_pcies_vs2019.cmd
.\build_hardware_vs2019.cmd
```

The first script extracts the archive into the ignored
`import/vendor-y240-26-01-00.1/source` directory and verifies the required
source, headers, and import libraries. Full Chinese instructions are in
[`libyunsdr-isac/docs/Y240_WINDOWS_QUICKSTART_zh.md`](../libyunsdr-isac/docs/Y240_WINDOWS_QUICKSTART_zh.md).
