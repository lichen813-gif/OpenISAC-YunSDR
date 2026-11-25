# OpenISAC-YunSDR v1.2.0

[中文](#中文) · [English](#english)

## 中文

### 自 v1.1.0 以来的更新

- **8×8 八层正式帧**。公开 Studio OpenISAC 集成中的版本 2 帧，包含八端口 FDM 或前置 DM-RS、帧头、LDPC 载荷布局、CRC-16、逐层 EVM 和 64 路 CSI。集成同时支持 1×1 SISO、2×2／4×4 空间复用和 2×2 Alamouti STBC。[帧结构](integrations/yunsdr_studio/FRAME_V2.md)
- **816 子载波与感知**。正式帧使用 1024 点 FFT、128 点循环前缀，活动索引为 ±1 至 ±408，DC 留空。Zadoff–Chu 前导使用相同频域范围。标称带宽是采样率的 817/1024（约 79.79%），在 122.88 MSa/s 下为 98.04 MHz。载荷空余位置以伪随机 QPSK 填充。星座和 EVM 使用实际承载 LDPC 编码比特的位置，距离–多普勒和 CFAR 使用这 816 个载波的 CSI。[集成说明](integrations/yunsdr_studio/README_zh.md)
- **视频链路**。短片回环和实时视频共用 816 载波正式帧。实时链路支持摄像头或本地视频、2×2 STBC、QPSK 至 256-QAM、FDM 或前置 DM-RS，以及 15.36、30.72、61.44 MSa/s。支持本机仿真、本机射频回环和双端设备收发。连续 IQ 使用 2 ms 时间戳 DMA，高采样率时先筛选候选同步窗口。集成面板提供画面、星座、EVM 趋势和 CSI 四宫格，可独立全屏。[视频配置与验证](integrations/yunsdr_studio/Y240_LIVE_VALIDATION.md)
- **Y240 接入与部署**。独立 Windows 子项目新增 `pcies:0.0` 后端、时间戳收发、SISO／2×2 空间复用／2×2 STBC、VLC/UDP 硬件视频桥与监视器。完善 SDK 包展开检查、软件与硬件构建目录、PowerShell 启动器，以及视频启动和短尾分片故障排查。[Y240 快速开始](libyunsdr-isac/docs/Y240_WINDOWS_QUICKSTART_zh.md) · [短尾分片排查](libyunsdr-isac/docs/Y240_FRAGMENT3_SHORT_TAIL_FAILURE_SOLUTION_zh.md)
- **Windows／DGX Spark 计算后端**。独立 C++ PHY 增加 DGX Spark ARM64 CPU 和可选 CUDA/cuFFT 后端、批量 OFDM 和最多八流 MIMO 检测、四端口 FDM 设备端处理，以及信道估计、软判决和 LDPC 工作线程优化。视频桥支持 Windows/POSIX，基准和复现脚本随源码发布。[Windows 与 DGX 结果](DGX_WINDOWS_CPU_GPU_RESULTS.md)

### 验证结果

| 验证 | 结果 |
| --- | --- |
| Windows 公开源码回归 | 独立 C++ PHY CPU CTest 10/10、Y240 软件基线 CTest 3/3、Python PHY 与工具测试 119/119 通过。 |
| 816 载波 C++ 自检 | 40 种拓扑、导频和调制组合通过 IQ、载荷、CRC、EVM 和 CSI 检查。 |
| Studio 集成回归 | 此前 Studio 全量 Windows 离线回归 74 项通过。加载从公开源码重新编译的库后，另有 16 项 OpenISAC 测试通过。八层 Python/C++ 双向交叉验证通过，仿真中各层最大 RMS EVM 为 0.5661%，频偏估计误差为 0.289 Hz。 |
| 仿真感知与视频 | 122.88 MSa/s 下，五种拓扑和两种导频均检测到 9 样点的延迟回波，约 10.98 m。15.36 MSa/s、30 dB AWGN 下，1、1.5、2 Mbit/s 各运行约 30 秒，均无数据报丢失。[视频仿真 CSV](results/benchmarks/studio_816_live_windows.csv) |
| 61.44 MSa/s 模拟 DMA 视频 | 64-QAM／前置 DM-RS，1、1.5、2 Mbit/s 各约 30 秒。分别恢复 2,845、4,273、5,646 个数据报，丢包、时间戳间断和主机丢块均为零。[模拟 DMA CSV](results/benchmarks/studio_6144_timed_mock.csv) |
| Y790 8T8R 有线射频回环（此前实测） | 30.72 MSa/s，八层 64-QAM／FDM 逐字节恢复 128 字节，CRC-16 通过并获得 64 路 CSI。双通道 2×2 STBC 视频停机排空后恢复 995,869／995,869 个数据报，显示延迟 P95 为 222.5 ms。[实测详情](integrations/yunsdr_studio/Y240_LIVE_VALIDATION.md#y790-8t8r-的实测记录) |
| DGX Spark（此前阶段结果） | CPU CTest 10/10、CUDA CTest 12/12 通过。Windows CPU、DGX CPU 和 DGX CUDA 共 150 轮模式基准均逐字节恢复，FER 和 UDP 丢包为零。[阶段基准](DGX_WINDOWS_CPU_GPU_RESULTS.md) |

### 使用范围

Studio 的正式帧及视频集成位于 [`integrations/yunsdr_studio/`](integrations/yunsdr_studio/README_zh.md)，其中的 C++ 自检可独立构建。集成面板依赖 Studio 宿主。Studio 桌面应用仓库继续保持私有。独立 Y240/VLC 与 CPU/CUDA 链路使用各自的帧布局。Linux 主程序 `BS`／`UE` 使用软件信道仿真。61.44 MSa/s 真板视频和双机真实射频仍待验收。

## English

### Changes since v1.1.0

- **Eight-layer 8×8 formal frame.** The Studio integration adds a version 2 header, eight-port FDM or front-loaded DM-RS, LDPC payload mapping, CRC-16, per-layer EVM, and 64 CSI paths. It also supports SISO, 2×2 and 4×4 spatial multiplexing, and 2×2 Alamouti STBC. [Frame specification](integrations/yunsdr_studio/FRAME_V2.md)
- **816 active subcarriers and sensing.** The frame uses a 1024-point FFT and a 128-sample cyclic prefix. Active indices are ±1 to ±408, with DC unused. The Zadoff–Chu preamble uses the same frequency span. Nominal bandwidth is 817/1024 of the sample rate (about 79.79%), or 98.04 MHz at 122.88 MSa/s. Pseudorandom QPSK fills unused payload positions. EVM uses the symbols carrying LDPC-coded bits. Range–Doppler and CFAR use CSI on the 816 active carriers. [Integration guide](integrations/yunsdr_studio/README.md)
- **Video transport.** Clip loopback and live video share the 816-carrier frame. Live video supports camera and file inputs, 2×2 STBC, QPSK through 256-QAM, FDM or front-loaded DM-RS, and 15.36, 30.72, or 61.44 MSa/s. Modes include local simulation, local RF loopback, and separate transmit and receive devices. Timestamped DMA uses 2 ms blocks, and high-rate reception screens synchronization candidates before decoding. The integration provides independently expandable video, constellation, EVM, and CSI panels. [Video setup and validation](integrations/yunsdr_studio/Y240_LIVE_VALIDATION.md)
- **Y240 hardware and deployment.** The Windows subproject adds the `pcies:0.0` backend, timestamped streams, SISO, 2×2 spatial multiplexing and STBC, and a VLC/UDP bridge with a monitor. It includes reproducible SDK staging, separate software and hardware builds, and troubleshooting guides for video startup and short-tail fragments. [Y240 adapter](libyunsdr-isac/README.md)
- **Windows and DGX Spark compute backends.** The standalone C++ PHY adds DGX Spark ARM64 CPU support, optional CUDA/cuFFT, batched OFDM and up to eight-stream MIMO detection, and a four-port device-resident FDM receive path. This release also includes channel-estimation and LDPC optimizations, a Windows/POSIX video bridge, and reproducible benchmarks. [Windows and DGX results](DGX_WINDOWS_CPU_GPU_RESULTS.md)

### Validation

| Check | Result |
| --- | --- |
| Windows public-source regression | C++ PHY CPU CTest passed 10/10, the Y240 software baseline passed 3/3, and 119 Python PHY and utility tests passed. |
| 816-carrier C++ PHY | 40 topology, pilot, and modulation combinations passed IQ, payload, CRC, EVM, and CSI checks. |
| Studio integration | The earlier full Studio Windows offline suite passed all 74 tests. Another 16 OpenISAC tests passed with a library rebuilt from the public source. Bidirectional eight-layer Python/C++ checks passed. Worst simulated layer RMS EVM was 0.5661%, with a CFO estimation error of 0.289 Hz. |
| Sensing and video simulation | At 122.88 MSa/s, all five topologies and both pilot types detected the nine-sample echo delay, about 10.98 m. At 15.36 MSa/s and 30 dB AWGN, about 30 seconds each at 1, 1.5, and 2 Mbit/s completed with zero datagram loss. [Video simulation CSV](results/benchmarks/studio_816_live_windows.csv) |
| 61.44 MSa/s mock DMA video | With 64-QAM and front-loaded DM-RS, the three rates recovered 2,845, 4,273, and 5,646 datagrams, with zero loss, timestamp gaps, and host drops. [Mock DMA CSV](results/benchmarks/studio_6144_timed_mock.csv) |
| Y790 8T8R cabled RF loopback (recorded earlier) | At 30.72 MSa/s, eight-layer 64-QAM/FDM recovered the exact 128-byte payload, passed CRC-16, and reported 64 CSI paths. Two-channel STBC video recovered 995,869 of 995,869 datagrams after draining. Display latency was 222.5 ms at P95. [Hardware record](integrations/yunsdr_studio/Y240_LIVE_VALIDATION.md#y790-8t8r-的实测记录) |
| DGX Spark (recorded earlier) | CPU CTest passed 10/10 and CUDA CTest passed 12/12. All 150 mode runs across Windows CPU, DGX CPU, and DGX CUDA recovered the payload byte for byte, with zero FER and UDP loss. [Benchmarks](DGX_WINDOWS_CPU_GPU_RESULTS.md) |

### Project scope

The [Studio integration](integrations/yunsdr_studio/README.md) includes a standalone C++ smoke test. The integration panels require the Studio host, whose repository remains private. The standalone Y240/VLC and CPU/CUDA paths use their respective frame layouts. The main Linux `BS` and `UE` programs use the software channel simulator. Hardware acceptance at 61.44 MSa/s and over a two-device RF link remains pending.
