# OpenISAC 高阶调制与 4×4/8×8 MIMO 扩展设计

> 目标平台：[zhouzhiwen2000/OpenISAC](https://github.com/zhouzhiwen2000/OpenISAC)
> 设计日期：2026-08-21
> 目标：在保持现有 SISO、单站/双站感知和 v1 空口兼容性的基础上，增加 16QAM、64QAM、256QAM，以及 2×2、4×4、8×8 MIMO 空间复用、波束赋形和 MIMO 感知能力。

## 1. 当前基线与需要解决的问题

当前 OpenISAC 通信物理层的主要基线为：

- payload 固定使用 QPSK；
- LDPC 默认采用 `(N,K)=(1008,504)`、码率 1/2；
- 通信接收模型以单发单收为主；
- BS 已经支持多通道单站感知 RX，但这不等于通信 MIMO；
- 当前 mini-header 为 64 bit，包含版本、flags、payload 长度、LDPC 块数、序号和 CRC16-CCITT；
- mini-header 还使用 BCH(127,64)，可纠正最多 10 bit 错误；
- 当前 CRC16 只保护 mini-header，没有覆盖整个用户 payload；
- 当前通信性能测量通过可重构测试 payload 做逐 bit 精确比较，从而计算 BER/BLER。

若直接把 QPSK 改成 64QAM，或者简单增加 USRP 通道数，会出现以下问题：

1. 接收端不知道当前调制阶数和空间层数；
2. 原有帧头容量无法容纳完整 MCS、rank、导频和预编码参数；
3. 没有 payload CRC，普通业务包无法可靠判定译码后的误块；
4. 现有导频不足以估计 `Nr × Nt` MIMO 信道矩阵；
5. 现有标量均衡器无法完成矩阵检测；
6. 当前仿真器通信路径为单天线，不能产生空间相关 MIMO 信道；
7. 8×8 IQ 数据吞吐和矩阵计算量会超过普通 CPU/10GbE 的实时能力；
8. 多台 USRP 若只有频率同步而没有相位相干，不能稳定进行空间复用和虚拟阵列感知。

因此应按照“帧格式、MCS、MIMO PHY、仿真器、实时实现、硬件同步”六个方面整体升级。

## 2. 目标能力

### 2.1 调制与编码

- BPSK：可选，用于极低 SNR 或控制信道；
- QPSK：保留现有模式；
- 16QAM；
- 64QAM；
- 256QAM；
- Gray 映射；
- 平均符号功率归一化；
- float 和 int16 定点软 LLR；
- 可配置 LDPC 码率；
- payload/TB CRC；
- 基于 SNR、BLER 或 CQI 的 MCS 自适应。

### 2.2 MIMO 模式

- SISO：`1×1`；
- SIMO/MISO：`1×N`、`N×1`；
- 空间复用：2×2、4×4、8×8；
- 可配置空间层数 `L ≤ min(Nt,Nr)`；
- 开环空间复用；
- Rank-1/多层波束赋形；
- 后续支持 SVD 预编码和 PMI/RI/CQI 反馈；
- TDD/FDD；
- MIMO 通信和 MIMO 感知共享统一资源网格。

### 2.3 接收算法

- ZF 检测；
- MMSE 检测，作为默认方案；
- QR 分解检测；
- 后续可选 SIC、K-best 或球形译码；
- 每子载波 MIMO 信道估计；
- 导频时频插值；
- 各层 EVM、SINR、BER、BLER、吞吐和信道条件数统计。

## 3. 总体架构

### 3.1 发射处理

```text
UDP/TB 数据
  → TB CRC24/CRC32
  → LDPC 分段、编码和 rate matching
  → 按 MCS 映射为 QPSK/16QAM/64QAM/256QAM
  → Layer Mapper，映射到 L 个空间层
  → 预编码 X[k,m] = W[k,m]S[k,m]
  → 为 Nt 个发射端口构建独立 OFDM 资源网格
  → Nt 路 IFFT + CP
  → timed multi-channel USRP TX
```

### 3.2 接收处理

```text
Nr 路 timed USRP RX
  → 帧同步、CFO/SFO 补偿
  → Nr 路 FFT
  → 根据正交参考信号估计 H[k,m] ∈ C^(Nr×Nt)
  → 导频插值与通道相位校准
  → ZF/MMSE/QR MIMO 检测
  → L 层软 QAM LLR
  → 去层映射、rate recovery、LDPC 解码
  → TB CRC 检查
  → UDP 输出、BER/BLER/EVM/吞吐统计
```

每个子载波的基本模型为：

```text
y[k,m] = H[k,m] W[k,m] s[k,m] + n[k,m]
```

其中：

- `y ∈ C^Nr`：Nr 个接收通道；
- `H ∈ C^(Nr×Nt)`：MIMO 信道矩阵；
- `W ∈ C^(Nt×L)`：预编码矩阵；
- `s ∈ C^L`：L 个空间层；
- `n`：噪声和剩余干扰。

默认 MMSE 检测器为：

```text
ŝ = (HᴴH + σ²I)^(-1) Hᴴy
```

实现中不应显式求逆，应使用 Cholesky、QR 或批量线性方程求解，以提高数值稳定性和运行效率。

## 4. 高阶调制设计

### 4.1 统一调制接口

在 `include/OFDMCore.hpp` 中将当前 QPSK 专用逻辑抽象为：

```cpp
enum class Modulation : uint8_t {
    BPSK   = 1,
    QPSK   = 2,
    QAM16  = 4,
    QAM64  = 6,
    QAM256 = 8,
};

class ConstellationMapper {
public:
    void map_bits(const uint8_t* bits,
                  size_t bit_count,
                  Modulation modulation,
                  std::complex<float>* symbols);
};

class SoftDemapper {
public:
    void demap_llr(const std::complex<float>* symbols,
                   const float* post_eq_noise,
                   size_t symbol_count,
                   Modulation modulation,
                   float* llr_out);
};
```

要求：

- 使用 Gray 映射；
- 星座平均功率归一化为 1；
- 支持标量参考实现；
- 支持 AVX2/AVX-512 SIMD；
- 支持 CUDA kernel；
- 支持 float 和 Q15/Q16 LLR；
- CPU、CUDA 的 bit 顺序和星座定义完全一致。

### 4.2 星座归一化

建议采用：

| 调制 | bit/symbol | 归一化系数 |
| --- | ---: | ---: |
| BPSK | 1 | `1` |
| QPSK | 2 | `1/√2` |
| 16QAM | 4 | `1/√10` |
| 64QAM | 6 | `1/√42` |
| 256QAM | 8 | `1/√170` |

### 4.3 软解调

第一阶段使用 Max-Log LLR：

```text
LLR(b_i) ≈ [min_{x∈S_i1}|z-x|² - min_{x∈S_i0}|z-x|²] / σ²
```

其中 `z` 是均衡或 MIMO 检测后的层符号。对 16/64/256QAM 应利用 I/Q 轴可分离的 PAM 结构实现，避免遍历完整星座。

MIMO 检测后应为每一层计算 post-equalization SINR 或等效噪声方差，不能让所有层共用同一个噪声值，否则高阶调制 LLR 会严重失配。

## 5. MCS 与编码设计

### 5.1 MCS 表

建议提供可配置 MCS 表：

| MCS | 调制 | Qm | 目标码率 | 典型用途 |
| ---: | --- | ---: | ---: | --- |
| 0 | BPSK | 1 | 1/2 | 控制、极低 SNR |
| 1 | QPSK | 2 | 1/2 | 当前兼容模式 |
| 2 | QPSK | 2 | 3/4 | 中低 SNR |
| 3 | 16QAM | 4 | 1/2 | 中等 SNR |
| 4 | 16QAM | 4 | 3/4 | 中高 SNR |
| 5 | 64QAM | 6 | 2/3 | 高 SNR |
| 6 | 64QAM | 6 | 3/4 | 高 SNR |
| 7 | 256QAM | 8 | 3/4 | 很高 SNR |
| 8 | 256QAM | 8 | 5/6 | 实验性最高吞吐 |

### 5.2 LDPC rate matching

当前固定 LDPC(1008,504) 只能提供 1/2 码率。升级方案：

1. 第一阶段保持同一校验矩阵，通过 puncturing/repetition 支持有限码率；
2. 第二阶段增加多组 LDPC 校验矩阵；
3. 长期可采用类似 NR 的 mother code + circular buffer rate matching；
4. 每个 code block 保留独立译码状态；
5. 帧头携带 `mcs_id`、`rv` 和 code block 数量。

## 6. CRC 与 BLER 设计

### 6.1 当前机制

当前 mini-header 为：

```text
version(4)
flags(4)
payload_len(16)
payload_blocks(8)
sequence(16)
CRC16-CCITT(16)
= 64 bit
```

该 64 bit header 再经过 BCH(127,64) 编码，header CRC 只保护帧头。普通 payload 当前没有独立 TB CRC。

LDPC 译码器虽然使用校验矩阵 syndrome 作为迭代停止条件，但当前解码接口只返回 hard bits，没有向上层返回可靠的“该 code block 校验成功/失败”状态，因此不应仅凭 LDPC 解码完成来计算业务 BLER。

### 6.2 新方案

为每个 Transport Block 增加：

- 默认 CRC24A；或
- 为实现方便使用 CRC32C。

处理顺序：

```text
payload → append TB CRC → segmentation → LDPC encode
LDPC decode → reassembly → check TB CRC → accept/drop
```

建议定义：

```text
TB_BLER = CRC失败TB数 / 已发送TB数
Packet Loss = 未收到或CRC失败包数 / 期望包数
Post-LDPC BER = 错误bit数 / 比较bit数
```

仿真测量仍保留已知 payload 的逐 bit 精确比较，用于发现 CRC 漏检和统计真实 BER。

## 7. v2 帧头设计

现有 64 bit mini-header 无法完整携带 MIMO/MCS 参数。建议保留 v1，并增加 v2 控制头。

### 7.1 控制区保持鲁棒调制

无论 payload 使用何种调制和 MIMO rank，以下资源固定使用 QPSK 或 BPSK：

- packet marker；
- v2 PHY header；
- ACK/HARQ/eRTM 控制信息。

这样接收端可以先解控制头，再选择 payload 的 MCS 和 MIMO 检测参数。

### 7.2 v2 header 字段建议

| 字段 | bit | 说明 |
| --- | ---: | --- |
| version | 4 | 帧格式版本 |
| frame_type | 4 | 数据、反馈、eRTM、测量等 |
| sequence | 16 | TB/packet 序号 |
| payload_len | 20 | payload 字节数 |
| mcs_id | 6 | 调制和码率 |
| layers | 4 | 空间层数 1–8 |
| pilot_pattern_id | 6 | MIMO 导频模式 |
| precoder_id | 8 | 预编码矩阵/码本索引 |
| rv | 2 | 重传版本 |
| harq_process | 4 | HARQ 进程号 |
| resource_map_id | 6 | 资源网格版本 |
| reserved | 20 | 后续扩展 |
| CRC24 | 24 | header CRC |

逻辑头约 124 bit，可填充为 128 bit。

为复用当前 BCH 实现，可以使用两个 BCH(127,64) codeword，将 128 bit 头编码成约 256 bit，再映射为 128 个 QPSK 控制符号。

## 8. MIMO 导频与信道估计

### 8.1 为什么必须正交

对于 `Nt` 个同时发射的天线，接收端需要估计：

```text
H[k,m] = [h_rt]，维度 Nr × Nt
```

如果所有发射天线在同一资源上发送相同导频，接收端只能观察各信道叠加，不能分离每个 `h_rt`。

### 8.2 推荐导频结构

采用两级参考信号：

1. **MIMO Channel Reference Symbol**：每帧或每若干帧插入；
2. **Comb Tracking Pilot**：数据符号内跟踪 CFO、SFO 和相位变化。

第一阶段建议使用最稳健的 TDM 正交方式：

- 4×4：连续 4 个全带宽参考符号，每个符号只激活一个 TX port；
- 8×8：连续 8 个全带宽参考符号；
- 其他 TX 保持静默；
- 每个 RX 可以直接估计对应 TX→RX 信道。

优点是实现简单、估计可靠；缺点是 8×8 导频开销较大。

第二阶段可增加：

- FDM 正交梳状导频；
- Walsh/OVSF CDM 导频；
- 不同 ZC root 或 cyclic shift；
- 相干时间较长时降低全带宽 MIMO 参考的发送频率。

### 8.3 同步结构

推荐：

- TX port 0 发送主 ZC，建立公共帧同步；
- 其他 TX port 在主同步符号保持静默，避免相关峰叠加；
- MIMO channel reference 再分别估计其他 TX port；
- 需要发射分集时，可以为不同端口使用可分离的 ZC root/cyclic shift。

## 9. MIMO 检测器

### 9.1 ZF

```text
W_ZF = (HᴴH)^(-1)Hᴴ
```

优点：简单。缺点：信道接近奇异时会强烈放大噪声。

### 9.2 MMSE

```text
W_MMSE = (HᴴH + σ²I)^(-1)Hᴴ
```

建议作为默认检测器，尤其适合 16QAM 以上调制。

### 9.3 后续增强

- MMSE-SIC；
- QRD-M；
- K-best；
- Sphere decoding；
- 基于神经网络的 MIMO detector，作为实验接口而非默认实现。

## 10. ChannelSimulator MIMO 扩展

当前仿真器的通信路径需要从单输入单输出升级为多输入多输出。

### 10.1 共享内存流

建议将流命名扩展为：

```text
dl.tx.0 ... dl.tx.Nt-1
rx.comm.0 ... rx.comm.Nr-1
ul.tx.0 ... ul.tx.NtUL-1
rx.ul.0 ... rx.ul.NrUL-1
rx.sensing.0 ... rx.sensing.NrSens-1
```

所有流继续共享同一个绝对样本时间轴。

### 10.2 MIMO 路径模型

每条传播路径 `p` 使用：

```text
H_p(t,τ) = α_p · a_rx(θAoA,p) · a_txᴴ(θAoD,p)
           · δ(τ-τp) · exp(j2πfD,p t)
```

接收通道为：

```text
y_r[n] = Σ_t Σ_p α_p a_rx,r(θAoA,p) a_tx,t*(θAoD,p)
         x_t[n-d_p] exp(j2πfD,p n/fs) + w_r[n]
```

### 10.3 信道模型类型

新增：

- `identity`：单位矩阵，用于基础正确性测试；
- `static_matrix`：用户配置固定复矩阵；
- `tdl`：抽头延迟线；
- `rayleigh`：瑞利衰落；
- `rician`：带 K 因子的莱斯信道；
- `spatial_cluster`：AoA/AoD 簇状空间信道；
- `kronecker`：可配置 Tx/Rx 空间相关矩阵；
- 后续支持 3GPP TDL-A/B/C/D/E 和 CDL-A/B/C/D/E。

### 10.4 配置示例

```yaml
mimo:
  enabled: true
  tx_ports: 4
  rx_ports: 4
  layers: 4
  detector: mmse
  precoding: identity
  pilot_pattern: tdm_fullband

simulation:
  mimo_channel:
    type: rician
    rician_k_db: 8
    normalize_power: true
    tx_correlation: 0.3
    rx_correlation: 0.3
    paths:
      - {delay_samples: 0, gain_db: 0,  doppler_hz: 0,  aoa_deg: 10,  aod_deg: -5}
      - {delay_samples: 7, gain_db: -8, doppler_hz: 30, aoa_deg: -25, aod_deg: 20}
```

### 10.5 仿真效率

4×4/8×8 不宜逐样点使用四重 C++ 循环。建议：

- 短 TDL 使用 SIMD 时域 FIR；
- 长时延扩展使用 overlap-save FFT 卷积；
- 矩阵混合使用 BLAS/cuBLAS；
- 同一个 path 的 Doppler 相位递推，避免逐样点调用三角函数；
- 支持固定随机种子，保证 BER/BLER 实验可复现；
- `pacing_enabled:false` 用于 Monte Carlo 批量仿真。

## 11. MIMO 感知设计

多接收通道只能形成 SIMO 感知。要形成真正的 MIMO 雷达虚拟阵列，还必须区分来自不同 TX port 的回波。

### 11.1 第一阶段：TDM-MIMO sensing

- 每个感知参考符号只激活一个 TX port；
- RX 根据符号位置分离不同 TX；
- 形成 `Nt × Nr` 虚拟通道；
- 4×4 最多形成 16 个虚拟通道；
- 8×8 最多形成 64 个虚拟通道；
- 进行距离-多普勒-角度处理。

### 11.2 第二阶段：FDM/CDM MIMO sensing

- 不同 TX 使用不同子载波集合或正交码；
- 提高时间更新率；
- 需要做缺失子载波插值、稀疏时延处理或联合重构；
- 必须补偿 TX port 之间的固定幅相和群时延。

### 11.3 数据辅助感知

BS 已知所有发射层的数据，但单个资源上 `Nr` 个观测不足以直接分离 `Nt×Nr` 个感知通道。应使用跨符号 LS：

```text
Y = HX + N
Ĥ = YXᴴ(XXᴴ + εI)^(-1)
```

要求处理窗口内信道近似不变且 `X` 满秩。第一阶段仍建议使用正交感知参考，以降低实现和验证风险。

## 12. 配置结构

建议在 BS/UE YAML 中增加：

```yaml
phy:
  frame_version: 2
  default_mcs: 3
  adaptive_mcs: false
  payload_crc: crc24a

mimo:
  enabled: true
  tx_channels: [0, 1, 2, 3]
  rx_channels: [0, 1, 2, 3]
  layers: 4
  detector: mmse
  precoder: identity
  pilot_pattern: tdm_fullband
  reference_period_frames: 1
  channel_interpolation: linear
  phase_calibration_file: calibration/mimo_phase_4x4.bin
  delay_calibration_file: calibration/mimo_delay_4x4.bin

mcs_table:
  - {id: 1, modulation: qpsk,   code_rate: 0.50}
  - {id: 3, modulation: qam16,  code_rate: 0.50}
  - {id: 5, modulation: qam64,  code_rate: 0.67}
  - {id: 7, modulation: qam256, code_rate: 0.75}
```

当 `mimo.enabled:false`、`frame_version:1` 时，必须保持当前 SISO/QPSK 行为。

## 13. 代码改造位置

| 文件/模块 | 主要改造 |
| --- | --- |
| `include/OFDMCore.hpp` | 通用 QAM mapper/demapper、v2 header、MCS、layer mapper、MIMO channel estimator/detector |
| `include/Common.hpp` | MIMO/MCS/CRC/多通道 TX-RX 配置结构和 YAML 解析 |
| `src/BS.cpp` | Nt 路下行资源网格、预编码、多通道 timed TX、MIMO 感知参考 |
| `src/UE.cpp` | Nr 路 RX、矩阵信道估计、MIMO 检测、TB CRC、分层 BER/BLER/EVM |
| `include/UplinkTxEngine.hpp` | 多端口上行 layer mapping 和 timed TX |
| `include/UplinkRxEngine.hpp` | 多通道上行 RX、MIMO 信道估计和检测 |
| `src/ChannelSimulator.cpp` | 多输入多输出共享内存流和 MIMO channel matrix |
| `src/SimBackend.cpp` | 多通道仿真 stream 管理 |
| `src/UhdBackend.cpp` | 多通道 timed TX/RX、跨设备同步和 metadata 对齐 |
| CUDA 模块 | batched FFT、MIMO Gram matrix、Cholesky/QR、QAM LLR、LDPC batch |
| Python 前端 | 每层星座、EVM/SINR、信道矩阵、奇异值、rank、吞吐和 BLER 显示 |

## 14. 实时性能设计

### 14.1 IQ 接口吞吐

以 `sc16`、50 MS/s 计算，每个复采样为 4 byte：

```text
4 通道单方向：50M × 4 × 4B = 800 MB/s ≈ 6.4 Gbit/s
8 通道单方向：50M × 8 × 4B = 1.6 GB/s ≈ 12.8 Gbit/s
```

全双工需要同时计算 TX 和 RX 流量。协议开销、缓存复制和控制流尚未计入，因此：

- 4×4、50 MS/s 对单 10GbE 已接近边界；
- 8×8、50 MS/s 不适合单 10GbE；
- 应使用多路 10/25/100GbE 或 PCIe；
- 可使用 `sc8` 降低一半线速，但需评估量化 EVM；
- 应使用 zero-copy、pinned memory 和批处理。

### 14.2 计算平台

建议：

- 2×2、20–50 MHz：高主频 CPU 可完成；
- 4×4、20–50 MHz：AVX-512 CPU 可作为基线，推荐 GPU；
- 4×4、100 MHz：GPU；
- 8×8、50–100 MHz：GPU + 100GbE/PCIe，必要时将部分 FFT/DDC/DUC 下沉 FPGA；
- 8×8 高阶调制：必须使用 batched cuFFT、cuBLAS/cuSOLVER 或专用 CUDA kernel。

### 14.3 内存布局

建议统一使用：

```text
[frame][symbol][subcarrier][rx_or_tx_port]
```

GPU 内部可转为 SoA：

```text
H[subcarrier][rx][tx]
Y[subcarrier][rx]
S[subcarrier][layer]
```

避免每个子载波单独分配矩阵对象。

## 15. 硬件方案

### 15.1 4×4

推荐：

- 单台 USRP X410 或 N310；或
- 多台 X310，配合统一 10 MHz、PPS、确定性 timed start 和相位校准；
- 10/25/100GbE 或 PCIe；
- GPU 主机；
- 对通信空间复用，所有 TX/RX 必须保持频率、时间和相位相干。

### 15.2 8×8

推荐：

- 两台 4×4 设备组合；或支持 8 通道的高端 SDR；
- 公共低相噪 10 MHz；
- 公共 PPS/trigger；
- 可选共享 LO，以提高跨设备长期相位相干；
- 设备间固定时延、幅度和相位校准；
- 100GbE 或 PCIe；
- Nvidia GPU。

B210 可以构造小规模多设备实验，但 USB 总线吞吐、设备间相位一致性和同步复杂度使其不适合作为 8×8 高带宽实时平台。

## 16. 校准要求

必须分别校准：

- TX 通道固定复增益；
- RX 通道固定复增益；
- 每条 TX/RX RF 链的群时延；
- 多设备 sample time offset；
- 多设备 carrier phase offset；
- 天线馈线和射频开关；
- 阵列天线物理位置；
- 空口互耦和阵列流形。

通信空间复用只进行一次静态校准通常不够。跨设备相位会随温度和本振漂移变化，应周期性发送 calibration reference 或使用共享 LO。

## 17. 验证方案

### 17.1 高阶调制单元测试

- 所有星座点 bit→symbol→bit 无噪声零错误；
- 星座平均功率为 1；
- CPU/CUDA/定点 LLR 符号一致；
- AWGN 下 BER 与理论曲线一致；
- 不同 MCS 的容量计算和 padding 正确；
- TB CRC 能检测随机 bit error。

### 17.2 MIMO 数字测试

1. `H=I`，无噪声：所有层零 BER；
2. 对角 H：验证每层增益和噪声方差；
3. 固定满秩复矩阵：验证 ZF/MMSE；
4. 近奇异矩阵：验证 MMSE 稳定性；
5. Rayleigh/Rician Monte Carlo：生成 BLER-SNR 曲线；
6. 频率选择性 MIMO：验证导频插值；
7. CFO/SFO + MIMO：验证公共和通道相位跟踪；
8. rank 1/2/4/8 切换：验证 header、layer mapper 和吞吐。

### 17.3 OTA 测试

按以下顺序进行：

1. 同一设备内部 RF loopback；
2. 多通道线缆 + 衰减器；
3. 固定 MIMO 信道模拟器；
4. 暗室 LOS；
5. 室内多径；
6. 真实移动目标和 MIMO 感知。

### 17.4 输出指标

- 同步成功率；
- header CRC failure rate；
- TB CRC failure rate；
- uncoded BER；
- post-LDPC BER；
- BLER；
- packet loss；
- 每层 EVM/SINR；
- channel-estimation NMSE；
- `H` 的 singular values 和 condition number；
- rank、MCS 和有效吞吐；
- RX queue drops；
- 每阶段处理延迟和实时负载。

## 18. 分阶段实施路线

### 阶段 A：SISO 高阶调制

- 通用 BPSK/QPSK/16/64/256QAM；
- Max-Log LLR；
- MCS 表；
- payload/TB CRC；
- SISO AWGN BER/BLER 测试；
- 保持 v1 QPSK 兼容。

验收：SISO 各 MCS 可在 ChannelSimulator 中输出正确 BER/BLER 曲线。

### 阶段 B：2×2 MIMO 基线

- v2 header；
- 双通道 TX/RX；
- TDM MIMO reference；
- 2×2 LS channel estimation；
- ZF/MMSE detector；
- identity/static_matrix 模型。

验收：无噪声零误码，AWGN/固定矩阵结果正确。

### 阶段 C：4×4 MIMO

- 4 通道 timed UHD；
- 4 层 mapper；
- CPU SIMD 和 CUDA detector；
- Rayleigh/Rician/TDL MIMO 模型；
- 分层 EVM/SINR/BLER；
- 4×4 TDM-MIMO sensing。

验收：4×4、16QAM/64QAM 实时运行，无持续 queue drop。

### 阶段 D：8×8 MIMO

- 多设备同步；
- 8 通道共享时钟/LO/触发；
- 100GbE/PCIe 数据路径；
- batched GPU FFT、MMSE/QR、QAM LLR 和 LDPC；
- 8×8 MIMO channel simulator；
- 64 虚拟通道 MIMO sensing。

验收：8×8 指定带宽下稳定运行，达到规定 BLER、吞吐和实时性指标。

### 阶段 E：自适应和高级算法

- CQI/MCS 自适应；
- RI/PMI feedback；
- SVD/码本预编码；
- MMSE-SIC/K-best；
- HARQ soft combining；
- 3GPP TDL/CDL；
- 联合通信感知波束设计。

## 19. 推荐的最小可行版本

不建议第一版直接做 8×8 + 256QAM。推荐 MVP：

```text
调制：QPSK、16QAM、64QAM
编码：保留 LDPC(1008,504)
CRC：TB CRC32C
MIMO：2×2，然后扩展 4×4
导频：TDM 全带宽正交参考
检测：ZF + MMSE
仿真信道：identity、static_matrix、Rayleigh、Rician
硬件：单台 4 通道 USRP
处理：CPU 参考实现 + CUDA batched MMSE
```

在该版本完成帧格式、数据布局、CRC、MCS、导频和矩阵处理接口后，8×8 主要是性能、同步和硬件规模扩展，而不需要再次重写整个 PHY。

## 20. 结论

OpenISAC 可以扩展到高阶调制和 4×4/8×8 MIMO，但核心不是简单增加 USRP 通道，而是同时升级：

1. 可配置 QAM mapper/soft demapper；
2. MCS 和 LDPC rate matching；
3. payload/TB CRC 与明确的 BLER 定义；
4. 可扩展的 v2 鲁棒控制头；
5. 正交 MIMO 导频和 `Nr×Nt` 信道估计；
6. ZF/MMSE/QR MIMO 检测；
7. MIMO ChannelSimulator；
8. 多通道 timed UHD 数据路径；
9. GPU 批量处理；
10. 多设备时频相位同步与 RF 校准。

最合理的实施顺序是“SISO 高阶调制 → 2×2 → 4×4 → GPU 化 → 8×8”，并在每一步用 TB CRC、已知 payload 精确比较和 ChannelSimulator Monte Carlo 同时验证 BER、BLER、吞吐和实时性。

## 21. 参考资料

1. [OpenISAC GitHub](https://github.com/zhouzhiwen2000/OpenISAC)
2. [OpenISAC 系统架构](https://openisac.zzw123app.top/zh-cn/docs/architecture/system/)
3. [OpenISAC OFDM 资源设计](https://openisac.zzw123app.top/zh-cn/docs/signal-processing/ofdm-resources/)
4. [OpenISAC ChannelSimulator](https://openisac.zzw123app.top/zh-cn/docs/tools-workflows/channel-simulator/)
5. [OpenISAC 当前 OFDMCore.hpp](https://github.com/zhouzhiwen2000/OpenISAC/blob/main/include/OFDMCore.hpp)
6. [OpenISAC 当前 UE.cpp](https://github.com/zhouzhiwen2000/OpenISAC/blob/main/src/UE.cpp)
7. [USRP X410](https://kb.ettus.com/X410)
8. [USRP N310](https://www.ettus.com/all-products/usrp-n310/)

## 22. Python模型当前实施进度（2026-08-21）

本阶段按“先算法仿真、后C++，先STBC链路、再空间复用，LDPC暂缓”的路线推进。当前已形成Windows/Linux共用的Python模型，完成内容如下：

- QPSK、16-QAM、64-QAM、256-QAM Gray映射与硬/软解调；
- CRC-16/CCITT-FALSE、BER、BLER、CRC failure rate和EVM；
- 可配置FFT、CP、子载波间隔、保护带、DC置零和comb pilot；
- 2Tx Alamouti STBC，支持1/2/4/8根接收天线；
- AWGN、固定平坦、Rayleigh平坦和可配置TDL多径信道；
- 星座图、Tx/Rx时域波形和各Tx→Rx链路频率响应；
- 连续接收流中的采样偏移和CFO注入；
- 基于CP相关的粗定时、基于CP相关相位的粗CFO估计及校正；
- GUI和YAML均可设置同步开关、采样偏移、搜索窗及CFO。

当前粗同步流程为：

```text
OFDM/STBC时域信号
  → MIMO/TDL信道与AWGN
  → 串行接收流
  → 插入采样偏移和连续CFO
  → 搜索CP与相隔N点样本的归一化相关峰
  → 从相关相位估计 CFO/Δf
  → CFO校正并截取两个Alamouti OFDM symbol
  → FFT和完美CSI Alamouti合并
  → QAM判决、CRC、BER/BLER/EVM
```

TDL信道会污染CP开头的若干样本，因此相关计算跳过“配置的最大信道时延”个CP样本；同步开启时要求最大时延严格小于CP。该实现目前使用配置的时延上界，后续可改为接收机固定设计裕量或由前导序列估计得到的时延扩展。

验证结果：

- pytest：23/23通过；
- 显式验收：34/34通过；
- 无噪声64-QAM、2×2 Alamouti、TDL、采样偏移5、CFO 1200 Hz：BER=0、CRC失败=0、定时成功率100%；
- 64-QAM、2×2 Alamouti、TDL、偏移5、CFO 500 Hz的15/25/35 dB扫描：定时成功率63.3%/97.9%/100%，35 dB BER=0。

带噪结果说明CP法已经能承担粗同步，但低中SNR下的偶发定时错误仍会造成整帧错误。下一步不是LDPC，而是利用现有comb pilot完成每个OFDM symbol的残余公共相位/CFO跟踪；随后再实现Tx正交导频与LS/LMMSE MIMO信道估计。

## 23. Comb pilot残余相位跟踪实施结果（2026-08-21）

CP粗同步之后已加入第二级同步：利用comb pilot估计两个Alamouti OFDM时隙各自的公共相位误差（CPE），在STBC合并之前校正接收频域网格。两时隙相位差同时换算为残余CFO诊断值。

当前算法流程：

```text
CP粗定时/CFO校正
  → FFT
  → 用pilot、当前完美CSI和Tx网格预测Rx pilot
  → 对所有pilot子载波和Rx天线累加 conj(predicted) × received
  → 相关相位得到每时隙CPE
  → 归一化相关得到coherence
  → coherence达到门限才校正该时隙
  → Alamouti合并和QAM判决
```

新增参数：

- `pilot_phase_tracking_enable`：导频相位跟踪开关；
- `pilot_phase_min_coherence`：相位校正门限，GUI/YAML默认0.9；
- 输出每时隙相位、coherence、应用率和残余CFO估计。

验证结果：

- pytest：28/28通过；
- 显式验收：37/37通过；
- 256-QAM、2×2 Alamouti、TDL、25 dB、3 kHz CFO：BER从0.016529降至0.015650，EVM从0.058379降至0.056998；
- 默认64-QAM TDL同步场景的15/25/35 dB对照中，0.9门限下导频校正应用率约为76.4%/98.9%/100%。

本阶段仍使用完美CSI预测接收导频，所以尚不能称为独立的实际接收机信道估计。下一阶段应重新设计2Tx正交导频，首先实现LS信道估计及NMSE测试，再加入频域插值和LMMSE；LDPC继续暂缓。

## 24. 2Tx正交导频与LS信道估计结果（2026-08-21）

已新增不读取真实信道的`ls_linear`接收模式。每个pilot子载波上的已知符号对通过Alamouti矩阵映射到两根Tx天线和两个OFDM时隙。对每根Rx天线，接收向量满足：

```text
[y0, y1]^T = Xpilot · [h0, h1]^T + n
```

由于`Xpilot`为正交Alamouti矩阵，LS解可直接由`Xpilotᴴ·y`得到，同时恢复`h0`和`h1`。该操作对每个pilot子载波、每根Rx天线独立执行，得到`[frame, pilot, Rx, Tx]`信道估计；随后在FFT-shift频率顺序中分别对实部和虚部作周期线性插值，生成完整`[frame, subcarrier, Rx, Tx]`信道矩阵。

新增能力：

- `receiver.channel_estimation: perfect | ls_linear`；
- pilot位置NMSE和全活动子载波NMSE；
- GUI手动切换完美CSI/LS；
- 信道响应图同时显示LS实线、完美参考虚线和pilot采样点；
- 独立配置`configs/mimo_2x2_alamouti_tdl_ls.yaml`。

验证结果：

- pytest：34/34通过；
- 显式验收：41/41通过；
- 平坦信道、无噪声、256-QAM：BER=0、CRC失败=0、LS NMSE低于`1e-28`；
- TDL无噪声：pilot位置NMSE低于`1e-28`；
- 相同TDL下，线性插值NMSE随pilot spacing从8减小到2，由约0.34327降至0.00880；
- TDL、QPSK、pilot spacing=2的20/30/40 dB扫描均为零误码，30 dB全频域NMSE约0.0116。

限制也已经显式暴露：深多径信道中，即使pilot点估计完全正确，简单线性插值仍存在约0.0088的高SNR误差地板，高阶QAM会先受到影响。下一阶段应实现利用有限时延支撑的DFT-LS，以及基于信道相关矩阵和噪声方差的LMMSE插值，再与当前`ls_linear`进行NMSE/BER对照。当前两个时隙的观测量刚好用于求解两根Tx信道，无法再独立分离未知的逐时隙CPE，因此LS模式暂时禁止启用依赖完美CSI的pilot-CPE跟踪。

## 25. DFT-LS与LMMSE插值结果（2026-08-21）

为消除`ls_linear`在深多径信道中的插值误差地板，接收端新增两种模式：

### 25.1 DFT-LS

在pilot位置获得LS频响后，假设信道冲激响应只占用前`L`个samples，构造pilot子载波到时域tap的部分DFT矩阵：

```text
Hp = Fp,L · h + n
ĥ = pinv(Fp,L) · Hp
Ĥ = FN,L · ĥ
```

当pilot数量不少于`L`且真实最大时延位于该支撑内时，无噪声下可以精确恢复全频域信道。配置参数为`channel_estimation_taps`；当前TDL最大时延为9，因此默认`L=10`。

### 25.2 LMMSE

LMMSE使用长度`L`、单位总功率的均匀PDP作为初始统计先验，建立全子载波与pilot子载波的互相关矩阵，并用实际AWGN方差正则化：

```text
Ĥ = Rall,p · (Rp,p + σ²I)⁻¹ · Hp
```

该实现不读取当前帧的真实信道；真实信道只用于NMSE验收。未来可把均匀PDP替换为配置的TDL统计PDP或在线估计PDP。

### 25.3 同条件结果

固定64-QAM、2×2 Alamouti、TDL、FFT=64、pilot spacing=4、信道长度10，并关闭定时/CFO扰动以隔离信道估计器：

| SNR | 方法 | BER | EVM | 信道NMSE |
|---:|---|---:|---:|---:|
| 20 dB | LS linear | 0.18060 | 0.39883 | 0.09346 |
| 20 dB | DFT-LS | 0.02615 | 0.12761 | 0.00715 |
| 20 dB | LMMSE | 0.02601 | 0.12828 | 0.00710 |
| 30 dB | LS linear | 0.17310 | 0.38309 | 0.08719 |
| 30 dB | DFT-LS | 0.000045 | 0.04052 | 0.000715 |
| 30 dB | LMMSE | 0.000047 | 0.04054 | 0.000714 |

DFT-LS无噪声下实现64-QAM零误码、全频域NMSE低于`1e-28`。当前自动测试为40/40，显式验收为45/45。公平对照数据位于`measurement/channel_estimator_comparison/results.csv`。

下一阶段应解决独立LS/LMMSE模式下的残余CPE问题。当前两时隙Alamouti导频刚好提供两根Tx信道的求解自由度，没有额外观测分离逐时隙CPE；需要增加跨块公共相位参考、专用相位跟踪导频或更多OFDM时隙。之后再加入采样时钟偏差和Doppler时变信道，LDPC继续暂缓。

## 26. 独立相位参考与无真实CSI的CPE校正（2026-08-21）

为解决两个Alamouti时隙之间的相位跳变，同时避免使用真实CSI，从comb pilot集合中保留少量专用相位参考子载波。在这些子载波上，Tx0在两个时隙发送相同单位参考，Tx1保持静默：

```text
y0(k,r) = h0(k,r) · p · exp(jφ0) + n0
y1(k,r) = h0(k,r) · p · exp(jφ1) + n1
```

对所有相位参考和Rx天线累加：

```text
C = Σ conj(y0) · y1
Δφ = angle(C) = φ1 - φ0
```

未知信道相位在共轭乘积中抵消。接收端用`exp(-jΔφ)`校正第二时隙，然后使用剩余正交Alamouti pilot执行DFT-LS或LMMSE信道估计。公共的`φ0`会被信道估计自然吸收，不影响后续STBC均衡。

新增配置：

- `phase_reference_tracking_enable`；
- `phase_reference_count`，默认2；
- `slot_phase_offset_deg`，用于注入可重复的时隙相位跳变；
- reference coherence和相位校正应用率；
- GUI中绿色方框显示相位参考子载波。

验证结果：

- 无噪声、64-QAM、TDL、DFT-LS、30°相位跳变：关闭参考时BER约0.165，启用2个参考后估计相位30°、BER=0、信道NMSE低于`1e-28`；
- 20 dB、LMMSE、30°相位跳变：BER由约0.19115降至0.04070，EVM由约0.39476降至0.16060，信道NMSE由约0.11574降至0.01608；
- 参考coherence约0.98684，校正应用率100%；
- pytest为46/46，显式验收为49/49。

完整同步配置`mimo_2x2_alamouti_tdl_lmmse_phase_reference.yaml`的15/20/25/30/35 dB扫描已经保存。35 dB时相位估计约30.00°、coherence接近1、BER约`3.47e-5`。

下一阶段加入采样时钟偏差（SFO）注入。SFO在频域表现为随子载波变化的相位斜率，不能只用一个公共相位量修正；需要使用多个分布在带宽上的相位参考或pilot，对相位随子载波的线性项进行加权拟合。随后再加入Doppler和Alamouti块内信道时变。

## 27. OpenISAC兼容ZC前导与1024/128基准帧（2026-08-21）

本阶段将原OpenISAC的独立前导结构接入Python模型。频域序列与C++ `generate_zc_freq`保持同一公式：

```text
Zq[n] = exp(-j·π·q·n(n+δ)/N),  δ=N mod 2
```

默认参数为`N=1024`、`CP=128`、`q=29`、`Δf=15 kHz`，因此采样率为15.36 MHz，每个OFDM符号1152 samples。Tx0发送一个完整的ZC CP-OFDM符号，Tx1静默；其后发送两个Alamouti数据符号，完整仿真帧共3个OFDM符号、3456 samples。

接收流程为：

```text
串行Rx流
  → 在定时搜索窗内滑动相关完整“ZC有效符号+CP”
  → 每根Rx天线独立归一化相关功率后做接收分集合并
  → 选择最大相关峰作为帧起点
  → 在前导和数据符号上累加CP相关，估计粗CFO
  → CFO校正并截取完整三符号帧
  → 丢弃前导，解调两个Alamouti数据符号
```

ZC根必须非零且与FFT点数互质。GUI、YAML和命令行均开放`preamble_enable/preamble_enabled`与`zc_root`；关闭前导但保留同步时，接收机自动回退到原CP相关定时。时域图会对ZC前导区域着色并标出三个符号边界。

1024/128、64-QAM、2×2 Alamouti、TDL三径、LMMSE信道估计、定时偏移20 samples、CFO 1200 Hz的显式结果为：

| Es/N0 | 定时成功率 | BER | EVM | CFO估计均值 |
|---:|---:|---:|---:|---:|
| 20 dB | 100% | 0.01436 | 0.10315 | 约1200 Hz |
| 25 dB | 100% | 0.001281 | 0.05746 | 约1200 Hz |
| 30 dB | 100% | `8.50e-6` | 0.03274 | 约1200 Hz |

当前pytest为53/53通过，显式验收为53/53通过。时域绘图默认显示完整帧，也可通过GUI的`Display samples`或命令行`--max-samples`限制显示点数，`0`表示全部。可复现配置为`python_phy/configs/mimo_2x2_alamouti_tdl_zc_1024.yaml`，结果保存于`OpenISAC-main/measurement/zc_preamble_1024_validation/`。下一阶段按原路线加入SFO注入和跨频率pilot相位斜率估计，LDPC继续暂缓。

## 28. SFO时域注入与跨频率相位斜率跟踪（2026-08-21）

本阶段加入采样时钟偏差SFO。定义为：

```text
SFO_ppm = (F_rx - F_tx) / F_tx × 1e6
```

接收连续复采样流使用24抽头有限窗sinc插值器重采样。相比简单线性插值，该方法能同时正确保持OFDM正、负频率子载波，并为后续C++多相分数延迟滤波器提供算法参考。

接收机继续使用Tx0跨两个Alamouti时隙重复发送的独立相位参考。对每个参考子载波和全部Rx天线计算：

```text
C(k) = Σr conj(y0(k,r)) · y1(k,r)
angle(C(k)) ≈ φ0 + βk
```

对展开后的相位进行相关幅度加权最小二乘拟合，得到公共相位截距`φ0`和每子载波相位斜率`β`。第二时隙按`exp[-j(φ0+βk)]`校正。SFO诊断换算为：

```text
SFO_ppm = -β · N / [2π(N+CP)] × 1e6
```

该方法不读取真实信道，因为`conj(y0)·y1`会抵消重复Tx0参考所经历的信道相位。至少需要2个参考才能拟合斜率；带噪宽带场景建议4–8个均匀分布的参考。GUI打开SFO跟踪时会自动启用独立相位参考，并在参考数过少时设置为8。

固定1024/128、64-QAM、2×2 Alamouti、TDL三径、LMMSE、SFO=50 ppm、CFO=1200 Hz、定时偏移20 samples的结果如下：

| Es/N0 | 跟踪 | BER | EVM | 信道NMSE | SFO估计 |
|---:|:---:|---:|---:|---:|---:|
| 20 dB | 关闭 | 0.02607 | 0.12947 | 0.04069 | — |
| 20 dB | 开启 | 0.01820 | 0.11323 | 0.02505 | 46.803 ppm |
| 25 dB | 关闭 | 0.01030 | 0.09852 | 0.04003 | — |
| 25 dB | 开启 | 0.00440 | 0.07555 | 0.02339 | 49.857 ppm |
| 30 dB | 关闭 | 0.00605 | 0.08754 | 0.03938 | — |
| 30 dB | 开启 | 0.00169 | 0.05947 | 0.02279 | 50.126 ppm |

pytest为58/58通过，显式验收为57/57通过。配置位于`python_phy/configs/mimo_2x2_alamouti_tdl_sfo_1024.yaml`，开关对照脚本为`python_phy/experiments/compare_sfo_tracking.py`，数据和图位于`measurement/sfo_tracking_validation/`。

当前校正消除了两个Alamouti时隙之间的主要相位斜率，但没有把整段接收流重新采样回标称时钟，因此SFO产生的ICI仍会形成残余误差。后续C++版本可根据估计SFO驱动闭环多相重采样器。下一阶段加入Doppler和Alamouti块内时变信道，验证正交STBC在信道不恒定时的性能退化。

## 29. 符号级Doppler与Alamouti块内信道失配（2026-08-21）

本阶段加入可重复的符号级时变信道。对每条活动`Rx×Tx×tap`路径，在配置的`[-fD,max,+fD,max]`范围内分配确定性Doppler频率：

```text
h(r,t,l,m) = h(r,t,l,0) · exp(j·2π·fD(r,t,l)·m·Tsym)
Tsym = (N+CP)/Fs
```

不同Tx、Rx和多径tap使用不同正负Doppler，因此不能只靠一个公共相位旋转完全校正。当前模型在一个OFDM符号内部保持tap不变，在前导和两个Alamouti符号之间连续更新，专门隔离STBC块内信道不一致问题；符号内连续旋转和ICI将在后续细化。

新增输出包括：

- `maximum_doppler_hz`与活动路径RMS Doppler；
- 两个数据时隙真实信道`channel_by_symbol`；
- `stbc_channel_variation_nmse = ||H1-H0||²/||H0||²`；
- 两时隙信道平均相关相位；
- GUI手动设置`Maximum Doppler (Hz)`。

固定1024/128、64-QAM、2×2 Alamouti、TDL三径、LMMSE、30 dB结果为：

| 最大路径Doppler | BER | EVM | 两时隙信道变化NMSE |
|---:|---:|---:|---:|
| 0 Hz | `3.19e-6` | 0.03154 | 0 |
| 100 Hz | `1.51e-4` | 0.04112 | 0.00132 |
| 300 Hz | 0.01073 | 0.08683 | 0.01191 |
| 500 Hz | 0.03305 | 0.13972 | 0.03300 |
| 1000 Hz | 0.08174 | 0.27411 | 0.13054 |

无噪声64-QAM小FFT显式验证中，静态信道BER为0；500 Hz Doppler使信道变化NMSE约0.0407并产生1814个bit错误，证明误差来自Alamouti正交假设被破坏，而不是AWGN。

pytest为60/60通过，显式验收为60/60通过。配置位于`python_phy/configs/mimo_2x2_alamouti_tdl_doppler_1024.yaml`，扫描脚本为`python_phy/experiments/compare_doppler.py`，结果位于`measurement/doppler_validation/`。

下一阶段加入SFBC。SFBC在相邻子载波上完成Alamouti配对，可避免时间选择性信道破坏两个时隙的正交关系，但要求配对子载波之间的频域信道足够相近；将与当前STBC在Doppler和频率选择性TDL条件下做同资源对照。

## 30. 2×2空间复用同步与时钟压力测试（2026-08-21）

本阶段把已完成的ZC前导、定时/CFO同步、FDM正交导频、逐符号LS信道估计和MMSE空间检测串成完整接收链。基准参数为：

- FFT=256、CP=32、子载波间隔15 kHz；
- 左/右保护子载波16/15，DC置零，pilot spacing=4；
- 2×2满层空间复用、64-QAM、MMSE；
- 独立Rayleigh信道、35 dB、200帧；
- 1个Tx0 ZC前导和2个空间复用数据符号；
- 每个数据符号分别估计`H[t,k,rx,tx]`。

联合场景正式结果为：

| 场景 | BER | EVM | 信道NMSE | 定时成功率 | CFO平均绝对误差 | CRC goodput |
|---|---:|---:|---:|---:|---:|---:|
| 基线 | 0.005021 | 0.06845 | 0.0002283 | 100% | 2.73 Hz | 12.137 Mb/s |
| 定时12点+CFO 1 kHz，不同步 | 0.24587 | 1.17877 | 1.44197 | 0% | 1000 Hz | 0 |
| 定时12点+CFO 1 kHz，开启同步 | 0.005021 | 0.06845 | 0.0002286 | 100% | 2.73 Hz | 12.137 Mb/s |
| Doppler 500 Hz | 0.005084 | 0.06954 | 0.0002286 | 100% | 2.80 Hz | 12.583 Mb/s |
| SFO 50 ppm | 0.005074 | 0.06891 | 0.002591 | 100% | 3.69 Hz | 11.959 Mb/s |
| 定时+CFO+Doppler+SFO联合 | 0.005134 | 0.07001 | 0.002682 | 100% | 3.75 Hz | 12.494 Mb/s |

负对照证明失去帧同步后，即使导频信道估计和MMSE检测仍存在，也无法恢复正确资源位置；开启ZC同步后BER、EVM、CSI和goodput均恢复到基线。对应脚本为`stress_spatial_impairments.py`，包含5项自动判定。

未补偿SFO边界扫描如下：

| SFO | BER | 信道NMSE | CRC goodput |
|---:|---:|---:|---:|
| 0 ppm | 0.005021 | 0.000228 | 12.137 Mb/s |
| 50 ppm | 0.005074 | 0.002591 | 11.959 Mb/s |
| 100 ppm | 0.005164 | 0.009671 | 11.602 Mb/s |
| 200 ppm | 0.005595 | 0.037809 | 8.924 Mb/s |
| 500 ppm | 0.013121 | 0.226606 | 0 |
| 1000 ppm | 0.047020 | 0.793754 | 0 |
| 2000 ppm | 0.120515 | 1.89463 | 0 |

500 ppm虽然原始BER仅约1.31%，但帧较长且每帧使用CRC-16判定，200帧全部CRC失败，因此goodput已经为0。当前STBC路径已有基于跨频率相位参考的SFO斜率估计，空间复用路径尚未接入该算法；后续应为每个Tx保留重复相位参考，联合估计CPE/SFO，再用估计结果驱动闭环多相重采样器。

Doppler从0扫到10 kHz时，CSI NMSE始终约`2.3e-4`，BER在约`0.0050–0.0073`之间波动。原因是当前模型只在OFDM符号边界更新tap，符号内部保持不变，而接收机对两个空间复用符号分别进行导频信道估计。这证明逐符号CSI能够跟踪符号间变化，但模型没有产生符号内时变导致的ICI，因此10 kHz扫描不是实机高速移动上限。下一步需把路径相位旋转下沉到连续时域采样，并增加ICI与信道估计过时测试。

本阶段增加1项pytest回归和3项显式检查；全量结果为75/75 pytest、77/77显式检查通过。正式数据与曲线位于`measurement/spatial_impairment_stress/`、`measurement/spatial_impairment_sweep/`和`measurement/python_phy_explicit_validation_spatial_sync_stage/`。

## 31. SFBC频分正交导频与估计CSI（2026-08-21）

SFBC已解除“pilot必须关闭、只能使用完美CSI”的限制。新的资源与接收流程为：

```text
每个OFDM符号的comb pilot
  → pilot频率位置按Tx0/Tx1交替分配
  → 一个pilot RE只有一根Tx发送单位功率BPSK
  → 每个时隙分别得到各Tx的pilot LS观测
  → LS-linear / DFT-LS / LMMSE全带宽插值
  → 生成H[t,k,rx,tx]
  → 相邻数据子载波Alamouti SFBC合并
```

这套设计不使用跨两个OFDM符号的正交导频，因此在符号级Doppler模型下不会重新引入STBC的“两个时隙信道必须相同”假设。SFBC数据仍只使用真正相邻的子载波对；pilot、DC和保护带切断连续区间后留下的单独数据子载波保持为空。

配置和GUI变化：

- `pairing=frequency`允许启用comb pilot；
- 支持`channel_estimation=ls_linear/ls_dft/lmmse`；
- DFT-LS最低pilot数量按每根Tx各自拥有的pilot数量检查；
- GUI选择SFBC时默认pilot spacing=4、LMMSE，并按所选Tx链路显示对应pilot；
- SFBC的pilot-CPE、独立相位参考和SFO跟踪仍暂缓。

正式对照参数为FFT=1024、CP=128、2×2、64-QAM、三径TDL、pilot spacing=8、最大路径Doppler=500 Hz、300帧。结果如下：

| SNR | CSI | BER | EVM | 信道NMSE | CRC goodput |
|---:|---|---:|---:|---:|---:|
| 30 dB | Perfect | `1.07e-5` | 0.03427 | 0 | 49.361 Mb/s |
| 30 dB | LS-linear | `5.69e-4` | 0.05047 | 0.001222 | 1.073 Mb/s |
| 30 dB | DFT-LS | `3.02e-5` | 0.03663 | 0.0001648 | 42.386 Mb/s |
| 30 dB | LMMSE | `3.02e-5` | 0.03663 | 0.0001648 | 42.386 Mb/s |
| 40 dB | Perfect | 0 | 0.01836 | 0 | 53.653 Mb/s |
| 40 dB | LS-linear | `6.32e-5` | 0.03388 | 0.0006604 | 30.940 Mb/s |
| 40 dB | DFT-LS | 0 | 0.01881 | `1.65e-5` | 53.653 Mb/s |
| 40 dB | LMMSE | 0 | 0.01881 | `1.65e-5` | 53.653 Mb/s |

无噪声QPSK有限时延测试中，DFT-LS实现零误码且CSI NMSE约`3.4e-30`。128点FFT三径TDL的相邻子载波信道变化NMSE约0.025；此时即使CSI精确，64-QAM仍可能因“一对子载波共享首个子载波信道”的传统正交合并近似产生误码。1024点FFT时该频率失配显著减小。后续可研究使用两个子载波各自CSI的广义线性最小二乘SFBC检测器。

正式配置为`python_phy/configs/mimo_2x2_sfbc_tdl_lmmse_doppler_1024.yaml`，扫描脚本为`python_phy/experiments/compare_sfbc_estimators.py`，数据位于`measurement/sfbc_csi_comparison/`。本阶段全量结果为78/78 pytest、80/80显式检查通过。

## 32. Kronecker天线相关与秩亏MIMO信道（2026-08-21）

空间复用已经加入可配置的发射相关系数、接收相关系数和显式空间秩。相关Rayleigh模型为：

```text
Hcorr = Rr^(1/2) · W · Rt^(1/2)
Rt(i,j) = rho_tx^|i-j|
Rr(i,j) = rho_rx^|i-j|
```

`W`为独立单位功率复高斯矩阵。`rho=0`对应独立天线，`rho→1`时空间方向逐渐重合。相关矩阵用Hermitian特征分解求平方根。Monte-Carlo单测使用50000个2×2样本，测得的Tx/Rx相关系数分别与配置0.8/0.6相符，误差小于0.02。

显式`spatial_rank`通过每帧SVD保留最强的指定数量奇异模式：

```text
H = U · diag(s) · V^H
Hrank = U[:,0:r] · diag(s[0:r]) · V[:,0:r]^H
```

截断后重新缩放，使`||Hrank||F = ||H||F`。因此结果隔离的是空间自由度损失，而不是总信道能量下降。`spatial_rank=0`保留自然秩；正整数不得超过`min(Nr,Nt)`。当前相关/秩控制应用于Rayleigh信道。

正式相关性扫描采用FFT=256、CP=32、2×2满层、64-QAM、完美CSI、40 dB和200帧：

| Tx/Rx rho | 平均条件数 | ZF BER | MMSE BER | ZF EVM | MMSE EVM | MMSE goodput |
|---:|---:|---:|---:|---:|---:|---:|
| 0 | 4.00 | `5.45e-4` | `5.50e-4` | 0.03062 | 0.03045 | 33.411 Mb/s |
| 0.50 | 5.17 | `8.34e-4` | `8.26e-4` | 0.03774 | 0.03751 | 31.088 Mb/s |
| 0.80 | 10.6 | 0.00650 | 0.00647 | 0.07429 | 0.07273 | 23.048 Mb/s |
| 0.95 | 38.7 | 0.07667 | 0.07335 | 0.26573 | 0.21998 | 1.429 Mb/s |
| 0.99 | 188 | 0.30091 | 0.26337 | 1.29014 | 0.51456 | 0 |
| 1.00 | 约`1.67e15`，rank=1 | 0.37506 | 0.37506 | 0.70736 | 0.70736 | 0 |

在ρ=1时，Tx和Rx相关矩阵均为rank-1，2×2信道只有一个空间自由度。MMSE能抑制病态信道中的噪声增强，但不能创造缺失的空间维度；因此完全秩亏时与ZF一样不能恢复两层独立数据。

相同条件下显式秩扫描结果为：

| MIMO | 可用rank | BER | EVM | CRC goodput |
|---|---:|---:|---:|---:|
| 2×2 | 2 | `5.50e-4` | 0.03045 | 33.411 Mb/s |
| 2×2 | 1 | 0.34429 | 0.70707 | 0 |
| 4×4 | 4 | 0.001203 | 0.04127 | 55.827 Mb/s |
| 4×4 | 3 | 0.24115 | 0.50029 | 0 |
| 8×8 | 8 | 0.002455 | 0.05324 | 87.385 Mb/s |
| 8×8 | 6 | 0.25609 | 0.49870 | 0 |

满层发送时，只要信道rank低于层数，至少存在一个不可观测的数据子空间；即使只缺一个或两个rank，长帧CRC goodput也会迅速归零。工程接收机需要同时输出有效rank、奇异值和条件数，发射端则应执行`layers <= rank`的自适应层数选择，并结合每层SINR选择MCS。

新增GUI/命令行参数为`tx_correlation`、`rx_correlation`和`spatial_rank`；结果增加平均奇异值与最小奇异值诊断。正式配置为`python_phy/configs/mimo_2x2_spatial_multiplexing_correlated_rayleigh_256.yaml`，实验脚本为`python_phy/experiments/stress_mimo_correlation_rank.py`，数据和两张曲线位于`measurement/mimo_correlation_rank/`。全量结果为83/83 pytest、83/83显式检查通过。

## 33. 20 ppm与低速移动工程接收子集（2026-08-22）

根据实际设备`|SFO|<=20 ppm`、最大移动速度1–2 m/s，重新扫描2×2、1024/128、64-QAM、40 dB、200帧工作区间。Doppler按`fd=v×fc/c`换算；2 m/s时3.5 GHz约23.3 Hz、5.8 GHz约38.7 Hz、24 GHz约160 Hz。

SFO方案对比如下：

| 方案 | BER | EVM | CRC goodput | 重采样负载 |
|---|---:|---:|---:|---:|
| 8参考音频域相位斜率 | 0.001132 | 0.04067 | 62.658 Mb/s | 0 |
| 8参考音+sinc8闭环重采样 | 0.001116 | 0.03892 | 63.016 Mb/s | 248 Mtap-MAC/s |

频域方案保留约99.4%的goodput，因此正式工程主链取消时域重采样、第二次CP-CFO和第二遍FFT。32个相位参考缩减为8个，空出的24个comb位置转为CSI导频；正式资源数变为672数据、216 FDM CSI导频、8相位参考和128空载波。

20 ppm与逐采样连续Doppler组合时，0/50/100/200 Hz的goodput分别为62.658/62.658/61.584/58.003 Mb/s。100 Hz仍保留约98.3%的零Doppler吞吐，因此第一版不加入ICI抵消；超过200 Hz优先降MCS或rank，再评估固定一次相邻三子载波抵消。

LDPC状态分为两部分：原OpenISAC C++已经包含LDPC(1008,504)，默认码率1/2、6次水平分层NMS迭代、自定义SIMD编码器、AFF3CT浮点译码、可选int16译码、交织/扰码和多线程队列；Python现已完成相同矩阵编码、6次分层min-sum、软LLR、扰码、21×48交织、marker和BCH mini-header黄金链。正式OFDM配置目前仍用CRC-16统计FER，下一步把128个稳健QPSK控制RE和LDPC高阶QAM载荷映射进2×2帧，不另造复杂C++算法。

正式配置为`python_phy/configs/mimo_2x2_spatial_multiplexing_realtime_1024.yaml`，工程验收脚本为`python_phy/experiments/validate_engineering_operating_region.py`，结果位于`measurement/engineering_operating_region/`。加入LDPC专项测试后，当前103/103项pytest和96/96项显式验收通过。
