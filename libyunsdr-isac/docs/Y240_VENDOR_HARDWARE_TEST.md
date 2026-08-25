# Y240 厂商测试程序验收说明

适用版本：`libyunsdr-26-01-00.1`
目标：Windows x64、VS2019、Y240、`pcies:0.0`

## 1. 已生成程序

运行目录：

```text
out/y240-sdk-26-01-00.1/bin/
  libusb-1.0.dll
  libyunsdr_ss.dll
  yunsdr_ss_txrx_multiport.exe
  yunsdr_ss_rate.exe
```

重新生成：

```powershell
cd C:\path\to\OpenISAC-YunSDR\libyunsdr-isac
.\build_vendor_y240_pcies_vs2019.cmd
```

构建脚本会强制检查 `libpcies.lib` 和 `libfirmware.lib`，构建 DLL、静态库以及
两个厂商测试程序，并将运行文件放到同一 `bin` 目录。默认 DLL 同时兼容
PCIES 和 USB3；`libusb-1.0.dll` 必须与 `libyunsdr_ss.dll` 保持在同一目录。

两个程序源码默认使用 `pciex:0`。Y240 验收时必须显式传入 `-a pcies:0.0`，不能
依赖默认值。

## 2. 测试顺序

### 2.1 参数和 DLL 加载检查

```powershell
cd out\y240-sdk-26-01-00.1\bin
.\yunsdr_ss_txrx_multiport.exe -h
.\yunsdr_ss_rate.exe -h
```

程序的 `-h` 分支按厂商源码返回非零退出码，这是工具自身行为；能够显示完整
参数说明即表示 EXE 已找到并加载 `libyunsdr_ss.dll`。

### 2.2 RX-only 吞吐测试（第一项硬件测试）

此项不调用 IQ 发送线程，先用于确认驱动、`pcies:0.0`、Y240 型号识别、采样率、
接收 DMA 和事件统计：

```powershell
.\yunsdr_ss_rate.exe -a pcies:0.0 -r 1 -c 0x1 -C 0x0 `
  -s 15360000 -f 2450000000 -g 6 -G 0 -N 18432 -n 5
```

其中：

- `-r 1`：只测试 RX rate；
- `-c 0x1`：只打开第一个 RX 通道；
- `-C 0x0`：不分配 TX 通道；
- `-s 15360000`：OpenISAC 当前 15.36 Msps；
- `-N 18432`：单次 DMA 测试块样点数；
- `-n 5`：先做 5 轮短测试。

通过条件：

1. 打开设备成功并报告 `device type: Y240`；
2. 实际采样率与 15.36 Msps 一致；
3. 单通道 IQ32 理论 payload 为 61.44 MB/s，短测平均值应接近该数量级；
4. `RX Channel overflow` 为 0；
5. 测试结束打印 `Done`，进程和设备句柄正常退出。

中心频率必须根据实际连接和合法实验条件调整。RX-only 阶段仍应避免给 RX 端
输入超出手册限制的信号。

### 2.3 SISO 多端口 TX/RX 自回环

`yunsdr_ss_txrx_multiport` 在当前源码中通过编译宏固定为 TXRX，会发送单音，
不是纯探测程序。只有在 TX/RX 之间已经串接经手册确认的固定衰减器后才能运行：

```powershell
.\yunsdr_ss_txrx_multiport.exe -a pcies:0.0 -c 0x1 -C 0x1 `
  -s 15360000 -f 2450000000 -g 6 -G 0 `
  -t 1000000 -T 0 -N 30720 -E 5
```

`-G 0` 使厂商示例请求最大候选 TX 衰减，但不能替代外部衰减器，也不能在尚未
确认 Y240 增益换算时当作安全功率保证。禁止 TX 与 RX 无衰减直接连接。

通过条件：

1. RX timestamp 连续，无反复的 subframe discontinuity；
2. 接收数据中能检测到 1 MHz 单音，频率方向和 I/Q 顺序正确；
3. IQ 没有大面积达到 `int16` 满量程；
4. TX timeout、TX underflow、RX overflow 均为 0；
5. 生成的 `rx_iq_samples_ch0.dat` 可供 Python 做频谱、时域和幅度检查。

### 2.4 全双工持续吞吐

SISO 自回环通过后，再使用 rate 程序的 TXRX 模式：

```powershell
.\yunsdr_ss_rate.exe -a pcies:0.0 -r 2 -c 0x1 -C 0x1 `
  -s 15360000 -f 2450000000 -g 6 -G 0 -N 18432 -n 21
```

通过条件：TX/RX 平均 payload 均接近单通道 61.44 MB/s，且 timeout、underflow、
overflow 全部为 0。完成 SISO 后才把 mask 扩为 `0x3` 验证两通道。

## 3. 两个程序的角色

| 程序 | 主要验证 | 是否可能发射 |
| --- | --- | --- |
| `yunsdr_ss_rate -r 1` | 驱动、打开设备、RX DMA、吞吐和 overflow | 不启动 IQ TX 线程 |
| `yunsdr_ss_rate -r 2` | 全双工持续吞吐、underflow/overflow | 是 |
| `yunsdr_ss_txrx_multiport` | 多端口 buffer、时间戳、单音回环和通道映射 | 是，固定 TXRX |

这两个工具用于验证厂商层是否正常，不替代 OpenISAC 的 ZC 同步、OFDM、EVM、
CRC 和 FER 测试。厂商层全部通过后，再运行 `yunsdr_probe` 和 OpenISAC SISO
硬件自回环。
