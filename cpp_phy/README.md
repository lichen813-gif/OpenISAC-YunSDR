# OpenISAC Windows C++ PHY 基线

这个目录是与原 Linux/UHD 实时程序分离的、无硬件依赖的 C++17 物理层核心。目标是先在 Windows 上复现并验证 Python 模型，再逐步补齐 FFT、同步、信道估计和 LDPC 译码，最后接入实时 I/O。

## 当前正式帧参数

- FFT 1024，CP 128；每帧 1 个 ZC 前导和 2 个数据 OFDM 符号；
- 672 个数据子载波，216 个梳状导频和 8 个相位参考；
- 128 个控制 RE，1216 个有效载荷物理 RE；
- 两个物理发射端口，实际载荷可由控制头切换 Rank 1/2 和 QPSK、16/64/256-QAM；
- LDPC(1008,504)，每帧按 Rank/MCS 容量容纳 2 到 19 个码块；Rank 2、64-QAM 基准为 14 块、880 字节用户数据加 2 字节 CRC；
- 发射链顺序为 CRC、LDPC 编码、扰码、21×48 分块交织、QAM 映射。

## 已实现与验证

- CRC-16/CCITT-FALSE；
- 无第三方依赖的 unitary radix-2 FFT/IFFT 和 CP 添加/移除；
- ZC 根 29 前导生成、双接收分支归一化滑动相关定时；
- 可配置确定性2×2 TDL多径信道，以及带独立收发相关系数和可重复种子的Kronecker空间相关TDL模型；
- 频分正交导频 LS 估计、周期复数线性插值；
- QPSK、16-QAM、64-QAM、256-QAM Gray 映射、硬判决和 max-log LLR；
- 正式帧资源分配、Marker、BCH(127,64) 控制字段，以及接收端基于 QPSK LLR 置信度的软 Marker 相关、最多 10 位 BCH 纠错和控制头 CRC 校验；硬标签接口继续保留用于黄金向量兼容；
- LDPC 码字 MSB-first 打包/解包、扰码、软解扰和 21×48 分块交织；
- 2×2 ZF/MMSE 检测以及工程化 Rank/MCS 控制器；
- CP 相关 CFO 估计/校正、8 个相位参考的 SFO 斜率估计与短帧相位校正；
- 面向长帧/连续流的四点三次 SFO 重采样器；
- 无第三方依赖的 LDPC(1008,504) 编码和归一化 Min-Sum 译码，支持 syndrome 早停；
- 基于实际 LS CSI、MMSE 后处理 MSE、CRC/失步状态的 Rank/MCS 推荐与迟滞控制；
- 控制头驱动的动态 Rank 1/2、QPSK/16/64/256-QAM 发射网格、软解调、LDPC/CRC 接收和跨帧控制闭环；
- 动态帧已经接入完整三径时域链：ZC 定时、CP-CFO、20 ppm SFO、FDM 导频 LS-CSI、Rank-1 MRC、Rank-2 MMSE 和控制反馈；
- 动态接收机支持跨帧 LS-CSI 指数平滑；默认验证系数 `alpha=0.25`，同步或控制头失败时立即清空历史 CSI，避免失锁后沿用旧信道；
- `DynamicLinkWorkspace` 复用时域流、FFT、信道、LLR、均衡和链路自适应顶层缓冲区；OFDM、SFO、ZC 定时和 LS-CSI 均提供复用输出接口，最大 Rank/MCS 预热后连续帧不再发生这些缓冲区的容量增长；
- 完整动态链路输出同步、FFT+CSI、检测+自适应、控制头+LDPC/CRC、仿真诊断和总耗时；真值 NMSE/EVM 诊断明确排除在接收算法耗时之外；
- 固定最大 8 流存储的通用 1×1 到 8×8 ZF/MMSE Cholesky 检测器；2×2 继续保留专用快速路径；
- 与当前正式 Rank-1/2 高阶 QAM 网格对齐的 2×2 已知波形感知信道估计，以及 64 帧距离-多普勒处理；
- 与 Python v2 黄金向量逐项核对资源索引、控制字段、14 个码块的扰码/交织结果、2432 个 QAM 符号、频域网格，以及两个数据 OFDM 符号的全部 4608 个时域复采样点。

动态帧库已经能真正按控制头切换 Rank/MCS，并可通过 `DynamicLinkReceiverState` 在连续调用间保持滤波 CSI；现有 `openisac_phy_diagnostics` 为保持历史 FER 曲线可比，仍固定发送 Rank 2/64-QAM，它输出的 Rank/MCS 是下一帧建议。尚未接入独立核心的是 4/8 发射端口的完整导频与资源网格、跨进程 CSI 状态持久化、网络实时 I/O 和最终 `libyunsdr` 采集适配器。4×4/8×8 检测内核已经迁入并通过无噪声数值回归。

## VS2019 一键构建

需要 Visual Studio 2019 的“使用 C++ 的桌面开发”工作负载。双击或在 `cmd.exe` 中运行：

```bat
cpp_phy\build_windows_vs2019.cmd
```

脚本自动寻找 VS2019，调用 v142 x64 编译环境，并使用 VS 自带的 CMake 和 Ninja 生成 Release 版本、运行测试和性能测试。使用 Ninja 是为了避开部分 Windows 环境中 `PATH` 与 `Path` 重复导致的 MSBuild 16 环境变量错误。

手动运行已生成程序：

```bat
cpp_phy\build\ninja-vs2019\openisac_phy_tests.exe
cpp_phy\build\ninja-vs2019\openisac_phy_bench.exe
```

## C++ 计算、Python 画图

运行下面的脚本即可完成 VS2019 构建、C++ 2×2 平坦信道仿真、MMSE 解调、CSV 导出和 Python 画图：

```bat
cpp_phy\run_cpp_python_plot.cmd
```

参数依次为 SNR、定时偏移样点数、TDL 多径、CFO Hz、SFO ppm 和随机种子。默认值为 40 dB、20 样点、`0:0:0+3:-4:45+9:-8:-80`、300 Hz、20 ppm、49239；每条路径格式为 `delay:gain_db:phase_deg`，批处理命令行用 `+` 分隔路径：

```bat
cpp_phy\run_cpp_python_plot.cmd 40 20 0:0:0+3:-4:45+9:-8:-80 300 20 49239
```

输出目录为 `measurement\cpp_windows_plot`：

- `waveform.csv`：C++ 计算的 ZC 前导、两个数据 OFDM 符号、定时偏移及双发双收完整时域 I/Q；
- `constellation.csv`：两层理想及 MMSE 均衡星座点；
- `metrics.csv`：SNR、BER/EVM、同步、CSI NMSE、Rank/MCS、LDPC syndrome、CRC 和 FER；
- `channel.csv`、`synchronization.csv`：逐子载波 2×2 信道响应和 ZC 搜索度量；
- `time_waveform.png`：显示完整接收流，并标注检测到的 ZC/OFDM 边界；
- `constellation.png`：Layer 0/1 的 64-QAM 星座图。
- `channel_synchronization.png`：ZC 定时峰以及四条 MIMO 链路的频率响应。

脚本不依赖 `py` 命令，优先使用 `python_phy\.venv\Scripts\python.exe`，其次查找 `python.exe`。也可以先设置完整路径：

```bat
set OPENISAC_PYTHON=C:\path\to\python.exe
cpp_phy\run_cpp_python_plot.cmd
```

Python 只负责读取 CSV 和绘图，不参与 ZC 定时、多径信道、FFT、导频 LS 信道估计、MIMO 检测、LDPC 或 BER/FER/EVM 计算。`metrics.csv` 同时给出导频估计 CSI 与完美 CSI 的结果，便于分离信道估计损失。ZC 全相关用于启动和失锁后的捕获，不应每帧执行；锁定后使用低成本 CP/导频跟踪。

短帧的 `sfo_ppm_estimated` 是相位斜率等效跟踪量，不作为晶振校准读数；当前验收依据是相位校正后的残差、BER/EVM 和 CRC。需要测量真实采样率 ppm 时，应在更长的连续样本窗口上运行时域时钟跟踪。

批量 FER 回归：

```bat
python_phy\.venv\Scripts\python.exe cpp_phy\run_cpp_regression.py
```

默认运行 28/32/36/40/42 dB、每点 5 个固定种子，输出 `measurement\cpp_windows_regression\frames.csv`、`summary.csv` 和 `ber_fer_evm.png`。

动态 Rank/MCS 与固定 Rank-2/64-QAM 的配对 TDL 回归：

```bat
cpp_phy\run_cpp_adaptive_tdl.cmd
```

默认每个 SNR 运行 100 帧，测试 28/32/36/40/44 dB，包含 300 Hz CFO、20 ppm SFO 和三径 TDL。相同种子同时运行自适应链路、原始 LS-CSI 固定 R2/64-QAM 和 `alpha=0.25` 平滑 CSI 固定 R2/64-QAM。输出位于 `measurement\cpp_adaptive_tdl`：逐帧控制轨迹 `frames.csv`、汇总 `summary.csv`、`adaptive_vs_fixed.png` 和 `csi_smoothing.png`。可用第一个参数修改每点帧数，例如 `cpp_phy\run_cpp_adaptive_tdl.cmd 500`。

最近 100 帧/点回归中，32 dB 的固定 R2/64-QAM 原始 LS-CSI FER 为 0.15，平滑后为 0；平均 EVM 从 21.19% 降至 17.94%，线性平均 CSI NMSE 从 -25.01 dB 降至 -25.59 dB。28 dB 下平滑仍能改善 NMSE/EVM，但不足以使固定 R2/64-QAM 通过 CRC，自适应链路则保持零 FER。

## 当前 VS2019 Release 基线

本机 v142/x64 测试全部通过。最近一次 Release 测量：2×2 MMSE 14.08 ns/RE，4×4 273.74 ns/RE，8×8 1305.45 ns/RE；64-QAM max-log 14.90 ns/RE，1024 点 FFT 加 CP 移除 6.63 us/符号；高 SNR LDPC 6.99 us/码块，14 块约 97.84 us/帧。64-QAM LLR 将固定 Gray 8-PAM 的同一组候选距离和比特最小值显式展开，仍保持精确 max-log 语义；2×2 检测使用厄米 Gram 矩阵解析逆和等价的后检测 MSE 对角公式；固定 1024 点 FFT 预计算位反转表和各级旋转因子。未计 LDPC 时，2×2 MMSE、软解调和每帧 4 次数据 FFT 的组合估算约 16197 帧/秒。

上述微内核数字不能直接当作完整实时吞吐量。现在已经增加完整接收链基准：

```bat
cpp_phy\run_cpp_realtime_benchmark.cmd
```

默认运行 200 对相同种子的 Rank-2/64-QAM、40 dB 三径帧，对比局部缓冲区旧路径、复用 `DynamicLinkWorkspace` 的旧顺序译码路径、固定 LDPC 池 1/2/4/8 线程，以及 8 线程加连续同步锁定态。输出 `measurement\cpp_realtime_benchmark\frames.csv`、`summary.csv` 和 `realtime_timing.png`。最近一次扩展测量使用每组 1000 帧，并在多轮连续满负载后执行：local、8 线程全搜索和 8 线程锁定态的接收中位数分别为 1193.0/897.1/800.4 us；锁定态 P95/P99 为 1159.5/1224.4 us，相对 local 为 1.49 倍。七组共 7000 帧 CRC 成功率均为 100%，所有复用路径在最大帧预热后，顶层和 LDPC 内部受跟踪容量增长均为 0。冷机低干扰轮次曾测得锁定态 741.9 us 中位数和 847.0 us P99，说明 Windows 调度、CPU 频率和温度必须纳入实时预算，正式结果采用更保守的持续负载数据。

`LdpcDecodeWorkspace` 复用 belief、边消息、判决和输入 LLR 缓冲区；`LdpcFrameDecoder` 在构造时创建固定工作线程，采用无逐码块锁的静态轮转分配，并为每个线程保留独立 Min-Sum 工作区。推荐在连续接收器生命周期内只构造一次：

```cpp
#include "openisac/ldpc_frame_decoder.hpp"

openisac::DynamicLinkWorkspace workspace;
openisac::LdpcFrameDecoder ldpc_decoder(codec, 8u);
auto result = openisac::simulate_dynamic_tdl_frame(
    mode, sequence, codec, config, &receiver_state, &workspace, &ldpc_decoder);
```

`DynamicLinkReceiverState` 现在实现 `SEARCH/TRACK/REACQUIRE`：首次捕获或失锁时对 53 个候选位置做全 ZC 搜索；锁定后默认只搜索预测位置 ±2 点，共 5 个候选。如果峰值低于绝对门限或上一帧峰值的比例门限，当帧自动退回全搜索。同步或控制头失败会清空 CSI 和定时锁定，下一帧进入重捕获。显式测试覆盖缓慢定时漂移、超窗口跳变、低 SNR 失锁和恢复。

持续负载下，8 线程锁定态平均分段为同步 24.71 us、FFT+CSI 143.16 us、检测+自适应 122.98 us、控制头 87.97 us、软解映射/解交织 202.89 us、LDPC+CRC 296.62 us。CFO 校正已改为每路一个相位步进器，锁定态同步比全搜索的 145.15 us 减少约 120.44 us。800.4 us 中位数仍是 225 us 帧间隔的 3.56 倍，因此不能宣称逐帧实时。`DynamicFrameDecodeWorkspace` 复用整帧 LLR 和 1008 元素块内交织缓冲，解扰 LFSR 也不再分配临时序列；最大帧预热后这些缓冲不再扩容。

完整链基准仍保留两级流水上限模型；此外已经实现可运行的 `DynamicFramePipeline`。它使用两个 `PreparedDynamicFrame` 槽位：调用线程完成软控制头、QAM LLR、解交织和解扰，固定后台线程调用 8 工作线程 `LdpcFrameDecoder` 完成 LDPC、打包与 CRC。槽位状态通过条件变量流转，不轮询，不复制时域/频域大数组；每个槽位只保留整帧 LLR 和一个 1008 元素块缓冲。

运行真实子流水基准：

```bat
cpp_phy\run_cpp_pipeline_benchmark.cmd 1000
```

最近 1000 帧 Rank-2/64-QAM 实测：串行软控制头+解映射+FEC 的实际间隔为 363.76 us/帧，双缓冲实际间隔为 214.12 us/帧，提升 1.70 倍、吞吐约 4670 帧/秒。生产级均值/中位/P99 为 211.90/212.90/244.10 us，后台 FEC 为 124.90/114.20/190.20 us，队列等待中位/P99 为 3.90/11.20 us，提交到完成的延迟中位/P99 为 336.10/424.00 us。1000 帧 CRC 全部通过，两个槽位和 LDPC 工作区预热后容量增长均为 0。结果位于 `measurement\cpp_pipeline_benchmark\frames.csv`、`summary.csv` 和 `pipeline_timing.png`。

214.12 us 已低于 225 us，但只证明软解映射/FEC 子链达到该吞吐。现在另有 `DynamicLinkPipeline` 完整延迟接口：两个槽位各自持有完整 `DynamicLinkWorkspace`，前台依次完成仿真发射/信道、同步、FFT/CSI、MIMO 检测及软解映射，后台固定线程池完成 LDPC/CRC；连续同步和 CSI 状态仍在前台严格按帧序更新，`receive()` 按提交顺序返回结果。

运行完整链双缓冲基准：

```bat
cpp_phy\run_cpp_full_pipeline_benchmark.cmd 1000
```

最近 1000 帧 Rank-2/64-QAM、45 dB、300 Hz CFO、20 ppm SFO、默认三径实测：完整仿真串行为 2172.05 us/帧，双缓冲为 1745.68 us/帧，即 572.84 帧/秒和 1.24 倍加速；接收前端均值/中位/P99 为 470.99/469.50/493.80 us，FEC 为 205.64/211.20/310.40 us，队列等待中位/P99 为 5.80/14.80 us，提交到完成延迟中位/P99 为 1954.00/2093.80 us。CRC 成功率 100%，预热后顶层与 LDPC 工作区容量增长均为 0。结果位于 `measurement\cpp_full_pipeline_benchmark\frames.csv`、`summary.csv` 和 `full_pipeline_timing.png`。

注意：这里的 1745.68 us 是完整**仿真器**墙钟间隔，包含发射编码、OFDM 波形生成、TDL/AWGN 和 CFO/SFO 注入，不能作为硬件接收机的纯 RX 速度。CSV 单独给出 `pipeline_receiver_front_us`，它只统计同步至软解映射的接收算法。现在已增加下述硬件采集帧入口，可直接测量不含发送和信道生成的完整接收流水吞吐。单线程固定池受 Windows 唤醒开销影响，工程默认仍建议 8 线程。

### 预生成/采集 IQ 的纯接收流水

硬件入口使用 `DynamicLinkCaptureFrame`，只包含采集序号、时间戳、导频种子和两路复数 IQ；接收配置通过独立参数传入。`DynamicLinkPipeline::submit_capture()` 复制采集 IQ 并执行同步、FFT、信道估计、控制头译码、MMSE 检测和软解调，再由后台线程完成 LDPC 与 CRC。接收端按导频种子本地重建并缓存 FDM 导频参考网格，导频种子与仿真噪声随机种子相互独立。接收器先从固定 QPSK/BCH 控制区域恢复帧序号、Rank/MCS 和负载长度，再按解出的模式处理负载；CRC 只依据接收到的数据计算，不比较已知载荷。

硬件入口现在使用独立的 `DynamicLinkReceiverConfig`，不再要求提供仿真SNR、真实CFO/SFO、真实定时偏移或TDL抽头。主要工程参数如下：

- `noise_variance_mode`：`fixed` 使用标定值，`pilot_residual` 使用两个数据OFDM符号的同一FDM导频残差自动估计；
- `fixed_noise_variance`、`minimum_noise_variance`、`maximum_noise_variance`：固定模式和自动估计的保护边界；
- `noise_smoothing_alpha`：自动估计的跨帧一阶IIR系数，默认0.25；
- `maximum_timing_offset_samples`：捕获缓冲区内允许搜索的最大前导偏移；
- `maximum_channel_delay_samples`：CP相关CFO估计使用的最大信道记忆，必须小于128点CP；
- CSI平滑和SEARCH/TRACK/REACQUIRE阈值继续由接收配置管理。

已有仿真调用保持兼容；`make_dynamic_link_receiver_config()` 可将仿真配置转换为接收配置，兼容重载默认使用固定噪声。真实采集建议显式创建 `DynamicLinkReceiverConfig`，其默认模式为导频残差自动估计。

`DynamicLinkIqFrame` 仅作为仿真测试扩展，额外保存发送模式、期望载荷和 EVM 真值。`submit_capture()` 接收其硬件基类视图，接收路径看不到这些旁路字段；`enable_truth_diagnostics=false` 关闭真实硬件中不存在的 NMSE/EVM 真值统计。保留的 `submit_iq()` 用于仿真诊断，不是硬件接入接口。

运行纯接收基准：

```bat
cpp_phy\run_cpp_iq_pipeline_benchmark.cmd 1000
```

长时间尾延迟回归可指定总帧数、循环IQ语料帧数和调度模式：

```bat
cpp_phy\run_cpp_iq_pipeline_benchmark.cmd 10000 256 normal
cpp_phy\run_cpp_iq_pipeline_benchmark.cmd 10000 256 windows-engineering
```

循环IQ语料只用于控制测试内存；每个提交仍完整执行同步、FFT、SFO、信道估计、2x2 MMSE、64-QAM软解映射、LDPC和CRC。`normal` 不修改操作系统调度；`windows-engineering` 将测试进程和接收调用线程设为“高于正常”，不自动绑核。CSV除原有分段计时外，现包含完整 submit 调用、双缓冲反压、P99.9、最大值和225 us超期统计。

最近 1000 帧 Rank-2/64-QAM 工程模式实测：IQ 生成 767.61 us/帧已在计时前完成并排除；串行接收为 350.41 us/帧，双缓冲接收为 202.80 us/帧，即 4930.96 帧/秒和 1.73 倍加速。前台提交 mean/median/P99 为 199.32/196.80/224.40 us，后台 FEC 为 138.20/131.40/240.00 us，队列等待 median/P99 为 3.90/43.60 us，IQ 提交到 CRC 完成延迟 median/P99 为 337.60/469.40 us。1000 帧 CRC 全部通过，控制头独立恢复帧序号和 Rank/MCS，预热后容量增长为 0。相邻复测为 205.82 us/帧，两轮均低于 225 us 平均帧周期。结果位于 `measurement\cpp_iq_pipeline_benchmark\frames.csv`、`summary.csv` 和 `iq_pipeline_timing.png`。

本阶段继续保持原算法语义并消除固定开销：缓存八种 Rank/MCS 帧布局；复用导频估计工作区；CSI 平滑直接更新跨帧状态；SFO 频域校正使用相位步进器；固定 1024 点 FFT 使用预计算计划。FFT 微内核由 10.05 降至 6.63 us/符号，FFT/CSI 整段由 117.02 降至 70.82 us；重复 Rank/MCS 布局构造消除后，控制阶段由约 34 us 降至 0.16 us。相对上一轮 305.38 us，最终流水间隔降至 202.80 us，改善约 33.6%。优化后前端分段均值为：同步 25.62 us、FFT/CSI 70.82 us（FFT 35.94、SFO 3.26、信道估计 23.87、平滑 7.75）、检测/自适应 26.91 us、控制头 0.16 us、软解映射 72.77 us。

202.80 us 已低于 225 us 目标约 9.9%，说明持续平均吞吐达到当前帧周期要求。它仍不是严格硬实时保证：相邻复测的前台 P99 曾达到 262 us，且后台 FEC/Windows 调度会造成队列和完成延迟尾部。硬件输入数据结构和本地导频重建已经完成；尚未连接实际 SDR/网络采集源，噪声方差和最大信道时延仍由接收配置给定。

### 10000帧持续负载和尾延迟结果

VS2019 v142/x64 Release、256帧IQ语料循环、Rank-2/64-QAM实测如下：

| 调度模式 | 双缓冲平均间隔 | 吞吐 | submit median / P99 / P99.9 / max | 超过225 us | 反压 mean / P99.9 / max |
|---|---:|---:|---:|---:|---:|
| normal | 207.47 us | 4819.96 帧/秒 | 208.00 / 235.50 / 387.10 / 990.90 us | 2.23% | 0.03 / 0.40 / 4.10 us |
| windows-engineering | 205.55 us | 4865.09 帧/秒 | 202.50 / 244.30 / 426.00 / 501.80 us | 3.13% | 0.04 / 0.50 / 3.00 us |

两种模式的10000帧CRC均为100%，预热后容量增长为0。普通模式FEC P99.9/max为278.00/645.60 us，提交到CRC完成延迟P99.9/max为631.40/1180.20 us；工程模式对应为278.40/318.30 us和587.20/702.20 us。工程优先级的平均值和最大尾部有所改善，但225 us超期比例在不同轮次有波动，不能把它视为硬实时保证。

反压P99.9不足1 us，证明两槽缓冲和后台FEC没有形成持续堵塞；异常帧通常同时放大多个前端阶段，符合Windows线程被抢占的特征，而不是某一个算法分支失控。因此当前不增加缓冲深度，也不自动选择CPU核。正式程序可在确定处理器拓扑后将采集/接收线程绑定到指定性能核，并以采集环形缓冲吸收操作系统抖动。详细结果位于 `measurement\cpp_iq_pipeline_long_run` 和 `measurement\cpp_iq_pipeline_long_run_engineering`。

### 独立接收配置与自动噪声估计结果

10000帧Rank-2/64-QAM、45 dB、300 Hz CFO、20 ppm SFO、默认三径，硬件格式入口启用 `pilot_residual`：串行接收355.95 us/帧，双缓冲207.21 us/帧，即4826.01帧/秒；submit median/P99/P99.9/max为202.00/236.80/384.90/459.40 us，225 us超期率3.09%。10000帧CRC全部通过，预热后容量增长为0。

导频残差估计平均耗时5.52 us/帧，原始和IIR后有效噪声方差均约1.356e-4。它高于45 dB纯AWGN理论值3.162e-5，因为该量同时吸收残余SFO/CFO、两符号信道不一致和信道估计误差；用于MMSE正则化和LLR缩放时比假定已知热噪声更保守。若射频前端已有可靠标定，可切换到 `fixed`。加入自动估计后，长时间平均帧周期仍低于225 us，详细结果和图表位于 `measurement\cpp_iq_receiver_config_benchmark`。

### 采集环形缓冲与文件IQ

`capture_io.hpp` 提供固定容量、线程安全的 `CaptureRingBuffer`。构造时预分配全部槽位和IQ空间，默认两路天线、每路最多4096点；队列满时 `try_push()` 返回 `false` 并丢弃最新帧，不覆盖正在处理的旧帧。这样硬件采集线程不会破坏接收线程持有的数据，也不会在稳定运行时反复申请内存。

每个 `DynamicLinkCaptureFrame` 包含：

- `capture_sequence`：采集源的64位连续帧号；
- `timestamp`：由采集源定义单位的64位硬件或系统时间戳；
- `pilot_seed`：本帧FDM导频序列种子；
- `samples[antenna][sample]`：当前为两路、等长 `complex<float>` IQ。

统计量区分三类问题：`source_sequence_gaps` 是进入环形缓冲前已经缺失的帧；`overflow_drops` 是缓冲满时本机拒绝的帧；`consumer_sequence_gaps` 是接收侧最终看到的序号缺口。另有乱序、时间戳回退和高水位统计。溢出帧仍参与源序号连续性检查，所以不会把同一问题重复记作源端丢帧。

```cpp
#include "openisac/capture_io.hpp"
#include "openisac/dynamic_link_pipeline.hpp"

openisac::CaptureRingBuffer ring(8u, 2u, 4096u);
openisac::DynamicLinkCaptureFrame capture;
capture.capture_sequence = hardware_sequence;
capture.timestamp = hardware_timestamp;
capture.pilot_seed = pilot_seed;
capture.samples = two_rx_branches;

if (!ring.try_push(capture)) {
    // 记录过载；该最新帧已丢弃，旧帧没有被覆盖。
}

openisac::DynamicLinkCaptureFrame scratch;
openisac::DynamicLinkReceiverConfig receiver;
openisac::submit_next_capture(ring, pipeline, receiver, scratch);
const auto stats = ring.statistics();
```

`IqCaptureFileWriter`/`IqCaptureFileReader` 使用版本化 `.oiq` 格式。文件头32字节，标识为 `OISACIQ1`；每帧40字节元数据头，后接按天线分支排列的 little-endian float32 I/Q。读写均为整帧批量I/O；干净文件末尾返回 `false`，损坏的头、异常尺寸或截断载荷会抛出错误。正式采集结束前调用 `writer.flush()` 检查落盘错误。PHY帧本身的CRC仍负责发现IQ损坏造成的数据错误。

写入与读取接口如下：

```cpp
openisac::IqCaptureFileWriter writer("capture.oiq", 2u);
writer.append(capture);
writer.flush();

openisac::IqCaptureFileReader reader("capture.oiq", 4096u);
while (reader.read_next(capture)) {
    // capture可送入环形缓冲或直接送入接收机。
}
```

Windows上一键回放并统计同步、控制头和CRC成功率：

```bat
cpp_phy\run_cpp_replay_iq.cmd capture.oiq 8 4096
```

采集边界长测：

```bat
cpp_phy\run_cpp_capture_io_benchmark.cmd 10000
```

本机干净构建后10,000帧Rank-2/64-QAM结果：每路3508点IQ的环形缓冲推入+弹出复制为0.94 us/帧；13.71 MiB文件样本写入1513.02 MiB/s、读取664.42 MiB/s；包含环形缓冲、同步、FFT/CSI、2x2 MMSE、软解调和LDPC/CRC的完整接收流水为207.02 us/帧，即4830.39帧/秒。CRC为10000/10000，溢出0、接收序号缺口0、预热后工作区容量增长0。文件吞吐受操作系统缓存影响，只用于确认适配层没有明显瓶颈，不作为磁盘持续写入保证。

### 最新物理层感知

`sensing.hpp`/`sensing.cpp` 在同步、去CP和FFT之后接收当前正式帧的已知发送网格与双路接收网格。原工程感知估计采用 `conj(TX) * RX`，只适合单位幅度QPSK；新实现对两个数据OFDM符号执行轻量2×2已知波形LS，因此不会把16/64/256-QAM的幅度变化误当成信道变化。单端口导频/控制RE直接相除，病态或单端口空洞在有效带内线性插值。热路径复用频响、掩码和插值工作区，不使用迭代算法、MUSIC或大矩阵求逆。

现已加入可选慢时间复均值静态杂波抑制和积分图二维CA-CFAR。默认训练窗为多普勒3、距离4，保护窗均为1，虚警概率 `1e-5`，检测后执行2×2 bin非极大值抑制；默认只搜索循环距离IFFT的非负延迟半区，避免把负延迟镜像报告成超远目标。关闭杂波抑制可保留静止目标，打开后用于抑制零多普勒直达泄漏或固定反射。

默认感知参数为1024点距离FFT、64帧/64点多普勒FFT、15 kHz子载波间隔、225 us帧周期和5.8 GHz中心频率。相干积累时间为14.4 ms；距离轴bin间隔9.759 m、理想无模糊距离约9993 m，多普勒bin间隔69.444 Hz；按单站往返路径计算，速度bin间隔1.795 m/s、无模糊速度约±57.43 m/s。距离bin间隔不等于包含窗函数和实际占用带宽后的分辨率。

VS2019 Release显式回归使用真实Rank-2/256-QAM网格验证无噪声LS，并验证强静态反射背景中的两个运动目标。CA-CFAR输出48.794 m/1.795 m/s和175.660 m/-5.384 m/s，静态反射被抑制；关闭抑制后同一静态目标可以正常检测。当前含杂波抑制和CA-CFAR的两次50批复测范围为：普通帧推入9.307–9.702 us，最后一帧连同二维变换及检测1689.210–1767.370 us，每64帧批次2276.918–2380.026 us，摊销35.577–37.188 us/帧。运行：

```bat
cpp_phy\run_cpp_sensing_benchmark.cmd 50
```

生成C++ CSV并由Python绘制距离-多普勒图、距离像、多普勒像及CA-CFAR标记：

```bat
cpp_phy\run_cpp_sensing_plot.cmd 50
```

输出位于 `measurement\cpp_sensing_plot`。另有完整时域共享前端回归：Rank-2/64-QAM信号经过TDL、300 Hz CFO、20 ppm SFO、ZC同步、去CP和FFT后，同一频域网格同时完成通信CRC和感知检测；64/64帧通信CRC通过，运动目标检测正确。当前尚未加入角度估计和实际设备连续流显示。完整记录见根目录 `最新物理层感知验证与libyunsdr路线.md`。

## 下一阶段顺序

1. 准备目标设备对应的 `libyunsdr` Windows头文件、`.lib`、DLL、驱动和最小双通道收发样例；
2. 实现连续IQ流分帧和 `libyunsdr` 薄适配器，将时间戳、overflow/underflow与采集序号映射到现有接口；
3. 先做线缆/衰减器回环，再做静止目标和1–2 m/s低速目标实测；
4. 在目标电脑上做持续采集、丢帧、时间戳和尾延迟回归；
5. 增加双接收通道联合与角度估计，再评估4×4/8×8感知资源网格。
## Windows VLC 视频信道仿真

`openisac_phy_video_bridge` 接收本机 UDP 50000 的 MPEG-TS 数据报，将其分片后逐片通过当前完整的 Rank-2/64-QAM、空间相关TDL/AWGN/CFO/SFO、MMSE、LDPC/CRC 链路，并把完整恢复的数据报发送到 UDP 50001。任一分片 CRC 失败时丢弃整个输入数据报。视频桥默认发射/接收相关系数均为0.2，可用 `--tx-correlation`、`--rx-correlation` 和 `--spatial-seed` 手动设置。

不依赖 VLC 的逐字节回环测试：

```bat
cpp_phy\test_video_channel_sim.cmd
```

一键启动信道、VLC接收端和VLC发送端：

```bat
cpp_phy\start_vlc_video_demo.cmd "D:\video\sample.mp4" 1500 45
```

带手动Rank/MCS、信道参数和实时Python监视窗口的一键入口：

```bat
cpp_phy\manual_video_phy_test.cmd
```

实时监视器由C++低频输出快照、Python/Tkinter+NumPy绘图，显示两层星座、一帧时域波形、四路2x2信道估计、EVM、频偏/SFO/定时估计、FER、信道条件数中位/P90和病态子载波比例，以及相干距离-速度热力图和CFAR目标。星座遥测按当前帧实际LDPC块数截断，不显示固定标签0的帧尾填充RE，避免短分片在64-QAM右上角形成误导性聚团；状态同时记录有效和排除符号数。视频TDL支持可选第四字段多普勒Hz：`delay:gain_db:phase_deg[:doppler_hz]`。默认每两秒额外处理一个128帧感知批次，速度bin间隔约0.897 m/s；其余时间不运行感知FFT。它会自动使用Codex随项目提供的Python环境，不要求系统存在 `py` 或把 `python` 加入PATH。

`manual_video_phy_test.cmd` 会等待视频发送完成；发送端正常结束后先等待物理层遥测静默至少3秒以排空入口队列和VLC缓存，再自动关闭本次测试启动的发送/接收VLC、Python监视器和物理层桥接器。接收VLC被关闭、桥接器退出或用户按 `Ctrl+C` 时直接清理。清理按进程ID执行，不影响测试前已经运行的其他VLC实例。

视频桥使用独立Winsock接收线程和默认8192包有界队列吸收VLC突发，日志与图形界面显示Socket接收数、队列丢包、当前深度和峰值。5 Mbit/s实际样片复测收到/恢复52983/52983包，队列峰值6958、队列丢包0、105966个PHY帧FER 0，VLC TS连续性跳变为0；旧版同线程入口在同一文件上出现3859次跳变。

动态感知回归使用9采样延迟、-8 dB、69.444 Hz运动路径：80个UDP包经192个Rank-2/64-QAM PHY帧全部恢复、FER 0；128帧感知峰值为87.830 m、1.795 m/s，CFAR最强目标正确。

空间相关TDL回归使用Rank-2/64-QAM、45 dB和相同随机种子。平坦信道下收发相关系数0/0.2/0.6/0.9的EVM约为0.91%/0.92%/1.55%/6.29%，条件数中位值约2.57/2.58/4.65/19.64；动态三径、相关系数0.2的80包综合回归得到192个PHY帧FER 0、EVM 2.02%、条件数中位/P90为2.78/4.21，并保持87.830 m、1.795 m/s感知峰值。

完整参数和分步操作见仓库根目录的 `Windows_VLC视频信道仿真使用说明.md`。
