# Y240 视频启动 `hardware warmup batch failed` 排查说明

## 1. 适用现象

本文用于排查 `run_y240_video.cmd` 或 `run_y240_video.ps1` 启动时出现的以下错误：

```text
Starting PHY bridge (attempt 1/3)...
FAIL: hardware warmup batch failed
Starting PHY bridge (attempt 2/3)...
FAIL: hardware warmup batch failed
Starting PHY bridge (attempt 3/3)...
FAIL: hardware warmup batch failed
PHY bridge did not pass hardware warmup after 3 startup attempts.
```

典型启动命令为：

```powershell
.\run_y240_video.cmd "D:\path\to\video.mp4" siso 64qam fdm 1500 60 20 1000
```

这个错误发生在 VLC 发送端和接收端启动之前，表示 Y240 射频物理层的固定数据
闭环没有通过，不表示视频文件损坏，也不是 VLC 编解码错误。

## 2. 预热测试实际做了什么

视频桥启动后先执行硬件预热：

1. 打开 `pcies:0.0`；
2. 配置 15.36 Msps、中心频率、带宽、TX/RX 增益和通道；
3. 启动时间戳；
4. 丢弃若干初始 RX DMA 块，使设备流进入稳定状态；
5. 生成默认 8 个、每个 1316 字节的确定性测试包；
6. 按正式 PHY 容量进行分片；
7. 经过 OFDM、FDM/DMRS、调制、LDPC、CRC 和 Y240 射频闭环；
8. 接收端完成同步、信道估计、均衡、解调、LDPC/CRC 和分片重组；
9. 将恢复数据与原始 8 个测试包逐字节比较。

只有所有测试包完全一致，启动脚本才会继续打开实时监视器和 VLC。默认每个视频桥
进程允许 8 次重试，即最多执行 9 次批传输；外层启动脚本还会重新启动视频桥三次。
三次都失败说明链路不是偶发单包错误，应先诊断硬件信号或时间戳，不应直接提高
视频码率。

相关实现：

- [视频桥预热与传输](../tools/yunsdr_video_bridge.cpp)
- [视频启动和三次重启](../run_y240_video.ps1)
- [Y240 传输后端](../src/libyunsdr_transport.cpp)

## 3. 从报错画面已经能够确认什么

如果画面已经显示 Y240 温度、FPGA 温度、PCIe 信息和设备连接码，通常表示：

- `libyunsdr_ss.dll` 与 `libusb-1.0.dll` 已经能够加载；
- Y240 驱动和 `pcies:0.0` 至少完成了设备打开；
- 命令行中的模式、调制、导频、频率和增益已经通过参数解析；
- 错误发生在硬件传输或 PHY 恢复阶段，而不是编译和文件查找阶段。

以下信息通常不是本错误的直接原因：

- `GPS is unlocked`：单台 Y240 内部时间戳有线闭环不依赖外部 GPS 锁定；
- 45°C 左右 RF 温度和 65°C 左右 FPGA 温度：属于截图所示的正常工作量级；
- 视频文件路径：预热阶段尚未读取 VLC 输入；
- 1000 kbit/s 视频码率：预热使用固定测试包，与 VLC 码率无关。

## 4. 首先读取完整日志

启动脚本将标准输出和厂商错误输出分开保存：

```text
libyunsdr-isac/out/hardware-video/
  bridge-YYYYMMDD-HHMMSS-attemptN.log
  bridge-YYYYMMDD-HHMMSS-attemptN.err.log
```

控制台为了保持简洁通常只显示错误日志末尾，真正的 PHY 失败分类通常位于不带
`.err` 的 `.log`。读取最近一次日志：

```powershell
cd D:\path\to\OpenISAC-YunSDR\libyunsdr-isac

$latest = Get-ChildItem .\out\hardware-video\bridge-*-attempt*.log |
  Where-Object Name -NotLike '*.err.log' |
  Sort-Object LastWriteTime -Descending |
  Select-Object -First 1

$latest.FullName
Get-Content -LiteralPath $latest.FullName -Tail 120
```

同时检查对应错误日志：

```powershell
Get-Content .\out\hardware-video\bridge-YYYYMMDD-HHMMSS-attempt3.err.log -Tail 120
```

## 5. 日志字段判读

| 日志现象 | 含义 | 优先检查 |
| --- | --- | --- |
| `timing=0 header=0 crc=0` | 没有找到可靠前导 | TX0/RX0 端口、线缆、中心频率、信号幅度 |
| `timing=1 header=0 crc=0` | 找到前导，但控制头无法恢复 | EVM、信道失真、增益、频偏、削顶 |
| `timing=1 header=1 crc=0` | 帧结构已锁定，但数据错误 | 64QAM EVM、噪声、相位变化、LDPC输入质量 |
| `capture timestamp mismatch` | 捕获块不是预定 TX 时间戳 | 旧 DMA 数据、设备占用、TX 调度、时间戳状态 |
| `TX timeout` 增长 | 计划发送时间已经错过 | 主机调度、PCIe压力、`LeadBlocks`、其他设备程序 |
| `TX underflow` 增长 | 发送数据供应不连续 | 主机负载、DMA、批量与调度 |
| `RX overflow` 增长 | 接收缓存来不及处理 | 主机负载、PCIe、其他进程、DMA读取节奏 |
| `timestamp discontinuity` 增长 | RX时间戳不连续 | 流重启、驱动/DMA、并发占用 |
| CRC通过但最终批失败 | 分片或包重组不完整 | packet/fragment序号、丢帧和重试日志 |

每次内部重试会输出类似：

```text
Batch decode failed; retry 1/8 (fragment 0 timing=0 header=0 crc=0)
```

这一行比最终的 `hardware warmup batch failed` 更有诊断价值。

## 6. 原因优先级

### 6.1 SISO 物理端口不匹配

SISO 模式使用 Y240 通道 0，即 TX0 和 RX0。日志中的：

```text
Rx channel 0x0 change to 0x1
```

表示只启用了 RX0，这是正常行为，但外部有线回环也必须连接到对应 TX0→RX0。
如果线缆接在第二组通道，设备可以正常打开，但接收不到预热信号。

### 6.2 接收电平不足或过载

1500 MHz、TX=60、RX=20 是此前特定线缆和衰减条件下验证过的参数，不保证另一套
线缆、衰减器或端口损耗相同。电平过低时前导检测或控制头失败；电平过高时削顶会
使 64QAM EVM 恶化。

只有在射频连接和外部衰减与已验证条件完全一致时，才可比较 RX=20 与 RX=25。
每次改变增益都应检查峰值、削顶率、EVM 和 CRC。禁止在未知衰减条件下盲目提高
TX 输出或将 TX/RX 无衰减直连。

### 6.3 其他程序占用设备

确认没有同时运行：

```powershell
Get-Process yunsdr_ss_rate,yunsdr_ss_txrx_multiport,yunsdr_phy_loopback,yunsdr_video_bridge `
  -ErrorAction SilentlyContinue
```

厂商 rate、multiport、旧回环或旧视频桥可能保留流和时间戳状态。关闭这些程序，
等待设备停止后再单独运行一个测试。

### 6.4 时间戳和 TX 调度

如果日志主要是 `capture timestamp mismatch` 或大量 `TX timeout`，说明射频幅度
可能不是首要问题。此时应：

1. 关闭所有 YunSDR 程序；
2. 重新打开设备，让时间戳关闭后再启用；
3. 检查主机是否有高负载任务；
4. 先运行短回环，而不是直接启动 VLC；
5. 确认仍有调度超时时，再将高级参数 `LeadBlocks` 从 48 调到 64。

## 7. 推荐的分层验证步骤

### 7.1 厂商持续收发检查

在已经确认安全的外部有线回环上，先确认之前能够工作的厂商测试仍然通过：

```powershell
.\out\y240-sdk-26-01-00.1\bin\yunsdr_ss_rate.exe `
  -a pcies:0.0 -f 1500000000 -g 25 -G 60 `
  -c 0x3 -C 0x3 -N 30720 -s 15360000 -r 2
```

如果该程序也失败，先处理驱动、设备、PCIe、端口或射频链路，不要继续排查 VLC。

### 7.2 SISO QPSK 回环

QPSK 对 EVM 的要求低于 64QAM，适合区分“完全没有链路”和“链路质量不足”：

```powershell
.\build\ninja-vs2019-hardware\yunsdr_phy_loopback.exe `
  --device pcies:0.0 `
  --mode siso `
  --modulation qpsk `
  --pilot fdm `
  --frames 20 `
  --frequency-mhz 1500 `
  --tx-gain 60 `
  --rx-gain 20
```

结果解释：

- QPSK 全部失败：优先检查端口、信号是否存在、时间戳和设备占用；
- QPSK 通过但 64QAM 失败：链路已连通，主要检查 EVM、增益和射频失真；
- QPSK 偶发失败并伴随 timeout：主要检查发送调度和 DMA 连续性。

### 7.3 SISO 64QAM 回环

QPSK 稳定后再恢复原调制：

```powershell
.\build\ninja-vs2019-hardware\yunsdr_phy_loopback.exe `
  --device pcies:0.0 `
  --mode siso `
  --modulation 64qam `
  --pilot fdm `
  --frames 20 `
  --frequency-mhz 1500 `
  --tx-gain 60 `
  --rx-gain 20
```

需要同时记录：成功帧数、EVM、RX峰值、clipping、timing index、TX timeout、
RX overflow 和 timestamp discontinuity。

### 7.4 固定包视频桥自检

正式 VLC 前使用 `--self-test` 检查视频分片和重组：

```powershell
.\build\ninja-vs2019-hardware\yunsdr_video_bridge.exe `
  --device pcies:0.0 `
  --mode siso `
  --modulation 64qam `
  --pilot fdm `
  --frequency-mhz 1500 `
  --tx-gain 60 `
  --rx-gain 20 `
  --self-test 16 `
  --batch-packets 8 `
  --lead-blocks 48 `
  --retries 8
```

只有该步骤通过，才继续运行 `run_y240_video.cmd`。

### 7.5 调度超时时的高级视频命令

仅当日志已经证明主要问题是 TX timeout，而不是同步/EVM 时，使用：

```powershell
.\run_y240_video.ps1 `
  -VideoFile "D:\path\to\video.mp4" `
  -Mode siso -Modulation 64qam -Pilot fdm `
  -FrequencyMHz 1500 -TxGain 60 -RxGain 20 `
  -VideoBitrateKbps 1000 `
  -BatchPackets 8 -LeadBlocks 64 -Retries 8
```

增加 `LeadBlocks` 只给主机更多发送准备时间，不能修复错误端口、无信号、削顶或
过差的 EVM。

## 8. 最短排查决策表

```text
厂商 rate/multiport 是否正常？
  否 -> 驱动、PCIe、设备、端口、射频链路
  是
   |
   +-- SISO QPSK 是否通过？
         否 -> 查看 timing、timestamp、timeout 和端口
         是
          |
          +-- SISO 64QAM 是否通过？
                否 -> 检查 EVM、RX增益、削顶、频偏
                是
                 |
                 +-- video bridge --self-test 是否通过？
                       否 -> 检查分片、重试、批量和调度
                       是 -> 再启动 VLC 视频、监视和感知
```

## 9. 需要保留的诊断材料

如果仍不能定位，请保留并提供：

1. 最新 `attemptN.log` 最后 120 行；
2. 对应 `attemptN.err.log` 最后 120 行；
3. QPSK 与 64QAM 回环的完整控制台输出；
4. 实际 TX/RX 端口和外部衰减连接方式；
5. `yunsdr_ss_rate -r 2` 是否仍能通过；
6. 当前 Git 提交：

```powershell
git rev-parse --short HEAD
```

有了上述信息即可区分射频电平、PHY同步、CRC、时间戳、TX timeout、RX overflow
或分片重组问题，避免通过反复改变增益掩盖真正原因。

## 10. 结论

`hardware warmup batch failed` 的准确含义是：Y240 已经进入硬件视频桥流程，但
固定预热测试包没有经过正式 PHY 和射频闭环被逐字节正确恢复。它是保护机制，
用于阻止 VLC 在底层链路尚未闭合时启动。排查时应先读不带 `.err` 的桥日志，
再按“厂商程序 → QPSK → 64QAM → 视频桥自检 → VLC”的顺序逐层验证。
