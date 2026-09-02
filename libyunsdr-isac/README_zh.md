# libyunsdr-isac 硬件接入说明

[English](README.md)

`libyunsdr-isac` 是 OpenISAC 的 YunSDR 硬件接入子项目。它把厂商 SDK、
设备访问、时间戳流调度和硬件工具与可复用的 OFDM、LDPC、MIMO 算法分离。

第一次学习 `libyunsdr`、Y240 PCIES、多通道 IQ、时间戳和实机测试时，请从
[《libyunsdr 完整学习与 YunSDR Y240 接入指南》](docs/LIBYUNSDR_LEARNING_GUIDE_zh.md)
开始。

从 GitHub 全新拉取后的完整搭建、构建、硬件验收、视频、监视和感知步骤见
[《YunSDR Y240 Windows 视频、监视与感知快速搭建》](docs/Y240_WINDOWS_QUICKSTART_zh.md)。
本次 GitHub 完整性检查、问题根因、修改清单和验证证据见
[《Y240 GitHub 完整性审计与可复现搭建更新记录》](docs/Y240_GITHUB_COMPLETENESS_UPDATE_20260902_zh.md)。

当前版本包含已经验证的 Y240 `pcies:0.0` 后端、正式 OpenISAC PHY 编解码、
射频时间戳回环，以及 SISO、2x2 双层空间复用和 2x2 Alamouti STBC 的
VLC/UDP 硬件视频桥。视频文件不属于源码发布内容。

## 发布边界

- OpenISAC 维护 PHY 算法和厂商无关的无线设备接口。
- 本子项目使用验证过的 `libyunsdr` SDK 实现该接口，不包含 UHD/USRP 实现。
- 厂商头文件、导入库、DLL、驱动、固件和示例不提交到公开仓库；请在确认许可后
  放入仓库根目录的 `import/`，该目录默认被 Git 忽略。
- 构建产物、测试日志、IQ 数据、截图和视频同样不会进入源码提交。

## Visual Studio 2019 构建

软件接口与算法回归构建：

```powershell
cd libyunsdr-isac
.\build_vs2019.cmd
```

此命令不需要 YunSDR SDK，也不连接硬件；脚本使用 Visual Studio 2019 自带的
CMake 和 Ninja，并明确关闭真实硬件后端。启用真实硬件构建前，需要先按
[厂商硬件测试说明](docs/Y240_VENDOR_HARDWARE_TEST.md) 准备 SDK。

厂商文件不会随 `git clone` 下载。请取得 `libyunsdr-26-01-00.1` 和 libusb
1.0.23 Windows 二进制包，并准备为：

```text
import/
  libyunsdr-26-01-00.1.zip.zip   # 也支持单层 .zip
  libusb-1.0.23/MS64/dll/libusb-1.0.lib
  libusb-1.0.23/MS64/dll/libusb-1.0.dll
```

构建脚本会自动把单层或双层的 libyunsdr 压缩包展开到忽略目录
`import/vendor-y240-26-01-00.1/source/`。也可以先显式执行：

```powershell
.\prepare_vendor_y240_sdk.ps1
```

兼容的 Y240 PCIES + USB3 SDK 构建命令为：

```powershell
.\build_vendor_y240_pcies_vs2019.cmd
.\build_hardware_vs2019.cmd
```

第一条命令生成兼容的厂商 DLL、导入库和验收程序，第二条命令使用暂存 SDK
构建 OpenISAC Y240 硬件桥。软件回归与硬件构建使用不同构建目录，避免 CMake
缓存把两种模式混在一起。

默认 OpenISAC 算法源码根目录为本子项目的父目录 `..`；独立使用时可通过
`OPENISAC_ROOT` 指定其他 OpenISAC-YunSDR 源码目录。

## 主要工具

- `yunsdr_probe`：设备与后端探测。
- `yunsdr_phy_loopback.exe`：正式 PHY 的时间戳射频回环。
- `yunsdr_video_bridge.exe`：VLC/UDP 数据的硬件 PHY 桥。
- `cpp_phy/live_phy_monitor.py`：星座图、波形、信道、EVM、同步、感知和 CFAR 监视。

详细接口、硬件流程、参数和验收步骤见 [docs](docs/)；默认 SISO 回环配置见
[configs/siso_loopback.yaml](configs/siso_loopback.yaml)。

## 测试

不连接硬件时可关闭硬件后端，构建并运行三个回归测试：基础接口、收发器状态机、
正式 PHY 编解码。真实 Y240 的操作步骤和已验证结果记录在硬件测试文档中。
