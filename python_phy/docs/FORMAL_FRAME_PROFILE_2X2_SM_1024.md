# 2×2空间复用正式仿真帧结构与参数基线

版本日期：2026-08-22  
适用范围：OpenISAC纯Python跨平台PHY黄金模型，Windows/Linux算法仿真与后续C++对照。  
对应配置：`configs/mimo_2x2_spatial_multiplexing_realtime_1024.yaml`

> 本文冻结的是已经通过自动测试的算法仿真基线，不是与5G、Wi-Fi或商用终端互通的空口标准。当前帧没有PHY控制头、编码块描述或MCS信令，收发两端必须使用相同配置。

## 1. 正式帧结构

一个完整帧固定包含3个CP-OFDM符号：

```text
帧开始
┌────────────────────────┬────────────────────────┬────────────────────────┐
│ Symbol 0               │ Symbol 1               │ Symbol 2               │
│ ZC同步前导             │ MIMO数据时隙0          │ MIMO数据时隙1          │
│ Tx0发送，Tx1静默       │ 数据+FDM导频+相位参考  │ 数据+FDM导频+相位参考  │
├──────────┬─────────────┼──────────┬─────────────┼──────────┬─────────────┤
│ CP 128   │ FFT 1024    │ CP 128   │ FFT 1024    │ CP 128   │ FFT 1024    │
└──────────┴─────────────┴──────────┴─────────────┴──────────┴─────────────┘
帧结束
```

- 每个OFDM符号：1152个复采样；
- 每帧：3456个复采样；
- 有效符号时长：66.6667 µs；
- CP时长：8.3333 µs；
- 单个CP-OFDM符号：75 µs；
- 完整帧时长：225 µs。

### Symbol 0：ZC同步前导

- FFT长度：1024；
- ZC root：29，与1024互质；
- Tx0在完整频域网格上发送OpenISAC兼容ZC序列；
- Tx1保持静默；
- 用途：帧起点匹配滤波、整数采样定时和后续CP粗CFO估计；
- 前导不承担双发射MIMO信道估计，双Tx CSI由后续FDM导频完成。

### Symbol 1/2：2×2空间复用数据符号

- 每个数据子载波同时发送2层独立64-QAM；
- Layer 0映射到Tx0，Layer 1映射到Tx1；
- 数据总功率按`1/sqrt(2)`归一化，确保增加空间层后总发射功率仍为1；
- 两个数据符号分别携带独立数据，同时分别放置FDM信道导频；
- 8个相位参考在两个数据符号中重复，用于差分公共相位和SFO斜率估计。

## 2. OFDM与子载波分配

| 参数 | 正式值 |
|---|---:|
| FFT | 1024 |
| CP | 128 samples |
| 子载波间隔 | 15 kHz |
| 复采样率 | 15.36 Msps |
| 左保护带 | 64 tones |
| 右保护带 | 63 tones |
| DC | 置零 |
| 活动子载波 | 896 |
| 数据子载波 | 672 |
| 信道导频 | 216 |
| 独立相位参考 | 8 |
| 空/保护/DC子载波 | 128 |

频率索引采用居中编号`k=-512...511`，有效带宽范围为`-448...448`并去除DC。原始comb位置满足：

```text
k mod 4 = 0
```

原始224个comb位置中，8个均匀分布位置保留为相位参考，其余216个用于CSI估计。正式相位参考位置为：

```text
-448, -320, -192, -64, 64, 192, 320, 448
```

216个CSI导频按频率顺序交替分配给Tx0/Tx1，因此每根发射天线在每个数据OFDM符号中获得108个正交导频。某个导频RE只有被分配的Tx发送单位功率BPSK，另一Tx静默。

8个相位参考RE由Tx0发送单位符号，Tx1静默，并在Symbol 1/2重复。接收端利用两个符号的复相关消除未知Tx0信道，再拟合`phase(k)=intercept+slope×k`。在实测边界`|SFO|<=20 ppm`内，8点闭式拟合已经足够，并比32点方案增加24个CSI导频。

## 3. 数据帧、调制与CRC

| 项目 | 正式值 |
|---|---:|
| 调制 | 64-QAM，Gray标签，单位平均功率 |
| 空间层 | 2 |
| 数据OFDM符号 | 2 |
| 每符号数据QAM数 | 672×2 = 1344 |
| 每帧数据QAM数 | 2688 |
| 每QAM比特 | 6 |
| 每帧编码前比特容量 | 16128 bits |
| 用户载荷 | 2014 bytes |
| CRC | CRC-16/CCITT-FALSE，2 bytes |
| 填充 | 0 bits |

当前比特帧格式为：

```text
┌─────────────────────────────────────────────┬──────────────────┐
│ Payload：2014 bytes                         │ CRC-16：2 bytes   │
└─────────────────────────────────────────────┴──────────────────┘
                       合计2016 bytes / 16128 bits
```

CRC参数：多项式`0x1021`、初值`0xFFFF`、非反射、无最终异或，即CRC-16/CCITT-FALSE。本节表格仍描述无FEC基线配置`mimo_2x2_spatial_multiplexing_realtime_1024.yaml`。正式LDPC配置另见`mimo_2x2_spatial_multiplexing_ldpc_realtime_1024.yaml`：它使用128个Tx0单层QPSK控制RE、14个LDPC(1008,504)块、882字节信息区和880字节用户载荷；完整定义见`docs/LDPC_MIMO_FORMAL_FRAME.md`。

## 4. 正式接收机处理顺序

```text
Rx连续复采样
  → ZC匹配滤波帧定时
  → CP相位粗CFO估计与校正
  → 单次OFDM解调
  → 相位参考差分相位/斜率拟合
  → 每帧SFO估计及相干性门限
  → 对Symbol 2执行频域公共相位和线性相位斜率校正
  → 每个数据符号独立FDM导频CSI估计
  → 2×2 MMSE检测
  → 64-QAM Max-Log软LLR
  → 去交织、软解扰、LDPC译码和CRC
  → CRC-16检查、BER/FER/BLER/EVM统计
```

工程主链不执行时域SFO重采样。相位校正只使用相位参考得到的估计，不读取仿真注入的真实`sfo_ppm`；相干性低于0.8时不应用校正。`sinc8/sinc24`重采样代码继续保留，用于超过20 ppm的压力测试和算法研究，不进入第一版C++实时接收路径。

## 5. 正式参数基线

### 发射与资源参数

| 参数 | 值 |
|---|---:|
| `modulation` | `64qam` |
| `nt / nr / layers` | `2 / 2 / 2` |
| `mode` | `spatial_multiplexing` |
| `detector` | `mmse` |
| `pilot_spacing / offset` | `4 / 0` |
| `phase_reference_count` | `8` |
| `pilot_phase_min_coherence` | `0.8` |

### 同步与接收参数

| 参数 | 值 |
|---|---:|
| ZC前导 | 开启 |
| 定时/CFO同步 | 开启 |
| `zc_root` | `29` |
| `timing_search_samples` | `32` |
| SFO斜率跟踪 | 开启 |
| SFO闭环重采样 | 关闭，仅保留频域相位斜率校正 |
| SFO插值器 | `sinc8`代码保留但工程配置不调用 |
| CSI估计 | `ls_linear`（当前正式Rayleigh平坦信道基线） |
| 信道估计长度 | `10` samples |

对于频率选择性TDL，应优先使用`lmmse`或`ls_dft`，且`channel_estimation_taps`必须覆盖最大有效路径时延。最大TDL时延不得超过CP 128 samples；同步开启时必须严格小于CP。

### 正常运行与压力测试的区别

- 正常无损伤配置：`timing_offset_samples=0`、`cfo_hz=0`、`sfo_ppm=0`、`doppler_hz=0`；接收补偿模块仍保持开启。
- 正式C++实时算法范围：`|sfo_ppm|<=20`，只使用8个参考音的频域相位斜率校正，不做第二遍FFT和时域重采样。
- 工程配置使用100 Hz逐采样连续Doppler作为保守验收点；实际Doppler应按`fd=v×fc/c`由载频计算。
- 50/500 ppm和200 Hz以上Doppler属于扩展压力测试，不作为第一版接收机的主要设计点。
- Monte-Carlo随机种子只用于结果复现，不属于空口参数。

## 6. 速率与验收基线

- 毛PHY速率：71.680 Mb/s；
- 扣除CRC后的净载荷上限：71.6089 Mb/s；
- 20 ppm、40 dB、200帧、8参考音、无重采样：BER 0.001132、EVM 0.04067、CRC goodput 62.658 Mb/s；
- 同条件sinc8重采样：BER 0.001116、EVM 0.03892、CRC goodput 63.016 Mb/s；
- 无重采样方案保留sinc8约99.4%的goodput，并消除约248 Mtap-MAC/s重采样计算和第二遍FFT；
- 20 ppm与100 Hz连续Doppler组合结果：BER 0.001277、EVM 0.04238、CRC goodput 61.584 Mb/s；
- 100 Hz相对零Doppler保留约98.3%的CRC goodput，因此第一版不加入ICI抵消；
- 500 ppm、24抽头离线压力结果：BER 0.01439、EVM 0.11999、CRC goodput 54.065 Mb/s；
- pytest：103/103通过；
- 显式检查：96/96通过；
- 实时插值对照：6/6通过；极端SFO对照：6/6通过。

## 7. 手动复验

```bat
cd /d C:\path\to\OpenISAC-YunSDR\python_phy
.venv\Scripts\python.exe experiments\compare_spatial_sfo_resampling.py
.venv\Scripts\python.exe experiments\compare_realtime_sfo_interpolators.py
.venv\Scripts\python.exe experiments\validate_engineering_operating_region.py
.venv\Scripts\python.exe -m pytest -q
.venv\Scripts\python.exe experiments\validate_explicit.py --frames 100
```

输出目录：

```text
C:\path\to\OpenISAC-YunSDR\measurement\spatial_sfo_closed_loop
C:\path\to\OpenISAC-YunSDR\measurement\engineering_operating_region
C:\path\to\OpenISAC-YunSDR\measurement\python_phy_explicit_validation
```

## 8. 当前边界

- 没有PHY控制头，调制、层数、FFT、CP和载荷长度由收发双方预配置；
- Python已有独立LDPC黄金链，但本正式OFDM配置仍是CRC-only；下一步加入128个QPSK控制RE和LDPC高阶QAM载荷；
- 当前SFO估计仅使用两个数据OFDM符号，残余偏差还可通过多符号训练和平滑降低；
- 已支持逐采样连续Doppler和符号内ICI；当前尚未加入专用ICI估计/抵消算法；
- C++实时复杂度边界见`docs/CPP_REALTIME_FEASIBILITY.md`；目标CPU上的真实p50/p99延迟仍需C++实现后实测；
- 该帧是Python/C++算法黄金基线，转入真实硬件前还需加入增益控制、量化、射频滤波、PA/ADC非线性和实测时钟模型。
