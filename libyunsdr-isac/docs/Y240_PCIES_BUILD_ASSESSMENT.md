# Y240 / `pcies:0.0` Windows 构建评估

评估日期：2026-08-24
目标平台：Windows x64、Visual Studio 2019、YunSDR Y240
目标设备 URI：`pcies:0.0`

## 1. 结论

本机的 VS2019、MSVC x64、Windows SDK 和 VS 自带 CMake 均可正常工作，
`libyunsdr` 可以由本项目继续负责搭建，不需要用户先手动配置编译器。

用户补充的 `libyunsdr-26-01-00.1.zip.zip` 是完整度足够的 Windows 构建包，
已经成功生成具有 `pcies:0.0` 能力的 VS2019 x64 DLL、静态库和厂商测试程序。

最终构建必须同时启用：

```text
ENABLE_PCIES=ON
ENABLE_FIRMWARE=ON
ENABLE_PCIEX=ON
```

原因是 `libpcies.lib` 依赖 `libfirmware.lib`，并引用
`interface_pciex_win.c` 提供的 `extract_subsystem_id()`。仅启用 PCIES 会产生
链接错误，不能把 PCIEX 简单关闭。

## 2. 已验证的源码路径

迁移快照的 `pcies` 路径为：

```text
yunsdr_open_device("pcies:0.0")
  -> yunsdr_open_pcies(...)
  -> INTERFACE_PCIES
  -> init_interface_pcies(...)
  -> libpcies
```

顶层 CMake 通过 `ENABLE_PCIES=ON` 打开该路径。`FindLIBPCIES.cmake` 查找：

```text
头文件：yunsdr_pcies.h
库名：  pcies.lib 或 libpcies.lib（Windows）
```

快照中存在 `yunsdr_pcies.h`，但不存在相应库。源码示例默认 URI 是
`pcies:0.0`；该 URI 已由真实 Y240、厂商 rate 程序和 OpenISAC 硬件程序验证。

## 3. VS2019 配置实测

执行的最小配置等价于：

```powershell
cmake -S <handoff-source> -B build/vendor-pcies-vs2019 `
  -G "Visual Studio 16 2019" -A x64 `
  -DENABLE_PCIE=OFF -DENABLE_PCIEX=OFF -DENABLE_PCIES=ON `
  -DENABLE_USB3=OFF -DENABLE_SFP=OFF -DENABLE_FIRMWARE=OFF `
  -DENABLE_EXAMPLE=OFF -DENABLE_PYTHON=OFF `
  -DENABLE_COMLABS=OFF -DENABLE_NUMA=OFF
```

成功项：

- Visual Studio 2019 generator 和 x64 平台识别正常；
- MSVC 19.29.30159 和 Windows SDK 10.0.19041.0 识别正常；
- SSE4.1、AVX、AVX2、FMA 的 CMake 检测正常。

首次分析快照的阻塞项是：

```text
LIBPCIES library not found
Could NOT find LIBPCIES (missing: LIBPCIES_LIBRARIES)
```

迁移快照还缺少：

```text
cmake/modules/YunSDRbuildinfo.cmake.in
src/yunsdr_ss/include/version.h.in
src/yunsdr_ss/include/VerInfo.rc.in
libyunsdr_ss.pc.in
cmake/cmake_uninstall.cmake.in
```

CMake 在找不到 `libpcies` 时会“继续配置但关闭 PCIES 支持”。以后必须把
`LIBPCIES_FOUND=TRUE` 和最终 DLL 的依赖检查设为硬门槛，不能只以 CMake 返回
成功判断 `pcies:0.0` 已可用。

这些缺失项已由 `libyunsdr-26-01-00.1.zip.zip` 补齐。完整包校验值：

```text
外层 ZIP SHA-256: DA51AA4823BFC3185DE150DA9D309CC789735180C758B609A574899CA51B0DCE
内层 ZIP SHA-256: E8F31E95222E291A33CFC1FA5DE707BC9867878B20DAC0E21628F0E04A55E55A
生成 DLL SHA-256: 5E6E3B9743D230730D50AFCEE2BC0C92A4D015A89E2538267016807E3582330C
```

生成的兼容 DLL 依赖同目录的 `libusb-1.0.dll` 以及 Windows 系统 DLL
`SETUPAPI.dll`、`KERNEL32.dll` 和 `ADVAPI32.dll`；PCIES 与 firmware 代码已经
静态链接。关键 API
`yunsdr_open_device`、multiport read/write、timestamp 和 channel event 均已
确认导出。

厂商 CMake 的 `gen_build_info` 原来通过 PATH 调用裸命令 `cmake`，在当前
Windows 环境会触发 PATH/Path 冲突或找不到命令。提取后的工作副本已改为
`${CMAKE_COMMAND}`，原始内层 ZIP 保持不变。

用户提供的 libusb 1.0.23 已通过版本检测。项目只保留同时支持 PCIES+USB3 的
兼容版本：

```text
out/y240-sdk-26-01-00.1/bin/libyunsdr_ss.dll
out/y240-sdk-26-01-00.1/bin/libusb-1.0.dll
```

不再生成或维护 PCIES-only 精简版。

本机环境检查还没有发现名称包含 YunSDR、Y240、XDMA 或 PCIES 的当前 PnP
设备/系统驱动，也没有在常见安装目录发现 `yunsdr`/`pcies` 头文件、`.lib` 或
DLL。该结果只说明当前 Windows 环境尚未安装或识别相应组件，不代表 Y240
硬件能力结论。

## 4. 仍需要的硬件输入

SDK 编译输入已经齐备。进入实机测试前仍需要：Y240 Windows 驱动、实际设备、
固件/FPGA bitstream 版本、射频安全参数以及回环衰减器。

## 5. 下一步

1. 安装匹配的 Y240 Windows 驱动并确认设备枚举；
2. 先运行 `yunsdr_ss_rate -r 1` 做 RX-only 测试；
3. 确认外部衰减后运行 `yunsdr_ss_txrx_multiport`；
4. 再运行 `yunsdr_ss_rate -r 2` 验证持续全双工吞吐；
5. 厂商工具通过后接入 OpenISAC `yunsdr_probe` 和 SISO PHY。

详细命令和验收条件见 `Y240_VENDOR_HARDWARE_TEST.md`。
