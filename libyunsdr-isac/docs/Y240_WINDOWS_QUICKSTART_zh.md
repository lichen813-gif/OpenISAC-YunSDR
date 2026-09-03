# YunSDR Y240 Windows 视频、监视与感知快速搭建

本文面向从 GitHub 全新拉取仓库的 Windows 用户，目标是在 Visual Studio
2019 与 Y240 `pcies:0.0` 上构建并运行 SISO、2x2 双层空间复用或 2x2
Alamouti STBC 视频链路，同时显示物理层监视与感知结果。

## 1. 发布边界

Git 仓库包含 OpenISAC PHY、Y240 适配器、视频桥、监视器、感知算法、构建
脚本和文档。以下厂商或用户文件不随公开仓库分发：

- `libyunsdr-26-01-00.1` 厂商 SDK；
- libusb 1.0.23 Windows 二进制包；
- Y240 驱动与固件；
- 测试视频、IQ 数据、日志和构建产物。

这些不是 OpenISAC 缺失源码。用户需要从有权使用的来源取得厂商依赖，并按下文
放入本机 `import/`。仓库不会自动从互联网下载或重新分发它们。

## 2. 前置环境

安装并确认：

1. Visual Studio 2019，包含“使用 C++ 的桌面开发”和 x64 工具集；
2. Python 3（安装时启用 Tcl/Tk，并建议加入 `PATH`）；
3. VLC 64 位，默认路径 `C:\Program Files\VideoLAN\VLC\vlc.exe`；
4. Y240 Windows 驱动，设备 URI 为 `pcies:0.0`；
5. 仅在确认的有线回环与衰减条件下连接射频端口。

在仓库根目录为实时监视器建立独立 Python 环境：

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r .\libyunsdr-isac\requirements-monitor.txt
```

启动脚本优先使用这个 `.venv`，否则依次查找 `pythonw.exe`、`python.exe` 和
Windows Python Launcher `py.exe`。监视器只额外依赖 NumPy；Tkinter 来自标准
Windows Python 安装。

## 3. 准备厂商依赖

从仓库根目录组织以下本地文件：

```text
import/
  libyunsdr-26-01-00.1.zip.zip   # 单层 .zip 也支持
  libusb-1.0.23/MS64/dll/libusb-1.0.lib
  libusb-1.0.23/MS64/dll/libusb-1.0.dll
```

然后执行：

```powershell
cd .\libyunsdr-isac
.\prepare_vendor_y240_sdk.ps1
```

脚本会兼容单层或双层 ZIP，展开到
`import\vendor-y240-26-01-00.1\source`，并校验 CMake 工程、YunSDR 头文件、
`libpcies.lib` 和 `libfirmware.lib`。`import/` 中除说明文件外的内容都被 Git
忽略。

## 4. 构建顺序

先运行不连接硬件的软件回归：

```powershell
.\build_vs2019.cmd
```

预期 3 个测试全部通过。随后构建兼容的 Y240 PCIES + USB3 厂商库和硬件桥：

```powershell
.\build_vendor_y240_pcies_vs2019.cmd
.\build_hardware_vs2019.cmd
```

关键产物：

```text
libyunsdr-isac/out/y240-sdk-26-01-00.1/bin/
  libyunsdr_ss.dll
  libusb-1.0.dll
  yunsdr_ss_rate.exe
  yunsdr_ss_txrx_multiport.exe

libyunsdr-isac/build/ninja-vs2019-hardware/
  yunsdr_probe.exe
  yunsdr_phy_loopback.exe
  yunsdr_video_bridge.exe
```

软件基线与硬件版使用不同的构建目录，避免 CMake 缓存交叉污染。硬件 DLL 会
自动复制到硬件可执行文件旁边。

## 5. 硬件验收

在已确认安全的有线回环条件下，先运行厂商速率与多端口程序。详细参数见
[Y240_VENDOR_HARDWARE_TEST.md](Y240_VENDOR_HARDWARE_TEST.md)。已验证的速率
命令示例：

```powershell
.\out\y240-sdk-26-01-00.1\bin\yunsdr_ss_rate.exe -a pcies:0.0 -f 1500000000 -g 25 -G 60 -c 0x3 -C 0x3 -N 30720 -s 15360000 -r 2
```

再做正式 PHY 小帧数回环：

```powershell
.\build\ninja-vs2019-hardware\yunsdr_phy_loopback.exe --device pcies:0.0 --mode siso --modulation 64qam --pilot fdm --frames 20 --frequency-mhz 1500 --tx-gain 60 --rx-gain 20
```

不要把示例增益直接用于未知的天线或回环链路。示例中的 TX=60 是用户已经确认
安全的外部有线回环条件；换线缆、衰减器或端口后应从更保守设置重新检查削顶、
EVM、CRC 与设备事件。

## 6. 视频、实时绘图与感知

从 `libyunsdr-isac` 目录运行：

```powershell
.\run_y240_video.cmd "C:\path\to\video.mp4" siso 64qam fdm 1500 60 20 1000
```

八个位置参数依次为：视频文件、模式、调制、导频、中心频率 MHz、TX 增益、
RX 增益和 VLC 视频码率 kbit/s。模式可选 `siso`、`mimo2`、`stbc`；调制可选
`qpsk`、`16qam`、`64qam`、`256qam`；导频可选 `fdm`、`dmrs`。

正常启动后：

- VLC 接收窗口在前台播放恢复视频；
- Python 窗口每 0.8 秒刷新星座图、4096 点时域波形、信道响应、EVM、CFO、
  SFO、定时、CRC/FER 和设备事件；
- 同一窗口显示基于实测信道快照的距离–多普勒图与 CFAR 检测；
- 遥测 CSV 与桥日志保存在 `libyunsdr-isac\out\hardware-video`；
- 视频结束或 Ctrl+C 后，启动脚本关闭其创建的 VLC、监视器和视频桥进程。

高级参数示例：

```powershell
.\run_y240_video.ps1 `
  -VideoFile "C:\path\to\video.mp4" `
  -Mode mimo2 -Modulation 64qam -Pilot fdm `
  -FrequencyMHz 1500 -TxGain 60 -RxGain 24 `
  -VideoBitrateKbps 1000 -BatchPackets 8 -LeadBlocks 48 -Retries 8 `
  -RefreshSeconds 0.8 -SensingCoherentFrames 16
```

感知相干帧数可选 16、32、64、128。当前视频突发内感知适合链路闭环、静态路径、
距离及较大多普勒检查；以 16 帧 FDM 直接分辨 1–2 m/s 仍受 CPI 限制。

## 7. 常见问题

- `libyunsdr source was not found`：压缩包没有放到仓库根目录 `import/`，版本名
  不匹配，或压缩包内部结构无效；先运行 `prepare_vendor_y240_sdk.ps1` 查看精确
  校验错误。
- `libusb was not found`：确认 `MS64\dll` 下同时存在 `.lib` 与 `.dll`。
- `Hardware video bridge not found`：必须先运行厂商构建，再运行
  `build_hardware_vs2019.cmd`；视频桥在 `build\ninja-vs2019-hardware`。
- `Python with Tkinter and NumPy was not found`：按第 2 节建立 `.venv`，并确认
  Python 安装包含 Tcl/Tk。
- VLC 找不到：安装 64 位 VLC 到默认位置，或在 `run_y240_video.ps1` 中显式修改
  `$vlc` 路径。
- 有星座点但 CRC 失败：先检查削顶、RX 增益、同步位置、TX timeout、端口映射和
  FDM/DMRS 是否一致，再增加视频码率或批量大小。

若启动阶段连续三次报告 `hardware warmup batch failed`，请按
[Y240 视频预热失败排查说明](Y240_VIDEO_WARMUP_FAILURE_TROUBLESHOOTING_zh.md)
读取桥日志并依次执行厂商程序、QPSK、64QAM 和固定包自检。

更完整的模式、增益实测、EVM、遥测字段和感知限制见
[Y240_HARDWARE_VIDEO.md](Y240_HARDWARE_VIDEO.md)。
