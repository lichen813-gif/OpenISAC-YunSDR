# OpenISAC：Python 建模、MIMO 验证到 C++ 实现路线

## 1. 目标

建立一套长期保留的 Python PHY 黄金参考模型，先完成 SISO，再逐步完成 2×2、4×4、8×8 MIMO；所有算法在 Python 中通过功能、误码率和数值稳定性验证后，使用固定接口和黄金向量逐模块迁移到 C++。

最终形成三层成果：

1. Python 跨平台算法仿真器：Windows/Linux 均可运行，用于算法研究、参数扫描和结果画图。
2. C++ 浮点 PHY 核心库：与 Python 结果一致，用于性能评估和实时系统集成。
3. 可选优化版本：只对明确瓶颈实施 SIMD、int16/int8 LDPC、CUDA 或 FPGA 定点化。

### 1.2 Windows C++ 最新进度（2026-08-22）

独立 `cpp_phy/` 已在 VS2019 v142/x64 下完成 1024/128、2×2 Rank-2/64-QAM 正式短帧的完整接收闭环：ZC 定时、三径 TDL、CP-CFO、20 ppm SFO 相位参考校正、FDM 导频 LS、MMSE、max-log LLR、解交织/解扰、归一化 Min-Sum LDPC、CRC、BER/FER/EVM 和 Rank/MCS 建议。14 个 C++ LDPC 编码码字逐 bit 对齐 Python 黄金向量。

25 帧 Windows C++ 显式回归在 36/40/42 dB 得到 FER=0；40 dB 平均 pre-LDPC BER 为 0.01421、post-LDPC BER 为 0、EVM 为 10.48%。高 SNR LDPC Release 基准约 7.5 us/块，14 块约 106 us/帧。正式帧结构和复现命令见 `Windows_C++_PHY正式帧结构与使用说明.md`。

下一迁移门是让控制头真正驱动 Rank 1/2 与 Qm 的下一帧发送映射，再迁移通用 4×4/8×8 C++ 检测接口；当前 Rank/MCS 只产生建议，不应误写成已经动态改变当前帧。

本路线不在第一阶段处理实时射频收发、Windows 多进程共享内存和完整 GUI；这些属于 C++ 算法核验证之后的应用集成工作。最终射频接口已调整为 `libyunsdr`，不再以 USRP/UHD 接入作为目标。

### 1.1 当前实施状态（2026-08-21）

第一版代码已在 `python_phy/` 建立，当前完成：

- 与现有 C++ `SquareQAM` 一致的 QPSK、16/64/256-QAM；
- max-log LLR，正数表示 bit 0；
- 酉归一化 CP-OFDM；
- 2Tx、1/2/4/8Rx Alamouti STBC 映射和完美 CSI 合并；
- AWGN、固定平坦信道、块 Rayleigh 平坦衰落；
- 与 OpenISAC 现有实现一致的 CRC-16/CCITT-FALSE；
- BER、BLER、CRC failure rate、EVM 指标；
- YAML 配置、CSV/JSON 输出入口和 11 项基础回归测试。

首轮 1000 帧确定性抽样结果如下，用于确认链路方向正确，不作为最终性能基准：

| 模式 | 调制 | SNR | BER | BLER/CRC failure | EVM RMS |
|---|---|---:|---:|---:|---:|
| 2×1 STBC Rayleigh | 16-QAM | 0 dB | 0.3023047 | 1.000 | 1.4058 |
| 2×1 STBC Rayleigh | 16-QAM | 20 dB | 0.0038613 | 0.234 | 0.1388 |
| 2×1 STBC Rayleigh | 16-QAM | 30 dB | 0.0001055 | 0.004 | 0.0453 |
| 2×2 STBC Rayleigh | 64-QAM | 5 dB | 0.2076706 | 1.000 | 0.4555 |
| 2×2 STBC Rayleigh | 64-QAM | 25 dB | 0.0001797 | 0.043 | 0.0462 |
| 2×2 STBC Rayleigh | 64-QAM | 35 dB | 0 | 0 | 0.0146 |

无噪声 256-QAM、2×2 Rayleigh 完美 CSI 回归已达到 bit error、block error、CRC failure 全部为零。

2026-08-21 已继续完成 MIMO TDL 第一阶段：支持手动配置每条路径的 sample 延迟、增益和相位；每个 Tx-Rx 链路执行时域线性卷积，接收端用已知 TDL FFT 作为完美 CSI 进行逐子载波 Alamouti 合并，并强制最大路径时延不超过 CP。GUI 已联动显示合并后星座、Tx/Rx 时域波形和指定 Tx-Rx 链路的频率响应。当前 pytest 为 16/16，通过显式验收为 27/27；2×2、256-QAM、TDL 无噪声链路的 bit/block/CRC error 均为零。当前仍未接入 LDPC、Doppler、同步和估计 CSI；这些按后续阶段逐项加入。

同日完成 OFDM 参数和 comb pilot 资源映射：GUI 已开放 FFT、CP、子载波间隔、左右保护带、DC null、pilot spacing 和 pilot offset；采样率由 `FFT×Δf` 推导。数据、导频和空载波使用显式资源索引，导频为确定性 BPSK Alamouti 符号对，频响图直接标出导频位置；非整字节容量使用尾部 padding，同时保持 CRC 覆盖范围明确。pytest 更新为 19/19，显式验收更新为 29/29。当前接收端仍为完美 CSI，尚未实现前导/CP 粗定时、导频残余同步跟踪和 LS/LMMSE 估计。

## 2. 总体原则

### 2.1 Python 是规范和黄金模型

- Python 使用 float64/complex128 作为高精度参考。
- 任何 C++、CUDA、FPGA 实现都必须与 Python 中间结果比较。
- Python 模型长期保留，不在 C++ 完成后废弃。
- 算法更改先进入 Python并更新测试，再进入 C++。

### 2.2 先正确，再优化

```text
Python float64
    ↓
C++ float32
    ↓
性能分析
    ↓
局部 SIMD / int16 LLR / int8或int16 LDPC / GPU
```

第一版 C++ 不做全链路定点。FFT、信道估计和 MIMO 检测保持 float32/complex64；优先考虑 LLR 与 LDPC 的定点化。

### 2.3 SISO 与 MIMO 共用一套流水线

SISO 是 `Nt=1, Nr=1, layers=1` 的 MIMO 特例。调制、编码、资源映射和接收接口从第一天就带天线/层维度，避免后期重写。

推荐统一张量顺序：

```text
TX frequency grid: [batch, ofdm_symbol, subcarrier, tx_antenna]
RX frequency grid: [batch, ofdm_symbol, subcarrier, rx_antenna]
Channel matrix:    [batch, ofdm_symbol, subcarrier, rx_antenna, tx_antenna]
Layer data:        [batch, layer, coded_bit_or_qam_symbol]
```

数组使用 C-contiguous；复数使用实部、虚部相邻的 complex64/complex128。

## 3. 必须优先冻结的 PHY 约定

建立 `spec/phy_conventions.md`，明确以下内容：

- bit 顺序：统一 MSB-first；
- 字节序：黄金数据文件使用 little-endian；
- QPSK/16/64/256-QAM Gray label；
- LLR 正数表示 bit 0，负数表示 bit 1；
- LLR 使用的噪声方差定义为 `E[|n|²]`；
- FFT/IFFT 归一化；
- 子载波索引、DC、保护带和 `fftshift` 规则；
- CP 插入、去除和信道卷积边界；
- OFDM symbol、subcarrier、layer、antenna 的维度顺序；
- MIMO 信号模型 `y[k] = H[k]x[k] + n[k]`，其中 `H` 为 `Nr×Nt`；
- CRC 多项式、初值、反射、终值异或和覆盖范围；
- LDPC 输入 bit、输出 LLR、交织和加扰顺序；
- SNR、Es/N0、Eb/N0 与码率、Qm、层数的换算；
- 导频功率与数据功率定义；
- BLER、packet loss 和 header failure 的统计口径。

这些约定一旦进入 C++ 阶段不得隐式修改；修改必须提升协议版本并更新黄金向量。

## 4. Python 工程结构

```text
python_phy/
├── pyproject.toml
├── requirements-lock.txt
├── openisac_phy/
│   ├── bits.py
│   ├── crc.py
│   ├── scrambling.py
│   ├── interleaving.py
│   ├── ldpc.py
│   ├── modulation.py
│   ├── framing.py
│   ├── resource_grid.py
│   ├── ofdm.py
│   ├── channels_siso.py
│   ├── channels_mimo.py
│   ├── pilots.py
│   ├── channel_estimation.py
│   ├── detectors.py
│   ├── stbc.py
│   ├── precoding.py
│   ├── metrics.py
│   └── pipeline.py
├── configs/
│   ├── siso_awgn.yaml
│   ├── siso_tdl.yaml
│   ├── mimo_2x2_identity.yaml
│   ├── mimo_2x2_rayleigh.yaml
│   ├── mimo_2x1_alamouti.yaml
│   ├── mimo_2x2_alamouti.yaml
│   ├── mimo_4x4_tdl.yaml
│   └── mimo_8x8_tdl.yaml
├── experiments/
│   ├── sweep_siso.py
│   ├── sweep_mimo.py
│   └── compare_detectors.py
├── tests/
│   ├── test_modulation.py
│   ├── test_ofdm.py
│   ├── test_framing_crc.py
│   ├── test_ldpc.py
│   ├── test_siso_pipeline.py
│   ├── test_mimo_channel.py
│   ├── test_channel_estimation.py
│   └── test_detectors.py
└── golden/
    ├── manifests/
    └── vectors/
```

核心依赖控制为：NumPy、SciPy、PyYAML、Matplotlib、Pandas、pytest。Numba、CuPy、PyTorch 均为可选加速后端，不进入初始算法规范层。

## 5. 分阶段实施计划

### P0：规范、测试框架和基线冻结

预计：3–5 人日。

任务：

- 编写 `phy_conventions.md`；
- 建立 YAML 配置结构；
- 建立 pytest；
- 固定随机种子和结果目录；
- 保存当前 C++ QPSK、QAM 和 LDPC 可用测试向量；
- 定义 BER、BLER、EVM、NMSE、SINR 统计接口。

验收：

- Windows 与 Linux 使用同一配置得到一致的 Python 数值结果；
- QPSK/16/64/256-QAM 所有星座点无噪声零错误；
- 同一随机种子的 Python 回归完全可复现。

### P1：完整 SISO 浮点 PHY

预计：7–10 人日。

任务：

- payload、CRC、LDPC、加扰和交织；
- QPSK、16/64/256-QAM；
- 资源栅格、导频、IFFT/FFT 和 CP；
- soft LLR、LDPC 解码和 CRC 判定；
- 无噪声、AWGN、静态多径；
- BER、post-LDPC BER、BLER、EVM。

验收：

- 无噪声全链路 BER/BLER 为 0；
- AWGN BER 随 SNR 单调下降，并与理论/参考曲线一致；
- Python QAM 与现有 C++ `SquareQAM` label、功率和 LLR 符号一致；
- 多径最大时延小于 CP 时，完美 CSI 均衡在高 SNR 下零错误。

### P2：完整 SISO 信道与同步误差

预计：5–8 人日。

任务：

- 可配置 TDL；
- Rayleigh、Rician；
- CFO、SFO、定时偏移和 Doppler；
- 完美同步与估计同步两条路径；
- LS/LMMSE 或现有 OpenISAC 信道估计方法；
- Monte Carlo 置信区间。

验收：

- 关闭单项损伤后回到基线结果；
- 各损伤的影响能够独立复现；
- 完美 CSI 与估计 CSI 的性能差距可解释；
- 每个 SNR 点达到规定最小 bit/block 数，而不是只运行固定少量帧。

### P3：2×2 MIMO，完美 CSI 基线

预计：7–10 人日。

任务：

- layer mapper/demapper；
- `Nr×Nt` 频域信道接口；
- identity、diagonal、固定满秩、病态矩阵；
- i.i.d. Rayleigh/Rician 平坦 MIMO；
- ZF、MMSE 检测；
- 分层 LLR、BER、BLER、EVM、post-detection SINR；
- 暂不加入预编码和信道估计误差。

验收顺序：

1. `H=I`、无噪声、所有层零误码；
2. 对角 `H` 的每层增益和 LLR 与 SISO 等效模型一致；
3. 固定满秩矩阵结果与 NumPy 直接矩阵求解一致；
4. 病态矩阵下 MMSE 优于或稳定于 ZF；
5. Rayleigh Monte Carlo 曲线随 SNR 单调改善；
6. 改变 layer/antenna 顺序的测试能够捕获维度错误。

### P4：MIMO 导频和信道估计

预计：8–12 人日。

第一版导频推荐采用正交方案：

- 通信信道估计：不同 TX 使用正交 TDM/FDM pilot RE；
- 第一阶段优先 TDM full-band reference，逻辑最清晰；
- 后续再评估 comb/FDM pilot 的频谱效率；
- ISAC TDM-MIMO sensing pilot 作为独立阶段，不与通信导频第一版耦合。

任务：

- `Nr×Nt` LS 信道估计；
- 时频插值；
- LMMSE 可选；
- channel-estimation NMSE；
- 导频开销与吞吐率统计；
- 完美 CSI、LS CSI、LMMSE CSI 对比。

验收：

- 无噪声 LS 能恢复已知静态 `H`；
- 导频映射正交性单元测试通过；
- NMSE 随 pilot SNR 增加而下降；
- 高 SNR 下估计 CSI 链路接近完美 CSI 基线。

### P5：频率选择性 2×2 与 4×4 MIMO

预计：10–15 人日。

任务：

- 每个天线对独立 TDL；
- Kronecker 或可配置 Tx/Rx 空间相关；
- 时变 Doppler；
- 4×4、rank 1/2/4；
- 条件数、奇异值、每层 SINR 输出；
- MMSE 批量子载波求解；
- 可选 SVD 预编码，先使用完美 CSI。

验收：

- 频域 `H[k]` 与时域卷积的 FFT 结果一致；
- 相关系数为 0 时退化到独立 Rayleigh；
- rank 与发送 layer 数不匹配时明确拒绝配置；
- 4×4 identity 高 SNR 零误码；
- ZF/MMSE 的 NumPy 批量实现与逐子载波参考实现一致。

### P6：8×8、检测器扩展和性能边界

预计：8–12 人日。

任务：

- 8×8、rank 1/2/4/8；
- correlated Rayleigh/Rician/TDL；
- MMSE-SIC 可选；
- K-best 仅在确有需求时加入；
- Monte Carlo 批处理和内存控制；
- 吞吐率、复杂度、矩阵条件数分布。

验收：

- 8×8 identity/diagonal 无噪声零误码；
- 固定矩阵结果与通用线性代数参考一致；
- 大规模扫描不依赖单次随机结果；
- 明确给出不同 rank、调制、检测器的 BLER/SNR 和计算开销。

### P7：Python 量化模型（可选但建议）

预计：5–10 人日。

任务：

- float32 对 float64；
- int16/int8 LLR；
- int16/int8 LDPC 输入；
- 饱和、舍入、动态缩放统计；
- 暂不对 FFT、信道估计和 MIMO 求解器做完整定点。

验收：

- float32 与 float64 曲线差异在 Monte Carlo 置信范围内；
- int16 LLR/LDPC 的性能损失有量化数据；
- 所有饱和事件可统计，禁止静默溢出。

## 6. Python 到 C++ 的迁移方案

### C0：建立 C++ 核心库边界

预计：5–8 人日。

```text
cpp/
├── include/openisac/
│   ├── modulation.hpp
│   ├── ofdm.hpp
│   ├── framing.hpp
│   ├── channel.hpp
│   ├── mimo.hpp
│   ├── channel_estimation.hpp
│   ├── detector.hpp
│   └── metrics.hpp
├── src/
├── tests/
└── tools/openisac_sim.cpp
```

`openisac_core` 不包含 UHD、Socket、共享内存、串口、线程优先级和 GUI。它只接受数组、配置和状态，输出数组和指标。

### C1：黄金向量格式

不要依赖 Python RNG 与 C++ RNG 产生相同随机序列。Python 直接导出输入和期望中间结果。

推荐格式：

```text
golden/case_0001/
├── manifest.json
├── input_bits.bin
├── coded_bits.bin
├── qam_symbols.bin
├── tx_grid.bin
├── tx_samples.bin
├── channel_matrix.bin
├── rx_samples.bin
├── rx_grid.bin
├── channel_estimate.bin
├── equalized_layers.bin
├── llr.bin
└── decoded_bits.bin
```

`manifest.json` 记录 dtype、shape、字节序、配置、随机种子、容差和文件 SHA-256。二进制数组使用 little-endian、C-contiguous，便于 C++ 无第三方依赖读取。

### C2：模块迁移顺序

预计：20–35 人日。

严格按以下顺序：

1. bit/CRC/加扰/交织；
2. QAM mapper/hard demapper；
3. soft LLR；
4. OFDM IFFT/FFT 和资源映射；
5. SISO 信道和均衡；
6. LDPC 接口；
7. MIMO channel；
8. ZF/MMSE detector；
9. MIMO pilot 和 LS/LMMSE；
10. pipeline 与指标统计。

每完成一个模块必须先通过黄金向量，再进入下一个模块。禁止只在最后比较 BER。

### C3：数值比较门槛

推荐初始容差：

| 数据 | 验收 |
|---|---|
| bit、CRC、label、交织索引 | 必须完全一致 |
| QAM constellation | float32 相对/绝对容差比较 |
| FFT/IFFT、channel output | 使用规模相关的 float32 容差 |
| channel estimate、ZF/MMSE | 同时检查相对误差和残差 `||y-Hx||` |
| LLR | 符号必须一致；幅值在定义容差内 |
| decoded payload | 必须完全一致 |
| BER/BLER 曲线 | 差异必须处于统计置信区间内 |

具体浮点容差不要在方案中写死，应根据 FFT 长度、矩阵条件数和算法阶段记录在每个 golden manifest 中。

### C4：Python/C++ 联合测试

两种方式并行保留：

1. 文件黄金向量：适合 CI 和版本冻结；
2. pybind11：Python 直接调用 C++ 模块，适合开发期快速逐函数对比。

CI 矩阵：

```text
Python: Windows + Linux
C++:    MSVC + GCC/Clang
Mode:   Debug + Release
Tests:  unit + golden + short Monte Carlo
```

长时间 BER/BLER 扫描放到 nightly CI，不放在每次提交的快速测试中。

### C5：C++ 性能优化

预计：10–25 人日，取决于目标吞吐率。

优化顺序：

1. 记录每阶段耗时和内存带宽；
2. 消除热路径内存分配；
3. 批量 FFT；
4. Eigen/MKL/OpenBLAS 或自有小矩阵 MMSE；
5. SIMD QAM LLR；
6. int16/int8 LDPC；
7. 多线程按 frame/subcarrier batch；
8. 只有 CPU 不满足目标后才进入 CUDA。

不得为性能修改 bit 顺序、LLR 定义或矩阵布局；若必须修改，先更新 Python 规范和黄金向量。

## 7. 统一 YAML 配置建议

```yaml
numerology:
  fft_size: 1024
  cp_length: 128
  symbols_per_frame: 100

coding:
  ldpc: ldpc_1008_504
  payload_crc: crc32c

modulation:
  scheme: 64qam
  llr: max_log

mimo:
  tx_antennas: 2
  rx_antennas: 2
  layers: 2
  detector: mmse
  precoder: none

pilots:
  mode: tdm_full_band
  channel_estimator: ls

channel:
  model: mimo_tdl
  snr_db: 20
  doppler_hz: 0
  tx_correlation: 0.0
  rx_correlation: 0.0
  taps:
    - {delay_samples: 0, gain_db: 0, phase_deg: 0}
    - {delay_samples: 3, gain_db: -4, phase_deg: 45}
    - {delay_samples: 9, gain_db: -8, phase_deg: -80}

simulation:
  frames: 1000
  seed: 23063
  batch_frames: 16
  save_golden: false
```

配置加载后执行统一校验：

- `layers <= min(Nt, Nr)`；
- CP 不小于配置要求的最大有效信道时延；
- 导频资源能区分全部 TX 天线；
- LDPC block、Qm、资源容量匹配；
- SNR 定义和噪声方差换算唯一；
- 不支持的 detector/precoder 组合立即报错。

## 8. 输出指标

SISO 与 MIMO 共用：

- uncoded BER；
- post-LDPC BER；
- BLER；
- CRC failure rate；
- header failure rate；
- EVM；
- channel-estimation NMSE；
- throughput 和有效频谱效率。

MIMO 增加：

- 每层 BER/BLER/EVM/SINR；
- `H[k]` 的奇异值；
- condition number；
- rank；
- detector residual；
- ZF/MMSE/MMSE-SIC 复杂度和耗时。

所有扫描结果输出 CSV；配置、代码版本、随机种子和依赖版本写入同目录的 JSON manifest。

## 8.1 STBC/SFBC 映射扩展

可以在统一 MIMO 资源映射器中加入 STBC，但应把它定义为与空间复用并列的发送模式，而不是 ZF/MMSE 空间复用前的附加步骤：

```text
mimo.mode = spatial_multiplexing | stbc | sfbc
```

- `spatial_multiplexing`：多层同时发送，目标是提高吞吐量；接收端使用 ZF/MMSE/MMSE-SIC。
- `stbc`：同一层在两个 OFDM symbol 和两根发射天线上做空时编码，目标是获得发射分集。
- `sfbc`：在两个相邻子载波和两根发射天线上编码，适用于时间变化较快、但相邻子载波信道足够接近的场景。

第一版只实现 `Nt=2` 的 Alamouti 正交 STBC，`Nr` 支持 1、2、4、8。对一对调制符号 `s0、s1`，在两个相邻 OFDM symbol 上的映射为：

| OFDM 时刻 | Tx0 | Tx1 |
|---|---:|---:|
| `t` | `s0` | `s1` |
| `t+1` | `-conj(s1)` | `conj(s0)` |

接收端对全部接收天线进行 Alamouti 合并，再进入 QAM 软解调、LDPC、CRC 和 BLER 统计。该模式满足：

- `layers=1、Nt=2、Nr>=1`；
- 支持 QPSK、16/64/256-QAM；
- 理想条件下码率为 1，即两个时隙发送两个调制符号；
- 主要收益是约 `2×Nr` 阶分集，而不是空间复用吞吐量；
- 以固定总发射功率比较时，每根发射天线必须按 `1/sqrt(2)` 归一化；
- 一个 Alamouti 块内的两个 OFDM symbol 应共享近似不变的信道；信道快速变化时应统计性能损失。

OFDM 中推荐先做时域配对 STBC：在 IFFT 之前完成资源栅格映射，每个数据子载波跨两个 OFDM symbol 配对。第二阶段再实现 SFBC；SFBC 解码要求配对子载波的信道响应足够接近，因此必须单独验证子载波间隔和时延扩展的影响。

建议配置如下：

```yaml
mimo:
  mode: stbc
  nt: 2
  nr: 1
  layers: 1
  stbc:
    scheme: alamouti
    pairing: time       # time=STBC，frequency=SFBC
    tx_power_normalize: true
```

STBC 验证顺序：

1. 无噪声固定信道：QAM 符号、LLR、解码 bit 必须正确；
2. 2×1 Alamouti、完美 CSI、平坦 Rayleigh：验证相对 SISO 的分集增益；
3. 2×2 Alamouti：验证多接收天线合并；
4. AWGN、Rayleigh、Rician、TDL 信道下扫描 BER/BLER/CRC failure；
5. 加入 LS/LMMSE 信道估计，验证导频能够分别估计 Tx0/Tx1；
6. 加入 Doppler，扫描一个 STBC 块内信道失配造成的性能下降；
7. 与 2×2 空间复用在相同总发射功率和相同 `Eb/N0` 定义下比较可靠性、吞吐量和复杂度。

4×4/8×8 阶段不直接套用全速率正交 STBC：高发射天线数的正交设计通常会损失码率。后续可按需求选择天线选择、成对 Alamouti、准正交 STBC 或空间复用；第一版不把它们列为必做项。

迁移到 C++ 时，`stbc_encode()` 和 `stbc_combine()` 作为独立纯函数，排在 MIMO channel 之后、ZF/MMSE detector 之前迁移。黄金向量增加 `stbc_tx_grid.bin`、`stbc_combined_symbols.bin` 和合并后的等效噪声方差；Python/C++ 的映射、功率归一化和 LLR 必须逐项一致。

## 9. 工作量与里程碑

以 1 名熟悉通信算法、NumPy 和 C++ 的开发者估算：

| 里程碑 | 内容 | 预计 |
|---|---|---:|
| M0 | 规范、测试框架 | 1 周 |
| M1 | SISO 完整 Python PHY | 2 周 |
| M2 | SISO 完整信道与同步损伤 | 1–2 周 |
| M3 | 2×2 完美 CSI、ZF/MMSE | 2 周 |
| M3A | 2×1/2×2 Alamouti STBC | 0.5–1 周 |
| M4 | MIMO 导频、LS/LMMSE | 2–3 周 |
| M5 | 4×4/8×8、TDL、相关信道 | 3–5 周 |
| M6 | Python 量化模型 | 1–2 周，可选 |
| M7 | C++ float32 核心迁移 | 4–7 周 |
| M8 | C++ 优化、定点 LDPC、CI | 2–5 周 |

Python SISO 到包含 Alamouti STBC 的完整 8×8 MIMO 参考模型约 11–16 周；完成验证后，C++ float32 核心约 4–7 周，其中 STBC 编解码移植通常增加 2–4 人日。两名开发者可以并行算法/测试与 C++ 基础设施，但规格冻结、MIMO 检测和跨语言验收存在顺序依赖，总日历时间不会简单减半。

## 10. 决策门与停止条件

每个阶段只有满足验收门才进入下一阶段：

```text
SISO 无噪声零错误
  ↓
SISO AWGN/TDL 曲线可信
  ↓
2×2 完美 CSI 正确
  ↓
2×2 估计 CSI 正确
  ↓
4×4 正确
  ↓
8×8 正确
  ↓
冻结 Python v1 黄金模型
  ↓
C++ 逐模块迁移
  ↓
性能 profiling
  ↓
局部定点/GPU/实时系统集成
```

遇到下列情况必须暂停扩展并修复基础模型：

- 无噪声仍有误码；
- Python Windows/Linux 结果不一致；
- LLR 符号或噪声方差定义不唯一；
- MIMO identity case 不能退化到并行 SISO；
- BER 曲线随 SNR 非单调且无法由统计波动解释；
- C++ 中间结果尚未通过黄金向量就开始性能优化。

## 11. 推荐的近期迭代

第一个 4 周迭代只做以下内容：

1. 第 1 周：冻结约定、建立 Python 包、测试和 YAML；
2. 第 2 周：完成 SISO 编码、QAM、OFDM、AWGN；
3. 第 3 周：加入 CRC、LDPC、多径、BER/BLER/EVM；
4. 第 4 周：完成 2×2 identity/static matrix、ZF/MMSE 完美 CSI 基线。

该迭代结束时应交付：

- 可在 Windows/Linux 执行的 Python CLI；
- SISO AWGN/TDL BER/BLER 曲线；
- 2×2 identity/static channel 结果；
- pytest 回归；
- 第一批跨语言黄金向量；
- 明确的 4×4/8×8 后续接口。

完成这一步后再决定导频结构、MIMO TDL、预编码和 C++ 迁移细节，可以显著降低返工风险。

## 12. 已完成阶段：2Tx Alamouti SFBC

当前Python黄金模型已经在原有跨时隙STBC旁增加跨频率SFBC。配置仍使用统一Alamouti入口，通过`mimo.stbc.pairing`选择：

```yaml
mimo:
  mode: stbc
  scheme: alamouti
  nt: 2
  nr: 2
  layers: 1
  stbc:
    pairing: frequency   # time=STBC，frequency=SFBC
```

对每个OFDM symbol中的相邻子载波`k、k+1`，SFBC映射为：

| 子载波 | Tx0 | Tx1 |
|---|---:|---:|
| `k` | `s0` | `s1` |
| `k+1` | `-conj(s1)` | `conj(s0)` |

两根发射天线仍乘`1/sqrt(2)`，保持总发射功率为1。资源分配器只组合频率上严格相邻的资源；不会跨越DC、保护带或pilot空洞配对。接收端对每个OFDM symbol和全部接收天线独立合并，输出继续进入同一套QAM硬判决、CRC-16、BER、BLER和EVM统计。

本阶段验收包括：

- 已知复数符号的SFBC映射矩阵逐元素一致；
- 平坦、无噪声、已知2x1/2x2信道精确恢复；
- 端到端高阶QAM、CRC零错误；
- 64项pytest和64项显式检查全部通过；
- 在1024点FFT、CP128、2x2、64-QAM、30 dB、三径TDL下完成`0/100/300/500/1000 Hz`扫描。

固定种子扫描显示，500 Hz时STBC BER约为`4.96e-2`，SFBC约为`1.09e-5`；1000 Hz时STBC约为`1.12e-1`，SFBC约为`4.68e-5`。原因是当前信道在一个OFDM symbol内按块不变，SFBC不跨时间配对；其代价是相邻子载波信道不一致会破坏频域Alamouti正交性。64点FFT、9点最大时延的压力测试已经明确观察到这一限制，因此不能把SFBC解释为在所有TDL条件下都优于STBC。

可复现实验入口：

```bat
cd /d E:\openisac\OpenISAC-main\python_phy
.venv\Scripts\python.exe experiments\compare_stbc_sfbc.py
.venv\Scripts\python.exe experiments\validate_explicit.py --frames 100
```

本阶段有意只支持SFBC完美CSI，并要求关闭comb pilot、pilot-CPE、独立相位参考和SFO跟踪。下一阶段主线转入2x2空间复用，建立ZF/MMSE检测基线；SFBC的双发射正交导频与估计CSI作为随后独立阶段加入，避免把频域编码验证与导频设计混在一起。

## 13. 已完成阶段：2×2空间复用ZF/MMSE

Python黄金模型已经加入`Nt=2、Nr>=2、layers=2`的空间复用模式。每个OFDM资源单元同时承载两层独立QAM符号，Tx0/Tx1均乘`1/sqrt(2)`，因此总发射功率与STBC/SFBC相同。首版使用完美CSI，并支持：

- ZF：`W=pinv(H_eff)`；
- 线性MMSE：`W=(H_eff^H H_eff + sigma_n^2 I)^-1 H_eff^H`；
- `H_eff=H/sqrt(2)`，与实际发射功率归一化一致；
- 每层BER/EVM、总体BER/BLER/CRC/EVM；
- 信道秩、条件数、等效检测MSE；
- 原始PHY速率、净载荷速率和CRC goodput。

配置示例：

```yaml
mimo:
  mode: spatial_multiplexing
  nt: 2
  nr: 2
  layers: 2
  detector: mmse
receiver:
  channel_estimation: perfect
```

为了保证模式对比使用完全相同的信道和噪声样本，随机源已经拆分为`seed`、`channel_seed`、`noise_seed`。正式实验使用1024点FFT、CP128、896个数据子载波、2×2独立Rayleigh、64-QAM、完美CSI、每点300帧：

| 模式 | 净载荷上限 | 30 dB BER | 35 dB BER | 35 dB CRC goodput |
|---|---:|---:|---:|---:|
| STBC | 71.573 Mb/s | 2.85e-5 | 0 | 71.573 Mb/s |
| SFBC | 71.573 Mb/s | 3.01e-5 | 3.10e-7 | 71.335 Mb/s |
| SM-ZF | 143.253 Mb/s | 1.279e-2 | 3.644e-3 | 97.890 Mb/s |
| SM-MMSE | 143.253 Mb/s | 1.265e-2 | 3.639e-3 | 98.367 Mb/s |

结果表明：空间复用原始速率约为分集模式的两倍，但在相同总功率下每层能量更低，并受到小奇异值噪声增强影响，因此BER/EVM明显高于STBC/SFBC。MMSE在15 dB时把EVM从ZF的约`0.600`降至`0.363`；高SNR时两者逐渐接近。由于当前没有LDPC且CRC覆盖长帧，空间复用需要约35 dB才在本场景中实现高于分集模式的实际goodput。

当前自动验收为69项pytest和69项显式检查，包含身份信道ZF/MMSE精确恢复、Rayleigh无噪声零CRC、MMSE/ZF EVM对照和双层速率严格翻倍。结果位于`measurement/mimo_mode_comparison/`和`measurement/python_phy_explicit_validation_spatial_mimo_stage/`。

下一阶段为双发射空间复用设计正交导频，并把LS/DFT-LS/LMMSE信道估计扩展到逐OFDM符号的`2×2 H[k]`；之后再扩展4×4/8×8检测接口。

## 14. 已完成阶段：空间复用正交导频与估计CSI

2×2空间复用已经从完美CSI基线扩展到可实际估计的逐符号信道。导频采用频分正交结构：全部comb pilot按频率顺序交替分给Tx0和Tx1，每个导频RE只有一根天线发送单位功率BPSK。两个OFDM数据符号分别发送同一分配规则下的已知序列，因此接收端独立估计：

```text
H[t, k, rx, tx],  t=0,1
```

导频LS结果按Tx拆分后分别执行：

- 复数线性周期插值；
- 有限时延DFT-LS；
- 基于均匀有限时延PDP的LMMSE。

估计后的完整`2×2 H[k]`直接送入ZF或MMSE检测，不读取真实信道；真实CSI只用于NMSE和虚线参考图。信道图只标出当前所选Tx链路实际占用的那一半导频。

正式验证采用1024点FFT、CP128、896个活动数据/导频资源、pilot spacing=4、2×2 64-QAM、三径TDL、MMSE检测和每点300帧。关键结果：

| CSI | 30 dB BER | 50 dB BER | 50 dB EVM | 50 dB信道NMSE |
|---|---:|---:|---:|---:|
| Perfect | 7.04e-2 | 1.14e-4 | 2.48e-2 | 0 |
| LS-linear | 9.91e-2 | 1.26e-3 | 4.27e-2 | 5.09e-5 |
| DFT-LS | 7.47e-2 | 1.39e-4 | 2.58e-2 | 8.31e-7 |
| LMMSE | 7.47e-2 | 1.39e-4 | 2.58e-2 | 8.31e-7 |

在10 dB的独立显式检查中，LMMSE把DFT-LS信道NMSE从`0.06608`降至`0.06169`；高SNR和密集导频条件下两者趋于一致。64点FFT的无噪声TDL测试达到零bit/CRC错误和约`3.4e-30` NMSE，证明映射、LS和有限时延重建代数闭环正确。

当前为72项pytest和72项显式检查全部通过。配置、实验和报告分别位于：

- `python_phy/configs/mimo_2x2_spatial_multiplexing_tdl_lmmse_1024.yaml`；
- `python_phy/experiments/compare_spatial_estimators.py`；
- `measurement/spatial_csi_comparison/`；
- `measurement/python_phy_explicit_validation_spatial_csi_stage/`。

下一主阶段扩展4×4/8×8通用空间复用矩阵接口和检测器维度；SFBC估计CSI与同步损伤联合测试保留为并行后续项。

## 15. 已完成阶段：4×4/8×8满层空间复用

空间复用接口已从固定2层泛化到`Nt=2/4/8`，满层模式约束为`layers=Nt、Nr>=layers`。主要变化包括：

- 数据映射功率从固定`1/sqrt(2)`改为`1/sqrt(Nt)`；
- ZF/MMSE使用任意`Nr×layers`信道矩阵；
- FDM导频按`pilot_index mod Nt`轮转到全部发射天线；
- LS/DFT-LS/LMMSE逐Tx插值到完整`H[t,k,Nr,Nt]`；
- 每层BER/EVM、秩、条件数和等效MSE自动扩维；
- GUI和命令行可选择2/4/8根Tx以及对应接收天线。

256点FFT、CP32、224个活动子载波、64-QAM、独立Rayleigh、完美CSI、MMSE、固定总功率1、每点50帧的结果为：

| 规模 | 净载荷上限 | 40 dB BER | 50 dB BER | 50 dB goodput |
|---|---:|---:|---:|---:|
| 2×2 | 35.733 Mb/s | 1.11e-2 | 2.84e-3 | 33.589 Mb/s |
| 4×4 | 71.573 Mb/s | 1.02e-3 | 3.72e-6 | 70.142 Mb/s |
| 8×8 | 143.253 Mb/s | 5.08e-3 | 1.88e-3 | 134.658 Mb/s |

小样本下不同维度的BER不要求严格按天线数排序，因为每个规模使用不同维度的独立Rayleigh矩阵，条件数分布和深衰落样本不同。验收重点是：身份矩阵ZF/MMSE逐元素恢复、4×4/8×8 Rayleigh无噪声bit/CRC零错误、平均秩分别为4和8，以及总功率与速率缩放正确。

当前74项pytest和74项显式检查全部通过。结果位于`measurement/mimo_size_comparison/`和`measurement/python_phy_explicit_validation_scalable_mimo_stage/`。下一阶段进行空间复用同步、CFO、SFO和Doppler联合压力测试，并加入相关MIMO/秩亏场景。
