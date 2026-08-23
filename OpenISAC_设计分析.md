# OpenISAC 开源 ISAC 平台设计分析

> 项目地址：[zhouzhiwen2000/OpenISAC](https://github.com/zhouzhiwen2000/OpenISAC)
> 整理日期：2026-08-21
> 适用版本：GitHub `main` 分支及 arXiv 论文 v2

## 1. 项目定位

OpenISAC 是一套运行在主机 CPU 上的实时 OFDM 通信感知一体化（ISAC）实验平台，定位在“纯 MATLAB/Python 仿真代码”和“完整 Wi-Fi/5G NR 协议栈”之间。

它的主要特点是：

- 可以直接连接 USRP 开展真实空口实验；
- 使用同一套 OFDM 波形同时承载通信数据并进行雷达式感知；
- 支持 BS 单站感知、UE 双站感知、上下行通信、TDD/FDD 和空口同步；
- C++ 后端承担实时物理层处理，Python 前端用于可视化、控制和算法迭代；
- 基于 UHD、AFF3CT、FFTW、ZeroMQ 等开源组件，无需商业软件或专用 FPGA 开发；
- 适合科研原型、论文复现以及波形、同步、通信和感知算法验证。

OpenISAC 不是标准兼容实现，不能直接与商用 Wi-Fi、LTE、5G NR 终端互通，也不是生产级基站或雷达协议栈。

需要注意的是，2026 年 7 月发布的论文 v2 主要验证了 SISO、下行通信、单站和双站感知；当前仓库 `main` 分支已经增加上下行、TDD/FDD、多通道单站感知和 eRTM 等功能，因此当前代码能力比论文原型更加丰富。

## 2. 系统总体架构

系统由一个基站节点 BS、一个用户节点 UE，以及可选的 Python 前端组成。

```text
业务数据/视频
    │ UDP/RTP
    ▼
┌───────────────────────────┐
│ BS C++ 后端               │
│ LDPC → QPSK → OFDM → USRP │
└─────────────┬─────────────┘
              │ 连续 OFDM 空口
       ┌──────┴──────────────┐
       │                     │
       ▼                     ▼
BS 感知 RX               UE 通信/感知 RX
单站回波                  下行通信 + 双站散射
       │                     │
       ▼                     ▼
距离/多普勒/角度          解调数据/双站时延-多普勒
       │                     │
       └─────────┬───────────┘
                 │ ZeroMQ
                 ▼
          Python 可视化与控制
```

### 2.1 BS 节点

BS 当前可以同时承担：

- 下行 OFDM 连续发送；
- UDP 业务数据接入；
- LDPC 编码、QPSK 调制和 OFDM 波形生成；
- 一个或多个通道的单站感知接收；
- 上行 OFDM 接收、均衡和 payload 解码；
- 单站感知结果和运行状态发布。

### 2.2 UE 节点

UE 当前可以同时承担：

- 下行帧同步、CFO/SFO 跟踪和信道估计；
- QPSK 均衡、软解映射和 LDPC 解码；
- 下行 payload UDP 输出；
- 根据接收信号形成双站感知结果；
- 可选的上行 LDPC/QPSK/OFDM 发射。

### 2.3 前端与数据传输

- 业务 payload 主要通过 UDP 输入和输出；
- 后端与感知前端之间通过 ZeroMQ 传输数据并发送控制命令；
- `plot_sensing_fast.py` 显示 BS 单站感知结果；
- `plot_bi_sensing_fast.py` 显示 UE 双站感知结果；
- `config_web_editor.py` 提供网页配置、进程启停和 CPU 隔离控制。

## 3. 功能整理

| 类别 | 主要能力 |
| --- | --- |
| 下行通信 | BS 接收 UDP 数据，完成 LDPC 编码、QPSK 调制、OFDM 发送；UE 同步、均衡、解调和 LDPC 解码后输出 UDP |
| 上行通信 | UE 编码并发送，BS 接收解码；支持 TDD 帧内窗口和 FDD 独立上行载频 |
| 单站感知 | BS 发射并接收自身照射目标的回波，形成距离像、距离-多普勒图和微多普勒谱 |
| 多通道感知 | 同步采集多个感知 RX 通道，保留跨通道复相位，可进行 ULA 到达角估计和波束形成 |
| 双站感知 | UE 使用 BS-目标-UE 散射路径，输出相对时延、双站多普勒和微多普勒 |
| 同步 | Zadoff-Chu 帧同步、初始 CFO 捕获、残余 CFO/SFO 跟踪、整数和分数时延补偿 |
| 双站定时 | 支持 OTA LoS tracking 和使用上下行信道的 eRTM 两种方案 |
| 杂波抑制 | IIR MTI 高通抑制直泄、静态环境和零多普勒杂波 |
| 实时控制 | YAML 配置、ZeroMQ 控制、CPU 绑定与隔离、Web 配置台 |
| 无硬件仿真 | `ChannelSimulator` 模拟 LoS、多径、目标、噪声、CFO、SFO、定时偏差和阵列响应 |
| 应用演示 | 可使用 UDP/RTP/FFmpeg 将视频作为真实业务 payload 通过空口传输 |

## 4. OFDM 通信原理

### 4.1 发送链路

发送端的基本流程为：

```text
UDP 数据
  → 分包和扰码
  → LDPC 信道编码
  → QPSK 映射
  → ZC/导频/数据资源映射
  → IFFT
  → 插入循环前缀 CP
  → USRP 连续发送
```

一帧可以包含：

- 全带宽 ZC 主同步符号；
- 可选的第二同步符号；
- 可选的重复 CFO 训练字段；
- 梳状导频；
- QPSK 数据资源；
- 可选的全带宽中帧信道参考；
- TDD 模式下的下行、保护和上行符号区间。

设 FFT 大小为 `N`，循环前缀为 `N_CP`，子载波间隔为 `Δf`，则：

```text
有效 OFDM 符号时长 T = 1 / Δf
带宽 B = N × Δf
采样周期 Ts = 1 / B
完整符号时长 To = (N + N_CP) × Ts
```

### 4.2 接收链路

UE 首先在 `SYNC_SEARCH` 状态利用 ZC 相关峰寻找完整帧边界，估计初始时偏和载波频偏。进入 `NORMAL` 状态后，利用参考符号和梳状导频持续完成：

1. 信道估计；
2. 残余 CFO 跟踪；
3. SFO/采样钟漂移跟踪；
4. MMSE 或配置的信道均衡；
5. QPSK 软解映射；
6. LDPC 解码；
7. payload 重组和 UDP 输出。

### 4.3 连续 OFDM 的意义

普通 Wi-Fi 是突发式分组发送，分组间隔可能不均匀，会使慢时间采样不连续，导致多普勒旁瓣和微多普勒伪影。

OpenISAC 连续发送等间隔 OFDM 符号，可以获得：

- 规则的慢时间采样；
- 更长的相干积累时间；
- 直接、稳定的 Doppler FFT；
- 更清晰的微多普勒谱；
- 灵活的感知符号间隔和积累长度。

## 5. BS 单站感知原理

BS 知道自身发送的 OFDM 资源网格 `B[n,m]`。接收到回波并完成 FFT 后得到 `Y[n,m]`，可逐元素消除通信调制：

```text
F[n,m] = Y[n,m] / B[n,m]
```

`F[n,m]` 可以看作目标、杂波和传播环境的时频信道采样。

后续处理包括：

1. 对慢时间执行 MTI 高通，抑制直泄和静态杂波；
2. 对子载波方向执行 IFFT，得到时延/距离轴；
3. 对慢时间方向执行 FFT，得到多普勒/速度轴；
4. 多通道情况下保留通道复相位，进行波束扫描或到达角估计；
5. 对选定距离单元进行 STFT，生成微多普勒谱。

基本单站距离分辨率为：

```text
Δr_mono = c / (2B)
```

慢时间相干处理长度为 `M_s`、采样间隔为 `T_slow` 时：

```text
多普勒分辨率 Δf_D = 1 / (M_s × T_slow)
两侧无模糊范围 |f_D| < 1 / (2T_slow)
```

多通道到达角估计依赖阵列通道之间准确的相对相位。因此在真实设备上必须校准不同 RF 通道的固定增益、相位和群时延，否则会形成明显的角度偏差。

## 6. UE 双站感知原理

UE 接收到的下行数据符号事先未知。OpenISAC 先对均衡后的 QPSK 符号进行硬判决，重构发送资源网格 `B_hat[n,m]`，再计算：

```text
F_UE[n,m] = Y_UE[n,m] / B_hat[n,m]
```

若判决正确，结果就是 BS 到 UE 通信/双站信道的时频采样；低信噪比下，错误判决会成为稀疏异常值，此时可只选用已知参考资源或高置信度数据资源。

### 6.1 双站感知的定时问题

通信系统只要求路径落在循环前缀范围内，少量时延误差可以被信道均衡吸收。但对于感知，时延本身就是待测量量。BS 和 UE 使用独立本振时，时钟漂移会直接表现为目标距离漂移或阶梯跳变。

OpenISAC 提供两种双站时偏补偿方式。

#### OTA LoS tracking

- 只使用 UE 下行信道；
- 持续跟踪直达 LoS 路径的分数时延；
- 将通信链路产生的整数定时修正加回连续坐标；
- 拟合采样钟漂移并补偿感知时延轴；
- 输出结果以 LoS 路径为零时延参考。

优点是无需上行链路；缺点是要求 LoS 长期可见。如果直达径消失或主峰切换，算法可能把传播环境变化误判为时钟漂移。

#### eRTM

- 同时使用 UE 的下行信道估计和 BS 的上行信道估计；
- 根据上下行信道互易关系估计 BS、UE 的差分时偏；
- 结合已校准的上下行 RF 群时延、BS 下行/上行时序差和 UE Timing Advance，分离两端时偏；
- 补偿后可以保留更接近真实值的传播时延。

eRTM 不要求始终存在 LoS，但需要启用上下行并完成 RF 群时延校准。TDD 条件通常更符合信道互易假设；FDD 上下行载频差异过大时，路径结构和复增益不再充分互易，可靠性会下降。

双站测量得到的是 BS-目标-UE 的总路径或相对 LoS 的路径差，不能直接按照单站雷达的 `r = cτ/2` 解释。

## 7. 典型参数及含义

仓库 X310 样例配置包括：

| 参数 | 样例值 | 含义 |
| --- | ---: | --- |
| 采样率 | 50 MS/s | 主机与 USRP 的基带采样率 |
| 模拟带宽 | 50 MHz | OFDM 占用带宽 |
| FFT 大小 | 1024 | 子载波总数 |
| CP 长度 | 128 samples | 2.56 μs |
| OFDM 符号数 | 100/frame | 每帧符号数量 |
| 感知符号数 | 100 | 一个处理批次的感知符号数 |
| 载频 | 2.4 GHz | 样例工作频率，可按硬件和许可修改 |
| 距离 FFT | 1024 | 时延轴处理长度 |
| 多普勒 FFT | 100 | 慢时间处理长度 |
| symbol stride | 20 | 每隔 20 个 OFDM 符号取一个慢时间样本 |

由这些参数可以得到：

- 理论单站距离分辨率约 `c/(2×50 MHz) ≈ 3 m`；
- CP 对应的无符号间干扰单站往返距离约 `c×2.56 μs/2 ≈ 384 m`；
- 这不是实际探测距离，实际距离仍取决于发射功率、天线增益、目标 RCS、噪声、直泄和接收动态范围；
- 增大带宽可以提高距离分辨率，但会增加 USRP 接口和主机实时处理压力；
- 减小 `symbol_stride` 可以提高慢时间更新率和无模糊速度，但会显著增加 CPU 负载。

## 8. 适用硬件

### 8.1 已有配置和优先推荐设备

| 设备 | 适用程度 | 使用建议 |
| --- | --- | --- |
| USRP X310 及兼容实现 | 最成熟 | 仓库有完整 BS/UE 配置，适合 50–100 MHz、稳定实时实验和多通道扩展 |
| USRP B210 及兼容实现 | 性价比最高 | 仓库有完整配置和双工配置，2×2 RF，可承担低成本 BS 或 UE |
| X310 + B210 混合 | 论文实测组合 | BS 使用 X310，UE 使用 B210；仓库也提供 B210 发射/X310 接收配置 |
| 无 USRP | 完整支持 | 使用 `ChannelSimulator`，适合先验证配置、处理链和前端 |

论文的实测系统为：

- BS：Luowave USRP-LW X310、Intel Core i7-10700、10GbE、外部 OCXO；
- UE：TQTT Tiny B210、Intel Core Ultra 5 225H、USB 3.0、OCXO 和 USB DAC；
- 带宽：50 MHz；
- 目标：包括 DJI Mavic Air 3S 无人机；
- 已验证单站距离-多普勒、单站微多普勒、OTA 同步和双站无人机感知。

### 8.2 可适配但需要自行验证

项目基于 UHD，作者声明设备覆盖范围可以从 B200 系列延伸到 X400 系列，但目前直接配置模板主要针对 X310 和 B210。

- **B200/B205mini 等单通道设备**：适合作为下行 UE 接收机；单台设备不适合同时承担 BS 发射和独立单站感知接收。
- **N300/N310**：通道数量较多、支持 10GbE，适合多通道接收阵列，但需要自行配置设备参数、通道映射和同步校准。
- **X410/X400 系列**：通道和瞬时带宽充足，但需要调整 UHD 设备参数、主时钟、FPGA image、传输接口和采样率。
- **其他 UHD USRP**：只要满足载频、采样率、同时收发通道、外部参考和主机吞吐要求，原则上可以移植。
- **LimeSDR、PlutoSDR 等非 UHD 设备**：不能直接运行，需要为 OpenISAC 新增 radio backend。

高端 USRP 的最大硬件带宽不等于 OpenISAC 的实时处理带宽。论文测试显示，50 MHz 和 100 MHz 配置可以稳定运行，而测试主机上的 200 MHz/4096 FFT 解调负载已经超过实时能力并发生队列丢帧。

## 9. 硬件连接要求

### 9.1 下行通信 + 单站 + 双站感知

最小完整系统需要：

- 2 台 USRP；
- 2 台后端计算机；
- BS：1 根 TX 天线和 1 根独立感知 RX 天线；
- UE：1 根 RX 天线；
- 两个独立高稳定度 OCXO 或 GPSDO；
- X 系列使用至少 10GbE；
- B 系列使用 USB 3.0。

### 9.2 启用上行

还需要：

- UE 增加独立 TX RF 链和天线；
- BS 增加独立的上行 RX RF 链和天线；
- 当前 BS 上行 RX 不应与感知 RX 共用同一 RF 通道；
- FDD 模式下设备必须同时支持所配置的上下行载频。

OTA 同步表示两个节点不需要通过线缆共享同一个参考时钟，但仍建议每台 USRP 使用独立的高稳定参考。软件负责估计并补偿两套参考之间剩余的 CFO 和 SFO。

## 10. 软件环境与编译

### 10.1 推荐环境

- 后端操作系统：Ubuntu 24.04 LTS；
- macOS Apple Silicon：主要用于本地开发和演示；
- 前端：Windows 或 Linux；
- C++：C++17；
- UHD：项目文档已测试 UHD v4.9.0.1；
- AFF3CT：用于 LDPC 前向纠错；
- Python：推荐 Python 3.13；
- Python GPU：可选 Nvidia CUDA/CuPy 或 Intel GPU `dpctl`/`dpnp`；
- 视频演示：需要 FFmpeg。

### 10.2 编译流程

安装 UHD、AFF3CT、FFTW、yaml-cpp、ZeroMQ 等依赖后：

```bash
git clone https://github.com/zhouzhiwen2000/OpenISAC.git
cd OpenISAC

mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

主要生成：

```text
build/BS
build/UE
build/ChannelSimulator
```

Python 前端：

```bash
conda create -n OpenISAC python=3.13
conda activate OpenISAC
pip install -r requirements.txt
```

## 11. 推荐使用流程

### 11.1 先进行无硬件仿真

```bash
cd build
cp ../config/BS_Sim.yaml BS.yaml
cp ../config/UE_Sim.yaml UE.yaml
```

在三个终端依次执行：

```bash
./ChannelSimulator
./BS
./UE
```

启动顺序很重要：`ChannelSimulator` 必须首先启动，因为它负责创建共享内存并提供统一的采样时间轴。

查看结果：

```bash
python3 scripts/plot_sensing_fast.py
python3 scripts/plot_bi_sensing_fast.py
```

仿真参数可以配置：

- 目标距离、速度、角度和增益；
- LoS 和静态多径；
- AWGN 噪声或目标 SNR；
- CFO；
- SFO/采样率偏差；
- 固定定时偏差；
- ULA 阵元间距；
- 自定义阵列流形；
- 单站和双站是否使用同一目标场景。

### 11.2 真实 OTA 运行

以 X310 为例，BS 节点：

```bash
cd build
cp ../config/BS_X310.yaml BS.yaml
sudo ../scripts/isolate_cpus.py
sudo ../scripts/isolate_cpus.py run ./BS
```

UE 节点：

```bash
cd build
cp ../config/UE_X310.yaml UE.yaml
sudo ../scripts/isolate_cpus.py
sudo ../scripts/isolate_cpus.py run ./UE
```

B210 使用：

```text
config/BS_B210.yaml
config/UE_B210.yaml
```

B210 TDD 双工使用：

```text
config/BS_B210_Duplex.yaml
config/UE_B210_Duplex.yaml
```

### 11.3 前端和 Web 控制

```bash
# BS 单站感知
python3 scripts/plot_sensing_fast.py

# UE 双站感知
python3 scripts/plot_bi_sensing_fast.py

# Web 配置与进程控制
python3 scripts/config_web_editor.py --host 127.0.0.1 --port 8765
```

浏览器访问：

```text
http://127.0.0.1:8765/
```

前端部署在其他计算机时，通过 `--host <后端IP>` 连接 BS 或 UE。YAML 中的 `default_out_ip` 主要用于目的型 UDP/debug 输出，不应误当作 ZeroMQ 监听地址。

## 12. 首次上机检查顺序

建议按以下顺序调试：

1. 使用 `uhd_find_devices` 和 `uhd_usrp_probe` 确认设备可见；
2. 确认外部参考时钟已经锁定；
3. 验证主机-USRP 接口吞吐，排除 USB/Ethernet 丢包；
4. 确保 BS/UE 的采样率、FFT、CP、频率、ZC root、导频位置和帧结构完全一致；
5. 先降低 TX/RX 增益，避免 BS 发射直泄使接收 ADC 饱和；
6. 先观察 ZC 同步、时延谱和星座图；
7. 确认通信稳定后再启用 MTI 和距离-多普勒处理；
8. 最后启用 OTA LoS tracking 或 eRTM；
9. 多通道测角前完成幅相和固定时延校准；
10. 出现实时丢帧时，执行性能调优和 CPU 隔离，增大 `symbol_stride` 或降低采样率/FFT 大小。

## 13. 适用场景

OpenISAC 特别适合：

- OFDM-ISAC 波形与资源映射研究；
- 无人机、车辆、人员和旋翼微多普勒实验；
- 单站和双站距离-多普勒感知；
- CFO、SFO、OTA 同步和异步双站算法研究；
- 多通道阵列、波束形成和 AoA 原型；
- BER、BLER、EVM、吞吐量与感知性能联合研究；
- 在真实无线信道下快速验证 PHY 算法；
- 教学、课程实验和论文原型复现。

## 14. 不适用场景与局限

OpenISAC 不适合：

- 与手机、Wi-Fi 设备或标准 5G gNB 直接互通；
- 完整 MAC、RLC、PDCP、核心网和认证测试；
- 无二次工程化直接用于商用雷达或生产系统；
- 强全双工自干扰场景；
- 未校准 RF 群时延却要求高精度绝对测距的场景；
- 只依靠单根天线、同时高功率发射和高增益接收的系统。

当前主要通过收发天线物理隔离、控制发射/接收增益和 MTI 抑制自干扰。如果直泄已经使 ADC 饱和或削顶，MTI 无法恢复信号，需要额外的环行器、隔离器、模拟自干扰消除或数字自干扰消除。

MTI 还会抑制接近零速度的真实目标。如果研究静止目标，应缩窄 MTI 阻带、关闭 MTI，或实现更有针对性的直泄消除算法。

## 15. 设备选型建议

### 低成本入门

- 2 台 B210；
- 2 台具备 USB 3.0 的高性能 PC；
- 3 根天线；
- 2 个稳定 10 MHz OCXO/GPSDO；
- 先从 20–50 MHz 带宽开始。

适合通信、单通道单站、双站和 OTA 同步入门。

### 稳定科研平台

- BS：X310 + 两通道宽带射频子板；
- UE：B210 或 X310；
- BS 主机：高主频 8 核以上 CPU、10GbE 网卡；
- 独立 TX/RX 天线和足够物理隔离；
- 50 MHz 起步，根据负载逐步提升到 100 MHz。

这是最接近论文验证系统、风险相对最低的组合。

### 多通道与阵列研究

- X310、多台同步 X310、N310 或 X410；
- ULA 阵列；
- 公共参考/PPS 或严格的多设备时钟同步；
- 通道幅相、群时延和阵列位置校准；
- 10/100GbE 和更强的主机处理能力。

需要注意：当前仓库已经提供多通道处理框架，但多设备相干、阵列标定和高带宽实时处理仍需要较多系统工程工作。

## 16. 总体评价

OpenISAC 最大的价值不是标准兼容性，而是把 OFDM-ISAC 算法快速从数学模型带到真实无线硬件：

- 物理层代码相对紧凑，便于修改；
- 通信、同步和感知共享同一资源网格，实验链路完整；
- 连续 OFDM 适合时延-多普勒和微多普勒处理；
- OTA 同步解决了低成本双站实验中最关键的独立时钟问题；
- 仿真后端与 USRP 后端共享处理链，适合先仿真再上机；
- 当前版本已经具备从单通道验证向上下行、多通道和阵列扩展的基础。

主要代价是主机实时负载、USRP 接口吞吐、收发自干扰和精密校准。对于追求波形灵活性、可读代码和快速 OTA 验证的科研团队，它是一套很有价值的基础平台；对于需要标准终端互通或完整网络协议栈的项目，应选择 OpenAirInterface、srsRAN、OpenWiFi 等更接近标准系统的方案。

## 17. Python跨平台算法模型进展

为降低直接改造原C++实时程序的风险，当前已经建立独立的纯Python MIMO-OFDM算法验证模型。该模型可在Windows和Linux运行，默认OFDM参数为1024点FFT、128点CP和15 kHz子载波间隔，支持QPSK、16-QAM、64-QAM、256-QAM、CRC-16、ZC前导、定时/CFO同步、多径、导频CSI估计、STBC、SFBC，以及2×2/4×4/8×8空间复用ZF/MMSE检测。

本阶段已完成2×2空间复用SFO闭环：Tx0在两个数据OFDM符号上发送重复的分布式相位参考，接收端拟合相位斜率估计每帧SFO；相干性达到门限后，以估计值而非仿真真值计算逆采样率，对完整接收流重采样，再重新执行ZC定时、CP粗CFO、FFT、残余相位校正、CSI估计和MMSE检测。

正式测试采用1024/128、2×2、64-QAM、MMSE、Rayleigh、500 ppm和200帧。40 dB时，仅做相位斜率校正的BER为0.11721、EVM为0.33313且CRC goodput为0；加入闭环重采样后，BER降为0.01439、EVM降为0.11999，CRC goodput恢复到54.065 Mb/s，初始SFO估计为497.21 ppm，平均绝对残余约70.90 ppm。

正式帧结构已经冻结为“1个ZC前导+2个空间复用数据符号”：FFT=1024、CP=128、15 kHz子载波间隔、672个数据子载波、216个FDM信道导频、8个重复相位参考；每帧携带2014字节载荷和2字节CRC-16。完整定义位于`OpenISAC-main/python_phy/docs/FORMAL_FRAME_PROFILE_2X2_SM_1024.md`。

下一阶段的符号内连续Doppler/ICI模型也已完成。新模型对每个活动MIMO路径逐采样旋转相位，并保持跨OFDM符号的绝对相位连续。正式1024/128、2×2、64-QAM、40 dB、200帧测试中，1000 Hz连续Doppler使BER达到0.020149、EVM达到0.16004、CRC goodput降至8.951 Mb/s；只在符号边界更新的旧模型则给出BER 0.001923和62.658 Mb/s goodput，证明旧模型会明显低估ICI。连续信道卷积属于离线测试，不进入实时接收机负载。

结合实际`|SFO|<=20 ppm`、移动速度1–2 m/s的新边界，正式接收子集进一步收敛为一次ZC搜索、一次CP-CFO、单遍固定1024 FFT、8参考音频域相位斜率校正、线性CSI和2×2闭式MMSE。20 ppm、40 dB、200帧时，无重采样方案BER为0.001132、EVM为0.04067、goodput为62.658 Mb/s；sinc8闭环为0.001116、0.03892和63.016 Mb/s。取消时域重采样只损失约0.6% goodput，却省掉约248 Mtap-MAC/s和第二遍FFT。与100 Hz逐采样连续Doppler组合后goodput仍为61.584 Mb/s，故第一版也不加入ICI抵消。当前全部103项pytest测试和96项显式检查通过，实时复杂度约束见`OpenISAC-main/python_phy/docs/CPP_REALTIME_FEASIBILITY.md`。

LDPC状态需要区分两条链路：原OpenISAC C++已经接入LDPC(1008,504)，默认码率1/2、6次迭代、水平分层NMS，包含自定义SIMD编码器、AFF3CT浮点译码、可选int16定点译码、交织/扰码、软解调和多线程译码队列；Python现已完成同矩阵编码、6次分层min-sum、软LLR、扰码、21×48交织、marker和BCH mini-header黄金链。工程实现应复用原C++ LDPC，不另写复杂译码器；正式OFDM配置仍是CRC-only，下一步加入混合QPSK控制区与LDPC高阶QAM载荷映射。

实现、配置与结果位置：

- `OpenISAC-main/python_phy/openisac_phy/simulation.py`：单遍工程接收主链及可选压力测试重采样；
- `OpenISAC-main/python_phy/openisac_phy/sampling_offset.py`：逐帧SFO与精确逆采样率；
- `OpenISAC-main/python_phy/configs/mimo_2x2_spatial_multiplexing_realtime_1024.yaml`：C++实时算法正式配置；
- `OpenISAC-main/python_phy/configs/mimo_2x2_spatial_multiplexing_sfo_closed_loop_1024.yaml`：500 ppm、24抽头离线压力配置；
- `OpenISAC-main/python_phy/experiments/compare_spatial_sfo_resampling.py`：对照实验；
- `OpenISAC-main/measurement/spatial_sfo_closed_loop/`：CSV、JSON和曲线图。
- `OpenISAC-main/python_phy/experiments/compare_continuous_doppler.py`：连续Doppler/ICI对照；
- `OpenISAC-main/measurement/continuous_doppler_ici/`：连续Doppler CSV、JSON和曲线图。
- `OpenISAC-main/python_phy/experiments/validate_engineering_operating_region.py`：20 ppm与0–200 Hz连续Doppler工程边界验收；
- `OpenISAC-main/measurement/engineering_operating_region/`：工程边界CSV和JSON结果。

下一阶段在正式2×2帧中加入128个Tx0单层QPSK控制RE与LDPC高阶QAM载荷，并进行低复杂度自适应rank/层数和MCS联动；ICI补偿仅在未来载频/速度使Doppler长期超过200 Hz时重新评估。

## 18. 参考资料

1. [OpenISAC GitHub 仓库](https://github.com/zhouzhiwen2000/OpenISAC)
2. [OpenISAC 中文文档](https://openisac.zzw123app.top/zh-cn/docs/)
3. [OpenISAC arXiv 论文 v2](https://arxiv.org/html/2601.03535v2)
4. [系统整体架构](https://openisac.zzw123app.top/zh-cn/docs/architecture/system/)
5. [OFDM 资源设计](https://openisac.zzw123app.top/zh-cn/docs/signal-processing/ofdm-resources/)
6. [初始同步](https://openisac.zzw123app.top/zh-cn/docs/signal-processing/initial-synchronization/)
7. [多通道单站感知](https://openisac.zzw123app.top/zh-cn/docs/signal-processing/monostatic-sensing/)
8. [UE 双站感知](https://openisac.zzw123app.top/zh-cn/docs/signal-processing/bistatic-sensing/)
9. [信道仿真器](https://openisac.zzw123app.top/zh-cn/docs/tools-workflows/channel-simulator/)
10. [硬件准备](https://openisac.zzw123app.top/docs/getting-started/hardware/)
11. [软件安装](https://openisac.zzw123app.top/docs/getting-started/installation/)
12. [编译说明](https://openisac.zzw123app.top/docs/getting-started/build/)
13. [首次 OTA 运行](https://openisac.zzw123app.top/docs/getting-started/first-ota-run/)
14. [Ettus USRP B210](https://www.ettus.com/all-products/ub210-kit/)
15. [Ettus X300/X310 Knowledge Base](https://kb.ettus.com/X300/X310)
16. [Ettus X410 Knowledge Base](https://kb.ettus.com/X410)
