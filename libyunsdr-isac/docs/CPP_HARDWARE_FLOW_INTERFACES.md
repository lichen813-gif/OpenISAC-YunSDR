# C++ 收发与视频硬件流程接口设计

状态：接口和 fake backend 单元测试已实现；真实 Y240 尚未连接。
目标模式：SISO、2x2 Rank-2 空间复用、2x2 Rank-1 Alamouti STBC。

## 1. 设计目标

厂商的 `yunsdr_ss_rate` 和 `yunsdr_ss_txrx_multiport` 已验证了推荐的设备流程：

```text
打开设备
  -> 查询型号/固件/拓扑
  -> 配置参考时钟、PPS、FDD
  -> 配置各 RF chip 的 LO/采样率/带宽/增益
  -> 固定 TX/RX channel mask
  -> 启动 timestamp/流
  -> RX 先运行并取得首个 timestamp
  -> multiport TX/RX
  -> 查询 timeout/underflow/overflow
  -> 停流
  -> 关闭设备
```

新接口把该流程固定在 `TransceiverSession`，收发测试和 VLC/UDP 视频链路复用同一
生命周期，不各自直接调用厂商 C API。

## 2. 已实现接口

### `transceiver.hpp`

- `ModeProfile`：物理端口、空间 rank、方案和 channel mask；
- `RadioSettings`：`pcies:0.0`、采样率、LO、带宽、增益、参考时钟和 FDD；
- `IVendorTransport`：真实 `libyunsdr` 与 fake backend 的最小边界；
- `TransceiverSession`：打开、配置、启动、RX、定时 TX、事件和有序关闭；
- `MutableMultiChannelBuffer`/`ConstMultiChannelBuffer`：每通道一个连续 buffer，
  热路径不要求重新分配；
- 软件 timestamp 连续性检查，并与厂商事件计数合并。

### `phy_pipeline.hpp`

- `IPhyFrameCodec::encode()`：UDP payload 到无信道损伤的正式 TX IQ；
- `IPhyFrameCodec::decode()`：硬件 RX IQ 到 CRC 校验后的 payload；
- `IVideoPacketSource`/`IVideoPacketSink`：隔离 VLC/UDP 与 PHY/SDR；
- `VideoFlowConfig`：只定义有界队列和超时，不把 socket 放入设备层。

厂商头文件、packed IQ16 和增益换算只能出现在未来的
`LibYunSdrTransport` 私有实现中，不能进入上述公共接口。

## 3. 模式映射

| 模式 | Tx/Rx 端口 | Rank | 方案 | TX/RX mask |
| --- | ---: | ---: | --- | ---: |
| SISO | 1/1 | 1 | spatial 单流 | `0x1/0x1` |
| 2x2 MIMO | 2/2 | 2 | spatial multiplexing + MMSE | `0x3/0x3` |
| 2x2 STBC | 2/2 | 1 | Alamouti | `0x3/0x3` |

三种模式只改变 `ModeProfile` 和 PHY codec，不改变设备生命周期、DMA buffer
布局、时间戳或视频队列接口。STBC 虽然只有一个空间数据流，仍必须向两个物理
TX 端口提供独立时域 IQ 分支。

## 4. 收发线程模型

```text
UDP ingress thread -> bounded packet queue -> PHY encode thread
  -> bounded TX-frame queue -> YunSDR TX thread

YunSDR RX thread -> continuous IQ ring / frame assembler
  -> bounded RX-frame queue -> PHY decode + LDPC/CRC thread
  -> bounded decoded-packet queue -> UDP egress thread

low-rate event thread -> timeout / underflow / overflow / timestamp telemetry
```

要求：

1. `open/configure/start` 在创建 TX/RX 工作线程前完成；
2. RX 先启动并成功返回一个 block，之后才允许 TX；
3. 一个 RX 线程和一个 TX 线程可并行调用 session；
4. timestamp 元数据以 block 为粒度加锁，不在样点循环加锁；
5. 所有队列有固定容量，满时记录并采用明确的丢帧策略；
6. 停止顺序为 UDP ingress、TX、RX、事件线程、停流、关设备；
7. overflow 或 timestamp 跳变必须清空 PHY 分帧状态并重新搜索 ZC。

## 5. OpenISAC PHY 对接

现有接收入口已经适合硬件：

```text
DynamicLinkCaptureFrame
  -> prepare_captured_iq_frame()
  -> DynamicLinkPipeline::submit_capture()
  -> LDPC / CRC
```

发送侧不能直接使用 `generate_dynamic_tdl_iq_frame()`，因为该函数还会加入
TDL、AWGN、CFO 和 SFO。下一步应从其中抽出纯发送部分，形成
`OpenIsacPhyFrameCodec::encode()`：

```text
encode_dynamic_frame()
  -> ZC 前导
  -> FDM pilot 或 NR-DMRS
  -> 每个物理 TX 端口独立 OFDM 调制
  -> 无损伤时域 IQ
```

接收实现 `decode()` 时，将 session 返回的每端口连续 IQ 和硬件 timestamp
填入 `DynamicLinkCaptureFrame`。SISO 传 1 个 branch；2x2 MIMO/STBC 传 2 个
branch。硬件模式必须关闭 simulator truth 诊断，EVM 改为导频/判决参考估计。

## 6. 无硬件验证结果

`libyunsdr_isac_transceiver_test` 使用 fake backend 覆盖：

- 三种模式的端口、rank、scheme 和 mask；
- `pcies:0.0` 打开与配置顺序；
- 未完成首个 RX 时禁止 TX；
- 从 RX block timestamp 推导定时 TX；
- SISO 与两端口 multiport buffer；
- timestamp 连续和跳变检测；
- stop 后 close 的有序释放。

这些测试只验证接口和控制流，不代表驱动、DMA、射频或 Y240 硬件已经通过。

## 7. 后续实现顺序

1. 实现动态加载 `libyunsdr_ss.dll` 的 `LibYunSdrTransport`；
2. 用 `yunsdr_ss_rate -r 1` 的结果校准 packed IQ16 和 RX block 行为；
3. 实现 OpenISAC 纯 TX waveform codec，先完成 SISO 离线闭环；
4. 接硬件 SISO RX-only、单音回环、正式 PHY、视频；
5. 复用同一接口依次放开 2x2 MIMO 和 2x2 STBC。
