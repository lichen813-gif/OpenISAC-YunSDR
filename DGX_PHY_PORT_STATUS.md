# DGX Spark C++ 物理层移植状态

更新时间：2026-08-27

## 平台

- 主机：DGX Spark（具体主机名和局域网地址不属于发布内容）；
- 工程：`/path/to/OpenISAC-YunSDR`；
- CPU/OS：ARM64，Ubuntu 24.04.4 LTS，Linux 6.17；
- GPU：NVIDIA GB10，驱动 580.173.02，计算能力 12.1；
- CPU/内存：20 核，121 GiB；
- 工具链：GCC/G++ 13.3，CMake 3.28.3，CUDA Toolkit 13.0.3，
  `nvcc` 13.0.88，cuFFT；
- CUDA 安装于 `/usr/local/cuda-13.0`，需要使用
  `/usr/local/cuda/bin/nvcc` 或把该目录加入 `PATH`；
- 当前没有安装 VLC、FFmpeg 和 Ninja。

## 已完成

1. `cpp_phy` 全部 C++17 源码在 ARM64/GCC Release 下编译成功；
2. 视频桥从 Winsock 扩展为 Windows/POSIX 双平台 socket；
3. Windows VS2019 回归通过，证明Linux修改没有破坏原版本；
4. DGX CPU CTest 10/10通过，CUDA CTest 12/12通过；
5. 新增通用 `openisac_phy_nxn_diagnostics --streams 1|2|4|8`；
6. 正式4×4频域帧、FDM/DMRS时域帧、LDPC/CRC、同步和感知通过；
7. 新增批量1024点OFDM CUDA/cuFFT后端；
8. 新增通用1/2/4/8流 CUDA MMSE/ZF Cholesky检测器；
9. DGX RelWithDebInfo + AddressSanitizer + UndefinedBehaviorSanitizer
   CPU回归10/10通过，无内存或未定义行为报告。

## 能力等级

| 能力 | DGX CPU状态 | 说明 |
|---|---|---|
| QPSK/16QAM/64QAM/256QAM | 通过 | 调制、软解调和单元测试 |
| SISO | 通过 | 正式帧和视频数据包闭环 |
| 2×2 Rank-2 | 通过 | MMSE、FDM/DMRS、正式帧和视频闭环 |
| 2×2 STBC | 通过 | FDM/DMRS和视频闭环 |
| 4Tx/4Rx Rank-2 | 通过 | FDM/DMRS和视频闭环 |
| 4×4 Rank-4 | 通过 | 正式频域/时域帧、视频数据包闭环 |
| 8×8检测器 | 通过 | ZF/MMSE Cholesky和性能基准 |
| 8×8 OFDM算法链 | 通过 | QAM/OFDM/TDL/FDM CSI/MMSE/BER/EVM诊断 |
| 8×8正式帧/视频 | 未实现 | 不能用检测器通过代替正式帧支持 |
| 感知 | 通过 | 距离-Doppler、CFAR和多目标杂波测试 |

## SISO、MIMO2和STBC应用数据基线

参数：64QAM、45 dB、默认TDL/CFO/SFO，20个UDP数据包，LDPC/CRC与
分片重组全部启用。六种组合均逐字节恢复，FER和UDP丢包均为0。

| 模式 | 导频 | PHY帧数 | 处理吞吐 | PHY延迟 |
|---|---|---:|---:|---:|
| SISO 1Tx/1Rx Rank-1 | FDM | 84 | 4.958 Mbit/s | 0.517 ms/帧 |
| 2×2 Rank-2 | FDM | 48 | 6.868 Mbit/s | 0.329 ms/帧 |
| 2×2 Alamouti STBC | FDM | 100 | 4.232 Mbit/s | 0.439 ms/帧 |
| SISO 1Tx/1Rx Rank-1 | NR-like DMRS | 84 | 4.124 Mbit/s | 0.547 ms/帧 |
| 2×2 Rank-2 | NR-like DMRS | 48 | 3.973 Mbit/s | 0.945 ms/帧 |
| 2×2 Alamouti STBC | NR-like DMRS | 100 | 2.353 Mbit/s | 1.032 ms/帧 |

这些短包数值用于功能基线，不等同于持续吞吐上限；正式性能结论需使用长时间
预热、多轮统计和固定CPU/GPU功耗状态。

## 1×1到8×8统一基线

参数：64QAM、50 dB、Tx/Rx相关系数0.02、5帧、1024 FFT、128 CP。

| 模式 | 平坦信道BER | 平坦EVM | 三径TDL BER | 三径TDL EVM | TDL完美CSI EVM |
|---|---:|---:|---:|---:|---:|
| 1×1 | 0 | 0.392% | 0 | 0.508% | 0.402% |
| 2×2 | 0 | 0.660% | 0 | 1.395% | 0.855% |
| 4×4 | 0 | 1.158% | 0.0037 | 5.785% | 3.064% |
| 8×8 | 0 | 1.978% | 0.0297 | 14.770% | 1.890% |

8×8在平坦信道闭环正常。三径TDL下完美CSI EVM仍为1.89%，但现有单符号
FDM导频估计EVM为14.77%，说明误差来自每端口导频间距和矩阵求逆放大，
不是Windows到ARM64移植错误。后续应增加正交块导频/多符号DMRS，再讨论
8×8正式帧和视频。

## 正式帧和流水线

- 4×4频域正式帧：5/5 header、CRC、payload通过，EVM 6.793%；
- 4×4时域FDM：5/5通过，平均EVM 5.664%，接收机约1130 us/帧；
- 4×4时域DMRS：5/5通过，平均EVM 5.927%，接收机约1534 us/帧；
- Rank-2完整模拟流水线：927.76帧/s，CRC 100%；
- Rank-4实际模拟流水线：约407.5帧/s，CRC 100%；
- 感知：64帧批处理摊销26.193 us/帧，目标峰值48.794 m、1.795 m/s，
  多目标杂波CFAR检查通过。

## Windows和DGX CPU微基准

| 模块 | Windows VS2019 | DGX ARM64 | 结果 |
|---|---:|---:|---|
| 2×2 MMSE | 12.01 ns/RE | 6.62 ns/RE | DGX更快 |
| 4×4 MMSE | 234.32 ns/RE | 273.18 ns/RE | Windows略快 |
| 8×8 MMSE | 1281.13 ns/RE | 1258.28 ns/RE | 接近 |
| 64QAM max-log | 14.98 ns/RE | 4.65 ns/RE | DGX更快 |
| 1024点FFT+去CP | 6.606 us | 4.076 us | DGX更快 |
| LDPC高SNR | 7.25 us/块 | 5.85 us/块 | DGX更快 |

## CUDA第一阶段结果

CUDA路径使用持久显存工作区，CPU路径保持不变。以下CUDA时间均包含每批次
H2D/D2H数据搬运，尚未把整帧常驻显存，因此是保守结果。

| 模块 | 批量 | DGX CPU | DGX CUDA | 加速比 | CPU/CUDA最大误差 |
|---|---:|---:|---:|---:|---:|
| 1024点IFFT+CP | 64符号 | 4.38 us/符号 | 0.666 us/符号 | 约6.6× | 7.25e-7 |
| 8×8 MMSE+预测MSE | 4096 RE | 1254.69 ns/RE | 45.46 ns/RE | 27.60× | 7.25e-7 |

MIMO一致性覆盖1×1、2×2、4×4和8×8；四种规模的符号最大误差均小于
7.3e-7，预测MSE最大误差3.26e-9。

## Windows与DGX模式性能对比

统一基准使用Release、64QAM、45 dB、CFO 300 Hz、SFO 20 ppm、定时偏差
20采样、空间相关系数0.02和固定随机种子。每模式预热20包、测量200包、
重复5轮并取中位数。Windows和DGX各测量10000包，全部FER=0、UDP丢包=0。

- Windows在SISO、2×2 Rank-2和STBC低并行度CPU路径的单帧延迟更低；
- DGX CPU在4端口Rank-2和Rank-4的延迟上快约1.20～1.29倍，吞吐快
  约1.36～1.55倍；
- 当前模式基准尚未调用CUDA，CUDA OFDM和8×8检测器结果必须作为模块
  性能单独解释；
- 完整表格、原始数据和测试方法见 `DGX_WINDOWS_PHY_PERFORMANCE.md`。

## 手动命令

```bash
cd /path/to/OpenISAC-YunSDR
chmod +x cpp_phy/build_linux_dgx.sh cpp_phy/run_linux_phy_baseline.sh
./cpp_phy/build_linux_dgx.sh
./cpp_phy/run_linux_phy_baseline.sh

# CUDA 13.0 + cuFFT + 全部 CPU/CUDA 回归
chmod +x cpp_phy/build_linux_cuda.sh
./cpp_phy/build_linux_cuda.sh
```

## 下一步

1. 建立帧级 `cpu/cuda/auto` 调度，把OFDM和MIMO批处理接入实际收发链；
2. 迁移TDL、CFO/SFO、信道估计和感知批处理；
3. 用相同随机种子逐帧比较CPU/CUDA的payload、CRC、EVM和检测结果；
4. 安装VLC/FFmpeg后进行Linux端到端视频测试；
5. 设计8端口正交块导频或多符号DMRS，消除三径TDL估计误差地板；
6. 8×8正式帧和视频属于新增能力，须在导频结构稳定后单独实施。
