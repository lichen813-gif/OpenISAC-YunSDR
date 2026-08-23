# Python/C++ LDPC对齐基线

版本日期：2026-08-22  
适用范围：OpenISAC Python黄金模型与原C++实时链路的LDPC固定向量、软LLR和包级帧格式对照。

## 1. 当前状态

Python已新增与原C++一致的包级LDPC参考实现：

- LDPC `(N,K)=(1008,504)`，码率1/2；
- 每个信息块504 bit/63 byte，每个码字1008 bit；
- 读取仓库根目录`LDPC_504_1008.alist`和`LDPC_504_1008G.alist`；
- 输入字节按MSB优先展开；
- 编码结果与C++自定义`LDPC5041008SIMD`的生成矩阵、位序一致；
- 正LLR表示bit 0；
- Python译码器为6次水平分层normalized min-sum，并支持syndrome提前停止；
- 码字先用初值`0x5A`的8 bit LFSR扰码，再逐1008 bit块执行`21×48`转置交织；
- 接收端先逐块反交织，再对软LLR翻转符号完成软解扰；
- 控制区包含64个marker QPSK符号和64个BCH mini-header QPSK符号；
- mini-header为64 bit，包含版本、调制flags、payload长度、LDPC块数、序号和CRC-16；
- BCH采用`(127,64), t=10`，第128个填充bit置零。

实现文件：`openisac_phy/ldpc.py`。

Python译码器用于清晰、确定的算法验证，不替代C++实时端AFF3CT SIMD译码器。工程部署仍复用C++的float/int16水平分层NMS路径。

## 2. 固定黄金向量

输入信息块固定为：

```text
00 01 02 ... 3e
```

即63字节，header使用64-QAM flag、序号`0x1234`。固定结果：

| 项目 | 黄金值 |
|---|---|
| mini-header word | `0x18003f011234c527` |
| 126-byte packed codeword SHA-256 | `829f8d6150c0bc13a310b74bd18f73a98d09fcea54d673177c72611f7bb4f189` |
| 扰码+交织后packed bits SHA-256 | `a575021d373a226dfefe0ae8b848752e62ab135e4b57b1e9d41aa0dc03d9bfcd` |
| 128个控制label的uint8 SHA-256 | `0b00bf28117d554431934a7cee8046420442da84dfc24c5a4be9a720c112f1d7` |

这些值用于后续Windows C++单元测试，任何位序、矩阵、扰码或交织变化都会直接导致摘要变化。

## 3. 验证结果

专项命令：

```bat
cd /d E:\openisac\OpenISAC-main\python_phy
.venv\Scripts\python.exe -m pytest tests\test_ldpc.py -q
.venv\Scripts\python.exe experiments\validate_ldpc_cpp_alignment.py
```

当前结果：

- LDPC专项pytest：7/7通过；
- 黄金向量及AWGN验收：7/7通过；
- BPSK/AWGN在-1 dB时：硬判信道BER `0.1033`，译码后BER `0.0200`；
- BPSK/AWGN在0 dB时：译码后BER约`7.75e-6`，FER `0.0039`；
- 14块、882字节、64-QAM信息区在15 dB及以上完成无误码软解调/LDPC回环；
- BCH mini-header可修正任意已测试的10个bit错误。

结果目录：`measurement/ldpc_cpp_alignment/`，包含CSV、JSON、黄金向量和曲线图。

## 4. 与正式2×2 OFDM帧的关系

正式帧在两个数据符号上共有1344个物理数据RE，其中控制区占用第一个符号的128个物理RE，并让Tx1静默。其余1216个物理RE恢复双层空间复用：

```text
128个单层稳健QPSK控制RE
+ 1216个双层载荷RE = 2432个64-QAM标签容量
+ 14个LDPC码块 × 1008 bit / 6 bit per 64-QAM标签 = 2352个标签
+ 80个填充标签
```

14个信息块对应882字节信息区，其中最后2字节为CRC-16，用户数据为880字节。

`simulate_mimo_ofdm()`现已启用该混合映射、控制头软判决、MMSE Max-Log LLR、去交织/软解扰、LDPC译码和CRC统计。非LDPC配置仍保留原统一QAM路径，用于基线比较。

## 5. 下一步

1. 用`experiments/export_cpp_frame_vectors.py`输出完整正式帧二进制向量；
2. 在C++侧逐项校验资源位置、控制标签、码字、发射网格和时域IQ；
3. 将Python给出的低复杂度rank/MCS建议闭环到下一帧控制头；
4. 在Windows目标CPU上测试MMSE、软解调和LDPC的p50/p99耗时。
