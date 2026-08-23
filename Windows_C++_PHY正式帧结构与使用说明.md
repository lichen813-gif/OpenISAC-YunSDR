# OpenISAC Windows C++ PHY 正式帧结构与使用说明

## 1. 当前冻结参数

| 项目 | 当前值 |
|---|---:|
| FFT / CP | 1024 / 128 samples |
| 子载波间隔 | 15 kHz |
| 采样率 | 15.36 Msps |
| OFDM 符号时长 | 75 us（有效 66.667 us + CP 8.333 us） |
| 帧长度 | `fdm`：3个符号/3456 samples/225 us；`nr-dmrs`：5个符号/5760 samples/375 us |
| MIMO | Windows时域及视频链支持两个物理端口Rank 1/2和四个物理端口Rank 4；8×8检测内核已具备，8×8完整时域帧尚未接入 |
| 数据调制 | 控制头动态选择 QPSK、16-QAM、64-QAM、256-QAM |
| 检测 | 2×2 专用 ZF/MMSE；通用 1×1 到 8×8 Cholesky ZF/MMSE |
| FEC | LDPC(1008,504)，归一化 Min-Sum，默认最多 6 次迭代、系数 0.8 |
| CRC | CRC-16/CCITT-FALSE，覆盖当前帧用户数据，载荷长度随 Rank/MCS 变化 |

## 2. 时域帧结构

```text
| ZC preamble | data OFDM 0 | data OFDM 1 |
| 1024+128    | 1024+128    | 1024+128    |
```

可选的统一前置DM-RS模式为：

```text
| ZC preamble | DM-RS 0 | DM-RS 1 | data OFDM 0 | data OFDM 1 |
| 1024+128    | 1024+128 | 1024+128 | 1024+128    | 1024+128    |
```

- 前导为根 29 的 1024 点 Zadoff-Chu 频域序列，经 IFFT 后添加 128 点 CP；只由 Tx0 发送，Tx1 静默。
- 接收机用双接收分支归一化 ZC 相关完成启动/失锁捕获。
- 捕获后用 CP 相关估计 CFO；估计无歧义范围为归一化 `[-0.5,0.5)`，在 15 kHz 子载波间隔下约为 ±7.5 kHz。
- CP 校正窗口跳过最大多径时延，因此要求 `maximum_path_delay < 128 samples`。
- `PilotMode::fdm`是默认值，确保旧脚本、旧IQ语料和225 us性能基准不变；`PilotMode::nr_dmrs`显式选择五符号帧。

## 3. 频域资源结构

1024 个 FFT bin 中，中心编号使用 `-512...511`。当前启用 `-448...448` 且 DC=0 置空，共 896 个活动资源：

| 资源 | 数量 | 规则 |
|---|---:|---|
| 普通数据子载波 | 672 | 非 DC、非 4 间隔导频 |
| FDM 信道导频 | 216 | 原 224 个梳状导频扣除 8 个相位参考；按频率交替分给 Tx0/Tx1 |
| 相位参考 | 8 | 两个数据符号均由 Tx0 重复发送单位 BPSK，Tx1 静默 |
| 保护带 + DC | 128 | 不发送 |

第一个数据 OFDM 符号从 672 个普通数据 RE 中抽取 128 个控制 RE：Tx0 发送 QPSK Marker+BCH mini-header，Tx1 静默。剩余有效载荷物理 RE 为：

```text
(672 - 128) + 672 = 1216 physical RE
1216 × Rank = 1216 或 2432 layer-QAM symbols
```

Rank 2、64-QAM 下，14 个 LDPC 码字占用 `14×1008/6 = 2352` 个 QAM 符号，剩余 80 个为容量 padding。控制区前 64 个 QPSK 标签是 Marker，后 64 个标签承载补零的 BCH(127,64) 码字；接收端先由 QPSK max-log LLR 计算归一化软 Marker 相关，再对 LLR 硬切片后的 BCH 码字进行最多 10 位纠错，最后检查 mini-header CRC16。旧硬标签解码接口仅作为无噪声/黄金向量兼容路径保留。

### 3.1 统一前置DM-RS端口映射

- Rank-1/SISO仍运行在两个物理射频端口框架中：有效载荷只用Tx0，但两个Tx端口都发送正交DM-RS，以便接收机保留完整2×2 CSI和后续Rank自适应能力。
- Rank-2的两个端口在每个活动子载波上同时发送，使用两个DM-RS符号的`[+,+]`与`[+,-]`时域OCC分离。
- Rank-4采用两个交织频域梳齿：梳齿0承载端口0/1，梳齿1承载端口2/3；每个梳齿内同样使用双符号时域OCC。每个端口在隔一个活动子载波处得到LS点，再做周期线性插值。
- 每个DM-RS RE的四端口总发射功率归一化为1。映射和估计均为固定次数复乘加及线性插值，不使用迭代算法或大矩阵求逆。
- 两个数据符号暂时保留原FDM/相位参考：用于导频差分噪声估计，并用已估DM-RS信道拟合公共相位和频率线性相位，跟踪前置CSI老化。这是类似NR的工程结构，不是逐字段兼容3GPP标准。

## 4. 数据处理顺序

Rank 2、64-QAM 基准发送链为：

```text
880-byte payload -> CRC16 -> 882 bytes
-> 14 × 63-byte information blocks
-> LDPC(1008,504)
-> scrambler(state=0x5A)
-> 21×48 block interleaver
-> 64-QAM -> Rank-2 layer mapping -> OFDM
```

接收执行严格逆序。LLR 约定为正数表示 bit 0、负数表示 bit 1。14 块信息共 7056 bit，即 882 字节；CRC 通过才计为有效帧。净用户数据率（不含更高层间隔）为：

```text
fdm:      880 × 8 / 225 us = 31.29 Mbit/s
nr-dmrs:  880 × 8 / 375 us = 18.77 Mbit/s
```

动态满载容量如下；表中已经扣除每帧 2 字节 CRC：

| Rank | QPSK | 16-QAM | 64-QAM | 256-QAM |
|---:|---:|---:|---:|---:|
| 1 | 124 B | 250 B | 439 B | 565 B |
| 2 | 250 B | 565 B | 880 B | 1195 B |

不足一个满载帧时允许发送较短用户数据，未使用的层符号位置作为 padding。mini-header 携带版本、Rank/MCS flags、实际信息长度、LDPC 块数和 16 位序号，接收端以控制头字段决定软解调阶数、层数和 LDPC 码块数量。

当前`nr-dmrs`不减少有效载荷RE，所以每帧字节容量与`fdm`相同；代价是帧周期增加到5/3，理论净空口吞吐降为`fdm`的60%。后续只有在验证数据符号中低密度跟踪参考可以安全稀疏化后，才会回收这部分开销。

## 5. CFO、SFO 与工程实现选择

- CFO 默认诊断值为 300 Hz，使用 CP 重复区的合并相关相位估计并对整帧旋转校正。
- SFO 定义为 `(Frx-Ftx)/Ftx × 1e6`，默认 20 ppm。
- 3符号FDM帧在20 ppm下累计漂移为`3456×20e-6=0.0691 sample`，5符号DM-RS帧为`5760×20e-6=0.1152 sample`。两种模式都用8个数据相位参考拟合跨数据符号相位斜率；DM-RS模式还把两个数据符号分别对齐到前置DM-RS信道。
- C++ 同时提供四点 Lagrange 三次重采样器，供未来长帧或连续流使用；不建议在当前短帧中多次级联重采样。
- `sfo_ppm_estimated` 是由两个数据符号的有效相位斜率换算出的跟踪量；当前 3 符号帧和 CP-CFO 联合估计下不把它作为晶振 ppm 校准读数。验收量是相位校正后的 `sfo_ppm_residual`、BER/EVM 和 CRC。

## 6. Rank/MCS 控制

接收机根据导频 LS CSI、信道 Gram 矩阵最小/最大特征值比和 MMSE 后处理 MSE，输出下一帧建议：

- Rank 2 保留特征值比 0.01 的近奇异保护，并要求瓶颈层 SINR 高于最低门限和 2 dB 实现余量；
- MCS 门限为 QPSK 4 dB、16-QAM 10 dB、64-QAM 16 dB、256-QAM 26 dB；64-QAM 门限已经按当前 LDPC/TDL 回归校准；
- Rank 1 和 Rank 2 分别计算可用 MCS，只有当 Rank-2 的 `2×Qm` 严格高于 Rank-1 的 `1×Qm` 时才升到双层，避免为了 Rank 而降低实际净容量；
- CRC 或 outage 立即降档；升档需要连续 3 帧确认并逐级提升。

动态帧 API 已把控制结果用于下一帧的实际 Rank/MCS、控制头、LDPC 容量和资源映射，并用连续帧测试验证三帧升档迟滞会依次生成 Rank-1 QPSK、Rank-1 16-QAM、Rank-1 64-QAM、Rank-2 64-QAM 帧。独立的动态 TDL 回归已经使用真正的动态发送；历史 `openisac_phy_diagnostics` 仍固定发送 Rank 2/64-QAM，以保持已有 FER 曲线可比，因此其中 `metrics.csv` 的 Rank/MCS 仍表示下一帧建议。

连续调用可传入 `DynamicLinkReceiverState` 保存上一帧 CSI，并按 `H_filtered = alpha×H_LS + (1-alpha)×H_previous` 做指数平滑。当前慢变链路回归使用 `alpha=0.25`；第一帧只初始化状态，同步或控制头失败时自动清空 CSI 与年龄计数。该实现只增加每个子载波四个复数乘加，适合后续实时 C++ 路径。

## 7. Windows 使用

VS2019 Community 已验证的一键构建：

```bat
cd /d E:\openisac\OpenISAC-main
cpp_phy\build_windows_vs2019.cmd
```

C++ 计算并由 Python 画星座、完整时域波形、信道和同步图：

```bat
cpp_phy\run_cpp_python_plot.cmd 40 20 0:0:0+3:-4:45+9:-8:-80 300 20 49239
```

六个参数依次为 SNR dB、整数定时偏移、TDL、CFO Hz、SFO ppm、随机种子。脚本不依赖 `py` 命令，优先使用 `python_phy\.venv\Scripts\python.exe`。

带视频和全部物理层参数的Windows入口支持在最后选择导频：

```bat
cpp_phy\manual_video_phy_test.cmd "E:\openisac\视频源\1920x1080_2Mbps_2.mp4" 4 64QAM 45 300 20 20 "0:0:0:0+3:-14:45:0+9:-8:-80:69.444444" 2 128 0.02 0.02 1000 nr-dmrs
```

不写最后的`nr-dmrs`时保持默认`fdm`。实时窗口会直接显示导频模式、帧符号数和帧周期。

批量 FER 回归：

```bat
python_phy\.venv\Scripts\python.exe cpp_phy\run_cpp_regression.py
```

## 8. 当前显式测试结果

统一DM-RS回归在Rank-1/2/4、64-QAM、45 dB、300 Hz CFO、20 ppm SFO和Tx/Rx相关系数0.02下各发送30个UDP数据报，全部逐字节恢复且FER为0；三种模式分别处理126、72和60个PHY帧。Rank-4固定种子100帧结果如下：

| 导频 | 帧周期 | EVM | 预FEC BER | CSI NMSE | CRC | 接收均值 |
|---|---:|---:|---:|---:|---:|---:|
| FDM | 225 us | 6.562% | 0.005047 | -25.736 dB | 100/100 | 838.996 us |
| 前置DM-RS | 375 us | 7.082% | 0.004446 | -25.683 dB | 100/100 | 986.250 us |

无CFO/SFO的30帧隔离对照中，DM-RS的EVM/CSI NMSE为5.823%/-45.982 dB，FDM为7.378%/-40.890 dB，说明梳齿+时域OCC估计本身有效；加入300 Hz/20 ppm后，前置CSI到数据区的时间差仍使EVM略高，但预FEC BER已低于FDM。当前结论是“统一帧与闭环通过，工程跟踪仍可继续优化”，不是“所有信道条件下DM-RS必然改善EVM”。原始CSV位于`measurement\dmrs_comparison\rank4_fdm`和`measurement\dmrs_comparison\rank4_nr-dmrs_tracked`。

默认三径 TDL、300 Hz CFO、20 ppm SFO、每个 SNR 5 个噪声种子：

| SNR | EVM mean | pre-LDPC BER | post-LDPC BER | FER |
|---:|---:|---:|---:|---:|
| 28 dB | 30.55% | 0.13574 | 0.07647 | 1.0 |
| 32 dB | 21.84% | 0.07521 | 0.001304 | 0.6 |
| 36 dB | 15.16% | 0.03367 | 0 | 0 |
| 40 dB | 10.48% | 0.01421 | 0 | 0 |
| 42 dB | 8.79% | 0.009269 | 0 | 0 |

25 帧的 ZC 定时均正确；CFO 平均绝对估计误差约 5.2–5.7 Hz。该样本数用于确定性显式回归，不足以代替产品级置信区间 BER/FER 测量。

本机最近一次 VS2019 Release 基准：2×2 MMSE 14.08 ns/RE、4×4 273.74 ns/RE、8×8 1305.45 ns/RE，64-QAM max-log 14.90 ns/RE，FFT 6.63 us/符号，高 SNR LDPC 6.99 us/码块（14 块约 97.84 us/帧）。64-QAM 将固定 Gray 8-PAM 的同一组候选距离和比特最小值显式展开，仍为精确 max-log；2×2 MMSE 使用厄米 Gram 解析逆和等价的后检测 MSE 对角公式；固定 1024 点 FFT 预计算位反转表和各级旋转因子。2×2 检测、软解调和四次数据 FFT 估算约 16197 帧/秒。4×4/8×8 数字仍包含固定存储 Cholesky 分解、符号求解和各层 MSE 对角计算；没有引入外部矩阵库或迭代检测。完整链路结论以本节后面的端到端基准为准。

## 9. 动态 Rank/MCS 连续 TDL 回归

运行：

```bat
cpp_phy\run_cpp_adaptive_tdl.cmd
```

测试使用默认三径、300 Hz CFO、20 ppm SFO，每个 SNR 100 帧。自适应链路从 Rank-1 QPSK 启动，升档需连续 3 帧确认；稳态窗口取最后 40% 帧：

| SNR | 自适应稳态模式 | 自适应 FER | 自适应稳态 goodput | 固定 R2/64 原始 FER | 固定 R2/64 平滑 FER | 平滑固定 goodput |
|---:|---|---:|---:|---:|---:|---:|
| 28 dB | R1/256-QAM | 0 | 20.09 Mbit/s | 1.00 | 1.00 | 0 |
| 32 dB | R1/256-QAM | 0 | 20.09 Mbit/s | 0.15 | 0 | 31.29 Mbit/s |
| 36 dB | R2/64-QAM | 0 | 31.29 Mbit/s | 0 | 0 | 31.29 Mbit/s |
| 40 dB | R2/64-QAM | 0 | 31.29 Mbit/s | 0 | 0 | 31.29 Mbit/s |
| 44 dB | R2/64-QAM | 0 | 31.29 Mbit/s | 0 | 0 | 31.29 Mbit/s |

结果说明自适应在低 SNR 避免固定高吞吐模式的链路中断，并在 36 dB 以上恢复完整双层吞吐量。32 dB 下相同种子配对测试中，CSI 平滑将固定模式总 FER 从 0.15 降到 0，平均 EVM 从 21.19% 降到 17.94%，线性平均 CSI NMSE 从 -25.01 dB 降到 -25.59 dB；28 dB 虽然 EVM 从 29.64% 降到 25.28%，但固定 R2/64 仍无法通过 CRC，不能用平滑代替链路降档。控制软头在全部 500 帧配对样本中均成功，另有单元测试显式验证 40/128 个低置信度 Marker 硬错误时软相关通过而旧硬 Marker 拒绝。100 帧/点仍属于确定性工程回归，不作为最终统计置信区间。

## 10. 帧级缓冲区与端到端耗时

连续处理时创建一个 `DynamicLinkWorkspace` 并传给每次 `simulate_dynamic_tdl_frame` 调用。工作区保留时域收发流、SFO 输出、FFT scratch、频域网格、LS-CSI、控制 LLR、均衡符号、有效噪声方差和 Rank/MCS 统计数组。先用最大容量 R2/256-QAM 预热一次后，QPSK 到 256-QAM、Rank 1/2 切换均不会使这些受跟踪的顶层缓冲区再次扩容。`release()` 用于明确释放保留内存。

底层 OFDM 调制/解调、三次 SFO 重采样、ZC 定时、FDM 导频 LS 估计和 TDL 卷积均保留原返回值接口，同时新增写入调用方已有输出缓冲区的重载。该做法不改变算法，不引入复杂矩阵库，便于后续替换仿真输入为实时 I/Q。

运行完整基准：

```bat
cpp_phy\run_cpp_realtime_benchmark.cmd
```

LDPC 实时化使用两级复用：`LdpcDecodeWorkspace` 保留输入 LLR、belief 和 check-to-variable 消息；`LdpcFrameDecoder` 在构造时启动固定线程，为每个线程保留独立工作区和结果缓冲。帧内各码块采用静态轮转分配，不在每个码块上进行锁或原子抢占。`Ldpc5041008` 只读矩阵可在线程间共享；同一个 `LdpcFrameDecoder` 同时只允许一个同步 `decode_blocks` 调用。

连续链路的推荐调用方式：

```cpp
#include "openisac/ldpc_frame_decoder.hpp"

openisac::DynamicLinkWorkspace workspace;
openisac::DynamicLinkReceiverState receiver_state;
openisac::LdpcFrameDecoder ldpc_decoder(codec, 8u);

auto result = openisac::simulate_dynamic_tdl_frame(
    mode, sequence, codec, config,
    &receiver_state, &workspace, &ldpc_decoder);
```

应先用最大 R2/256-QAM 帧预热两次，再进入计时区。传入空 `ldpc_decoder` 仍走原顺序译码，结果兼容；`ldpc_worker_threads` 和 `ldpc_capacity_growths_this_frame` 可用于确认实际线程数及稳态扩容。线程数范围为 1 到 19，但并非越多越适合所有 CPU。

连续同步使用 `DynamicLinkReceiverState` 保存预测帧起点和上一帧相关峰。状态机行为如下：

- `SEARCH`：首次调用对完整 53 个候选位置做 ZC 捕获；
- `TRACK`：默认只计算预测位置 ±2 个采样，共 5 个候选；
- `REACQUIRE`：跟踪峰低于 `tracking_min_metric` 或 `last_metric × tracking_metric_ratio` 时，当帧退回全搜索；同步或控制头失败后，下一帧也从全搜索恢复。

配置项 `enable_continuous_tracking`、`tracking_half_window_samples`、`tracking_min_metric` 和 `tracking_metric_ratio` 可调整或关闭跟踪。结果中的 `synchronization_mode_used`、`tracking_fallback`、`timing_candidates_evaluated` 和 `synchronization_lock_age_frames` 用于显式观测。测试已经覆盖每两帧漂移一个采样、超过窗口的定时突变、-20 dB 失锁以及恢复帧重捕获。

当前 VS2019 v142/x64 Release、Rank-2/64-QAM、40 dB、默认三径，每组 1000 个相同种子配对帧结果：

| 路径 | 接收 mean | median | P95 | P99 | 相对 local 中位加速 | 225 us 比值 |
|---|---:|---:|---:|---:|---:|---:|
| 每帧局部缓冲区、旧顺序译码 | 1234.1 us | 1193.0 us | 1461.6 us | 1734.5 us | 1.00x | 5.30x |
| 复用顶层工作区、旧顺序译码 | 1130.0 us | 1114.0 us | 1254.9 us | 1446.3 us | 1.07x | 4.95x |
| 固定池 1 线程 | 1178.9 us | 1124.2 us | 1588.1 us | 1760.6 us | 1.06x | 5.00x |
| 固定池 2 线程 | 1018.3 us | 946.4 us | 1238.2 us | 1457.5 us | 1.26x | 4.21x |
| 固定池 4 线程 | 985.0 us | 984.7 us | 1093.5 us | 1279.1 us | 1.21x | 4.38x |
| 固定池 8 线程、逐帧全搜索 | 910.8 us | 897.1 us | 1121.8 us | 1233.6 us | 1.33x | 3.99x |
| 固定池 8 线程、锁定态 | 878.3 us | 800.4 us | 1159.5 us | 1224.4 us | 1.49x | 3.56x |

七组共 7000 帧 CRC 成功率均为 100%，解码模式和 syndrome 统计一致。锁定态 1000 帧全部使用 5 个候选，没有发生错误回退；除故意每帧重建的 local 路径外，最大帧预热后的顶层和 LDPC 内部容量增长均为 0。1 线程固定池受 Windows 条件变量唤醒和线程切换影响，比直接顺序调用更慢。

8 线程全搜索与锁定态的平均同步耗时分别为 145.15 us 和 24.71 us，减少 120.44 us。CFO 校正每路只计算一次复数相位步长，然后递推本振相位。持续负载锁定态其余平均分段为 FFT+SFO 相位修正+LS-CSI 143.16 us、MRC/MMSE 检测与 Rank/MCS 统计 122.98 us、软控制头 87.97 us、软解映射/解交织/解扰 202.89 us、LDPC/打包/CRC 296.62 us。`DynamicFrameDecodeWorkspace` 保留整帧 LLR 和 1008 元素块内交织缓冲，解扰直接推进 LFSR，不再创建临时序列。仿真专用真值 NMSE/EVM、发射编码、TDL、AWGN 和 CFO/SFO 注入均不属于接收算法时间。

连续跟踪已经有效，但持续负载最佳中位数仍是 225 us 帧周期的 3.56 倍，P99 为 5.44 倍。冷机低干扰轮次曾测得 741.9 us 中位数和 847.0 us P99，因此 Windows 调度、CPU 温度/频率会显著影响尾延迟，工程预算采用更保守的持续结果。完整链基准中的两级模型仍只是调度上限。

现在已经实现真实的 `DynamicFramePipeline` 子流水。发送端调用 `submit(frame_id, control_llrs, equalized_symbols, variances)`；该调用在前台完成软控制头、精确 max-log LLR、块内解交织和解扰，然后把 `PreparedDynamicFrame` 放入两个固定槽位之一。后台单一消费者线程按提交顺序调用一个固定的 8 工作线程 `LdpcFrameDecoder`，`receive()` 返回解码结果并释放槽位。队列使用条件变量，不忙等；异常延迟到对应 `receive()` 抛出；结果始终按提交顺序返回。

运行：

```bat
cpp_phy\run_cpp_pipeline_benchmark.cmd 1000
```

1000 帧 Rank-2/64-QAM 实测结果：

| 指标 | 结果 |
|---|---:|
| 串行软控制头+解映射+FEC 实际间隔 | 363.76 us/帧 |
| 双缓冲实际间隔 | 214.12 us/帧 |
| 实际加速 | 1.70x |
| 实际吞吐 | 4670 帧/秒 |
| 生产级 mean / median / P99 | 211.90 / 212.90 / 244.10 us |
| FEC mean / median / P99 | 124.90 / 114.20 / 190.20 us |
| 队列等待 median / P99 | 3.90 / 11.20 us |
| 提交到完成延迟 median / P99 | 336.10 / 424.00 us |

1000 帧 CRC 成功率为 100%，帧序号、模式和用户载荷与串行路径一致；双槽位 LLR、块缓冲及 LDPC 工作区在预热后均无容量增长。结果保存在 `measurement\cpp_pipeline_benchmark\frames.csv`、`summary.csv` 和 `pipeline_timing.png`。

214.12 us 已低于 225 us 子链目标，但不等于完整接收机已经实时。

### 完整动态链双缓冲

现已增加 `DynamicLinkPipeline`。每个槽位拥有独立的 `DynamicLinkWorkspace` 和 `PreparedDynamicLinkFrame`：`submit()` 在调用线程中执行发射/信道仿真、同步、FFT/CSI、MIMO 检测及软解映射，后台消费者使用固定 8 工作线程完成 LDPC/CRC，`receive()` 按提交顺序返回并释放槽位。`DynamicLinkReceiverState` 只由前台按帧序更新，因此 SEARCH/TRACK/REACQUIRE、CSI 平滑和 Rank/MCS 统计不会被后台乱序破坏。

调用形式：

```cpp
#include "openisac/dynamic_link_pipeline.hpp"

openisac::DynamicLinkPipeline pipeline(codec, 8u);
pipeline.submit(frame_id, mode, sequence, config);
auto previous = pipeline.receive();
```

一键运行和画图：

```bat
cpp_phy\run_cpp_full_pipeline_benchmark.cmd 1000
```

VS2019 v142/x64 Release，1000 帧 Rank-2/64-QAM、45 dB、300 Hz CFO、20 ppm SFO、默认三径结果：

| 指标 | 结果 |
|---|---:|
| 完整串行仿真墙钟间隔 | 2172.05 us/帧 |
| 完整双缓冲仿真墙钟间隔 | 1745.68 us/帧 |
| 仿真墙钟加速 / 吞吐 | 1.24x / 572.84 帧/秒 |
| 接收前端 mean / median / P99 | 470.99 / 469.50 / 493.80 us |
| FEC mean / median / P99 | 205.64 / 211.20 / 310.40 us |
| 队列等待 median / P99 | 5.80 / 14.80 us |
| 提交到完成延迟 median / P99 | 1954.00 / 2093.80 us |

1000 帧 CRC 成功率 100%，双工作区和 LDPC 工作区预热后均无容量增长。12 帧显式回归还逐项比较了串行/流水的帧序、CRC、MCS、同步模式、搜索候选数、锁定帧龄及最终 CSI/同步状态，结果一致。明细位于 `measurement\cpp_full_pipeline_benchmark\frames.csv`、`summary.csv` 和 `full_pipeline_timing.png`。

必须区分两类时间：1745.68 us 包含发射编码、OFDM 波形生成、TDL/AWGN 以及 CFO/SFO 注入，是算法仿真器完整墙钟间隔；`pipeline_receiver_front_us` 才是同步到软解映射的接收前端算法时间。现在已增加硬件采集 IQ 入口并剥离发送端和信道生成，纯接收结果见下一节。

### 硬件采集 IQ 的纯接收工程路径

现已将完整链进一步拆成：

1. `generate_dynamic_tdl_iq_frame()`：编码、OFDM 发射、TDL/AWGN、CFO/SFO 注入，生成双通道复数 IQ；
2. `prepare_captured_iq_frame()`：IQ 复制、同步、CFO 校正、FFT、SFO 相位校正、本地导频重建、LS-CSI、控制头译码、MMSE 检测和软解调；
3. `finish_dynamic_tdl_frame()`：LDPC、CRC 和结果整理。

`DynamicLinkCaptureFrame` 是硬件入口格式，只包含 `timestamp`、`pilot_seed` 和两路 `samples`。接收配置作为独立参数传给 `prepare_captured_iq_frame()` 或 `DynamicLinkPipeline::submit_capture()`。接收端根据 `pilot_seed` 本地重建并缓存 FDM 导频参考网格；导频种子与每帧噪声/信道随机种子已分离，因此固定空口配置不会逐帧重建资源网格。

工程模式设置 `enable_truth_diagnostics=false`，关闭真实硬件中不存在的信道真值 NMSE 和发射符号 EVM 对比。接收顺序是先解固定 QPSK/BCH 控制区，再根据控制头恢复帧序号、Rank/MCS、负载长度并创建负载布局；LDPC 之后的 CRC 仅验证恢复数据，不比较仿真器已知载荷。`DynamicLinkIqFrame` 仍可为仿真诊断保存真值，但 `submit_capture()` 只接收其硬件基类视图，接收路径无法读取发送模式、编码帧、已知载荷或发射符号。

运行：

```bat
cpp_phy\run_cpp_iq_pipeline_benchmark.cmd 1000
```

持续负载测试：

```bat
cpp_phy\run_cpp_iq_pipeline_benchmark.cmd 10000 256 normal
cpp_phy\run_cpp_iq_pipeline_benchmark.cmd 10000 256 windows-engineering
```

三个参数依次为总帧数、循环IQ语料帧数和调度模式。循环语料避免长期测试占用过多内存，不会省略任何接收算法。`windows-engineering` 只提高测试进程和接收调用线程优先级，不自动绑核。

VS2019 v142/x64 Release，预生成 1000 帧 Rank-2/64-QAM、45 dB、300 Hz CFO、20 ppm SFO、默认三径结果：

| 指标 | 结果 |
|---|---:|
| 已排除的 IQ 生成时间 | 767.61 us/帧 |
| 串行纯接收墙钟间隔 | 350.41 us/帧 |
| 双缓冲纯接收墙钟间隔 | 202.80 us/帧 |
| 加速 / 吞吐 | 1.73x / 4930.96 帧/秒 |
| 接收前端 submit mean / median / P99 | 199.32 / 196.80 / 224.40 us |
| 后台 FEC mean / median / P99 | 138.20 / 131.40 / 240.00 us |
| 队列等待 median / P99 | 3.90 / 43.60 us |
| IQ 提交到 CRC 完成 median / P99 | 337.60 / 469.40 us |

1000 帧 CRC 成功率 100%，帧序和控制头解出的 Rank/MCS 正确，预热后顶层及 LDPC 工作区容量增长均为 0。202.80 us 已排除发送和信道仿真，低于 225 us 平均帧周期约 9.9%；相邻复测为 205.82 us/帧，两轮均达到持续吞吐目标。它仍不是严格硬实时保证：相邻复测的前台 P99 曾达到 262 us，后台 FEC 和 Windows 调度仍会形成尾延迟。明细和图表位于 `measurement\cpp_iq_pipeline_benchmark\frames.csv`、`summary.csv` 和 `iq_pipeline_timing.png`。

硬件输入缩减和本地导频重建已经完成。本阶段缓存八种 Rank/MCS 固定布局、复用导频估计工作区、让 CSI 平滑直接更新跨帧状态、以相位步进器执行 SFO 校正，并为固定 1024 点 FFT 预计算位反转表和旋转因子。FFT 微内核从 10.05 降至 6.63 us/符号，FFT/CSI 整段从 117.02 降至 70.82 us；重复布局构造消除后控制阶段从约 34 us 降至 0.16 us。流水间隔从上一轮 305.38 us 降至 202.80 us，改善约 33.6%。优化后前端分段均值为同步 25.62 us、FFT/CSI 70.82 us（FFT 35.94、SFO 3.26、信道估计 23.87、平滑 7.75）、检测/自适应 26.91 us、控制头 0.16 us、软解映射 72.77 us。硬件目标现已调整为 `libyunsdr`；实际SDK适配在感知算法和共享前端验证完成后进行。

### 10000帧尾延迟结论

| 调度模式 | 平均帧间隔 / 吞吐 | submit median / P99 / P99.9 / max | 225 us超期率 | 反压 mean / P99.9 / max |
|---|---:|---:|---:|---:|
| normal | 207.47 us / 4819.96 帧/秒 | 208.00 / 235.50 / 387.10 / 990.90 us | 2.23% | 0.03 / 0.40 / 4.10 us |
| windows-engineering | 205.55 us / 4865.09 帧/秒 | 202.50 / 244.30 / 426.00 / 501.80 us | 3.13% | 0.04 / 0.50 / 3.00 us |

两轮各10000帧CRC均为100%，预热后无容量增长。双缓冲反压P99.9不足1 us，说明队列和后台FEC不是平均吞吐瓶颈；超时帧会同时放大FFT、信道估计、检测和软解映射等多个阶段，主要属于Windows调度抢占。提高优先级能改善部分均值和最大值，但超期比例会随运行轮次波动，仍不构成硬实时保证。当前保持两槽设计，不盲目增加缓存或自动绑核；接入真实采集时应使用独立环形缓冲记录丢帧/溢出，并在明确CPU拓扑后手动指定性能核。详细CSV和图表位于 `measurement\cpp_iq_pipeline_long_run` 与 `measurement\cpp_iq_pipeline_long_run_engineering`。

### 独立接收机配置

硬件采集不再传入 `DynamicLinkSimulationConfig`，而是使用不含信道真值的 `DynamicLinkReceiverConfig`：

```cpp
openisac::DynamicLinkReceiverConfig receiver;
receiver.noise_variance_mode =
    openisac::NoiseVarianceMode::pilot_residual;
receiver.maximum_timing_offset_samples = 64u;
receiver.maximum_channel_delay_samples = 16u;  // 必须小于128点CP
receiver.noise_smoothing_alpha = 0.25f;

pipeline.submit_capture(frame_id, capture, receiver);
```

自动模式比较两个数据OFDM符号上的同一FDM导频：由第一符号形成通道预测，用第二符号残差计算每个接收支路的有效噪声，取中位数并做指数分布修正，再经过跨帧一阶IIR和上下限保护。算法只有导频遍历、一次 `nth_element` 和一个标量IIR，适合后续C++实时实现。`fixed` 模式保留给已有噪声标定的射频系统。

仿真配置到接收配置的转换函数为 `make_dynamic_link_receiver_config()`。旧的仿真重载仍可使用，默认转换为固定噪声模式；新的硬件重载完全不读取SNR、真实CFO/SFO、真实定时偏移或TDL抽头。同步全搜索上限使用 `maximum_timing_offset_samples`，并自动受实际捕获缓冲长度限制；`maximum_channel_delay_samples >= 128` 会立即拒绝。

### 自动估计性能验证

10000帧Rank-2/64-QAM硬件格式入口结果：

| 指标 | 结果 |
|---|---:|
| 串行接收 | 355.95 us/帧 |
| 双缓冲接收 | 207.21 us/帧 |
| 吞吐 | 4826.01帧/秒 |
| submit median / P99 / P99.9 / max | 202.00 / 236.80 / 384.90 / 459.40 us |
| 225 us超期率 | 3.09% |
| 自动噪声估计耗时 | 5.52 us/帧 |
| 原始 / IIR后有效噪声方差 | 1.356e-4 / 1.356e-4 |
| CRC成功率 / 预热后容量增长 | 100% / 0 |

45 dB纯AWGN理论方差是3.162e-5，导频残差值还包含残余同步和信道估计误差，因此定义为“有效噪声方差”，用于MMSE和软LLR时更保守。加入自动估计后仍满足225 us持续平均帧周期目标。结果位于 `measurement\cpp_iq_receiver_config_benchmark`。

### 采集环形缓冲

硬件采集帧现在具有独立于PHY控制头帧号的64位 `capture_sequence`，并保留64位 `timestamp` 和 `pilot_seed`。`DynamicLinkPipelineResult` 会原样返回 `capture_sequence` 与 `capture_timestamp`，可以把PHY译码失败与采集源时间线对应起来。

`CaptureRingBuffer` 在构造时固定帧槽数、天线数和每路最大采样点数，所有内部IQ空间一次性预分配。`try_push()` 和 `try_pop()` 可由采集线程、接收线程并发调用。满队列采用“拒绝最新帧”策略：返回 `false`、增加 `overflow_drops`，但不会覆盖尚未接收的旧帧。当前1024子载波、128点CP、3个OFDM符号及默认20点前置偏移的仿真采集长度为每路3508点，因此正式程序建议以4096点作为初始槽容量。

```cpp
openisac::CaptureRingBuffer capture_ring(8u, 2u, 4096u);
openisac::DynamicLinkCaptureFrame capture;
capture.capture_sequence = hardware_sequence;
capture.timestamp = hardware_timestamp;
capture.pilot_seed = pilot_seed;
capture.samples = rx_iq;

const bool accepted = capture_ring.try_push(capture);
openisac::DynamicLinkCaptureFrame scratch;
if (accepted) {
    openisac::submit_next_capture(
        capture_ring, pipeline, receiver, scratch);
}
const auto statistics = capture_ring.statistics();
```

统计含义：

| 字段 | 含义 |
|---|---|
| `push_attempts` | 采集源尝试提交的帧数 |
| `frames_pushed` / `frames_popped` | 实际进入/离开环形缓冲的帧数 |
| `overflow_drops` | 队列满而拒绝的最新帧数 |
| `source_sequence_gaps` | 进入本机缓冲前已存在的序号缺口 |
| `consumer_sequence_gaps` | 接收侧最终看到的缺口，包含本机溢出影响 |
| `out_of_order_frames` | 源端重复或倒序帧数 |
| `timestamp_regressions` | 时间戳回退次数 |
| `high_watermark` | 历史最大占用槽数 |

### `.oiq` 文件格式与回放

文件使用跨Windows/Linux一致的little-endian格式：

| 区域 | 长度 | 内容 |
|---|---:|---|
| 文件头 | 32字节 | `OISACIQ1`、字节序标记、版本、天线数、complex-float32格式号 |
| 帧头 | 40字节 | 帧标记、头长度、采集序号、时间戳、导频种子、每路采样数、载荷字节数 |
| IQ载荷 | 可变 | branch-major排列，每点依次为float32 I、float32 Q |

`IqCaptureFileWriter::append()` 整帧写入，结束采集前使用 `flush()` 检查落盘；`IqCaptureFileReader::read_next()` 在干净EOF返回 `false`，遇到错误文件头、异常天线/采样尺寸或截断帧时抛出异常。文件适配器最多接受64路天线，当前接收算法入口仍明确要求2路。

```cpp
openisac::IqCaptureFileWriter writer("capture.oiq", 2u);
writer.append(capture);
writer.flush();

openisac::IqCaptureFileReader reader("capture.oiq", 4096u);
while (reader.read_next(capture)) {
    capture_ring.try_push(capture);
}
```

Windows手动回放：

```bat
cpp_phy\run_cpp_replay_iq.cmd capture.oiq 8 4096
```

程序输出读入/提交/完成帧数，同步、控制头和CRC成功数，以及源序号缺口、乱序、时间戳回退、环形缓冲溢出、接收侧缺口和高水位。

### 采集边界性能验证

运行：

```bat
cpp_phy\run_cpp_capture_io_benchmark.cmd 10000
```

VS2019 x64 Release、本机10,000帧Rank-2/64-QAM结果：

| 指标 | 结果 |
|---|---:|
| 每路采样数 | 3508点 |
| 环形缓冲推入+弹出复制 | 0.94 us/帧，111.13 GiB/s |
| 13.71 MiB `.oiq` 写入/读取 | 1513.02 / 664.42 MiB/s |
| 环形缓冲到CRC完整流水 | 207.02 us/帧，4830.39帧/秒 |
| CRC成功率 | 10000/10000 |
| 溢出 / 接收序号缺口 | 0 / 0 |
| 预热后工作区容量增长 | 0 |

文件吞吐包含Windows文件缓存影响，不代表物理磁盘的持续写速；环形缓冲复制约占完整帧周期的0.45%，当前不是性能瓶颈。

### 当前正式帧的感知升级

新增 `DynamicSensingProcessor`，直接使用正式帧的两个数据OFDM符号、两路发送参考网格和两路接收频域网格。Rank-2资源单元使用2×2已知波形LS，单端口资源单元直接相除，其余有效带空洞插值，因而兼容非单位幅度的高阶QAM。64帧距离-多普勒回归在目标bin(5,+1)得到48.794 m、1.795 m/s；序号缺口及时间戳回退会清空不完整批次，避免跨丢帧做错误相干积累。

现已加入可选慢时间复均值杂波抑制和积分图二维CA-CFAR。强静态反射加两个运动目标回归输出48.794 m/1.795 m/s和175.660 m/-5.384 m/s，静态反射被抑制；关闭抑制后静止目标仍可检测。默认检测范围限制在循环距离IFFT的非负延迟半区，排除负延迟镜像。

默认64帧相干时间14.4 ms，距离/速度轴bin间隔分别为9.759 m和1.795 m/s（5.8 GHz、单站往返定义）。含杂波抑制与CA-CFAR的两次50批VS2019 Release复测摊销35.577–37.188 us/帧，最后一帧连同二维变换及检测为1689.210–1767.370 us。该变换按64帧批处理，不占用每个225 us通信帧的关键路径。手动运行：

```bat
cpp_phy\run_cpp_sensing_benchmark.cmd 50
cpp_phy\run_cpp_sensing_plot.cmd 50
```

完整时域共享前端也已验证：Rank-2/64-QAM经过TDL、300 Hz CFO、20 ppm SFO、ZC同步、去CP和FFT后，同一接收网格同时送入通信与感知，64/64帧CRC通过，延迟5点、慢时间+1 bin目标检测正确。仿真器现在保留本地正式发送网格作为感知参考，真实硬件由本地发射链提供同一参考。

最终硬件只接 `libyunsdr`。官方2026.2公开头文件已确认提供设备开关、RX/TX采样率与本振配置、多端口读写、时间戳和通道overflow/underflow事件接口；但当前工作区没有目标设备对应的Windows `.lib`、DLL、驱动或厂家样例，因此本轮不提交不可链接、不可运行的设备代码。取得这些文件后，适配器填充现有 `DynamicLinkCaptureFrame`/采集环形缓冲，并映射硬件时间戳、连续序号和事件；同步、MIMO检测、LDPC和感知核心保持不变。

### Rank-4正式频域帧手动验证

Rank-4已完成四端口正式发送网格、FDM导频信道估计、控制头MRC软译码、4×4 MMSE、LDPC和CRC闭环。默认运行：

```bat
cd /d E:\openisac\OpenISAC-main
cpp_phy\run_cpp_4x4_formal_diagnostics.cmd
```

手动选择参数：

```bat
cpp_phy\run_cpp_4x4_formal_diagnostics.cmd --frames 20 --snr 50 --modulation 64QAM --payload-bytes 1132 --tx-correlation 0.2 --rx-correlation 0.2 --check --output measurement\cpp_4x4_formal_diagnostics
```

`--modulation`支持 `QPSK`、`16QAM`、`64QAM`、`256QAM`；`--payload-bytes 0`表示使用当前MCS最大容量；`--check`要求每一帧控制头、CRC和用户负载全部通过。输出目录包含汇总CSV、四层星座CSV和PNG图。这个入口尚未加入ZC前导、CFO/SFO与视频UDP，不能等同于现有2×2连续时域链路。

### Rank-4四通道时域同步验证

第三阶段入口已加入端口0 ZC前导、四路联合定时、CP-CFO、两数据符号SFO相位校正和连续 `SEARCH/TRACK/REACQUIRE` 状态：

```bat
cpp_phy\run_cpp_4x4_time_diagnostics.cmd
```

默认注入300 Hz CFO、20 ppm SFO，定时从20点开始并每帧漂移1点。手动参数示例：

```bat
cpp_phy\run_cpp_4x4_time_diagnostics.cmd --frames 20 --snr 50 --modulation 64QAM --payload-bytes 0 --timing 20 --timing-drift 1 --cfo 300 --sfo 20 --tx-correlation 0.2 --rx-correlation 0.2 --ldpc-threads 12 --check --output measurement\cpp_4x4_time_diagnostics
```

`frames.csv`逐帧记录同步模式、定时、CFO/SFO、噪声估计、EVM、CSI NMSE、CRC和耗时；`summary.csv`给出汇总和各阶段均值；星座图显示四层MMSE输出。Rank-4接收器复用FFT、网格、CSI、噪声样本、控制LLR、均衡符号及动态帧译码工作区，并将18个LDPC码块分配给持久线程池。默认12线程，可用 `--ldpc-threads 1..19` 调整；应以目标电脑的P99而不是只看均值选线程数。

本机50帧VS2019 Release、12线程复测：CRC及负载50/50，纯接收均值856.5 us、P50 820.9 us、P99 974.4 us；原基线均值1504.9 us，提升1.76倍。平均分段为同步55.3 us、FFT+SFO 62.8 us、CSI 122.0 us、控制头及4×4检测236.3 us、软解映射98.4 us、LDPC/CRC 281.7 us。LDPC实际最大迭代数为2；预热后的接收和LDPC缓冲区扩容帧数均为0。当前三符号空口帧长225 us，接收均值仍约为空口周期3.8倍。另需注意，这个入口先按Rank-4导频模板估计CSI，再用控制头验证Rank/MCS；统一Rank-1/2/4盲选模板尚未合并。

### Rank-4双缓冲流水验证

`Rank4TimePipeline` 有两个独立 `Rank4TimeWorkspace` 槽位。前台完成波形仿真、同步、FFT/SFO、CSI、控制头、4×4 MMSE和软解映射；后台固定线程池只执行LDPC、字节打包和CRC。一个后台消费者保证输出顺序与提交顺序一致。物理同步状态在定时和控制头通过后即可进入TRACK，不等待后台CRC；CRC仍决定载荷能否交付。

运行200帧、12线程基准：

```bat
cd /d E:\openisac\OpenISAC-main
cpp_phy\run_cpp_4x4_time_pipeline_benchmark.cmd 200 12
```

本机结果为CRC及载荷200/200通过、预热后工作区和LDPC扩容均为0。串行接收均值810.9 us；前端均值/P50/P99为534.7/532.1/563.4 us，后台FEC为283.7/284.7/364.6 us，队列等待P50/P99为9.4/25.2 us。若输入已经是四路采集IQ，双缓冲的工程服务周期预计受534.7 us前端限制；但当前 `submit()` 内仍包含发射与TDL信道生成，实际完整仿真间隔只从4136.0 us降到3463.3 us，即1.19倍。结果位于 `measurement\cpp_4x4_time_pipeline`；在加入四路预生成/采集IQ入口前，不把534.7 us作为实测硬件吞吐。
