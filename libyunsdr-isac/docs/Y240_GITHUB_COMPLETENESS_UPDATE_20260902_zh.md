# Y240 GitHub 完整性审计与可复现搭建更新记录

日期：2026-09-02

目标仓库：<https://github.com/lichen813-gif/OpenISAC-YunSDR>

审计后远端提交：`8209313fb46d81e362cef9a062fd7511eabccc28`

## 1. 更新目标

本次工作不是新增一套独立 PHY，而是确认既有 YunSDR Y240 设计是否完整进入
GitHub，并修复用户从全新克隆开始构建时遇到的依赖定位和说明问题。最终要求是：

1. OpenISAC 自行维护的 Y240 适配、PHY、视频桥、监视和感知源码全部受 Git 管理；
2. 用户能够根据公开文档准备不能再分发的厂商依赖；
3. 软件基线、厂商 SDK 和硬件启用版本使用清晰、互不污染的构建步骤；
4. 能够生成 Y240 验收工具、正式 PHY 回环工具和 VLC 视频桥；
5. 视频运行时能够显示物理层监视与感知测量，并在结束时自动清理进程。

## 2. 审计范围与初始结论

审计覆盖远端 `main`、本地发布仓库、实际 Y240 开发目录和一份从 GitHub 新建的
干净克隆，重点检查：

- Git 跟踪文件、子模块、Git LFS 指针和对象完整性；
- `libyunsdr-isac` 源码、头文件、CMake、CMD/PowerShell 脚本和文档；
- 正式 C++ PHY、Python PHY、实时监视器和感知处理依赖；
- 本地开发目录与发布目录之间的文件和内容差异；
- README 中的本地链接、构建命令、产物名称和产物路径；
- 全新克隆后的软件构建、厂商构建与硬件启用构建。

审计确认，提交 `6b152e5` 已经包含主要 Y240 实现源码，包括：

- `pcies:0.0` YunSDR 后端和厂商无关 `RadioBackend` 边界；
- 时间戳收发、流状态和设备事件处理；
- SISO、2x2 双层空间复用和 2x2 Alamouti STBC 正式 PHY；
- FDM/DMRS、QPSK/16QAM/64QAM/256QAM、LDPC 和 CRC；
- VLC/UDP 分片、射频传输、重组和转发；
- 星座图、时域波形、信道、EVM、CFO/SFO、同步和 FER 遥测；
- 距离–多普勒处理、CFAR 检测和感知 CSV 输出。

但当时不能把仓库称为“全新拉取后可直接按说明完成硬件搭建”，因为构建脚本、
厂商依赖边界和 README 之间仍存在不一致。

## 3. 用户遇到的错误及根因

原始错误为：

```text
libyunsdr source was not found:
...\import\vendor-y240-26-01-00.1\source
```

它由以下问题共同导致：

1. 仓库根目录的 `import/` 被整体忽略，因此全新克隆中没有目录说明；
2. 用户已有 `libyunsdr-26-01-00.1.zip.zip`，旧脚本却只查找已经手动展开的
   `vendor-y240-26-01-00.1/source`；
3. 压缩包是双层 ZIP，旧流程没有自动识别和展开；
4. `build_vs2019.cmd` 被 README 描述为软件基线，但旧实现强制启用硬件后端，
   导致没有暂存 SDK 时基线也失败；
5. 软件基线和硬件构建曾共用构建目录，CMake 缓存容易留下错误状态；
6. 视频启动器仍查找旧路径 `build\ninja-vs2019\yunsdr_video_bridge.exe`，而
   独立硬件构建应输出到 `build\ninja-vs2019-hardware`；
7. 根 README 底部仍保留过时的“尚未接入 libyunsdr”描述，与实际实现矛盾；
8. Python 监视器曾依赖开发机上的特定运行时路径，不适合其他用户的 Windows。

因此，截图中的报错不是 OpenISAC PHY 源码缺失，而是“受限厂商依赖没有随 Git
分发”与“旧脚本未能从用户已有压缩包自动准备依赖”没有被正确区分。

## 4. 已实施的修改

### 4.1 厂商包自动准备

新增 `prepare_vendor_y240_sdk.ps1`，支持：

- `libyunsdr-26-01-00.1.zip.zip` 双层压缩包；
- `libyunsdr-26-01-00.1.zip` 单层压缩包；
- 自动寻找真正的 libyunsdr 源码根目录；
- 展开到被 Git 忽略的 `import/vendor-y240-26-01-00.1/source`；
- 校验 `CMakeLists.txt`、`yunsdr_api_ss.h`、`libpcies.lib` 和
  `libfirmware.lib`；
- 失败时输出用户需要放置的准确文件和路径。

`build_vendor_y240_pcies_vs2019.cmd` 在默认源码目录不存在时会自动调用该脚本，
同时检查 libusb x64 `.lib`，并将 VS2019 自带 CMake 加入规范的 Windows
`Path`。这避免依赖全局 CMake，也修复了大小写不同的 `Path`/`PATH` 在旧版
MSBuild 中可能触发的环境字典冲突。

### 4.2 软件与硬件构建分离

`build_vs2019.cmd` 现在默认使用：

```text
LIBYUNSDR_ISAC_ENABLE_HARDWARE=OFF
build/ninja-vs2019/
```

新增 `build_hardware_vs2019.cmd`，明确使用：

```text
LIBYUNSDR_ISAC_ENABLE_HARDWARE=ON
build/ninja-vs2019-hardware/
```

这样没有 SDK 的用户也可以先验证厂商无关接口和 PHY 算法；准备好 SDK 后再构建
硬件后端，不会把两种配置混入同一个 CMake 缓存。

### 4.3 视频、监视和感知启动修复

`run_y240_video.ps1` 已改为查找：

```text
build/ninja-vs2019-hardware/yunsdr_video_bridge.exe
```

Python 监视器按以下顺序查找解释器：

1. 仓库根目录 `.venv/Scripts/pythonw.exe`；
2. 仓库根目录 `.venv/Scripts/python.exe`；
3. 系统 `pythonw.exe`；
4. 系统 `python.exe`；
5. Windows Python Launcher `py.exe -3`。

新增 `requirements-monitor.txt`，实时监视器只要求额外安装 NumPy；Tkinter 来自
标准 Windows Python。视频结束、用户中断或异常退出时，启动脚本继续负责关闭
它所创建的 VLC 发送端、VLC 接收端、Python 监视器和 PHY 视频桥。

### 4.4 文档与 Git 发布边界

`.gitignore` 现在允许提交 `import/README.md`，但继续忽略目录中的实际厂商包、
展开源码和二进制。新增或更新的主要文档包括：

| 文档 | 作用 |
| --- | --- |
| `import/README.md` | 解释哪些本地依赖不进入 Git，以及准确目录结构 |
| `Y240_WINDOWS_QUICKSTART_zh.md` | 全新克隆后的环境、构建、验收、视频、监视和感知步骤 |
| `LIBYUNSDR_LEARNING_GUIDE_zh.md` | libyunsdr、Y240 PCIES、流、时间戳和 PHY 的完整学习指南 |
| `Y240_VENDOR_HARDWARE_TEST.md` | 厂商 rate/multiport 工具参数和硬件验收顺序 |
| `Y240_HARDWARE_VIDEO.md` | 视频模式、增益/EVM 实测、监视字段和感知限制 |
| 根目录及子项目 README | 发布边界、正确构建顺序、产物路径和入口链接 |

## 5. 全新克隆验证结果

推送后重新从 GitHub 克隆 `main`，验证 HEAD 为：

```text
8209313fb46d81e362cef9a062fd7511eabccc28
```

验证结果如下：

| 验证项 | 结果 |
| --- | --- |
| 软件基线从零配置和编译 | 通过 |
| 软件基线 CTest | 3/3 通过 |
| 单层/双层厂商 ZIP 自动准备 | 通过 |
| PCIES + USB3 厂商 DLL 构建 | 通过 |
| `yunsdr_ss_rate.exe` | 成功生成 |
| `yunsdr_ss_txrx_multiport.exe` | 成功生成 |
| 硬件启用版从零配置和编译 | 通过 |
| 硬件启用版 CTest | 3/3 通过 |
| `yunsdr_probe.exe` | 成功生成 |
| `yunsdr_phy_loopback.exe` | 成功生成 |
| `yunsdr_video_bridge.exe` | 成功生成 |
| Python 监视器编译和 `--help` | 通过 |
| 视频桥参数解析和 `--help` | 通过 |
| PowerShell 语法检查 | 通过 |
| 非网站 Markdown 本地链接 | 无缺失 |

仓库级补充检查中，Windows C++ PHY CTest 10/10 通过，Python PHY 测试
114/114 通过。Git 对象检查未发现损坏，没有子模块缺口，也没有未下载的 Git LFS
指针。验证期间没有重新执行射频发射或 VLC 实机播放；已有硬件视频、EVM、同步、
感知和自动清理结果记录在 `Y240_HARDWARE_VIDEO.md`。

## 6. 用户最终搭建流程

### 6.1 拉取和 Python 监视器

```powershell
git clone https://github.com/lichen813-gif/OpenISAC-YunSDR.git
cd OpenISAC-YunSDR
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r .\libyunsdr-isac\requirements-monitor.txt
```

### 6.2 放置本地厂商文件

```text
import/
  libyunsdr-26-01-00.1.zip.zip
  libusb-1.0.23/MS64/dll/libusb-1.0.lib
  libusb-1.0.23/MS64/dll/libusb-1.0.dll
```

### 6.3 构建

```powershell
cd .\libyunsdr-isac
.\build_vs2019.cmd
.\prepare_vendor_y240_sdk.ps1
.\build_vendor_y240_pcies_vs2019.cmd
.\build_hardware_vs2019.cmd
```

### 6.4 视频、监视与感知

```powershell
.\run_y240_video.cmd "C:\path\to\video.mp4" siso 64qam fdm 1500 60 20 1000
```

第二个参数可改为 `mimo2` 或 `stbc`。正常运行时会同时启动前台 VLC 接收窗口和
实时 PHY/感知监视器。完整参数、安全说明和故障排查以
[Y240 Windows 快速搭建](Y240_WINDOWS_QUICKSTART_zh.md)为准。

## 7. Git 中包含与不包含的内容

### 已完整发布

- OpenISAC 正式 PHY、Y240 适配器和厂商无关接口；
- SISO、2x2 双层 MIMO 和 2x2 STBC 硬件视频链；
- 同步、信道估计、LDPC/CRC、遥测、监视绘图和感知处理；
- CMake、VS2019 构建脚本、运行脚本、配置和测试；
- 用户搭建、硬件验收、视频测试和算法学习文档。

### 需要用户单独准备

- libyunsdr 厂商 SDK 源码和二进制；
- libusb Windows 二进制包；
- Y240 驱动和固件；
- VLC、Python、Visual Studio 2019；
- 测试视频、射频线缆和满足设备手册要求的衰减链路。

厂商文件未进入 Git 是许可和发布边界，不应再被描述为 OpenISAC 仓库缺失文件。

## 8. 维护约束

后续修改 Y240 子项目时应保持以下规则：

1. 软件基线必须继续在没有厂商 SDK 时可构建和测试；
2. 硬件工具统一从 `build/ninja-vs2019-hardware` 运行；
3. 不提交 `import/` 厂商内容、`build/`、`out/`、IQ、日志或视频；
4. 改动视频桥参数时同步更新 CMD、PowerShell、快速搭建和视频说明；
5. 改动遥测 CSV 时同步验证 `cpp_phy/live_phy_monitor.py`；
6. 声明“全新拉取可搭建”前至少重新验证软件基线、厂商构建、硬件构建和 CTest；
7. 没有重新运行实机射频测试时，必须明确区分编译回归与硬件测量结论。

## 9. 最终结论

截至提交 `8209313`，OpenISAC 自行维护的 Y240 视频传输、实时监视绘图和感知
测量设计已经完整进入目标 GitHub 仓库。用户从全新克隆开始，在合法取得并按说明
放置 libyunsdr 与 libusb 后，可以复现厂商库、硬件适配器和视频桥的完整构建。
厂商受限文件仍不随 Git 分发，但仓库已经提供明确的目录说明、自动准备脚本、错误
诊断和完整操作文档。
