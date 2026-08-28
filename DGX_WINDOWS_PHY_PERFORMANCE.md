# Windows CPU、DGX CPU 与 DGX CUDA 物理层性能对比

测试参数：Release、1024 子载波、CP 128、64QAM、45 dB、CFO 300 Hz、SFO 20 ppm、定时偏差 20 采样、空间相关系数 0.02、固定随机种子。Windows CPU、DGX CPU 与最新 DGX CUDA 每种模式均预热 20 包、测量 200 包、重复 5 轮，表中为中位数。

三组各 50 轮，共 150 轮；所有 UDP 数据逐字节回环一致，FER=0、UDP 丢包=0。普通帧关闭仿真专用 BER/EVM/NMSE 真值统计，周期遥测/感知帧仍保留；CUDA 加速比为 DGX CPU 延迟 / DGX CUDA 延迟，大于 1 才表示 GPU 更快。

上一轮把21×48逆交织和软解扰融合进已有的MMSE/QAM kernel，普通4端口帧直接向LDPC译码器回传最终顺序LLR，没有增加额外kernel。GPU结果与CPU参考逐项一致，decoder-order LLR最大误差为0。

接收阶段观测确认4端口接收前端中CPU信道估计占较大比例。通用 `ChannelNxN` 为8×8预留空间，原实现每帧会清零约1 MiB缓冲，而4×4实际只使用16个矩阵元素。现已改为复用缓冲并只覆盖有效元素；FDM信道估计中位耗时降低约37%到39%，NR-DMRS降低约27%到29%，四端口接收前端整体降低约12%到17%。

2026-08-28阶段进一步定位LDPC调度：DGX上高信噪比单块归一化最小和译码仅约5.6 us，但6线程帧译码曾达到约0.6 ms，主要成本是短任务的线程唤醒与同步。新增单worker调用线程直译路径后，DGX 4端口模式LDPC降到约230.5到370.3 us/frame；2/4/6/8 worker扫描均更慢。Windows多核唤醒成本较低，8 worker仍比直接路径快约55%到65%。因此自动默认值改为Windows/CPU=8、DGX/CUDA=1，`--workers N`仍可覆盖。译码核心还去除了冗余数组清零、硬判决二次合法性扫描和每块1008-float中间拷贝。

随后增加了显式 `--profile-backend` 开关，用 CUDA event 分解 OFDM 与 MIMO 的 H2D、kernel、D2H 时间；默认关闭，因此正式运行没有 event 记录开销。四端口每帧两阶段 H2D+D2H 合计约43到53 us，kernel约31 us（Rank-2）或52 us（Rank-4），CPU侧网格/SFO/信道估计仍是更大的前端开销。一次“GPU转置cuFFT输出网格”试验在每帧仅8个FFT时没有稳定收益，已撤回。保留的优化改为按子载波一次写完全部4x4通道矩阵元素，使FDM插值内存访问连续：Rank-2/Rank-4信道估计分别再降低14.8%/12.3%，接收前端降低4.5%/2.5%。

本阶段又完成了普通四端口 FDM 帧的设备驻留接收路径。cuFFT 输出不再整网格回传主机，仅回传 8 个相位参考以保持原有 SFO 加权回归；FDM 导频残差、4×4 信道插值、帧内 CSI 平均、跨帧 CSI 平滑、Rank-2 的 4×2 DFT 等效信道和控制区 MRC 均在 GPU 完成。控制头译码后，紧凑的载荷接收向量和信道矩阵直接交给原有融合 MMSE/QAM/逆交织/解扰 kernel，只回传 LDPC 顺序 LLR。周期遥测、感知、真值诊断和 NR-DMRS 帧继续走完整主机可观测路径，因此现有星座、EVM、信道和感知输出不受影响。

设备驻留专项复测使用 50 包预热、500 包测量、5 轮重复、1 个 LDPC worker。Rank-2/Rank-4 FDM 的接收前端中位数由 234.61/283.07 us 降至 192.58/227.11 us，分别改善 17.9%/19.8%；物理层延迟中位数为 1.396/1.749 ms/帧，5 轮全部 FER=0。作为对照，未进入新路径的 NR-DMRS 前端由 344.43/388.71 us 变为 346.29/395.46 us，处于运行波动范围。另用 16 帧相干感知采集验证了“CPU 诊断回退 → CUDA 普通帧”的 CSI 状态切换，Rank-2 共 170 帧、Rank-4 共 100 帧均逐字节恢复且 FER=0。

相对最初每帧回传星座/MSE并在CPU完成软解调后处理的CUDA基线，两轮累计改善如下：

| 4端口模式 | 最初CUDA ms/帧 | 当前CUDA ms/帧 | 累计延迟降低 |
|---|---:|---:|---:|
| Rank-2 FDM | 2.7930 | 1.3956 | 50.0% |
| Rank-2 NR-DMRS | 3.3134 | 1.9814 | 40.2% |
| Rank-4 FDM | 3.0527 | 1.7490 | 42.7% |
| Rank-4 NR-DMRS | 3.4996 | 2.3033 | 34.2% |

Nsight Systems显示主要可优化开销仍是主机/设备边界和同步，而不是MMSE kernel本身。此前验证的页锁定内存加异步复制方案反而使OFDM与8×8 MMSE微基准变慢，因此未保留；当前实现优先减少中间结果和CPU遍历。

| 模式 | Windows CPU ms/帧 | DGX CPU ms/帧 | DGX CUDA ms/帧 | CUDA/CPU 加速 | Win/CUDA 加速 |
|---|---:|---:|---:|---:|---:|
| siso_fdm | 0.1892 | 0.9313 | 0.3731 | 2.50x | 0.51x |
| siso_nr-dmrs | 0.2380 | 1.0342 | 0.4037 | 2.56x | 0.59x |
| mimo2_fdm | 0.3245 | 0.9482 | 0.5916 | 1.60x | 0.55x |
| mimo2_nr-dmrs | 0.3927 | 1.1696 | 0.8381 | 1.40x | 0.47x |
| stbc_fdm | 0.2340 | 0.9199 | 0.3453 | 2.66x | 0.68x |
| stbc_nr-dmrs | 0.3130 | 1.0289 | 0.4173 | 2.47x | 0.75x |
| rank2_4port_fdm | 2.1445 | 2.7828 | 1.3956 | 1.99x | 1.54x |
| rank2_4port_nr-dmrs | 3.1255 | 3.3444 | 1.9814 | 1.69x | 1.58x |
| rank4_fdm | 2.4992 | 3.0754 | 1.7490 | 1.76x | 1.43x |
| rank4_nr-dmrs | 3.4549 | 3.7084 | 2.3033 | 1.61x | 1.50x |

| 模式 | Windows CPU Mbit/s | DGX CPU Mbit/s | DGX CUDA Mbit/s | CUDA/CPU 吞吐比 |
|---|---:|---:|---:|---:|
| siso_fdm | 5.173 | 3.487 | 5.899 | 1.69x |
| siso_nr-dmrs | 3.598 | 3.003 | 4.643 | 1.55x |
| mimo2_fdm | 5.659 | 4.651 | 5.895 | 1.27x |
| mimo2_nr-dmrs | 4.014 | 3.696 | 4.380 | 1.19x |
| stbc_fdm | 3.449 | 2.933 | 4.598 | 1.57x |
| stbc_nr-dmrs | 2.280 | 2.431 | 3.321 | 1.37x |
| rank2_4port_fdm | 1.855 | 1.710 | 3.198 | 1.87x |
| rank2_4port_nr-dmrs | 1.262 | 1.403 | 2.139 | 1.52x |
| rank4_fdm | 2.715 | 2.377 | 4.193 | 1.76x |
| rank4_nr-dmrs | 1.932 | 1.935 | 3.111 | 1.61x |

## 4端口阶段耗时

以下为设备驻留专项复测（50 包预热、500 包、5 轮）的中位数，单位为微秒/帧。`front` 是同步、FFT/SFO、信道估计、检测和软解调的接收前端总时间；`LDPC` 在后台消费者中执行，并与下一帧前端部分重叠。设备驻留 FDM 的信道准备包含在 `FFT+SFO` 的后端调用中，因此 `信道估计` 接近零；比较优化前后应以 `front` 总时间为准。

| 模式 | 同步 | FFT+SFO | 信道估计 | 检测 | 前端总计 | LDPC/CRC |
|---|---:|---:|---:|---:|---:|---:|
| Rank-2 FDM | 57.3 | 98.2 | 0.09 | 36.8 | 192.6 | 226.8 |
| Rank-4 FDM | 57.5 | 114.2 | 0.11 | 55.3 | 227.1 | 368.5 |
| Rank-2 NR-DMRS | 81.0 | 55.9 | 134.9 | 74.6 | 346.3 | 225.5 |
| Rank-4 NR-DMRS | 81.1 | 60.3 | 137.6 | 114.9 | 395.5 | 365.6 |

当前Rank-2的LDPC已低于接收前端，Rank-4两者基本相当。独立CUDA LDPC需要额外H2D/D2H和kernel启动，而CPU单块仅约5.6 us，预计会倒退，因此本阶段没有加入“主机LLR进、主机比特出”的独立GPU译码器。后续若实现CUDA LDPC，应让融合MMSE/QAM kernel输出的LLR继续驻留GPU，批量译码后只回传504-bit信息块，避免新增边界复制。

## CUDA 接入范围

- SISO、2x2、STBC、4Tx/4Rx Rank-2、4Tx/4Rx Rank-4 都通过同一个 `--backend cpu|cuda|auto` 接口选择执行后端。
- CUDA 路径已经进入正式整帧接收链：批量 1024 点 OFDM FFT/去 CP；空间复用模式还包含批量 2x2、4x2、4x4 MMSE 检测。
- 4 端口空间复用把 MMSE、QPSK/16/64/256-QAM max-log软解调、21x48 逆交织和软解扰融合在同一 CUDA kernel；普通帧直接回传 LDPC 顺序 LLR，遥测帧才额外回传星座和 MSE。
- 前导同步、TDL/CFO/SFO 信道仿真、SFO 稀疏回归和 LDPC/CRC 仍在 CPU。普通四端口 FDM 的导频残差、信道插值、CSI 平滑、控制 LLR 和载荷准备已在 GPU；NR-DMRS 与诊断帧的信道估计仍在 CPU。这仍是 CPU+CUDA 混合整帧实现，不是全 GPU 实现。
- STBC 当前只有 FFT 在 CUDA，Alamouti 合并仍在 CPU，所以其 GPU 收益有限。

## 结论

- CUDA 对 4 端口模式收益最稳定；Rank-4 两种导频模式均降低整帧延迟。
- SISO、2x2、STBC 的单帧批量很小，GPU 内存传输和 kernel 启动开销占比高，不能用 CUDA 微基准的峰值加速比外推整帧结果。
- 当前 Windows CPU 仍是低阶模式最低延迟方案；DGX 的优势会随端口数和子载波批次增加而变大。
- 普通四端口 FDM 的 SFO/信道估计/MMSE 主机设备边界已经收缩；Rank-2当前主要受CPU LDPC限制，Rank-4的前端与LDPC基本均衡。下一轮优先把相同设备驻留结构扩展到 NR-DMRS，再评估让现有 LLR 继续驻留 GPU 的批量 CUDA LDPC；不会加入“主机 LLR 进、主机比特出”的孤立 GPU 译码器。

NR-DMRS 下一阶段已经开始：新增了从“每个导频一个复相关值”直接拟合公共相位与频率线性相位的紧凑接口，现有 CPU 残余相位跟踪已复用该接口，并与完整网格算法做逐项等价测试。后续 CUDA DM-RS kernel 将依次完成 OCC 匹配、两梳状端口插值、稀疏残余相位相关、CSI 平滑及紧凑控制/载荷准备；每一步保留 CPU 回退，不改变五符号帧格式和 LDPC。

发布数据：`results/benchmarks/platform-benchmark/windows-ldpc-direct-20260828/mode_performance.csv`、`results/benchmarks/platform-benchmark/dgx-cuda-ldpc-direct-20260828/mode_performance.csv`；worker扫描：`results/benchmarks/ldpc-worker-sweep-direct-v1/ldpc_worker_performance.csv`、`results/benchmarks/ldpc-worker-sweep-windows-direct-v1/ldpc_worker_performance.csv`；CUDA边界对照、缓存友好型FDM插值和设备驻留复测：`results/benchmarks/cuda-boundary-optional-profile-20260828/normal.csv`、`profiled.csv`、`cache_local_fdm.csv`、`device_resident_fdm.csv`。
