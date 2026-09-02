# libyunsdr 完整学习与 YunSDR Y240 接入指南

本文把本仓库中分散的 `libyunsdr` 分析、架构、构建、接口、收发、测试和
硬件验证内容整理成一份可以独立阅读的中文学习手册。阅读本文不要求先了解
OpenISAC，但需要具备基本的 C/C++、复数 IQ、采样率和 SDR 收发概念。

适用基线：

- 目标硬件：YunSDR Y240；
- 目标系统：Windows x64；
- 工具链：Visual Studio 2019、MSVC x64、VS 自带 CMake/Ninja；
- SDK 基线：`libyunsdr-26-01-00.1`；
- 已验证设备地址：`pcies:0.0`；
- OpenISAC 采样率：15.36 Msps；
- 本文状态日期：2026-08-30。

厂商 SDK 的头文件、导入库、DLL、驱动、固件和原始手册可能受再分发许可限制，
因此不包含在公开仓库中。本文讲解的是本项目已经分析或验证过的接口语义，不能
代替与实际 SDK 版本配套的厂商手册和硬件安全规格。

## 1. 如何判断一条结论是否可信

本项目经历过静态分析、接口设计、厂商程序测试和 OpenISAC 实机闭环多个阶段。
为了避免把早期计划误当成最终事实，本文采用以下证据等级：

| 等级 | 含义 | 可以如何使用 |
| --- | --- | --- |
| A：当前源码 | 当前适配器真实执行的行为 | 可直接用于理解和修改本仓库代码 |
| B：Y240 实机验证 | 2026-08-25 在 Y240 `pcies:0.0` 上得到的结果 | 可作为相同硬件、SDK 和连接条件的复现基线 |
| C：静态分析/计划 | 从迁移包、参考源码或早期设计推导 | 实现前必须用实际 SDK 和硬件重新确认 |

如旧文档与本文冲突，按“当前源码 > 后续实机结果 > 早期静态分析/计划”的顺序
判断。早期文档中的“尚未连接硬件”等状态描述只代表当时阶段，不是当前状态。

## 2. libyunsdr 在系统中的位置

`libyunsdr` 是 YunSDR 的设备访问层，负责打开设备、设置射频参数、启动时间戳、
读写多通道 IQ 和查询硬件事件。它不负责 OpenISAC 的 OFDM、MIMO、LDPC、CRC、
同步、信道估计或感知算法。

完整数据流如下：

```text
应用数据/VLC UDP
  -> OpenISAC 分片和正式 PHY 编码
  -> 每个物理 TX 端口的 complex<float> 时域 IQ
  -> libyunsdr-isac 格式转换和时间戳调度
  -> libyunsdr multiport 写入
  -> Y240 TX / 射频连接 / Y240 RX
  -> libyunsdr multiport 读取
  -> 每个物理 RX 端口的 complex<float> 时域 IQ
  -> OpenISAC ZC 同步、OFDM、信道估计、MIMO、LDPC/CRC
  -> 恢复应用数据/VLC UDP
```

理解这个边界很重要：

- `libyunsdr` 处理设备、DMA、射频配置和采样流；
- `libyunsdr-isac` 处理厂商 API 到项目公共接口的适配；
- `cpp_phy/openisac_phy` 处理物理层算法；
- VLC/UDP、监视器和日志属于应用与观测层。

## 3. 仓库中的实现层次

### 3.1 厂商无关的会话层

[`include/libyunsdr_isac/transceiver.hpp`](../include/libyunsdr_isac/transceiver.hpp)
定义：

- `IVendorTransport`：最小厂商传输接口；
- `TransceiverSession`：设备生命周期和时间戳状态机；
- `RadioSettings`：采样率、频率、带宽、增益、参考时钟和双工模式；
- `ModeProfile`：端口数、空间 rank、方案和 channel mask；
- `MutableMultiChannelBuffer` / `ConstMultiChannelBuffer`：多通道 IQ 视图；
- `ChannelEventCounters`：超时、下溢、上溢和时间戳不连续统计。

这一层不包含 `yunsdr_api_ss.h`，因此 fake backend、单元测试和上层算法不需要
链接厂商 SDK。

### 3.2 真实 libyunsdr 适配层

[`src/libyunsdr_transport.cpp`](../src/libyunsdr_transport.cpp) 是当前真实实现。只有
启用 `LIBYUNSDR_ISAC_ENABLE_HARDWARE=ON` 时它才参与编译。厂商头文件被限制在
这个实现文件内，不泄漏到公共头文件。

它完成：

- `yunsdr_open_device()` / `yunsdr_close_device()`；
- 型号、固件和设备拓扑查询；
- 参考时钟、PPS、FDD/TDD 配置；
- TX/RX LO、采样率、带宽和增益配置；
- timestamp 关闭、重新开启和流起始；
- multiport IQ16 与 `complex<float>` 相互转换；
- TX/RX channel event 查询。

### 3.3 OpenISAC PHY 桥

[`include/libyunsdr_isac/openisac_phy_codec.hpp`](../include/libyunsdr_isac/openisac_phy_codec.hpp)
和 [`src/openisac_phy_codec.cpp`](../src/openisac_phy_codec.cpp) 把应用 payload 转为
正式 PHY IQ，并把采集 IQ 解码为通过 CRC 的 payload。

厂商层和 PHY 层分离后，可以单独判断问题属于：

1. 设备/驱动/SDK；
2. 时间戳和流调度；
3. IQ 格式或通道映射；
4. PHY 同步、估计、检测或译码；
5. UDP/VLC 应用。

## 4. 设备 URI 与 PCIE 名称

当前 Y240 已验证 URI 是：

```text
pcies:0.0
```

不要混淆三个名字：

| 名称 | 本项目含义 |
| --- | --- |
| `pcie` | 另一条构建/接口路径，当前 Y240 构建关闭 |
| `pciex` | Windows 辅助实现和链接依赖；不是当前设备 URI |
| `pcies` | Y240 实际使用的设备后端，依赖 `libpcies` |

厂商兼容 DLL 构建时同时使用：

```text
ENABLE_PCIE=OFF
ENABLE_PCIEX=ON
ENABLE_PCIES=ON
ENABLE_FIRMWARE=ON
ENABLE_USB3=ON
```

`ENABLE_PCIEX=ON` 的原因是 `libpcies.lib` 依赖 `libfirmware.lib`，并引用 PCIEX
实现中的辅助符号；这不表示运行时应把 URI 改成 `pciex:0`。`ENABLE_USB3=ON`
用于维护一份同时兼容 PCIES 和 USB3 的 DLL；使用 `pcies:0.0` 时实际仍走 PCIES。

## 5. 设备生命周期与推荐调用顺序

推荐流程是：

```text
打开设备
  -> 查询型号、固件、子设备数、RF 芯片数
  -> 设置参考时钟和 PPS
  -> 设置 FDD/TDD、LO、采样率、带宽和增益
  -> 固定 TX/RX channel mask
  -> 重置并启动 timestamp
  -> RX 先读到第一个有效 block
  -> 从 RX timestamp 推导未来 TX timestamp
  -> multiport TX/RX
  -> 低频查询 timeout/underflow/overflow
  -> 停止 timestamp/流
  -> 关闭设备
```

当前 `TransceiverSession` 状态机为：

```text
closed -> configured -> streaming -> stopped -> streaming
   ^                                      |
   +---------------- close() <------------+
```

关键约束：

- `open_and_configure()` 只能从 `closed` 调用；
- `start()` 只能从 `configured` 或 `stopped` 调用；
- `receive()` / `transmit()` 只能在 `streaming` 状态调用；
- 默认要求 RX 已经返回有效 block 后才能 TX；
- 析构函数会调用 `close()`，异常路径也会关闭设备；
- 推荐一个 RX 线程和一个 TX 线程并行使用同一 session；配置和启停在工作线程
  启动前完成。

## 6. 当前实际调用的厂商 API

### 6.1 打开和身份信息

```text
yunsdr_set_log_level(LOG_LVL_INFO)
yunsdr_open_device(uri)
yunsdr_get_model_version(...)
yunsdr_get_firmware_version(...)
yunsdr_get_device_configuration(...)
```

打开失败时 `yunsdr_open_device()` 返回空指针。其他当前使用的控制 API 若返回
负数，适配器会抛出包含操作名称和返回码的异常。

### 6.2 时钟、PPS 和双工

```text
yunsdr_set_ref_clock(..., INTERNAL_REFERENCE/EXTERNAL_REFERENCE)
yunsdr_set_pps_select(..., PPS_INTERNAL_EN/PPS_EXTERNAL_EN)
yunsdr_set_rx_ant_enable(...)
yunsdr_set_trxsw_fpga_enable(...)
yunsdr_tx_cyclic_enable(...)
yunsdr_set_duplex_select(..., FDD/TDD)
```

默认配置使用内部参考、内部 PPS 和 FDD。切换外部参考前，必须保证外部时钟和
PPS 的频率、电平及连接满足 Y240 手册要求。

### 6.3 每个 RF 芯片的射频参数

```text
yunsdr_set_tx_lo_freq(...)
yunsdr_set_tx_sampling_freq(...)
yunsdr_set_tx_rf_bandwidth(...)
yunsdr_set_tx1_attenuation(...)
yunsdr_set_tx2_attenuation(...)

yunsdr_set_rx_lo_freq(...)
yunsdr_set_rx_sampling_freq(...)
yunsdr_set_rx_rf_bandwidth(...)
yunsdr_set_rx1_gain_control_mode(..., RF_GAIN_MGC)
yunsdr_set_rx2_gain_control_mode(..., RF_GAIN_MGC)
yunsdr_set_rx1_rf_gain(...)
yunsdr_set_rx2_rf_gain(...)
```

当前适配器遍历 `device_->nchips`，对每个 RF 芯片配置两路 TX/RX。实际启用哪些
逻辑通道由后续 multiport 的 channel mask 决定。

当前 Y240 TX 参数映射为：

```text
attenuation_mdb = (90.0 - tx_gain_db) * 1000
```

这只是本项目当前 Y240/SDK 的映射，不应推广到其他 YunSDR 型号。RX 使用手动
增益 `RF_GAIN_MGC`，并把 `rx_gain_db` 四舍五入为整数传入 RF gain API。

### 6.4 timestamp 和流

```text
yunsdr_enable_timestamp(device, 0, 0)
等待 1 秒
yunsdr_enable_timestamp(device, 0, 1)
等待 1 秒
```

停止时再次关闭 timestamp。此重置过程用于建立可预测的设备样点时间基准。

### 6.5 多端口 IQ 读写

```text
yunsdr_read_samples_multiport(
    device, per_channel_buffers, samples_per_channel,
    rx_channel_mask, &timestamp)

yunsdr_write_samples_multiport(
    device, per_channel_buffers, samples_per_channel,
    tx_channel_mask, timestamp, flags)
```

本项目统一使用 multiport API，即使 SISO 也使用 mask `0x1`。这样 SISO、2×2
空间复用和 2×2 STBC 共用同一条设备数据路径。

## 7. 多通道 buffer 与 channel mask

multiport 不是“每个样点依次交织所有通道”，而是“每个通道一个连续 buffer”：

```text
pointers[0] -> ch0: I0,Q0,I1,Q1,...
pointers[1] -> ch1: I0,Q0,I1,Q1,...
```

三种编号必须分清：

| 表示 | 编号规则 | 示例 |
| --- | --- | --- |
| OpenISAC 通道数组 | 0 起始 | `channels[0]`、`channels[1]` |
| multiport channel mask | bit 0 对应逻辑通道 0 | `0x1`、`0x3` |
| 某些事件查询 API | 1 起始 | 通道 1、通道 2 |

当前模式映射：

| 模式 | TX/RX 端口 | Rank | TX/RX mask |
| --- | ---: | ---: | ---: |
| SISO | 1/1 | 1 | `0x1/0x1` |
| 2×2 空间复用 | 2/2 | 2 | `0x3/0x3` |
| 2×2 Alamouti STBC | 2/2 | 1 | `0x3/0x3` |

STBC 只有一个空间数据流，但仍有两个独立物理 TX IQ 分支。不能因为 rank 为 1
就只提供一个 TX buffer。

当前 `libyunsdr-isac` 公共 buffer 上限是 2 个端口。主 OpenISAC C++ PHY 已有
4×4/8×8 CPU/CUDA 算法，不等于当前 Y240 硬件适配器已实现 4/8 端口收发。

## 8. IQ 数据格式与数值转换

参考资料中的“IQ32”指一个复样点占 32 bit：16 bit I + 16 bit Q，不是两个
32-bit float。当前适配器实际用交错 `int16_t` 保存每个通道：

```text
packed[2*n]     = I
packed[2*n + 1] = Q
```

RX 转为 `complex<float>`：

```text
I_float = I_int16 / 32768.0
Q_float = Q_int16 / 32768.0
```

TX 转为 IQ16：

```text
输入限制到 [-1.0, 0.9999695]
量化值 = round(sample * 32768.0)
```

学习和移植时必须用已知复数序列确认：

1. I/Q 顺序；
2. 正负号和频谱方向；
3. 字节序；
4. 每通道连续还是通道交织；
5. 幅度缩放和饱和边界；
6. API 的 count 单位是复样点还是标量分量。

不要仅凭频谱“看起来正确”就跳过这些检查。I/Q 交换或 Q 取反可能仍能看到单音，
但会改变频率方向并破坏高阶调制和 MIMO 相位关系。

## 9. timestamp 的核心语义

时间戳以设备样点时钟为调度基础。当前代码把一个 block 的下一时刻计算为：

```text
expected_next_rx = last_rx_timestamp + last_rx_samples
tx_timestamp = last_rx_timestamp + last_rx_samples + tx_lead_samples
```

使用原则：

- `timestamp=0` 在厂商参考语义中表示立即发送；
- 非零 timestamp 表示定时发送；
- 本项目默认不允许在首个 RX block 到来前发送；
- 传入 `transmit(..., timestamp=0, ...)` 且启用 RX priming 时，session 会自动
  调用 `next_transmit_timestamp()`；
- 当前 RX timestamp 如果不等于上一 block 的 timestamp 加样点数，软件会增加
  `timestamp_discontinuities`；
- timestamp 跳变、RX overflow 或设备重启后，PHY 分帧器必须丢弃未完成帧并
  重新搜索 ZC 前导。

设备发送队列需要未来量。视频桥用 `lead_blocks` 把 TX 安排到若干 DMA block
之后。过小容易出现“计划时间已经过去”的 TX timeout；过大则增加端到端延迟。

## 10. 事件与错误处理

当前适配器轮询：

```text
TX_CHANNEL_TIMEOUT
TX_CHANNEL_UNDERFLOW
RX_CHANNEL_TIMEOUT
RX_CHANNEL_OVERFLOW
```

事件 API 的通道号从 1 开始，因此逻辑通道 0 查询时传入 1。

推荐处理：

| 事件 | 含义 | 处理原则 |
| --- | --- | --- |
| TX timeout | 定时命令过晚、DMA/调度压力或设备未接受 | 丢弃该发送计划，增加 lead，重新基于新 RX timestamp 调度 |
| TX underflow | 设备发送侧数据供应中断 | 记录事件并重建未来发送计划 |
| RX timeout | 规定时间内没有获得数据 | 有界重试；同时检查驱动、mask 和流状态 |
| RX overflow | 主机没有及时取走接收数据 | 清空分帧状态，重新同步，降低负载或优化 DMA/线程 |
| timestamp discontinuity | 样点流不连续 | 不拼接跨缺口 PHY 帧，重新搜索 ZC |
| API 负返回码 | 厂商调用失败 | 报告具体 API 和返回码，停止或有界重开设备 |
| 设备断开 | 驱动、线缆或设备状态丢失 | 停止 TX/RX 并关闭句柄，禁止静默切换到仿真 |

当前 `IVendorTransport` 有 `timeout_seconds` 参数，但当前厂商调用封装没有把它
传给底层 read/write API；它主要用于公共接口校验。不要据此假设底层调用一定能
在指定秒数返回。生产化前应根据实际 SDK 的取消/超时机制补充可中断封装。

## 11. 控制面与数据面必须分离

LO、采样率、带宽、增益和 channel mask 修改可能阻塞或触发设备内部复位。静态
分析中，某些 Windows 控制事务存在约 2000 次、每次 1 ms 的等待上限；RX mask
改变还可能导致约 1.2 秒等待。

因此：

- 配置操作只在启动、停止或明确重配置状态执行；
- RX/TX 热路径只调用读写 API；
- 运行期间固定 channel mask；
- 切换 SISO/MIMO/STBC 时停止并重建 session，不逐帧改 mask；
- 事件由低频线程轮询；
- UDP、PHY、TX、RX 和事件线程之间使用有界队列；
- 退出必须唤醒阻塞线程并有序停流、关设备。

推荐线程模型：

```text
UDP 输入 -> 有界 packet queue -> PHY 编码 -> 有界 TX frame queue -> TX

RX -> 连续 IQ/分帧 -> 有界 RX frame queue -> PHY 解码/LDPC/CRC
   -> 有界 decoded queue -> UDP 输出

低频事件线程 -> timeout/underflow/overflow/timestamp 遥测
```

## 12. DMA block 与 PHY 帧不是同一概念

OpenISAC 正式波形：

- FFT：1024；
- CP：128；
- FDM 帧：3 个 OFDM symbol，共 3456 复样点；
- NR-DMRS 帧：5 个 OFDM symbol，共 5760 复样点。

设备 DMA block 用于稳定搬运数据，不应强制等于 PHY 帧。早期工程候选值是
7680 样点，厂商测试也使用过 18432 和 30720 样点。连续 RX 应写入环形缓冲，
由 ZC 同步器跨 block 搜索并抽取 3456/5760 样点帧。

这样做可以：

- 独立优化 DMA 吞吐；
- 兼容不同长度的 FDM/DMRS 帧；
- 容忍前导跨越 block 边界；
- 在 overflow 后明确重置同步状态。

## 13. 吞吐预算

IQ16 每复样点 4 byte，不计厂商帧头和对齐：

| 模式 | 单方向 | 全双工 |
| --- | ---: | ---: |
| SISO @ 15.36 Msps | 61.44 MB/s | 122.88 MB/s |
| 2 通道 | 122.88 MB/s | 245.76 MB/s |
| 4 通道 | 245.76 MB/s | 491.52 MB/s |

如果应用边界使用两个 float 的 `complex<float>`，内存侧是 8 byte/复样点，转换和
内存带宽需求约翻倍。视频码率只有应用 payload，不能代表 SDR DMA 压力。

## 14. 构建和部署

### 14.1 必需环境

- Visual Studio 2019 C++ x64 工具集；
- Windows SDK；
- VS Installer 附带的 `vswhere.exe`；
- 与 Y240 匹配的 `libyunsdr` 源码/SDK；
- MSVC x64 libusb 1.0.23；
- Y240 Windows 驱动、固件/bitstream 和 PCIES 运行环境。

公开仓库不会分发这些厂商文件。

### 14.2 输入目录

构建脚本默认从仓库根目录的忽略目录读取：

```text
import/
  libyunsdr-26-01-00.1.zip.zip
  vendor-y240-26-01-00.1/source/
  libusb-1.0.23/MS64/dll/libusb-1.0.lib
```

不要把 `import/`、厂商 DLL/LIB/PDB、驱动或固件提交到公开 Git。
全新克隆只需先放置压缩包和 libusb；构建脚本会自动生成上面的 `source/`
目录。也可以先运行 `.\prepare_vendor_y240_sdk.ps1` 单独验证单层或双层压缩包。

### 14.3 生成兼容 SDK

```powershell
cd libyunsdr-isac
.\build_vendor_y240_pcies_vs2019.cmd
.\build_hardware_vs2019.cmd
```

脚本会强制检查 `libpcies.lib`、`libfirmware.lib` 和 `libusb-1.0.lib`，生成并暂存：

```text
out/y240-sdk-26-01-00.1/
  include/yunsdr_api_ss.h
  lib/yunsdr_ss.lib
  lib/libyunsdr_ss.lib
  lib/libpcies.lib
  lib/libfirmware.lib
  bin/libyunsdr_ss.dll
  bin/libusb-1.0.dll
  bin/yunsdr_ss_rate.exe
  bin/yunsdr_ss_txrx_multiport.exe
```

`libusb-1.0.dll` 必须与 `libyunsdr_ss.dll` 放在同一运行目录。

### 14.4 构建适配器和工具

```powershell
.\build_vs2019.cmd
```

该脚本明确设置：

```text
LIBYUNSDR_ISAC_ENABLE_HARDWARE=ON
```

因此必须先具备完整的暂存 SDK。构建会同时运行三个回归测试，并把
`libyunsdr_ss.dll`、`libusb-1.0.dll` 复制到硬件工具旁边。

如果只想学习公共接口或运行无硬件测试，可手动配置：

```powershell
cmake -S . -B build\software-only -G Ninja `
  -DLIBYUNSDR_ISAC_ENABLE_HARDWARE=OFF
cmake --build build\software-only
ctest --test-dir build\software-only --output-on-failure
```

## 15. 一个最小收发程序应如何组织

下面是项目接口层的伪代码，不直接暴露厂商头文件：

```cpp
auto transport = libyunsdr_isac::make_libyunsdr_transport();
libyunsdr_isac::TransceiverSession session(*transport);

libyunsdr_isac::SessionConfig config;
config.radio.uri = "pcies:0.0";
config.radio.sample_rate_hz = 15.36e6;
config.radio.center_frequency_hz = 1.5e9;
config.radio.bandwidth_hz = 15.36e6;
config.radio.tx_gain_db = 60.0;
config.radio.rx_gain_db = 20.0;
config.mode = libyunsdr_isac::PhyMode::siso;
config.dma_block_samples = 7680;
config.tx_lead_samples = 48 * config.dma_block_samples;

auto identity = session.open_and_configure(config);
session.start();

// 1. 准备每通道连续的 RX complex<float> buffer。
// 2. 先 session.receive(...)，取得首个 RX timestamp。
// 3. 计算 session.next_transmit_timestamp()。
// 4. 准备每通道连续的 TX buffer。
// 5. session.transmit(..., future_timestamp, ...)。
// 6. 循环 RX/TX，并低频 session.poll_events()。

session.stop();
session.close();
```

真实程序还必须实现有界重试、退出信号、异常处理、buffer 生命周期和射频安全
检查。完整实现可参考 `tools/yunsdr_phy_loopback.cpp` 和
`tools/yunsdr_video_bridge.cpp`。

## 16. 从厂商工具开始验证

不要一开始就运行 OpenISAC 视频。先确认驱动、SDK 和 DMA。

### 16.1 DLL 和参数检查

```powershell
cd out\y240-sdk-26-01-00.1\bin
.\yunsdr_ss_txrx_multiport.exe -h
.\yunsdr_ss_rate.exe -h
```

厂商工具的 `-h` 可能返回非零退出码；能显示帮助且没有 DLL 缺失错误即可继续。

### 16.2 RX-only

以下频率只是示例，必须改成实际连接和合法实验条件允许的频率：

```powershell
.\yunsdr_ss_rate.exe -a pcies:0.0 -r 1 -c 0x1 -C 0x0 `
  -s 15360000 -f 1500000000 -g 6 -G 0 -N 18432 -n 5
```

检查：

- 报告 Y240；
- 实际采样率为 15.36 Msps；
- 单通道吞吐接近 61.44 MB/s 数量级；
- RX overflow 为 0；
- 程序能正常结束并释放设备。

### 16.3 SISO 单音回环

只有在确认 Y240 最大输入/输出并接入足够外部衰减后才能执行：

```powershell
.\yunsdr_ss_txrx_multiport.exe -a pcies:0.0 -c 0x1 -C 0x1 `
  -s 15360000 -f 1500000000 -g 6 -G 0 `
  -t 1000000 -T 0 -N 30720 -E 5
```

检查 1 MHz 单音方向、I/Q 顺序、timestamp 连续、削顶比例和四类硬件事件。

### 16.4 全双工吞吐

```powershell
.\yunsdr_ss_rate.exe -a pcies:0.0 -r 2 -c 0x1 -C 0x1 `
  -s 15360000 -f 1500000000 -g 6 -G 0 -N 18432 -n 21
```

SISO 通过后才扩展为 `0x3/0x3`。厂商工具验证设备层，不代替 OpenISAC PHY 的
同步、EVM、CRC 和 FER 验证。

## 17. OpenISAC 硬件测试顺序

### 17.1 无硬件回归

三个测试分别覆盖：

- `libyunsdr_isac_baseline`：公共接口和 stub 基线；
- `libyunsdr_isac_transceiver`：模式、mask、状态机、RX priming、定时 TX、
  timestamp 连续性和有序关闭；
- `libyunsdr_isac_phy_codec`：正式 PHY 编解码。

这些测试通过不代表驱动、DMA 或射频已经通过。

### 17.2 确定性 PHY 回环

当前项目的已验证命令示例：

```powershell
build\ninja-vs2019-hardware\yunsdr_phy_loopback.exe `
  --device pcies:0.0 --mode siso --modulation 64qam --pilot fdm `
  --frames 20 --frequency-mhz 1500 --tx-gain 60 --rx-gain 20
```

模式可以改为 `mimo2` 或 `stbc`。增益只适用于已经验证的实验链路，不能照搬到
另一块设备、另一组端口或不同衰减器。

### 17.3 视频分片自测

```powershell
build\ninja-vs2019-hardware\yunsdr_video_bridge.exe `
  --device pcies:0.0 --mode siso --modulation 64qam --pilot fdm `
  --frequency-mhz 1500 --tx-gain 60 --rx-gain 20 `
  --self-test 64 --batch-packets 8 --lead-blocks 48 --retries 8
```

先使用 `--self-test`，确认分片、重组和硬件 PHY，再接 VLC UDP。

### 17.4 VLC 与监视器

```powershell
.\run_y240_video.cmd "C:\path\to\video.mp4" `
  siso 64qam fdm 1500 60 20 1000
```

监视器显示星座、时域 IQ、信道、EVM、CFO、SFO、定时、FER、硬件事件以及
距离-Doppler/CFAR。视频文件不属于源码仓库。

## 18. 已验证的 Y240 结果

截至 2026-08-25：

- `pcies:0.0` 已由厂商工具和 OpenISAC 工具验证；
- SISO、2×2 Rank-2 空间复用和 2×2 STBC 的 FDM/DMRS 编解码闭环通过；
- 厂商全双工 rate 测试在 15.36 Msps、mask `0x3/0x3` 下通过；
- PHY 回环成功帧：SISO 18/20、2×2 16/20、STBC 18/20；成功帧的 header、CRC
  和 payload 均正确，未成功 burst 与 TX timeout 同时出现；
- 2×2 固定数据视频自测恢复 64/64 packet，无重试和应用丢包；
- 实际 VLC 测试三种模式均恢复约 1245 个 packet，UDP 丢包为 0；
- 10 秒带监视器的 2×2 VLC 测试完成 1102 packet，应用层零丢包；
- 该次测试仍记录 718 个 TX-timeout 事件，桥接器通过重试恢复。因此“应用零丢包”
  不等于“设备事件为零”，TX 调度/DMA 压力仍是需要继续优化的部分。

这些数字是特定设备、SDK、驱动、线缆、衰减、频率和增益下的工程结果，不是
Y240 产品规格或所有环境的保证。

## 19. 增益、EVM 与饱和判断

硬件 EVM 使用判决导向方法：均衡点与最近理想 QAM 点比较。它适合扫描增益和
发现压缩，但符号越过判决边界时可能低估真实 EVM。

已验证链路的经验：

- RX 太低时不削顶，但 SNR 不足，64QAM EVM 很高；
- 提高 RX 增益通常先改善 EVM；
- RX 峰值接近 1.0 后继续加增益会进入压缩，EVM反而变差；
- 频率变化会同时改变设备、线缆、衰减器和端口路径表现；
- 不能用一组频率/增益结果推断所有射频连接。

调试时同时观察：

```text
CRC/FER + decision EVM + RX peak + clipping ratio
+ channel condition number + timeout/overflow/underflow
```

不要只看 CRC，也不要只看 EVM。

## 20. 常见问题定位

### 20.1 `yunsdr_open_device` 返回空指针

按顺序检查：

1. URI 是否明确为 `pcies:0.0`；
2. Y240 是否被 Windows 驱动正确枚举；
3. 驱动、固件/bitstream 和 SDK 是否配套；
4. DLL 位数是否为 x64；
5. 设备是否被另一个进程占用；
6. 厂商 `yunsdr_ss_rate` 是否也失败。

### 20.2 找不到 DLL 或入口点

检查 `libyunsdr_ss.dll` 和 `libusb-1.0.dll` 是否都在 EXE 旁边，导入库与 DLL
是否来自同一 SDK 构建，并确认不是把 Debug/Release 或 x86/x64 混用。

### 20.3 CMake 成功但 `pcies:0.0` 不可用

厂商 CMake 可能在找不到 `libpcies` 时继续配置但关闭 PCIES。检查
`CMakeCache.txt` 中 `LIBPCIES_LIBRARIES` 是否指向真实 `libpcies.lib`，并确认
`libfirmware.lib` 和导出 API 都存在。不能只看 CMake 返回码。

### 20.4 持续 TX timeout

检查：

- TX timestamp 是否已落后于设备时间；
- 是否先持续 drain RX，再启动应用输入；
- `lead_blocks` 是否过小；
- batch 是否过大；
- DMA/CPU 是否被绘图、日志或其他进程阻塞；
- timeout 后是否基于最新 RX timestamp 重建计划。

项目曾因 VLC 启动期间不持续读取 RX，使用了过期 timestamp，导致 TX timeout
和持续 CRC 失败；现在桥接器等待 UDP 时也会持续 drain RX。

### 20.5 RX overflow 或 timestamp 跳变

检查 RX 线程是否被 PHY/日志阻塞、buffer 和队列是否有界、DMA block 是否合理、
通道数带宽是否超过主机处理能力。发生缺口后不能继续拼接旧帧。

### 20.6 单音频率方向错误或高阶调制完全失败

优先检查 I/Q 顺序、Q 符号、字节序、每通道 buffer 布局和 count 单位，不要先
修改同步或均衡算法。

### 20.7 两通道表现差但 SISO 正常

检查 mask 到物理端口的映射、两通道 timestamp 对齐、共享 LO/参考时钟、线缆
长度和幅相差、端口隔离以及 MIMO 信道条件数。

## 21. 射频安全

禁止无衰减把 TX 直接连接到 RX。开始发射前必须从目标硬件手册确认：

- TX 最大输出功率；
- RX 最大安全输入功率；
- 各增益/衰减参数的实际定义；
- 所需固定衰减器、DC block 和线缆额定值；
- 中心频率和合法实验条件。

推荐顺序是 RX-only、低幅度 TX 配合功率测量、加入外部固定衰减后的 SISO 单音、
正式 PHY、视频，最后才扩展多端口。软件中的 TX gain/attenuation 不能替代外部
衰减器。

## 22. 当前实现的已知边界

- 公开硬件适配器目前只覆盖最多两端口，不宣称 Y240 4×4/8×8；
- `timeout_seconds` 尚未映射到厂商底层可取消超时；
- 当前 read/write 格式转换缓冲会 `resize`，容量通常会复用，但还不是严格证明的
  全程零分配实时实现；
- 当前实现假定 multiport 调用完成请求的每通道样点数，短传输语义需继续结合
  SDK 验证；
- TX gain 到毫分贝衰减的换算是 Y240 当前工程映射，不是通用公式；
- 事件使用轮询计数器，不是异步消息队列式接口；
- 硬件视频可以通过重试实现应用层零丢包，但仍可能存在 TX timeout；
- 厂商 SDK 和驱动不在公开仓库中，克隆仓库后不能直接完成硬件构建。

## 23. 推荐学习路线

### 第一天：建立整体认识

1. 阅读本文第 1～10 节；
2. 阅读 `transceiver.hpp`；
3. 运行无硬件三项 CTest；
4. 用 fake backend 跟踪 `closed -> configured -> streaming -> stopped`。

### 第二天：理解真实适配

1. 对照本文第 6～10 节阅读 `libyunsdr_transport.cpp`；
2. 手工画出通道数组、mask、事件通道号之间的转换；
3. 用几个已知复数计算 IQ16 量化和反量化；
4. 推导 RX timestamp、block 长度、lead samples 和 TX timestamp。

### 第三天：理解构建和厂商工具

1. 阅读 `build_vendor_y240_pcies_vs2019.cmd`；
2. 检查 CMake 的 PCIES/firmware/libusb 硬门槛；
3. 依次理解 `rate -r 1`、multiport 单音、`rate -r 2` 的验证对象；
4. 不接 TX，先完成帮助/DLL/RX-only 检查。

### 第四天及以后：OpenISAC 实机链路

1. 阅读 `yunsdr_phy_loopback.cpp`；
2. 从 SISO QPSK/FDM 开始；
3. 同时记录 CRC、EVM、peak、clipping 和设备事件；
4. 再增加 64QAM、DMRS、2×2、STBC；
5. 最后阅读 `yunsdr_video_bridge.cpp` 和 VLC 启动脚本。

## 24. 术语速查

| 术语 | 含义 |
| --- | --- |
| LO | 本振频率，决定射频中心频率 |
| IQ16 / IQ32 sample | I、Q 各 16 bit，一个复样点合计 32 bit |
| multiport | 一次 API 调用读写多个逻辑射频通道 |
| channel mask | 用 bit 位选择参与读写的逻辑通道 |
| DMA block | 一次设备/主机数据搬运块，不等于 PHY 帧 |
| timestamp | 设备样点时间，用于连续性检查和定时发送 |
| TX lead | 把发送安排到当前设备时间之后的安全样点间隔 |
| underflow | TX 需要数据时主机未及时提供 |
| overflow | RX 数据未被主机及时取走 |
| EVM | 接收星座与参考星座的误差矢量幅度 |
| FER | PHY 帧错误率 |
| FDM pilot | 频分复用导频，本项目为较短的三符号帧 |
| NR-like DMRS | 类 NR 解调参考信号，本项目为五符号帧 |
| STBC | Alamouti 空时分组编码，两个 TX 端口、空间 rank 1 |

## 25. 进一步阅读和源码入口

- [子项目中文说明](../README_zh.md)
- [架构边界](ARCHITECTURE.md)
- [C++ 收发与视频接口设计](CPP_HARDWARE_FLOW_INTERFACES.md)
- [导入的 libyunsdr 经验评估](IMPORTED_LIBYUNSDR_ASSESSMENT.md)
- [Y240 PCIES 构建评估](Y240_PCIES_BUILD_ASSESSMENT.md)
- [Y240 厂商测试程序验收](Y240_VENDOR_HARDWARE_TEST.md)
- [Y240 硬件视频测试和实测结果](Y240_HARDWARE_VIDEO.md)
- [硬件接入实施计划](SDR_HARDWARE_INTEGRATION_PLAN.md)
- [`TransceiverSession` 公共接口](../include/libyunsdr_isac/transceiver.hpp)
- [真实 libyunsdr 适配器](../src/libyunsdr_transport.cpp)
- [PHY 回环工具](../tools/yunsdr_phy_loopback.cpp)
- [视频桥工具](../tools/yunsdr_video_bridge.cpp)

学习时建议先使用本文建立统一模型，再把其他文档当成阶段证据和专题参考。遇到
冲突时，回到第 1 节的证据等级，并以实际 SDK 版本、硬件手册和当前源码为准。
