# C++正式帧黄金向量接口

运行：

```bat
.venv\Scripts\python.exe experiments\export_cpp_frame_vectors.py
```

输出目录为`measurement/cpp_frame_vectors/`。`manifest.json`记录每个文件的类型、形状、字节数和SHA-256；所有多字节数均为小端，packed bit在每字节内按MSB优先，复数采用`std::complex<float>`兼容的real/imag交错布局。

主要向量包括：

- `data_fft_indices_i16.bin`、`pilot_fft_indices_i16.bin`：原生FFT数组索引；
- `control_data_positions_i16.bin`：控制RE在672个数据子载波中的位置；
- `payload_time_indices_u8.bin`、`payload_data_positions_i16.bin`：1216个载荷物理RE的时间/频率位置；
- `information_bytes_u8.bin`：880字节确定性载荷和2字节CRC；
- `codewords_packed_msb_u8.bin`：14个原始LDPC码字；
- `transmitted_bits_packed_msb_u8.bin`：扰码、交织后的发射bit；
- `control_labels_u8.bin`、`payload_labels_u8.bin`：QPSK控制和完整2432标签载荷区；
- `payload_symbols_cf32.bin`：未做天线功率缩放的双层64-QAM；
- `tx_grid_cf32.bin`：形状`[2 symbol, 1024 FFT, 2 Tx]`；
- `tx_time_cf32.bin`：形状`[3 symbol, 2 Tx, 1152 sample]`，含一个ZC前导。

C++单元测试应先核对文件SHA-256，再按上述顺序逐级比较。频域/时域浮点比较建议先用绝对误差`1e-5`；bit、label和索引必须完全一致。

## Rank/MCS实时接口

Python函数`recommend_2x2_rank_mcs()`的C++输入仅需每帧平均Gram矩阵的三个独立元素`a=G00`、`d=G11`、`b=G01`，以及两层平均检测MSE。两个特征值使用闭式公式：

```text
lambda_max/min = 0.5 * (a + d +/- sqrt((a-d)^2 + 4*|b|^2))
ratio = lambda_min / max(lambda_max, epsilon)
```

默认当`ratio >= 0.05`且Rank-2瓶颈SINR留出2 dB实现裕量后不低于QPSK门限时保持Rank-2，否则下一帧回退Rank-1。MCS阈值表为QPSK/16-QAM/64-QAM/256-QAM对应`4/10/18/26 dB`。Python已实现一帧反馈延迟、CRC/质量快速降档以及连续3帧确认的逐级升档，并把rank写入mini-header的`flags[0]`。第一版Rank-1为Tx0单流加2Rx MRC，不是Alamouti/SFBC。

控制器参考实现为`openisac_phy/adaptive_link.py`，闭环验收为`experiments/validate_adaptive_link.py`，详细结果见`docs/ADAPTIVE_LINK_CLOSED_LOOP.md`。
