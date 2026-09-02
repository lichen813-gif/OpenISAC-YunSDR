# OpenISAC-YunSDR：搭建与使用说明

[English](README.md) | [设计文档](docs/design/README.md) | [更新日志](CHANGELOG.md)

本仓库当前版本包含完整的 C++/Python 程序源码、配置文件和中英文设计说明，不包含视频文件。

> 当前后端状态：Linux 主程序 `BS`/`UE` 仍以软件信道仿真为主，原 UHD/USRP 实现继续保持移除。新增独立的 Windows 子项目 [`libyunsdr-isac/`](libyunsdr-isac/README.md)，已经通过厂商无关的 `RadioBackend` 边界接入 YunSDR Y240 `pcies:0.0`。厂商 SDK 二进制、驱动、固件和视频不在本仓库中再分发。

## 组成

| 组件 | 入口 | 作用 |
| --- | --- | --- |
| 信道仿真 | `ChannelSimulator` | 通信/感知共享内存链路、噪声、CFO/SRO、时延、多径和目标模拟 |
| 基站 | `BS` | OFDM 下行、可选上行接收、单站感知、UDP/ZMQ 输入输出 |
| 终端 | `UE` | 下行同步解调、可选上行发送、双站感知 |
| 前端工具 | `scripts/` | 感知绘图、诊断、配置与控制工具 |
| 正式 PHY | `cpp_phy/`、`python_phy/` | 跨语言帧结构、MIMO、调制、LDPC 和回归验证 |
| CPU/CUDA 加速 | `cpp_phy/cuda/`、`results/benchmarks/` | Windows CPU 与 DGX Spark ARM64 CPU/CUDA 后端、测试、基准及发布结果 |
| YunSDR 硬件 | `libyunsdr-isac/` | Y240 适配器、时间戳收发、射频环回、VLC/UDP 桥、诊断与硬件测试 |

## 环境依赖

主实时程序面向 C++17 Linux 环境，依赖 Boost 1.66+、OpenMP、AFF3CT 3.0.2+、FFTW3f（含 `fftw3f_threads`）、yaml-cpp、libzmq 和 cppzmq；DPDK 可选。

Ubuntu/Debian 可先安装系统依赖：

```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config \
  libboost-all-dev libfftw3-dev libyaml-cpp-dev \
  libzmq3-dev cppzmq-dev nlohmann-json3-dev python3-venv
```

另外安装 AFF3CT 3.0.2 或更高版本并记下安装前缀。本版本不需要安装 UHD。

## 算法仿真

- Windows C++ PHY：[总体说明](cpp_phy/README.md)、[正式帧结构](Windows_C++_PHY正式帧结构与使用说明.md)、[VLC视频测试](Windows_VLC视频信道仿真使用说明.md)
- Python黄金模型：[模型与验证说明](python_phy/README.md)
- 设计路线：[Python MIMO到C++](Python_MIMO到C++实施路线.md)、[4×4/DM-RS验证](4x4_MIMO设计与第一阶段验证.md)

## YunSDR 硬件接入

Windows 硬件适配器说明见 [`libyunsdr-isac/README_zh.md`](libyunsdr-isac/README_zh.md)。源码包括 Y240 `pcies:0.0` 后端、正式 PHY 编解码桥、带时间戳的 SISO/2×2 收发、射频环回和 VLC/UDP 工具。全新拉取后的逐步搭建见 [`Y240 Windows 快速搭建`](libyunsdr-isac/docs/Y240_WINDOWS_QUICKSTART_zh.md)。已验证的厂商 SDK 需要在本机单独放置，Git 仓库不会上传或再分发该 SDK。

```powershell
cd libyunsdr-isac
.\build_vs2019.cmd
```

上面的命令只构建不依赖厂商 SDK 的软件接口和算法回归。Y240 硬件版本需先把
`libyunsdr-26-01-00.1` 与 libusb 1.0.23 放入忽略的 `import/` 目录，再运行：

```powershell
.\build_vendor_y240_pcies_vs2019.cmd
.\build_hardware_vs2019.cmd
```

压缩包目录结构和故障排查见子项目说明；厂商文件不属于仓库缺失源码。

## Windows 与 DGX Spark CPU/CUDA

独立 C++ PHY 现已支持 Windows x64 CPU，以及 DGX Spark ARM64 CPU/CUDA
构建。CUDA 为可选后端，通过 `--backend cpu|cuda|auto` 选择；CPU 路径始终
作为数值基准和不可用时的回退路径保留。

- [DGX Spark 移植状态](DGX_PHY_PORT_STATUS.md)
- [Windows CPU、DGX CPU 与 CUDA 性能对比](DGX_WINDOWS_PHY_PERFORMANCE.md)
- [视频传输与 CUDA 移植方案](DGX_Spark视频传输与CUDA移植方案.md)
- [英文阶段摘要](DGX_WINDOWS_CPU_GPU_RESULTS.md)
- [发布的基准原始数据](results/benchmarks/README.md)

## 编译

```bash
git clone https://github.com/lichen813-gif/OpenISAC-YunSDR.git
cd OpenISAC-YunSDR
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAFF3CT_ROOT=/path/to/aff3ct/prefix
cmake --build build -j"$(nproc)"
```

AFF3CT 位于系统标准路径时可省略 `-DAFF3CT_ROOT`。主要产物为 `build/ChannelSimulator`、`build/BS` 和 `build/UE`。

## 仓库结构

| 路径 | 说明 |
| :--- | :--- |
| `src/`、`include/` | 核心 C++ PHY、感知、线程与运行时逻辑 |
| `config/` | BS/UE纯仿真配置样例 |
| `scripts/` | Python 前端、网页配置控制台、Linux 性能调优脚本 |
| `python_phy/` | 跨Windows/Linux的纯Python算法模型、配置、实验和单元测试 |
| `cpp_phy/` | 独立于UHD的C++17 PHY核心、VS2019脚本、基准、视频桥和实时监视器 |
| `cpp_phy/cuda/` | DGX Spark 可选 CUDA/cuFFT OFDM 与 MIMO 加速后端 |
| `results/benchmarks/` | Windows CPU、DGX CPU 与 DGX CUDA 的发布基准快照 |
| `libyunsdr-isac/` | YunSDR Y240 硬件适配器、配置、验证工具和硬件说明 |
| `capture/` | 离线感知结果绘图工具 |
| `docs/` | 项目静态站点，以及架构/信号处理说明页 |

Python 工具环境：

```bash
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -r requirements.txt
```

## 第一次运行仿真

BS 和 UE 必须使用成对配置。从仓库根目录执行：

```bash
cp config/BS_Sim.yaml build/BS.yaml
cp config/UE_Sim.yaml build/UE.yaml
```

在 `build/` 下打开三个终端，按顺序启动：

```bash
# 终端 1
./ChannelSimulator BS.yaml

# 终端 2
./BS BS.yaml

# 终端 3
./UE UE.yaml
```

仿真器必须先启动，BS 和 UE 会连接它创建的共享内存会话。上行/双工实验使用 `BS_Sim_Duplex.yaml` + `UE_Sim_Duplex.yaml`，eRTM 使用 `BS_Sim_eRTM.yaml` + `UE_Sim_eRTM.yaml`；ResourceMap 配置也应按文件名成对使用。

在仓库根目录可启动感知前端：

```bash
python scripts/plot_sensing_fast.py
python scripts/plot_bi_sensing_fast.py
```

## 运行时物理层可调参数

主程序 `BS`/`UE` 的帧结构由 YAML 控制。所有收发双方共享的结构参数必须一致。

| YAML 区域 | 可调整字段 | 作用 |
| --- | --- | --- |
| `rf_sampling` | `sample_rate`、`bandwidth` | 采样时钟和占用带宽 |
| `ofdm_frame` | `fft_size`、`cp_length`、`num_symbols`、`sync_pos` | 基本 OFDM 帧格式 |
| `ofdm_frame` | `enable_sec_sync_symbol`、`enable_cfo_training_sequence`、`cfo_training_period_samples` | 同步与 CFO 训练格式 |
| `ofdm_frame` | `zc_root`、`pilot_positions`、`midframe_pilot_symbols`、`midframe_pilot_seed` | 前导和导频布局 |
| `downlink` | `center_freq`、`modulation` | 调制支持 `qpsk`、`16qam`、`64qam`、`256qam` |
| `uplink` | 双工开关/模式、`symbol_start`、`symbol_count`、`guard_symbols` | 双工预设中的 TDD/FDD 和上行时隙 |
| `sensing` | `rx_channel_count`、`range_fft_size`、`doppler_fft_size`、`symbol_stride`、`output_mode` | 感知阵列与处理维度 |
| `simulation` | `target_snr_db`、`noise_power_dbfs`、`cfo_hz`、`sample_rate_offset_ppm`、`timing_offset_samples` | 链路损伤 |
| `simulation` | `comm_multipath_taps`、`targets`、`bistatic_targets`、阵元间距 | 多径和感知场景 |

配置约束：

- BS/UE 的采样率、FFT/CP、帧符号数、同步格式、ZC 根、导频位置/种子、调制方式和双工窗口必须匹配。
- 导频与同步索引必须在 FFT/帧范围内；`sensing_symbol_num` 不得大于 `num_symbols`。
- TDD 下 `symbol_start + symbol_count` 不得超过 `num_symbols`，保护符号会占用上行窗口。
- CP 应长于需要避免符号间干扰的最大信道时延。
- ChannelSimulator、BS、UE 必须使用相同的 `simulation.session`；同时运行多组实例时不能复用 UDP/ZMQ 端口。

[`config/`](config/) 中的样例是可直接运行的权威配置，并带字段级注释。

## C++/Python 正式帧

独立的 `cpp_phy` 兼容配置比运行时 YAML 更严格：FFT 1024、CP 128、1 个 ZC 符号 + 2 个数据符号、128 个控制 RE、两个物理发射端口和 LDPC(1008,504)。可动态选择 Rank 1/2 与 QPSK/16/64/256-QAM，对应用户载荷容量为：

| Rank | QPSK | 16-QAM | 64-QAM | 256-QAM |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 124 B | 250 B | 439 B | 565 B |
| 2 | 250 B | 565 B | 880 B | 1195 B |

在正式帧路径中，FFT 大小和 128 控制 RE 是兼容常量，不是运行时旋钮；修改时必须同步更新 C++/Python 实现与测试向量。详见[正式帧结构与使用说明](Windows_C++_PHY正式帧结构与使用说明.md)、[C++ PHY](cpp_phy/README.md)和 [Python PHY](python_phy/README.md)。

## libyunsdr 接入状态

仓库已经包含厂商无关无线接口、Y240 `pcies:0.0` 适配器、SISO/2×2/STBC
正式 PHY、时间戳流、射频回环、VLC/UDP 桥和实机测试说明。受许可和设备安全
约束，`libyunsdr` 厂商源码/二进制、libusb 包、驱动、固件与视频不会随 Git
分发，必须按 [`libyunsdr-isac/README_zh.md`](libyunsdr-isac/README_zh.md)
单独准备。路线背景见[最新物理层感知验证与 libyunsdr 路线](最新物理层感知验证与libyunsdr路线.md)。

## 测试

```bash
python -m pytest python_phy/tests -q
```

C++ 正式 PHY 的 Windows 辅助脚本位于 `cpp_phy/`，包括 `build_windows_vs2019.cmd` 和 `run_cpp_tests.cmd`。

## 许可与原项目引用

本仓库保留适用的[许可证](LICENSE)。原有开源项目介绍、作者、社区和服务信息不再放在首页；原项目与引用信息请查看[原始 OpenISAC 仓库](https://github.com/zhouzhiwen2000/OpenISAC)和 [OpenISAC 论文（arXiv:2601.03535）](https://arxiv.org/abs/2601.03535)。
