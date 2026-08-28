# DGX Spark 视频传输与 CUDA 移植方案

## 1. 目标与范围

目标是在 DGX Spark 上运行现有 OpenISAC 视频物理层仿真，并在保持
Windows 版本结果兼容的前提下逐步加入 CUDA 加速。

首个可交付版本是纯算法闭环：

```text
VLC/FFmpeg UDP发送 -> OFDM/LDPC/MIMO发送机 -> 信道仿真
                  -> 同步/信道估计/MIMO检测/LDPC接收机
                  -> UDP输出 -> VLC播放与实时监视器
```

完整 C++ 物理层迁移必须覆盖 Windows 版本现有能力：

- SISO Rank-1；
- 2Tx/2Rx Rank-2 空间复用；
- 2Tx/2Rx Alamouti STBC；
- 4×4 Rank-2/Rank-4；
- 通用 1×1 到 8×8 ZF/MMSE 检测、TDL 信道和高阶调制算法；
- FDM 导频和 NR-like DMRS；
- QPSK、16QAM、64QAM、256QAM；
- AWGN、TDL 多径、CFO、SFO、定时偏差、空间相关；
- 星座图、时域波形、EVM、同步、信道估计、FER 和距离-速度感知。

视频是完整物理层迁移后的端到端应用验收，不代表算法能力上限。8×8 的
验收分为检测器/信道算法、正式帧链路和视频链路三个等级，文档和测试结果
必须分别说明，不能把 8×8 检测器通过等同于 8×8 视频已经打通。

## 2. 平台事实和当前状态

DGX Spark 是 ARM64 Grace Blackwell 统一内存平台。当前 NVIDIA 官方软件栈
基于 Ubuntu/DGX OS，并提供 CUDA、开发工具、Docker 和 NVIDIA Container
Runtime。因此不能复用 Windows x64 二进制，必须从源码在 ARM64 上重新
编译。

截至 2026-08-27，环境接入和CPU迁移已经完成：

- DGX 地址：使用实际设备的主机名或局域网地址；
- DGX 工程目录：`/path/to/OpenISAC-YunSDR`；
- TCP 22端口和SSH密钥登录已经验证；具体账号不属于发布内容；
- `cpp_phy` 全部C++17源码已在ARM64/GCC Release编译；
- 视频桥已使用Windows/POSIX双平台socket；
- Windows 10/10、DGX CPU 10/10、DGX CUDA 12/12回归通过；
- CUDA Toolkit 13.0.3、`nvcc` 13.0.88和cuFFT可用；
- 批量OFDM CUDA和通用1/2/4/8流CUDA检测器已经完成第一阶段验证；
- `libyunsdr-isac` 当前硬件配置只识别 Windows `.lib/.dll`。在获得 ARM64
  Linux 版 `libyunsdr.so` 和设备接口验证前，DGX 阶段不直接控制 Y240。

VLC和FFmpeg尚未安装，所以Linux视频播放阶段尚未开始；当前先完成物理层
算法和CUDA数值等价迁移。

## 3. 实施原则

1. CPU 是正确性基准。任何 CUDA 后端必须和 CPU 使用相同帧结构、随机
   种子和测试向量。
2. Windows 与 Linux 共用算法源码，只在 socket、进程启动和显示层使用
   小型平台适配。
3. CUDA 采用可选后端：`--backend cpu|cuda|auto`。CUDA 不可用时仍可运行
   CPU 版本。
4. 先剖析后加速。优先迁移计算密集且批处理友好的模块，不先改控制流和
   UDP/VLC 部分。
5. 每个阶段必须有自动测试、性能数据和可回退的 CPU 路径。
6. 首先保证包零差错和视频连续播放，再追求最低延迟。

## 4. 分阶段路线与成果

### 阶段 0：DGX 环境接入和基线快照

工作：

- 建立 SSH 密钥登录；
- 采集 `uname -a`、`uname -m`、DGX OS、GCC、CMake、Python、VLC/FFmpeg、
  `nvidia-smi`、`nvcc --version` 和 CUDA 库信息；
- 检查磁盘、统一内存、CPU governor、网络 MTU 和时钟；
- 固定一份可复现的构建环境，优先裸机 CMake，另提供容器方案作为补充。

成果：

- `out/dgx-spark/environment.txt`；
- DGX 构建工具清单；
- SSH 无交互登录和文件同步命令；
- CUDA 架构与 CMake 配置实测值。

验收：Windows 主机可以无密码执行 DGX 上的 `hostname`，DGX 可以读取工程
源码，且 CUDA sample/deviceQuery 通过。

状态：已完成。

### 阶段 1：Linux ARM64 CPU 正确性版本

工作：

- 把视频桥 Winsock 调用封装为 Windows/POSIX 双平台接口；
- Linux 使用 BSD socket、`select`/`poll` 和 POSIX signal；
- CMake 在 Windows 链接 `ws2_32`，Linux 生成同名视频桥；
- 增加 `build_dgx.sh` 和 Linux CTest；
- 排查 ARM64 下字节序、类型宽度、未对齐访问和线程行为。

成果：

- DGX 原生 `openisac_phy_video_bridge`；
- Windows 和 DGX 共用的源码；
- SISO、MIMO2、STBC、FDM、DMRS 自动回归报告。

验收：

- 全部 C++ 单元测试通过；
- 固定随机种子时 payload、CRC、帧数与 Windows 一致；
- AddressSanitizer/UndefinedBehaviorSanitizer 自检无错误；
- 256QAM 高 SNR 自检逐字节恢复。

状态：已完成。RelWithDebInfo + AddressSanitizer +
UndefinedBehaviorSanitizer回归10/10通过。

### 阶段 2：DGX CPU 视频闭环

工作：

- 增加 Linux 启动脚本，管理 VLC/FFmpeg 发送、接收、物理层和监视器；
- 支持前台播放、无界面测试和自动清理；
- 保持 UDP 50000 输入、50001 输出和现有遥测 CSV 格式；
- 对两个现有测试视频分别跑 SISO、MIMO2、STBC。

成果：

- `run_dgx_video.sh`；
- 三种模式的日志、FER/EVM、吞吐量和端到端延迟；
- Linux 监视器或通过 SSH X11/Web 方式查看的远程监视方案。

验收：

- 每种模式连续运行 10 分钟；
- 应用 UDP 丢包为 0，接收视频可连续播放；
- 脚本结束后无遗留 VLC、桥接器和监视器进程；
- CPU 负载、实时倍率和内存稳定。

预计：1～2 天。

### 阶段 3：CPU 性能基线和热点剖析

工作：

- 分模块统计 FFT/IFFT、TDL 卷积、同步、信道估计、MIMO MMSE/ZF、LDPC、
  感知和 CSV 输出耗时；
- 使用 Nsight Systems、`perf` 和现有 benchmark 分析串并行关系；
- 记录每帧主机内存流量和可批处理规模。

成果：

- CPU 每模式每帧耗时占比；
- 1、2、4、8、16 帧 batch 的吞吐/延迟曲线；
- CUDA 迁移优先级和预期收益表。

验收：性能数据可重复三次，波动不超过 10%；能说明实时视频瓶颈来自哪个
模块，而不是凭经验选择。

预计：0.5～1 天。

### 阶段 4：CUDA 基础后端

优先迁移：

1. 批量 OFDM FFT/IFFT，使用 cuFFT；
2. TDL 多径、CFO/SFO 和 AWGN 信道核；
3. 导频提取、信道均衡和星座软信息；
4. 2×2 MMSE/ZF 小矩阵批量检测；
5. 4端口检测仅在 2×2 稳定后加入。

实现：

- 新建稳定的 C++ `IPhyComputeBackend` 接口；
- `CpuBackend` 保持现有算法；
- `CudaBackend` 使用 CUDA stream、事件计时和持久工作区；
- 利用 DGX Spark CPU/GPU 统一内存特性，避免每帧重复分配；是否使用
  managed memory、pinned memory 或显式 prefetch 由实测决定；
- 支持 `--backend cpu|cuda|auto` 和 `--cuda-batch N`。

成果：

- 可独立启停的 CUDA 后端；
- CPU/CUDA 数值一致性测试；
- 每模块 kernel 时间、端到端吞吐和延迟报告。

验收：

- CPU 与 CUDA 的 CRC/payload 结果完全一致；
- 浮点中间量使用明确容差，EVM 差异不超过 0.1 个百分点；
- 2×2 64QAM 视频至少实时运行；
- 与 CPU 相比取得明确收益；若单帧 CUDA 更慢，则保留 CPU 自动回退并通过
  批处理验证适用范围。

预计：3～5 天。

### 阶段 5：LDPC 和感知 CUDA 加速

顺序：

- 先迁移距离 FFT、Doppler FFT、链路合并和 CFAR；
- LDPC 编解码只有在阶段 3 确认其占用显著时迁移；
- LDPC GPU 版本保留与 `(1008,504)` 码、CRC16、软判决约定完全相同。

成果：

- CUDA 距离-速度图与 CPU 图逐点对比；
- CUDA CFAR 检测列表；
- 可选 GPU LDPC 后端及批量吞吐报告。

验收：感知峰值距离/速度落在同一 bin，CFAR 检测集合一致或差异有明确的
浮点阈值解释；视频 payload 仍逐字节一致。

预计：2～4 天。

### 阶段 6：实时稳定性和视频编解码优化

工作：

- 增加有界队列、背压、批处理动态调整和运行时监控；
- 评估 VLC 软件编码与 FFmpeg NVENC/NVDEC；
- 遥测刷新和视频处理解耦，避免绘图阻塞物理层；
- 连续运行、温度、功耗、统一内存压力和错误恢复测试。

成果：

- CPU/CUDA 一键启动脚本；
- 1、2、5 Mbit/s 视频档位；
- 30 分钟和 2 小时稳定性报告；
- CUDA/CPU 自动回退和异常退出清理。

验收：2×2 64QAM 视频运行 2 小时无应用丢包、无内存持续增长，平均处理
速度高于输入速度并留有不少于 20% 余量。

预计：2～3 天。

### 阶段 7：Y240 硬件接入决策

优先确认 YunSDR 是否提供：

- ARM64 Linux 头文件和 `libyunsdr.so`；
- Y240 在 DGX Spark 上可用的 PCIe/USB 驱动；
- 时间戳、多端口收发和事件计数 API 与 Windows 版一致。

分支：

- 若 ARM64 SDK 和设备链路均可用：把 `libyunsdr-isac` 后端移植到 Linux，
  直接做 DGX+Y240 回环；
- 若只有 x86/Windows SDK：Y240 继续由 Windows 主机采集，DGX 只做算法
  仿真和离线 IQ 加速；
- 只有在 10GbE 延迟/吞吐测量满足要求后，才考虑 Windows SDR I/O 与 DGX
  GPU 处理分离的网络 IQ 架构。

成果：硬件兼容性矩阵、推荐拓扑和实测带宽/延迟，不在缺少 ARM64 SDK 时
强行包装 Windows DLL。

预计：直连支持资料齐全时 3～7 天；否则输出不可行证据和替代拓扑。

## 5. 工期和里程碑

| 里程碑 | 阶段 | 预计累计工作量 | 可演示成果 |
|---|---|---:|---|
| M1 Linux 正确性 | 0～1 | 1.5～2.5 天 | DGX CPU 全模式自检通过 |
| M2 Linux 视频 | 2 | 2.5～4.5 天 | DGX CPU 播放 SISO/MIMO2/STBC 视频 |
| M3 CUDA 主链路 | 3～4 | 6～10.5 天 | CPU/CUDA 对比及 2×2 实时视频 |
| M4 CUDA 感知 | 5 | 8～14.5 天 | GPU 距离-速度图与 CFAR |
| M5 工程稳定版 | 6 | 10～17.5 天 | 两小时稳定运行和一键脚本 |
| M6 Y240 决策/接入 | 7 | 依 SDK 增加 3～7 天 | DGX 直连或经过验证的替代拓扑 |

工期以 SSH 可用、工程能同步、系统 CUDA 环境正常为起点。CUDA 的最终
加速比不能在剖析前承诺；验收以端到端实时倍率、延迟和零差错为准。

## 6. 第一轮远端命令

SSH 开通后，先执行只读环境检查：

```bash
hostname
uname -a
uname -m
cat /etc/os-release
gcc --version
g++ --version
cmake --version
nvidia-smi
nvcc --version
python3 --version
vlc --version | head -n 1
ffmpeg -version | head -n 1
```

确认环境后才同步代码和安装缺失依赖。不会在环境未知时直接升级驱动、
CUDA 或 DGX OS。

## 7. 立即执行顺序

1. 用户启用 DGX SSH 或告知实际 SSH 端口和用户名；
2. 保存阶段 0 环境快照；
3. 完成视频桥 POSIX socket 移植；
4. 在 Windows 重跑现有回归，确保无退化；
5. 在 DGX 编译并跑 CPU 自检；
6. 跑通无界面 UDP 视频闭环；
7. 获取 CPU profile 后启动 CUDA 阶段。
