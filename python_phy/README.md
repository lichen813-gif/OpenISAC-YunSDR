# OpenISAC Python PHY 黄金模型

这是 OpenISAC MIMO 演进的第一版可运行模型，当前包含：

- 与 `include/QAM.hpp` 一致的 QPSK、16/64/256-QAM Gray 映射；
- 正 LLR 表示 bit 0 的 max-log 软解调；
- 酉归一化 CP-OFDM；
- 2Tx Alamouti STBC/SFBC，支持 1/2/4/8 个接收天线；
- 2×2/4×4/8×8满层空间复用，支持ZF和线性MMSE检测；
- AWGN、固定平坦信道和块 Rayleigh 平坦衰落；
- 与 OpenISAC 相同参数的 CRC-16/CCITT-FALSE；
- BER、BLER、CRC failure rate 和 EVM 统计；
- Windows/Linux 共用 YAML 配置与命令行入口。

## 固定的数据约定

- bit 顺序：MSB-first；
- QAM：方形 Gray 映射、单位平均符号功率；
- FFT：IFFT 乘 `sqrt(N)`，FFT 除 `sqrt(N)`；
- 网格：`[batch, ofdm_symbol, subcarrier, antenna]`；
- 信道：`H` 的最后两维为 `[rx, tx]`；
- 噪声方差：单个复数样本的 `E[|n|^2]`；
- STBC：Alamouti 对跨两个连续 OFDM symbol；
- SFBC：Alamouti 对跨相邻子载波，两个 OFDM symbol 各自独立编码；
- 空间复用：每个资源单元发送两层独立QAM，两根天线各乘`1/sqrt(2)`；
- 发射功率：两根天线各乘 `1/sqrt(2)`，总功率为 1；
- CRC：CRC-16/CCITT-FALSE，结果按大端附加到帧末尾。

## Windows 运行

在 `python_phy` 目录执行：

如果系统没有 `py` 命令，直接运行自动查找 Python 的批处理入口：

```bat
cd /d E:\openisac\OpenISAC-main\python_phy
run_validation_windows.cmd --frames 1000
```

脚本优先使用项目的 `.venv`，然后查找环境变量 `OPENISAC_PYTHON`、PATH 中的 `python.exe`、`py.exe` 和 Codex 随附的 Python。也可以显式指定解释器：

```bat
set "OPENISAC_PYTHON=C:\完整路径\python.exe"
run_validation_windows.cmd --frames 1000
```

如果已经安装标准 Python，则可以安装项目及测试依赖：

```powershell
py -m pip install -e ".[test]"
py -m pytest
py -m openisac_phy.cli --config configs/mimo_2x1_alamouti.yaml
```

当前工作区已经建立项目虚拟环境时，也可以先激活再运行：

```bat
cd /d E:\openisac\OpenISAC-main\python_phy
.venv\Scripts\activate.bat
python -m pytest -q
python -m openisac_phy.cli --config configs\mimo_2x1_alamouti.yaml
```

如果只安装了 NumPy、尚未安装 pytest，可先运行不依赖 pytest 的回归入口：

```powershell
py tests/run_tests.py
```

也可以覆盖 SNR 扫描：

```powershell
py -m openisac_phy.cli `
  --config configs/mimo_2x2_alamouti.yaml `
  --snr-db 5,10,15,20,25,30 `
  --output results/mimo_2x2_64qam.csv
```

## Linux 运行

```bash
python3 -m pip install -e '.[test]'
python3 -m pytest
python3 -m openisac_phy.cli --config configs/mimo_2x1_alamouti.yaml
```

CSV 保存逐 SNR 指标，同名 JSON 保存完整配置和结果。随机种子固定时结果可重复。

## 显式验收测试

以下入口会逐项打印测试输入、实测误差、验收条件和 PASS/FAIL，并将检查记录与 BER 曲线保存为 CSV/JSON：

```powershell
run_validation_windows.cmd --frames 1000
```

该入口不依赖 PyYAML 或 pytest，只需要 NumPy，适合在新 Windows/Linux 环境中进行首次验收。

## 星座图

推荐直接双击 `show_constellation_windows.cmd`，无需输入命令。窗口中可以手动选择：

- QPSK、16-QAM、64-QAM、256-QAM；
- 1、2、4、8 根接收天线；
- AWGN、固定信道、Rayleigh 和频率选择性 TDL 信道；
- Es/N0 和仿真帧数。

OFDM 参数已经开放：

- FFT 点数和 CP samples；
- 子载波间隔，采样率按 `Fs = FFT × 子载波间隔` 自动计算；
- 左右保护子载波；
- DC 是否置零；
- comb pilot 间隔和偏移，间隔为 `0` 时关闭导频。
- **Alamouti pairing**：`time`选择STBC，`frequency`选择相邻子载波SFBC。
- **MIMO mode**：选择`stbc`、`sfbc`或`spatial_multiplexing`；
- **SM detector**：空间复用模式选择`zf`或`mmse`。

前导与粗同步参数也已开放：

- **Enable ZC frame preamble**：在Tx0上发送一个独立ZC前导OFDM符号，其他Tx静默；
- **ZC root**：ZC根序号，默认29，必须与FFT点数互质；
- **Enable timing/CFO synchronization**：有ZC前导时使用整符号匹配滤波定时，没有前导时回退到CP相关定时；两种模式都用CP相位估计粗CFO；
- **Timing offset**：人为插入的接收流采样偏移；
- **Timing search**：接收端搜索窗口，必须覆盖插入偏移；
- **CFO**：人为插入的载波频偏，当前CP估计要求绝对值小于半个子载波间隔；
- **Pilot CPE tracking**：分别校正两个OFDM symbol的导频公共相位；
- **CPE min coherence**：导频预测值与接收值的一致性门限，低于门限时不应用相位校正；
- **Channel estimate**：选择`perfect`、`ls_linear`、`ls_dft`或`lmmse`；
- **CSI taps**：DFT-LS/LMMSE假设的时域信道长度，默认10 samples；
- **Independent phase references**：启用不依赖真实CSI的双时隙相位参考；
- **Reference tones**：从comb pilot中保留的相位参考数量；
- **Slot phase jump**：人为注入第二个OFDM时隙的公共相位跳变。
- **SFO (ppm)**：按`(F_rx-F_tx)/F_tx×1e6`注入采样时钟偏差；
- **SFO phase-slope tracking**：用分布在带宽上的相位参考拟合截距和斜率，校正第二个Alamouti时隙，建议使用4–8个参考。
- **Closed-loop SFO resampling**：作为大偏差压力测试保留；实际`|SFO|<=20 ppm`的工程主链只做频域相位斜率校正，不执行时域重采样和第二遍FFT。

默认 `FFT=1024、CP=128、Δf=15 kHz、DC置零、pilot spacing=8`，对应 `Fs=15.36 MHz`、127个导频子载波和896个数据子载波。GUI默认启用一个ZC前导，因此完整帧为“1个ZC前导+2个Alamouti数据符号”，共3个OFDM符号、3456个复采样。STBC导频使用跨两个时隙的Alamouti BPSK符号对；SFBC和空间复用使用按频率轮转Tx端口的单位功率FDM正交导频。

已经过测试并冻结的2×2空间复用正式仿真帧使用pilot spacing=4、216个FDM信道导频、8个相位参考、672个数据子载波、2014字节载荷和CRC-16；完整定义见[`docs/FORMAL_FRAME_PROFILE_2X2_SM_1024.md`](docs/FORMAL_FRAME_PROFILE_2X2_SM_1024.md)。

C++实时实现的算法复杂度、缓存上限、允许/禁止方案和benchmark门槛见[`docs/CPP_REALTIME_FEASIBILITY.md`](docs/CPP_REALTIME_FEASIBILITY.md)。

选择 `tdl` 后，可在 **TDL taps** 输入框按下面格式手动设置路径：

```text
0:0:0, 3:-4:45, 9:-8:-80
```

每项依次为 `delay_samples:gain_db:phase_deg`。最大时延不得超过 CP 长度；当前 GUI 默认 CP 为128 samples。

**Maximum Doppler (Hz)**设置每条活动`Rx×Tx×path`的最大多普勒范围。**Doppler model**可选`symbol`或`continuous`：`symbol`只在OFDM符号边界更新tap，用于隔离跨时隙失配；`continuous`在每个输出采样上连续旋转每条路径，在FFT窗口内自然产生ICI，更接近高速移动信道。

Rayleigh信道还可设置 **Tx correlation**、**Rx correlation** 和 **Spatial rank**。相关矩阵使用`R(i,j)=rho^|i-j|`的Kronecker模型；rank为`0`时保留自然秩，正整数则把信道截断到指定空间秩并保持每帧Frobenius能量不变。相关性和显式秩控制当前只用于Rayleigh信道。

SFBC已经支持频分正交导频和`ls_linear/ls_dft/lmmse`估计CSI。每个OFDM符号内，comb pilot按频率交替分配给Tx0/Tx1，独立估计`H[t,k,rx,tx]`；GUI选择`frequency`时默认切换到pilot spacing=4和LMMSE。SFBC只把连续的两个数据子载波组成一对；DC、保护带或导频边界处没有相邻伙伴的单独子载波保持为空。该模式能避免STBC两个时隙之间的Doppler失配，但要求一对子载波上的信道足够接近。pilot-CPE、独立相位参考和SFO跟踪暂未接入SFBC。

点击 **Run simulation** 更新星座图和 BER/BLER/CRC/EVM，点击 **Save PNG** 保存当前图片。

同一窗口下方显示时域波形，可在不重新仿真的情况下切换：

- `Tx0`、`Tx1` 和当前配置的 `Rx0...RxN`；
- I/Q 双路、仅实部、仅虚部或幅度；
- **Display samples**：`0`表示显示完整帧，也可输入正整数只看前若干采样；
- 虚线标出OFDM symbol边界；启用前导时，前1152个采样以阴影标为ZC preamble。

第三张图显示所选 `Tx→Rx` 链路的完美 CSI 频率响应，可用于观察多径频率选择性和深衰落位置。
橙色空心圆标出实际导频子载波位置。

生成 PNG 并弹出交互窗口：

```bat
cd /d E:\openisac\OpenISAC-main\python_phy
.venv\Scripts\activate.bat
python experiments\show_constellation.py --modulation 64qam --nr 2 --snr-db 25 --show
```

只保存图片、不弹窗：

```bat
python experiments\show_constellation.py --modulation 256qam --nr 2 --snr-db 30
```

SFBC估计CSI星座、时域波形和信道响应：

```bat
python experiments\show_constellation.py --mode sfbc --modulation 64qam --nr 2 --channel tdl --doppler-hz 500 --snr-db 35 --pilot-spacing 8 --channel-estimation lmmse --channel-estimation-taps 10 --show
```

在相同1024/128、2x2、64-QAM、TDL和总发射功率下扫描STBC/SFBC：

```bat
python experiments\compare_stbc_sfbc.py
```

结果写入`measurement/stbc_sfbc_validation/`。在本次固定种子、30 dB扫描中，最大路径Doppler为`0/100/300/500/1000 Hz`时，STBC BER约为`2.17e-6 / 4.52e-4 / 2.17e-2 / 4.96e-2 / 1.12e-1`，SFBC BER约为`6.51e-6 / 6.51e-6 / 7.13e-6 / 1.09e-5 / 4.68e-5`。

SFBC完美CSI、LS线性、DFT-LS和LMMSE同条件对照：

```bat
python experiments\compare_sfbc_estimators.py
```

结果写入`measurement/sfbc_csi_comparison/`。固定1024/128、2×2、64-QAM、TDL、pilot spacing=8、500 Hz符号级Doppler和300帧时，40 dB下DFT-LS/LMMSE的信道NMSE约`1.65e-5`，BER为0，CRC goodput为`53.653 Mb/s`；LS线性信道NMSE约`6.60e-4`，BER约`6.32e-5`，goodput约`30.940 Mb/s`。无噪声有限时延测试中DFT-LS NMSE低于`1e-25`。

2×2空间复用MMSE星座和完整波形：

```bat
python experiments\show_constellation.py --mode spatial_multiplexing --detector mmse --modulation 64qam --nr 2 --channel rayleigh --snr-db 35 --no-preamble --no-synchronization --timing-offset-samples 0 --timing-search-samples 0 --cfo-hz 0 --show
```

在相同1024/128、2×2、64-QAM、完美CSI、总发射功率、Rayleigh信道和随机样本下比较STBC、SFBC、SM-ZF、SM-MMSE：

```bat
python experiments\compare_mimo_modes.py --snr-db 15,20,25,30,35,40
```

结果写入`measurement/mimo_mode_comparison/`。STBC/SFBC净载荷上限约`71.573 Mb/s`，双层空间复用约`143.253 Mb/s`。35 dB时SM-MMSE BER约`3.64e-3`、CRC goodput约`98.37 Mb/s`，已经超过分集模式的`71.57 Mb/s`；40 dB时约`130.84 Mb/s`。MMSE相对ZF在中低SNR下显著降低EVM，并小幅降低BER。

### 空间复用导频与估计CSI

空间复用支持双发射频分正交导频：comb pilot按频率顺序交替分配给Tx0/Tx1，每个导频资源只有一根天线发送单位功率BPSK；两个OFDM符号分别估计`H[t,k,rx,tx]`。接收端可选择`ls_linear`、`ls_dft`或`lmmse`，随后把估计信道送入ZF/MMSE检测器。

```bat
python experiments\show_constellation.py --mode spatial_multiplexing --detector mmse --modulation 64qam --nr 2 --channel tdl --snr-db 50 --pilot-spacing 4 --channel-estimation lmmse --channel-estimation-taps 10 --no-preamble --no-synchronization --timing-offset-samples 0 --timing-search-samples 0 --cfo-hz 0 --show

python experiments\compare_spatial_estimators.py --snr-db 30,35,40,45,50
```

正式结果位于`measurement/spatial_csi_comparison/`。1024/128、2×2、64-QAM、三径TDL、pilot spacing=4、300帧条件下，50 dB时完美CSI/DFT-LS/LMMSE BER约为`1.14e-4 / 1.39e-4 / 1.39e-4`，DFT-LS/LMMSE信道NMSE约`8.31e-7`；LS线性插值存在约`5.09e-5`的插值误差平台。本固定TDL矩阵条件较差，所以即使完美CSI也需要很高SNR，说明空间复用性能同时受信道估计误差和信道条件数约束。

TDL 命令行示例：

```bat
python experiments\show_constellation.py --modulation 64qam --nr 2 --channel tdl --snr-db 25 --taps "0:0:0,3:-4:45,9:-8:-80" --preamble --zc-root 29 --synchronization --pilot-phase-tracking --pilot-phase-min-coherence 0.9 --timing-offset-samples 20 --timing-search-samples 64 --cfo-hz 1200
```

独立LS信道估计示例：

```bat
python experiments\show_constellation.py --modulation qpsk --nr 2 --channel tdl --snr-db 40 --pilot-spacing 2 --channel-estimation ls_linear --no-pilot-phase-tracking --synchronization --timing-offset-samples 5 --timing-search-samples 16 --cfo-hz 0 --show
```

LMMSE示例：

```bat
python experiments\show_constellation.py --modulation 64qam --nr 2 --channel tdl --snr-db 30 --pilot-spacing 4 --channel-estimation lmmse --channel-estimation-taps 10 --no-pilot-phase-tracking --no-synchronization --timing-offset-samples 0 --timing-search-samples 0 --cfo-hz 0 --show
```

LMMSE加独立相位参考示例：

```bat
python experiments\show_constellation.py --modulation 64qam --nr 2 --channel tdl --snr-db 30 --pilot-spacing 4 --channel-estimation lmmse --channel-estimation-taps 10 --no-pilot-phase-tracking --phase-reference-tracking --phase-reference-count 2 --slot-phase-offset-deg 30 --no-synchronization --timing-offset-samples 0 --timing-search-samples 0 --cfo-hz 0 --show
```

在完全相同的信道、pilot和随机种子下比较三种估计器：

```bat
python experiments\compare_channel_estimators.py --frames 1000
```

支持 `qpsk/16qam/64qam/256qam`、`nr=1/2/4/8`、STBC/SFBC、2×2空间复用ZF/MMSE和 `awgn/static/rayleigh/tdl`。星座图中的蓝点是均衡/检测后的接收符号，红色叉号是理想参考点。TDL 已支持延迟、增益、相位和符号级Doppler。

注意：当前已经完成OpenISAC同公式ZC前导、匹配滤波帧定时、CP相位粗CFO估计、导频资源映射、导频 EVM 验证、comb pilot残余公共相位跟踪，以及2Tx正交Alamouti导频LS信道估计。`ls_linear`使用两个OFDM时隙的正交导频独立恢复每个`Rx×Tx`链路，再按周期OFDM频谱对实部和虚部进行线性插值。LS模式不会读取真实信道；真实信道只用于计算NMSE和绘制虚线参考曲线。

当前pilot-CPE跟踪仍依赖完美CSI预测接收导频，因此不能与`ls_linear`同时启用。GUI切换到LS时会自动关闭该开关，配置文件中同时打开会明确报错。TDL同步会跳过由已配置最大时延扩展污染的CP前部，因此同步开启时最大TDL时延必须严格小于CP长度。

当前自动验收包括：114项pytest测试和96项显式检查。2×2/4×4/8×8结果保存在`measurement/mimo_size_comparison/`，空间复用估计CSI结果保存在`measurement/spatial_csi_comparison/`，相关/秩亏结果保存在`measurement/mimo_correlation_rank/`，SFO极端参考结果保存在`measurement/spatial_sfo_closed_loop/`，20 ppm/低Doppler工程边界结果保存在`measurement/engineering_operating_region/`，LDPC黄金结果保存在`measurement/ldpc_cpp_alignment/`。

### 空间复用同步与动态信道压力测试

固定256/32、2×2满层空间复用、64-QAM、MMSE、Rayleigh、35 dB、ZC前导、pilot spacing=4和逐符号LS信道估计，可运行：

```bat
python experiments\stress_spatial_impairments.py
python experiments\sweep_spatial_impairments.py
```

200帧联合场景结果如下。所有场景均保留相同的前导和导频开销：

| 场景 | BER | EVM | CSI NMSE | 定时成功率 | CFO平均绝对误差 | CRC goodput |
|---|---:|---:|---:|---:|---:|---:|
| 基线 | 0.005021 | 0.06845 | 0.0002283 | 100% | 2.73 Hz | 12.137 Mb/s |
| 定时12点+CFO 1 kHz，不同步 | 0.24587 | 1.17877 | 1.44197 | 0% | 1000 Hz | 0 |
| 定时12点+CFO 1 kHz，ZC同步 | 0.005021 | 0.06845 | 0.0002286 | 100% | 2.73 Hz | 12.137 Mb/s |
| Doppler 500 Hz | 0.005084 | 0.06954 | 0.0002286 | 100% | 2.80 Hz | 12.583 Mb/s |
| SFO 50 ppm | 0.005074 | 0.06891 | 0.002591 | 100% | 3.69 Hz | 11.959 Mb/s |
| 四项联合 | 0.005134 | 0.07001 | 0.002682 | 100% | 3.75 Hz | 12.494 Mb/s |

未补偿SFO扫描中，0/50/100/200/500/1000/2000 ppm的BER约为`0.00502/0.00507/0.00516/0.00560/0.01312/0.04702/0.12051`；500 ppm开始全部200帧CRC失败，goodput降为0。该旧扫描配置有意关闭后来实现的相位参考和闭环重采样，用来保留未补偿边界；补偿结果见下节。

该旧压力脚本固定使用`doppler_model=symbol`：每个OFDM符号内部信道不变，仅在符号之间更新。因此扫到10 kHz时CSI NMSE仍约`2.3e-4`，只能验证逐符号信道跟踪，不能解释为10 kHz实机移动能力。新的逐采样连续tap/ICI结果见后文。

### 相关MIMO与秩亏压力测试

相关信道使用标准Kronecker模型：

```text
H = Rr^(1/2) · W · Rt^(1/2)
R(i,j) = rho^|i-j|
```

运行2×2相关性扫描及2×2/4×4/8×8显式秩扫描：

```bat
python experiments\stress_mimo_correlation_rank.py
```

固定256/32、64-QAM、完美CSI、40 dB、200帧时，2×2 MMSE结果为：

| Tx/Rx相关系数 | 平均条件数 | BER | EVM | CRC goodput |
|---:|---:|---:|---:|---:|
| 0 | 4.00 | `5.50e-4` | 0.03045 | 33.411 Mb/s |
| 0.50 | 5.17 | `8.26e-4` | 0.03751 | 31.088 Mb/s |
| 0.80 | 10.6 | 0.00647 | 0.07273 | 23.048 Mb/s |
| 0.95 | 38.7 | 0.07335 | 0.21998 | 1.429 Mb/s |
| 0.99 | 188 | 0.26337 | 0.51456 | 0 |
| 1.00 | 约`1.67e15`，rank=1 | 0.37506 | 0.70736 | 0 |

ρ=0.95时ZF EVM约0.26573，MMSE约0.21998；ρ=0.99时ZF EVM约1.29014，MMSE约0.51456，说明MMSE正则化在病态但仍满秩的信道上更稳定。ρ=1或显式rank小于发送层数时，线性接收机无法恢复缺失的空间自由度。

显式秩测试保持每帧总信道能量不变。满秩2×2/4×4/8×8的goodput约为`33.411/55.827/87.385 Mb/s`；2×2降为rank-1、4×4降为rank-3、8×8降为rank-6后，长帧CRC goodput均为0。这说明发送层数必须满足`layers <= 有效信道rank`；实际系统应根据奇异值、条件数或CQI动态减少层数，而不能固定满层发送。

### 空间复用SFO估计与闭环重采样

2×2空间复用已经支持Tx0独占的重复相位参考。第一遍接收机在全带宽参考上拟合两数据符号间的相位斜率，得到每帧SFO估计；通过相干性门限后，将估计值转换为精确逆采样率`1/(1+SFO)`。第二遍复用已有整数定时，仅重新计算CP粗CFO和FFT，不再重复完整ZC搜索。闭环只使用接收估计，不读取`config.sfo_ppm`真值。

运行正式对照：

```bat
python experiments\compare_spatial_sfo_resampling.py
```

以下500 ppm结果使用24抽头高精度参考，属于离线极端压力测试：

| SNR | 处理方式 | BER | EVM | CRC goodput | 初始SFO估计 | 平均绝对残余 |
|---:|---|---:|---:|---:|---:|---:|
| 30 dB | 仅相位斜率 | 0.12957 | 0.34804 | 0 | 501.20 ppm | - |
| 30 dB | 闭环重采样 | 0.02868 | 0.15800 | 13.248 Mb/s | 501.20 ppm | 70.74 ppm |
| 35 dB | 仅相位斜率 | 0.12042 | 0.33802 | 0 | 501.56 ppm | - |
| 35 dB | 闭环重采样 | 0.01677 | 0.12974 | 38.669 Mb/s | 501.56 ppm | 69.70 ppm |
| 40 dB | 仅相位斜率 | 0.11721 | 0.33313 | 0 | 497.21 ppm | - |
| 40 dB | 闭环重采样 | 0.01439 | 0.11999 | 54.065 Mb/s | 497.21 ppm | 70.90 ppm |

40 dB时，相干性门限允许88.5%的帧执行重采样；被拒绝帧保持原始采样率，避免低置信度估计破坏整帧。闭环仍存在约1.44% BER，主要来自残余SFO、sinc有限窗误差、未补偿ICI以及2×2 Rayleigh病态帧，后续可通过更长训练序列、多符号SFO跟踪和自适应rank/MCS继续改善。

### C++实时SFO接收子集

正式C++接收配置为`configs/mimo_2x2_spatial_multiplexing_realtime_1024.yaml`，按实际边界固定为`|SFO|<=20 ppm`、8个相位参考、100 Hz连续Doppler验收点。运行：

```bat
python experiments\validate_engineering_operating_region.py
```

20 ppm、40 dB、200帧时，8参考音频域相位斜率校正得到BER `0.001132`、EVM `0.04067`和goodput `62.658 Mb/s`；sinc8闭环为`0.001116`、`0.03892`和`63.016 Mb/s`。主链取消重采样只损失约0.6% goodput，却省掉约`2.48e8 tap-MAC/s`以及第二遍CP-CFO/FFT。组合100 Hz连续Doppler后goodput仍为`61.584 Mb/s`，因此第一版也不加入ICI抵消。

### 符号内连续Doppler与ICI

`doppler_model=continuous`对每个活动`Rx×Tx×path`在每一个输出采样处应用连续相位旋转。路径相位跨OFDM符号边界连续，延迟路径仍按线性卷积叠加，因此FFT窗口内的信道变化会产生真正的子载波间干扰。接收机使用FFT窗口中点的瞬时CSI作为对角参考；该CSI可以去除主对角信道，但不会人为消除ICI。

正式对照命令：

```bat
python experiments\compare_continuous_doppler.py
```

固定正式1024/128帧、2×2满层空间复用、64-QAM、MMSE、Rayleigh、40 dB和200帧时：

| 最大路径Doppler | 模型 | BER | EVM | 符号内变化NMSE | CRC goodput |
|---:|---|---:|---:|---:|---:|
| 0 Hz | 两种模型 | 0.001132 | 0.03881 | 0 | 62.658 Mb/s |
| 100 Hz | 连续 | 0.001275 | 0.04050 | 0.000953 | 61.584 Mb/s |
| 300 Hz | 连续 | 0.002967 | 0.05654 | 0.008568 | 55.139 Mb/s |
| 500 Hz | 连续 | 0.006224 | 0.08394 | 0.023757 | 39.385 Mb/s |
| 1000 Hz | 符号块 | 0.001923 | 0.04385 | 0 | 62.658 Mb/s |
| 1000 Hz | 连续 | 0.020149 | 0.16004 | 0.094213 | 8.951 Mb/s |

1000 Hz时，活动路径在一个1024点FFT窗口内最大旋转约23.71°。符号块模型没有符号内变化，明显高估高速链路；连续模型显示ICI已成为主要限制。连续信道卷积只属于离线测试平台，不进入C++实时接收负载。正式实验6/6项检查通过；加入正式LDPC帧和Rank/MCS闭环后，全量为114项pytest和96项显式检查。

### Python/C++ LDPC黄金链

`openisac_phy/ldpc.py`已经实现原C++同款LDPC(1008,504)矩阵编码、6次水平分层min-sum软译码、`0x5A`扰码、逐块`21×48`交织、64-symbol marker和BCH(127,64) mini-header。运行：

```bat
python experiments\validate_ldpc_cpp_alignment.py
```

专项结果为7/7黄金/AWGN检查通过。-1 dB BPSK/AWGN下译码显著降低BER；14块、882字节的64-QAM信息区在15 dB及以上完成无误码软解调/LDPC回环。固定码字、扰码交织流和控制label摘要见[`docs/LDPC_CPP_ALIGNMENT.md`](docs/LDPC_CPP_ALIGNMENT.md)。

正式`simulate_mimo_ofdm()`已接通128个Tx0单层QPSK控制RE、14块LDPC、MMSE软LLR和CRC闭环；配置与资源计数见[`docs/LDPC_MIMO_FORMAL_FRAME.md`](docs/LDPC_MIMO_FORMAL_FRAME.md)。运行`experiments\validate_ldpc_mimo_frame.py`可生成前/后LDPC BER、CRC曲线和报告。运行`experiments\export_cpp_frame_vectors.py`可输出完整C++二进制对照向量，接口见[`docs/CPP_FRAME_VECTOR_INTERFACE.md`](docs/CPP_FRAME_VECTOR_INTERFACE.md)。

### Rank/MCS下一帧闭环

`openisac_phy/adaptive_link.py`已经实现一帧反馈延迟、质量/CRC快速降档和连续3帧确认升档。Rank-1为Tx0单流加2Rx MRC，Rank-2为双层MMSE；rank、MCS、LDPC块数和payload长度都由控制头显式传递。运行：

```bat
.venv\Scripts\python.exe experiments\validate_adaptive_link.py
.venv\Scripts\python.exe experiments\validate_adaptive_link.py --frames 48 --snr-db 28 --output-dir ..\measurement\adaptive_link_28db
```

40 dB、64帧时自适应与固定Rank-2/64-QAM均为零CRC失败，自适应goodput为`35.314 Mb/s`，比固定方案`31.289 Mb/s`提高12.9%。28 dB压力点中，自适应把CRC失败率从`16.667%`降到`2.083%`，goodput从`26.074 Mb/s`降至`23.404 Mb/s`，体现当前门限以可靠性优先。详细结果见[`docs/ADAPTIVE_LINK_CLOSED_LOOP.md`](docs/ADAPTIVE_LINK_CLOSED_LOOP.md)。

## 当前边界和下一步

当前版本已经完成Alamouti STBC/SFBC、2×2/4×4/8×8满层空间复用ZF/MMSE、Kronecker天线相关与显式秩亏信道，以及可扩展的频分正交导频与逐符号LS/DFT-LS/LMMSE信道估计。频率选择性TDL、采样偏移/CFO/SFO、符号块及逐采样连续Doppler/ICI、ZC帧同步和CP粗CFO也已完成；工程主链已针对20 ppm和低速移动收敛。下一步依次加入：

1. 用已冻结的完整帧二进制向量建立C++单元测试，并进行固定尺寸2×2 MMSE、软解调、LDPC和自适应控制器的Windows CPU benchmark；
2. 根据实测PER曲线微调Rank门限和2 dB实现裕量，并加入升/降档计数器的跨语言黄金向量；
3. 将当前Tx0单流Rank-1升级为频域Alamouti/SFBC，比较额外发射分集收益与映射复杂度；
4. 只有载频/速度使Doppler长期超过200 Hz时，才重新评估固定一次相邻三子载波ICI抵消。

### 4×4/8×8空间复用

`nt`、`nr`和`layers`已经泛化；满层模式要求`nt=layers`且`nr>=layers`。数据功率按`1/sqrt(nt)`归一化，正交导频按`pilot_index % nt`轮转分配，ZF/MMSE矩阵维度自动跟随层数。

```bat
python experiments\compare_mimo_sizes.py --snr-db 30,40,50

python experiments\show_constellation.py --mode spatial_multiplexing --nt 8 --nr 8 --detector mmse --modulation 64qam --fft-size 256 --cp-length 32 --guard-left 16 --guard-right 15 --pilot-spacing 0 --channel-estimation perfect --channel rayleigh --snr-db 50 --frames 10 --no-preamble --no-synchronization --timing-offset-samples 0 --timing-search-samples 0 --cfo-hz 0 --show
```

256/32、64-QAM、独立Rayleigh、完美CSI、总功率固定为1时，50 dB下2×2/4×4/8×8净载荷上限约为`35.73/71.57/143.25 Mb/s`，实测CRC goodput约为`33.59/70.14/134.66 Mb/s`。增加层数会提高吞吐量上限，但每层功率下降且更容易受到小奇异值影响。
