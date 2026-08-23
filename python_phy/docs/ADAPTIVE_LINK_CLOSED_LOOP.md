# Rank/MCS闭环验证

## 实现范围

闭环控制器以固定Rank-2/64-QAM状态启动，接收当前帧的平均2x2 Gram矩阵、检测MSE、CRC和outage状态，为下一帧选择rank和MCS：

- 反馈延迟固定为1帧；
- 质量下降或rank条件不满足时立即切到建议模式；
- CRC失败时至少快速下降一级；
- 升档要求目标连续出现3帧，每次只提高一级MCS或切换一次rank；
- MCS门限为`4/10/18/26 dB`，统一保留2 dB实现裕量；
- Rank-2要求最小/最大Gram特征值比不低于0.05；
- Rank-1使用Tx0单流和2Rx MRC，尚未实现SFBC发射分集。

控制头在payload之前完成MRC和BCH/CRC解码，`flags[3:2]`携带MCS，`flags[0]`携带rank，因此接收机不需要盲猜payload模式。

## 验证模型

验证使用正式1024/128帧、20 ppm、100 Hz连续Doppler、LS线性CSI和LDPC。为反映1至2 m/s下相邻帧信道具有相关性，测试把同一2x2瑞利信道保持8帧，再切换到下一独立相干块；噪声和payload每帧更新。

| SNR | 帧数 | 自适应CRC失败率 | 固定R2/64失败率 | 自适应goodput | 固定goodput |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 40 dB | 64 | 0 | 0 | 35.314 Mb/s | 31.289 Mb/s |
| 32 dB | 48 | 0 | 0 | 28.022 Mb/s | 31.289 Mb/s |
| 28 dB | 48 | 2.083% | 16.667% | 23.404 Mb/s | 26.074 Mb/s |

40 dB时控制器利用Rank-2/256-QAM把goodput提高约12.9%。28 dB压力点中，固定Rank-2在一个病态相干块内连续失败；自适应首帧收到CRC失败后切换Rank-1，把失败率从16.7%降至2.1%，代价是goodput下降约10.2%。这符合第一版“可靠性优先”的门限设计。

## 工程结论

闭环已经真实改变下一帧的资源容量、调制、rank、LDPC块数和控制头，不是离线标签统计。当前策略适合保守工程基线，但32 dB附近可能过早使用Rank-1，后续可以根据实测PER曲线微调0.05的rank阈值和2 dB裕量。

运行命令：

```bat
.venv\Scripts\python.exe experiments\validate_adaptive_link.py
.venv\Scripts\python.exe experiments\validate_adaptive_link.py --frames 48 --snr-db 28 --output-dir ..\measurement\adaptive_link_28db
```
