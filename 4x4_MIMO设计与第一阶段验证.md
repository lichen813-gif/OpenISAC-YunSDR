# OpenISAC 4×4 MIMO设计与第一阶段验证

## 1. 阶段目标

本阶段先建立独立于现有2×2正式视频物理层的Rank-4算法闭环，验证以下关键模块：

```text
4层高阶QAM映射
-> 4路1024点OFDM与128点CP
-> 4×4空间相关TDL多径与AWGN
-> 4接收端FFT
-> 16路FDM导频LS信道估计与频域插值
-> 通用4×4 MMSE Cholesky检测
-> BER、EVM、完美CSI EVM和CSI NMSE统计
```

这一闭环不包含正式控制头、ZC前导、CFO/SFO、LDPC/CRC和视频UDP，目的是先稳定4端口的导频、信道和检测算法，再修改已经通过长期测试的2×2动态链路。

## 2. 已实现接口

- `ChannelNxN`和`detect_nxn()`：固定最大8流存储，支持1×1至8×8 ZF/MMSE；4×4采用Cholesky分解，不执行通用矩阵求逆。
- `ImpulseResponseNxN`和`build_correlated_tdl_nxn()`：支持1至8端口方阵TDL；相关矩阵采用 `R[i,j] = rho^abs(i-j)` 指数Toeplitz模型，并以Kronecker结构生成空间信道。
- `apply_tdl_nxn_symbol()`：对每个收发端口执行时域多径卷积。
- `estimate_fdm_pilot_channel_linear_nxn()`：支持1至8端口；输入按 `[时间][FFT][端口]` 排列，每个导频RE只允许一个发射端口激活。
- `simulate_nxn_ofdm_link()`：可配置流数、调制、SNR、TDL、收发相关系数、种子和帧数的纯算法仿真入口。

所有新增热路径都使用固定最大8端口的小矩阵或预分配工作区，没有迭代检测、SVD或动态大矩阵求逆，适合后续C++实时化。

## 3. 4×4导频设计

1024点FFT中继续使用896个有效子载波并保留DC。Rank-4第一版采用频分导频：

| 项目 | 数值 |
|---|---:|
| 总导频间隔 | 2个子载波 |
| 单端口导频间隔 | 8个子载波 |
| 每符号导频RE | 448 |
| 每端口导频RE | 112 |
| 每符号数据子载波 | 448 |
| Rank-4数据QAM符号 | 448×4 = 1792 |

最初曾直接沿用2×2的总导频间隔4。四端口轮流分配后，每端口间隔变为16，三径信道下CSI NMSE约−31.8 dB，经过Rank-4矩阵求解后EVM达到15.6%。把总间隔改为2后，CSI NMSE改善到约−41.7 dB，EVM降至约6.6%。

作为初步资源比较，当前2×2网格每个数据OFDM符号约为672个数据子载波×2层，即1344个空间QAM RE；4×4密集导频网格为1792个，增加约33.3%。正式帧接入后还要扣除控制和相位参考RE，因此最终净载荷需以LDPC块布局重新计算。

## 4. Windows手动验证

在VS2019环境下执行：

```bat
cd /d E:\openisac\OpenISAC-main
cpp_phy\run_cpp_4x4_diagnostics.cmd
```

默认参数是Rank-4、64-QAM、45 dB、10个OFDM帧、Tx/Rx相关系数0.2和三径TDL。程序生成：

- `measurement\cpp_4x4_diagnostics\summary.csv`
- `measurement\cpp_4x4_diagnostics\constellation.csv`
- `measurement\cpp_4x4_diagnostics\rank4_constellation.png`

可手动设置参数，例如：

```bat
cpp_phy\build\ninja-vs2019\openisac_phy_4x4_diagnostics.exe --frames 100 --snr 45 --modulation 64QAM --pilot-spacing 2 --tx-correlation 0.2 --rx-correlation 0.2 --seed 311383 --check
python_phy\.venv\Scripts\python.exe cpp_phy\plot_4x4_diagnostics.py measurement\cpp_4x4_diagnostics
```

`--check`当前使用第一阶段的未编码验收门限：BER小于1%、EVM小于8%。这不是正式链路FER门限；接入LDPC/CRC后必须改为以CRC和FER为最终判据。

## 5. 当前验证结果

默认Rank-4/64-QAM、45 dB、Tx/Rx相关系数0.2、三径TDL、10帧结果：

| 指标 | 结果 |
|---|---:|
| 导频/数据子载波 | 448 / 448 |
| 检测QAM符号 | 17,920 |
| 比较比特 | 107,520 |
| 未编码BER | 0.00505 |
| 估计CSI EVM | 6.59% |
| 完美CSI EVM | 4.35% |
| CSI NMSE | −41.72 dB |

Windows VS2019全量C++测试通过，新增固定种子回归同时检查4×4端口数、导频/数据资源数量、检测符号数、BER、EVM和CSI NMSE。

## 6. 下一阶段正式帧接入

1. 将控制字段的Rank编码从1位扩展为两位：`00=Rank-1`、`01=Rank-2`、`10=Rank-4`，保留 `11`。
2. 将正式帧布局缓存由8种扩展为12种，即Rank-1/2/4乘以QPSK、16/64/256-QAM。
3. Rank-4采用总间隔2的FDM导频；Rank-1/2保持现有网格和黄金向量不变。
4. ZC前导先只从端口0发送，四路接收共同完成定时与CFO估计，避免增加前导长度。
5. 控制区域继续由端口0发送，利用四路接收CSI做MRC；载荷区域使用4×4 MMSE。
6. 接入现有LDPC(1008,504)、CRC16和软解调，进行BER/FER/EVM与吞吐测试。
7. 通过动态帧回归后，再扩展视频桥、四层实时星座和16路信道显示。

4×4感知、阵列测角和真实四通道硬件接口不在下一小阶段内，待4×4通信正式帧稳定后再加入。

## 7. 第二阶段完成：正式帧与LDPC/CRC闭环

第二阶段已经完成第1、2、3、5、6项，并保持原Rank-1/2黄金向量不变。新增内容如下：

- 控制头低两位正式定义为 `00=Rank-1`、`01=Rank-2`、`10=Rank-4`、`11=保留`；
- 动态布局缓存扩展为Rank-1/2/4乘以四种MCS，共12种；
- Rank-4正式网格为4个物理端口、总导频间隔2、每端口导频间隔8；
- 控制区域仍只由端口0发送，仿真接收端使用4路MRC软判决；
- 载荷使用4×4 MMSE Cholesky检测，输出后处理MSE供max-log软解调；
- 直接复用生产链路的CRC16、LDPC(1008,504)、扰码和21×48交织实现；
- 新增 `simulate_rank4_formal_link()` 可复用接口及自动回归。

Rank-4/64-QAM正式布局如下：

| 项目 | 数值 |
|---|---:|
| FFT / CP | 1024 / 128 |
| 数据OFDM符号 | 2 |
| 物理发射/接收端口 | 4 / 4 |
| 每符号数据/全部导频 | 448 / 448 |
| 普通FDM导频/相位参考 | 440 / 8 |
| 控制RE | 128，位于第一个数据符号、端口0 |
| 载荷层QAM位置 | 3072 |
| 有效LDPC码块 | 18 |
| 用户数据/CRC | 1132 / 2字节 |

固定种子、50 dB、Tx/Rx相关系数0.2、三径TDL、4帧满载验证结果：

| 指标 | 结果 |
|---|---:|
| 控制头通过 | 4 / 4 |
| CRC通过 | 4 / 4 |
| 用户负载逐字节一致 | 4 / 4 |
| LDPC syndrome失败 | 0 |
| LDPC前硬判决BER | 0.0039 |
| EVM | 6.07% |
| CSI NMSE | −41.01 dB |

说明：这里的50 dB是接收采样点AWGN参数，EVM主要受四端口FDM导频插值误差与4×4矩阵条件数限制，因此不会简单等于理想AWGN的0.316%。LDPC已将约0.39%的前向硬判决误码全部纠正，最终CRC和负载均通过。

Windows手动执行：

```bat
cd /d E:\openisac\OpenISAC-main
cpp_phy\run_cpp_4x4_formal_diagnostics.cmd
```

自定义示例：

```bat
cpp_phy\run_cpp_4x4_formal_diagnostics.cmd --frames 20 --snr 50 --modulation 64QAM --payload-bytes 1132 --tx-correlation 0.2 --rx-correlation 0.2 --check --output measurement\cpp_4x4_formal_diagnostics
```

输出为 `summary.csv`、`constellation.csv` 和四层星座图 `rank4_constellation.png`。

## 8. 当前边界与第三阶段

当前正式Rank-4闭环从两个数据OFDM符号开始，接收端已知本次测试使用Rank-4导频模板。尚未完成的是：

1. 将现有单端口ZC前导扩展为四路接收定时/CFO，并在不知道Rank的条件下完成导频模板选择；
2. 接入20 ppm SFO校正、跨帧CSI平滑及连续SEARCH/TRACK/REACQUIRE状态机；
3. 把视频UDP桥扩展为四发四收，并增加四层实时星座与16路信道显示；
4. 完成4×4感知共享前端，最后再对接 `libyunsdr` 四通道采集。

第三阶段应先做第1、2项的四通道时域接收闭环，继续采用ZC相关、CP-CFO、线性FDM导频和MMSE检测，避免加入SVD、迭代检测等不利于实时C++实现的复杂算法。

## 9. 第三阶段完成：四通道时域同步闭环

现已新增完整三符号Rank-4时域链：

```text
端口0发送1个ZC前导
-> 4Tx/4Rx三径相关TDL
-> AWGN、定时偏移、CFO、SFO
-> 四接收支路联合ZC定时
-> 三个OFDM符号CP相关CFO估计与校正
-> 两数据符号相位参考SFO估计与校正
-> 四端口FDM导频LS及频域插值
-> 控制头四路MRC软译码
-> Rank-4 MMSE、max-log LLR、LDPC和CRC
```

连续接收状态支持 `SEARCH`、`TRACK` 和 `REACQUIRE`。首次接收做全窗口搜索；锁定后只搜索预测位置左右2点。小窗口峰值不足时，同一帧自动回退到全搜索。自动测试还注入了超出跟踪窗的定时跳变，重捕获后负载仍逐字节一致。

默认5帧测试条件：Rank-4/64-QAM满载、50 dB、三径TDL、Tx/Rx相关系数0.2、300 Hz CFO、20 ppm SFO，起始定时20点且每帧增加1点。结果如下：

| 指标 | 结果 |
|---|---:|
| 定时/控制头/CRC/负载通过 | 5 / 5 |
| 第一帧同步 | SEARCH，全窗口 |
| 后续同步 | 4帧TRACK，每帧最多5个候选点 |
| CFO估计误差范围 | −2.95 至 +1.43 Hz |
| SFO二次校正后残差 | 小于0.001 ppm |
| LDPC前硬判决BER | 0.00488 |
| 平均EVM | 6.38% |
| 平均CSI NMSE | −25.56 dB |

时域CSI NMSE比无CFO/SFO的频域仿真差，主要来自短帧SFO相位斜率、保守噪声估计和残余相位，而最终EVM、LDPC及CRC仍通过。`receiver_us`不包含EVM、BER和真值CSI NMSE统计，但当前实现仍为功能优先版本，存在临时向量分配且LDPC单线程运行，不能作为最终实时性能结论。

手动运行：

```bat
cd /d E:\openisac\OpenISAC-main
cpp_phy\run_cpp_4x4_time_diagnostics.cmd
```

自定义示例：

```bat
cpp_phy\run_cpp_4x4_time_diagnostics.cmd --frames 20 --snr 50 --modulation 64QAM --timing 20 --timing-drift 1 --cfo 300 --sfo 20 --tx-correlation 0.2 --rx-correlation 0.2 --check --output measurement\cpp_4x4_time_diagnostics
```

输出目录包含逐帧同步/CFO/SFO/CRC指标、汇总CSV、四层星座CSV和PNG图。

## 10. 下一阶段：实时化整理

下一步不立即接视频或SDR，而是先把Rank-4时域接收器改为与现有2×2链路相同的工程结构：

1. 增加可复用工作区，消除每帧FFT、网格、CSI、LLR和星座向量分配；
2. 将生产 `LdpcFrameDecoder` 固定线程池接入18码块译码；
3. 分离同步、FFT/CSI、4×4检测、软解映射和LDPC耗时，建立P50/P99统计；
4. 根据实测决定采用子载波并行还是帧双缓冲，保持Cholesky MMSE算法不变；
5. 达到稳定吞吐后再扩展四通道视频桥、绘图和感知共享前端。

50帧固定定时性能基线（VS2019 x64 Release）为：50/50帧CRC通过，接收均值1504.9 us、P50 1448.2 us、P99 1699.3 us。该时间已经排除发射、TDL/AWGN/CFO/SFO生成以及BER/EVM/NMSE真值统计，但仍包含每帧临时分配和单线程LDPC。按15 kHz子载波间隔，当前三符号空口帧长225 us，因此功能版约慢6.7倍，等效满载用户处理吞吐约6.0 Mbit/s；不能宣称已经达到实时。这个基线明确了下一阶段必须先优化接收工作区、4×4检测流水和18码块LDPC并行。

当前时域入口是专用Rank-4接收路径，因而接收端先选择Rank-4导频模板，再由控制头验证Rank/MCS。统一Rank-1/2/4实时接收还需增加候选导频模板探测，或在前导/固定控制导频中携带Rank提示；在该协议选择确定前不会把专用路径错误描述为自动Rank盲检。

## 11. 第四阶段完成：稳定内存与并行LDPC

Rank-4时域接收器现已增加长期复用的 `Rank4TimeWorkspace`。同步前导、四路FFT输入/输出、接收网格、导频参考、16路CSI、噪声样本、控制LLR、四层均衡符号、后处理方差和动态帧译码缓冲均跨帧保留。预热后若任一受跟踪缓冲区再次扩容，`--check` 会直接判失败；`release()` 可在停止链路时明确释放保留内存。

18个LDPC码块由一个持久 `LdpcFrameDecoder` 线程池并行译码，线程不会每帧创建。Windows手动入口默认12线程；实测18线程虽然部分轮次均值略低，但P99更易受调度影响，因此12线程作为当前目标电脑的均值/尾延迟折中，不把线程数写死在算法接口中。

50帧固定种子、Rank-4/64-QAM满载、50 dB、300 Hz CFO、20 ppm SFO、每帧1点定时漂移、Tx/Rx相关系数0.2的VS2019 x64 Release结果：

| 指标 | 原功能基线 | 优化后12线程 |
|---|---:|---:|
| CRC及载荷通过 | 50 / 50 | 50 / 50 |
| 接收均值 | 1504.9 us | 856.5 us |
| 接收P50 | 1448.2 us | 820.9 us |
| 接收P99 | 1699.3 us | 974.4 us |
| 均值加速 | 1.00× | 1.76× |
| 预热后工作区扩容帧 | 未约束 | 0 |
| 预热后LDPC扩容帧 | 未约束 | 0 |

优化后平均分段耗时为：同步55.3 us、FFT与SFO校正62.8 us、CSI估计122.0 us、控制头及4×4检测236.3 us、软解映射98.4 us、LDPC/打包/CRC 281.7 us。LDPC在全部50帧中实际最多迭代2次，因此降低当前10次安全上限不会改善这些帧的性能，却会减少恶劣信道下的纠错余量。

结果保存在 `measurement\cpp_4x4_time_optimized`。当前856.5 us仍是225 us空口帧周期的约3.8倍；下一节继续说明已经完成的前端与FEC双缓冲流水。

## 12. 第五阶段完成：Rank-4双缓冲流水

时域链已拆分为 `prepare_rank4_time_frame()` 和 `finish_rank4_time_frame()`。前者完成发射/信道仿真、同步、FFT/SFO、CSI、控制头、4×4 MMSE和max-log软解映射；后者只执行LDPC、打包和CRC。原 `simulate_rank4_time_frame()` 仍按顺序调用这两个接口，保持现有调用兼容。

`Rank4TimePipeline` 为两个槽位分别保留工作区和准备结果。前台完成一帧软解映射后立即入队，后台单消费者调用一个12工作线程的持久LDPC池；前台可同时准备下一帧。结果按提交顺序返回，后台异常延迟到对应 `receive()` 抛出。同步状态只依赖定时和可靠控制头，因此下一帧可以立即使用TRACK窗；CRC继续作为数据交付判据。

200帧、Rank-4/64-QAM、50 dB、300 Hz CFO、20 ppm SFO、相关系数0.2、12个LDPC工作线程的VS2019 Release结果：

| 指标 | 结果 |
|---|---:|
| CRC及载荷通过 | 200 / 200 |
| 串行接收均值 | 810.9 us |
| 前端均值 / P50 / P99 | 534.7 / 532.1 / 563.4 us |
| 后台FEC均值 / P50 / P99 | 283.7 / 284.7 / 364.6 us |
| 队列等待P50 / P99 | 9.4 / 25.2 us |
| 预热后工作区/LDPC扩容 | 0 / 0 |
| 完整仿真串行/流水间隔 | 4136.0 / 3463.3 us |
| 完整仿真加速 | 1.19× |

未来四路采集IQ直接进入前端时，双缓冲服务周期理论上由较慢一侧决定，本次为534.7 us，而不是串行的810.9 us。但当前流水 `submit()` 仍负责发射和TDL信道生成，所以534.7 us是基于已测前端/FEC分段的捕获输入服务边界，不是当前硬件入口实测值。运行：

```bat
cpp_phy\run_cpp_4x4_time_pipeline_benchmark.cmd 200 12
```

结果保存在 `measurement\cpp_4x4_time_pipeline`。随后已增加显式用户载荷入口 `prepare_rank4_time_payload_frame()`、`simulate_rank4_time_payload_frame()` 和双缓冲 `submit_payload()`，并把Rank-4接入Windows UDP/VLC视频信道仿真桥。统一桥接受 `--rank 1|2|4`：Rank-1/2保留原2x2流水，Rank-4使用4x4时域流水，默认Rank-2不变。

Rank-4/64-QAM、45 dB、300 Hz CFO、20 ppm SFO、空间相关三径、12个LDPC线程的视频桥回归中，30个UDP数据报拆为60个PHY帧，30/30逐字节恢复，FER 0，完整仿真平均约3.892 ms/PHY帧。该结果验证的是Windows离线视频链路功能，不宣称满足225 us实时空口周期。下一性能阶段仍应增加四路预生成/采集IQ的 `submit_iq()` 并继续优化4×4控制/MMSE检测。

STBC兼容采用“独立模式、共享帧封装”的路线。现有Rank-1/2/4字段只表达空间复用层数；后续2Tx Alamouti STBC固定为单数据流，在两个OFDM时隙完成编码和合并，共享前导、导频、LDPC、CRC、UDP分片与统计。Python中的STBC/SFBC实现作为C++移植参考。不会把Rank-2空间复用静默解释为STBC，也暂不优先实现低码率的4Tx正交STBC。

## Rank-4 感知闭环

Rank-4时域配置新增 `channel_time_seconds`，所有TDL路径在构造4×4冲激响应之前先按该帧仿真时间推进多普勒相位。视频桥按所选导频模式使用225 us（FDM）或375 us（DM-RS）帧周期，因此慢时间相位与距离–多普勒坐标一致，不使用Windows实际计算耗时作为感知时间轴。

通信接收器可按需保留两符号平均后的16条信道频响、有效子载波掩码和Rx0时域波形。`DynamicSensingProcessor::push_channel_frame()` 对16条链路分别执行距离IFFT、静态杂波抑制和慢时间多普勒FFT，再对功率做非相干合并。该方案不依赖四个射频通道之间的绝对相位校准；阵列角度估计仍留到实际天线几何和通道校准具备后实现。

Windows视频桥回归参数为Rank-4/64-QAM、45 dB、300 Hz CFO、20 ppm SFO、Tx/Rx相关系数0.2、128帧相干积累，运动路径为9采样、-8 dB、69.444 Hz。结果：80/80个UDP包、160个PHY帧逐字节恢复，FER 0；感知输出87.830 m、69.444 Hz、1.795 m/s，CFAR显示目标1个。遥测含四层星座各756点、3508点时域波形和全部16条信道频响，Python监视器无窗口读取检查通过。

## Rank-4 EVM工程优化与Rank-2对比

对MMSE直接去偏置、DFT时延截断和频域短窗平滑进行了固定种子扫描。直接去偏置把20帧平均EVM从7.045%恶化到7.420%；DFT截断在当前保护带外插值结构下把EVM恶化到约15%；频域3点窗口基本无变化，5点以上逐步恶化。这三项均未保留。

最终保留两项低复杂度改进：第一，对两个完成SFO相位斜率校正的数据符号CSI逐子载波相干平均；第二，把导频残余噪声的MMSE加载系数从1.0标定为0.5。500帧、Rank-4/64-QAM、45 dB、300 Hz CFO、20 ppm SFO、Tx/Rx相关系数0.02的串行对照中，EVM从6.938%降至6.605%，预FEC BER从0.005709降至0.005088，CRC保持500/500通过，接收均值从810.9 us增至824.5 us。结果分别保存于 `measurement\rank4_evm_baseline_serial` 和 `measurement\rank4_evm_optimized_serial`。

视频遥测新增通用复Hermitian Gram矩阵的Jacobi特征值条件数计算，只在低频快照执行，不进入每个RE的实时检测路径。C++结果中位/P90为7.711/15.707，与同一CSV的NumPy结果7.708/15.655一致；条件数大于10的有效子载波为32.25%。

同条件Rank-2视频快照EVM约2.29%，条件数中位/P90为2.75/4.41且无病态子载波；Rank-4快照EVM约5.58%。总发射功率归一化使Rank-4每层比Rank-2低3 dB，且4×4需要同时分离三个干扰层，随机方阵出现小奇异值的概率和影响都更高。每个端口的有效FDM导频间隔均约为8，因此差距主要不是导频密度，而是空间层功率和矩阵条件数。

## NR风格前置DM-RS阶段完成：Rank-1/2/4统一兼容

现已在保留原FDM导频回归模式的同时实现`pilot-mode=fdm|nr-dmrs`。默认FDM仍是ZC加两个数据符号、225 us；DM-RS模式为ZC、两个连续前置DM-RS和两个数据符号，共375 us。Rank-1/SISO在双物理端口框架中只发送单层数据，但DM-RS仍探测两个Tx端口；Rank-2在满活动带宽上用双符号时域OCC分离两个端口；Rank-4使用两个交织频域梳齿，每个梳齿内以双符号时域OCC分离两个端口。该映射避免了首版四路频时Walsh块对“相邻子载波信道完全相同”的假设。

接收端完成DM-RS匹配LS、每端口梳齿线性插值、4×4 MMSE接入和感知CSI复用。两个数据符号保留低密度参考，用于差分噪声估计，以及相对于前置DM-RS信道的公共相位/频率线性相位跟踪。所有计算都是固定次数复乘加、线性回归和插值，没有增加迭代检测或大矩阵优化，适合后续Windows C++及libyunsdr入口。

Rank-1/2/4、64-QAM、45 dB、300 Hz CFO、20 ppm SFO、Tx/Rx相关系数0.02的30包视频桥回归全部逐字节恢复，FER为0。Rank-4固定种子100帧对照为：

| 导频 | EVM | 预FEC BER | CSI NMSE | CRC | 接收均值 | 帧周期 |
|---|---:|---:|---:|---:|---:|---:|
| FDM | 6.562% | 0.005047 | -25.736 dB | 100/100 | 838.996 us | 225 us |
| 前置DM-RS | 7.082% | 0.004446 | -25.683 dB | 100/100 | 986.250 us | 375 us |

无CFO/SFO的30帧隔离对照中，DM-RS达到5.823% EVM和-45.982 dB CSI NMSE，优于FDM的7.378%和-40.890 dB。加入实际上限20 ppm后，前置CSI到数据区的时间老化使EVM仍比FDM高约0.52个百分点，但BER略低且CRC全通过。下一轮优化应聚焦低密度数据参考辅助的CSI幅相更新，而不是增加更复杂的检测器。由于当前每帧载荷不变而帧由3符号增至5符号，DM-RS模式净空口吞吐为FDM的60%；后续需在跟踪可靠后再评估去除重复FDM信道导频。

当前视频信道仿真只支持通过Tx/Rx空间相关系数和随机种子间接改变信道条件数，尚不支持直接指定目标条件数。条件数是每个有效子载波上实际复信道矩阵奇异值之比，因此同一个相关系数也会产生一组不同的条件数。若后续需要严格的EVM对照，应新增“受控奇异值信道”诊断模式，通过构造 `H = U diag(sigma) V^H` 精确设置平坦信道条件数；该模式用于算法压力测试，不替代物理TDL相关信道。
