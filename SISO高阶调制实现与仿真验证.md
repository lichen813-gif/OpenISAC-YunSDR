# OpenISAC SISO 高阶调制实现与信道仿真验证

## 1. 本阶段结论

本阶段已完成 OpenISAC 下行 SISO 数据面的高阶调制改造：

- 保留原 QPSK，并新增 16-QAM、64-QAM、256-QAM；
- 控制区继续使用 QPSK，避免削弱 marker、BCH 帧头和 CRC 的鲁棒性；
- 数据区使用 Gray 编码、单位平均功率归一化的方形 QAM；
- 接收端按帧头指示的调制阶数生成 max-log LLR；
- LDPC(1008,504)、交织、加扰和现有 PRBS BER/BLER 统计保持不变；
- 增加 AWGN、静态三径 CP-OFDM 验证，以及完整 `ChannelSimulator -> BS -> UE` 扫描脚本。

当前实现范围是“下行 SISO 高阶调制”。上行仍为 QPSK，4×4/8×8 MIMO 不在本阶段内。

## 2. 空口格式

一个 LDPC 数据包的物理资源单元数量变为：

```text
128 个 QPSK 控制符号
+ payload_blocks × 1008 / Qm 个 QAM 数据符号
```

其中 `Qm` 为每符号比特数：QPSK=2、16-QAM=4、64-QAM=6、256-QAM=8。

控制区不变：

```text
64 个 marker QPSK 符号
+ 64 个 BCH/CRC mini-header QPSK 符号
```

mini-header 的 4 bit flags 字段分配如下：

| flags 位 | 含义 |
|---|---|
| bit 0 | ARQ feedback |
| bit 1 | eRTM timing |
| bit 3:2 = 00 | QPSK payload |
| bit 3:2 = 01 | 16-QAM payload |
| bit 3:2 = 10 | 64-QAM payload |
| bit 3:2 = 11 | 256-QAM payload |

因此接收端能够在进入 LDPC 前检查本地配置与帧头 Qm 是否一致；不一致时明确丢帧并告警。

## 3. 关键实现

| 文件 | 改动 |
|---|---|
| `include/QAM.hpp` | Gray 方形 QAM 映射、硬判决、重调制、max-log LLR |
| `include/Common.hpp` | `downlink.modulation` 配置及合法化 |
| `include/OFDMCore.hpp` | 调制 flags、QAM 包容量计算、内部 payload symbol tag |
| `include/LDPCCodec.hpp`, `src/LDPCCodec.cpp` | 将编码比特按 Qm 打包成 QAM label |
| `src/BS.cpp` | 控制区 QPSK、数据区可配置 QAM；容量和 ARQ airtime 计算随 Qm 变化 |
| `src/UE.cpp` | 先解控制头，再按包生成可变 Qm LLR；支持 float 和 int16 LDPC 输入 |
| `config/BS_Sim.yaml`, `config/UE_Sim.yaml` | 增加 `downlink.modulation` 示例 |
| `scripts/config_web_editor_schema.yaml` | Web 配置界面增加调制选择 |

QPSK label 与原项目完全兼容：

```text
00 -> (+1,+1)/sqrt(2)
01 -> (+1,-1)/sqrt(2)
10 -> (-1,+1)/sqrt(2)
11 -> (-1,-1)/sqrt(2)
```

16/64/256-QAM 的 I、Q 两轴分别使用 Gray label，星座归一化系数为：

```text
sqrt(2/3 × (M-1))
```

## 4. 配置方法

BS 与 UE 的 YAML 必须使用相同配置：

```yaml
downlink:
  modulation: 16qam   # qpsk / 16qam / 64qam / 256qam
```

高阶调制只改变 payload。同步 ZC、comb pilot、mid-frame pilot、marker 和 BCH/CRC mini-header 均不改变。

## 5. 已执行的验证

### 5.1 C++ 星座和 LLR 单元测试

测试文件：`tests/test_qam.cpp`。

验证内容：

- 原 QPSK 四点映射逐点兼容；
- QPSK、16-QAM、64-QAM、256-QAM 所有星座点无噪声 label 往返零错误；
- 所有星座单位平均功率；
- max-log LLR 正负号与发送 bit 一致；
- AWGN + 已知复数平坦信道回归。

实测结果（每种 200,000 个符号）：

| 调制 | SNR | uncoded BER |
|---|---:|---:|
| 16-QAM | 18 dB | 7.025e-4 |
| 64-QAM | 26 dB | 3.75e-5 |

### 5.2 CP-OFDM AWGN/三径仿真

脚本：`scripts/validate_siso_qam.py`。

参数：FFT=256、CP=32、400 个 OFDM block；三径为 `(0,0 dB,0°)`、`(3,-4 dB,45°)`、`(9,-8 dB,-80°)`；接收端使用已知频域信道做单抽头均衡。

部分结果：

| 调制 | 信道 | SNR | BER | BLER |
|---|---|---:|---:|---:|
| 16-QAM | AWGN | 12 dB | 2.7876e-2 | 1.0000 |
| 16-QAM | AWGN | 18 dB | 1.78223e-4 | 0.1725 |
| 16-QAM | AWGN | 24 dB | 0 | 0 |
| 16-QAM | 三径 | 24 dB | 3.82812e-3 | 0.9775 |
| 16-QAM | 三径 | 30 dB | 1.53809e-4 | 0.1425 |
| 64-QAM | AWGN | 18 dB | 2.43018e-2 | 1.0000 |
| 64-QAM | AWGN | 24 dB | 1.26953e-4 | 0.1675 |
| 64-QAM | AWGN | 30 dB | 0 | 0 |
| 64-QAM | 三径 | 24 dB | 1.88835e-2 | 1.0000 |
| 64-QAM | 三径 | 30 dB | 3.14616e-3 | 0.9950 |
| 16/64-QAM | AWGN/三径 | 80 dB | 0 | 0 |

这里的 BLER 是“一个 256 子载波 OFDM 数据符号中存在任意 bit error”，并且没有 LDPC，因此会明显高于 OpenISAC 完整链路的 post-LDPC BLER。三径信道存在频率选择性深衰落，故同样平均 SNR 下 BER 高于 AWGN，这是合理结果。

完整结果保存在 `measurement/siso_qam_validation/results.csv`。

## 6. 完整 ChannelSimulator 验证

新增脚本 `scripts/run_siso_qam_channel_sim.py`，会自动：

1. 生成匹配的 BS/UE YAML；
2. 启动 `ChannelSimulator -> UE -> BS`；
3. 通过 `SNR ` 控制命令扫描目标 SNR；
4. 启用内部 PRBS payload；
5. 汇总 post-LDPC BER、BLER、EVM 和估计 SNR。

Ubuntu 编译完成后运行：

```bash
python3 scripts/run_siso_qam_channel_sim.py \
  --modulation 16qam --channel awgn \
  --snr-db 8,12,16,20,24,28

python3 scripts/run_siso_qam_channel_sim.py \
  --modulation 64qam --channel multipath \
  --snr-db 12,16,20,24,28,32
```

输出目录：

```text
measurement/siso_qam_channel_sim/<run_id>/
  BS.yaml
  UE.yaml
  ChannelSimulator.log
  BS.log
  UE.log
  bs_measurement_summary.csv
  ue_measurement_summary.csv
  sweep_summary.csv
```

本次 Windows 工作区没有 UHD、AFF3CT、FFTW3f、ZeroMQ 的完整 Linux 构建环境，也没有可用 WSL，因此无法在本机启动三个原生进程；已执行的是可独立运行的 C++ 星座测试和 CP-OFDM 信道仿真。完整进程级验收需在项目支持的 Ubuntu 环境执行上述脚本。

## 7. CRC 与 BLER 口径

当前 mini-header 仍包含 CRC16-CCITT，并受 BCH(127,64) 保护。payload 仍只有 LDPC，没有新增 TB CRC。

完整 ChannelSimulator 脚本采用项目已有的内部 PRBS 精确 payload 比较：

- BER = 解码 payload 与期望 PRBS 的逐 bit 错误数 / 比较 bit 数；
- BLER = 期望包中未正确收到的包数 / 期望包数。

因此本阶段可以验证误码率和误帧率，但若要让任意业务 payload 在运行时具备独立的“正确/错误”判据，下一阶段仍应加入 payload CRC32C，并把 CRC failure 作为 BLER 的主要判据。

## 8. 尚未完成项

- 上行高阶调制；
- payload CRC32C；
- 基于 CQI 的自适应 MCS；
- 256-QAM 的完整实时吞吐与 BLER 扫描；
- 4×4、8×8 MIMO 信道、导频、检测和多通道射频数据面；
- 高阶 QAM 的 CUDA demapper（当前仓库实际接收路径为 CPU worker）。

建议下一步先在 Ubuntu 上跑 16-QAM、64-QAM 的完整 ChannelSimulator 扫描，确认 post-LDPC 曲线和实时 CPU 负载，再进入 2×2/4×4 MIMO。
