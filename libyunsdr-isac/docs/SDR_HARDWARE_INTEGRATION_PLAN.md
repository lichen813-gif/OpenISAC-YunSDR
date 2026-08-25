# SDR 硬件接入实施计划

目标：在 Windows + VS2019 上，通过 `libyunsdr` 将现有 OpenISAC C++ SISO
物理层接入一台支持同时收发的 YunSDR，先完成安全的射频线缆自回环，再完成
VLC 视频传输。SISO 稳定后再扩展 STBC、2x2 和 4x4。

本计划以 `IMPORTED_LIBYUNSDR_ASSESSMENT.md` 为输入。导入参考源码是只读证据，
真实实现必须以目标设备随附 SDK、驱动和硬件说明为准。

当前目标已冻结为 YunSDR Y240、Windows x64、VS2019，初始设备 URI 为
`pcies:0.0`。该含子设备号的地址已由真实 Y240 和 SDK 验证，作为项目默认 URI。
`pcies` 依赖独立的 `libpcies`，不得与 `pciex` 路径混用。

## 1. 推荐实现边界

第一阶段不修改 OpenISAC 主程序的 `radio::make_device()`，而是在本子项目建立
独立硬件运行器：

```text
VLC UDP输入
  -> OpenISAC视频分片/正式PHY发送器
  -> 有界TX队列
  -> LibYunSdrTxStream
  -> YunSDR TX
  -> 衰减器 + 射频线缆
  -> YunSDR RX
  -> LibYunSdrRxStream
  -> 连续IQ环形缓冲 + ZC同步/分帧
  -> OpenISAC正式PHY接收器/LDPC/CRC
  -> VLC UDP输出
```

这样可以先验证硬件自回环而不影响现有仿真程序。SISO 视频达到稳定性门槛后，
再把同一适配器注册为 OpenISAC 的 `radio_backend: libyunsdr`。

## 2. 计划新增模块

```text
include/libyunsdr_isac/
  vendor_api.hpp              # 厂商C API的私有封装
  device.hpp                  # radio::IDevice
  tx_stream.hpp               # radio::ITxStream
  rx_stream.hpp               # radio::IRxStream
  channel_map.hpp             # 0/1起始编号和mask转换
  timestamp.hpp               # tick与radio::TimeSpec转换
  sample_converter.hpp        # fc32/IQ16转换
  event_monitor.hpp           # overflow/underflow计数差分
  stream_ring.hpp             # 预分配SPSC IQ环形缓冲
  frame_assembler.hpp         # 连续流ZC同步和正式帧抽取

src/
  vendor_api.cpp
  device.cpp
  tx_stream.cpp
  rx_stream.cpp
  channel_map.cpp
  timestamp.cpp
  sample_converter.cpp
  event_monitor.cpp
  frame_assembler.cpp

tools/
  yunsdr_probe.cpp
  yunsdr_rx_capture.cpp
  yunsdr_tx_tone.cpp
  yunsdr_siso_loopback.cpp
  yunsdr_video.cpp
```

厂商头文件只允许出现在 `vendor_api.cpp` 或私有 PIMPL 中，不进入公共 OpenISAC
头文件。实时路径启动后不得动态分配大块内存。

## 3. 分阶段工作与验收门槛

### P0：冻结设备、SDK 和射频安全信息

工作：

1. 记录硬件型号、序列号脱敏值、RF 端口数、固件和 bitstream 版本；
2. 记录 Windows 驱动、SDK、头文件、`.lib`、DLL 的版本和 SHA-256；
3. 确认 SDK 与导入快照的 API 差异；
4. 从硬件手册确认 TX 最大输出、RX 最大安全输入和推荐回环衰减；
5. 确认同机 FDD/全双工能力和允许使用的实验频段。
6. 对 Y240 明确确认 `ENABLE_PCIES` 所需的 `pcies/libpcies` 头文件、导入库、
   DLL 和 Windows 驱动，禁止 CMake 在找不到库时静默降级为无 PCIES 支持。

产物：`docs/HARDWARE_PROFILE.md`、`docs/SDK_VERSION_LOCK.md`。
门槛：版本、ABI、端口和安全衰减均有可追溯依据，否则不能接射频线缆。

### P1：Windows SDK 链接和设备探测

工作：

1. 增加 `cmake/FindLibYunSDR.cmake` 和运行时 DLL 部署；
2. 将现有 stub 与真实 backend 分成两个 CMake target；
3. 扩展 `yunsdr_probe`：枚举、打开、版本、配置、范围查询、关闭；
4. 对每个厂商 API 返回值做统一错误转换；
5. 验证重复打开/关闭 100 次没有句柄泄漏或设备失联。

门槛：VS2019 Release 可重复构建；设备连续探测 100 次成功；错误信息包含具体 API
和厂商返回码；没有射频发送。

预计：1～3 个工程日，前提是 SDK 和驱动完整可用。

### P2：流适配器和无射频单元测试

工作：

1. 实现 `IDevice/ITxStream/IRxStream`；
2. SISO 也走 multiport API，固定 `channel_mask=0x1`；
3. 完成 timestamp、channel mask、事件和样本格式转换；
4. 所有读写使用固定容量 buffer，处理短传输、负返回值和停止请求；
5. 用 fake vendor API 测试超时、溢出、下溢和设备断开。

门槛：无硬件测试全部通过；`complex<float>`/IQ16 往返误差、I/Q 顺序和饱和
行为有确定结果；线程可在 2 秒内协作退出。

预计：3～5 个工程日。

### P3：安全的 SISO 波形自回环

工作顺序：

1. 仅 RX：采集环境噪声，验证时间戳连续和无 overflow；
2. 仅 TX：发送低幅度单音或 ZC，先用频谱仪/功率计确认输出；
3. 加入按手册计算的固定衰减器后连接 TX 到 RX，禁止直接线缆连接；
4. RX 先启动，TX 后启动，使用固定增益和固定 channel mask；
5. 比较立即发送与定时发送，记录时间戳增量和事件计数；
6. 保存原始 IQ、配置、设备版本和完整日志。

初始参数候选：

| 参数 | 候选值 | 说明 |
| --- | ---: | --- |
| 采样率 | 15.36 Msps | 必须先确认硬件可精确设置 |
| DMA read block | 7680 samples | 与 PHY 分帧解耦，后续实测调整 |
| 通道 | TX0/RX0 | multiport mask `0x1` |
| 模式 | FDD | 必须确认设备支持同机同时收发 |
| 增益 | 保守固定值 | 不启用 AGC，按硬件手册设置 |
| 发送幅度 | 满量程的 0.05～0.1 起步 | 逐级增加并监测饱和 |

门槛：连续 10 分钟无不可恢复错误；时间戳单调；RX 不削顶；overflow、underflow
和 timeout 均为 0，或每个非零事件都有可解释日志。

预计：2～4 个工程日，不含硬件/驱动故障排查。

### P4：连续 SISO OpenISAC PHY

工作：

1. 将 OpenISAC `openisac_phy` 作为子目录依赖链接，不复制算法源码；
2. 实现连续 IQ 环形缓冲和跨 DMA block 的 ZC 同步；
3. 抽取 FDM 3456 样点正式帧，后续兼容 NR-DMRS 5760 样点帧；
4. 从 QPSK 开始验证，再依次增加 16QAM、64QAM；
5. 接入现有 CFO、EVM、信道估计、LDPC、CRC、FER 遥测；
6. overflow、序号跳变或 timestamp 不连续时丢弃当前帧并重新同步。

阶段门槛：

- 10,000 帧 QPSK CRC 通过率达到 99.9% 以上；
- 64QAM 在线缆良好条件下 EVM 不高于 8%，FER 不高于 1%；
- 连续 30 分钟无死锁、无无界队列增长、无不可恢复流中断；
- 性能日志分别给出 RX I/O、同步、FFT、信道估计、检测和 LDPC 用时。

这些是本项目初始工程门槛，不是硬件规格承诺；实测后再调整。

预计：4～8 个工程日。

### P5：VLC 视频硬件自回环

工作：

1. 复用现有 UDP 分片、重组和 CRC/FER 统计；
2. 新建 `yunsdr_video`，默认 UDP 输入 50000、输出 50001；
3. TX/RX/PHY/UDP 分线程并使用有界队列；
4. 短时错误允许丢帧并重新同步，禁止阻塞整条链路；
5. 继续输出星座图、时域图、EVM、CFO、信道估计、事件计数和感知遥测；
6. 退出时自动停止流、关闭设备、VLC 和绘图窗口。

门槛：两个现有测试视频均可播放；1 小时运行无进程崩溃和队列持续增长；结束后
所有本次启动的进程和设备句柄均释放。

预计：3～6 个工程日。

### P6：STBC、2x2、4x4 和感知

只在 P5 完成后进行：

1. 2x2 STBC Rank-1；
2. 2x2 空间复用 Rank-2；
3. 4Tx/4Rx Rank-2；
4. 4x4 Rank-4；
5. 多通道相位/幅度/时延校准及感知角度维。

每增加一个通道数，必须重新验证物理端口映射、共享时钟、timestamp 对齐、DMA
吞吐、通道隔离和校准稳定性。不得仅因 API 有 8 个通道枚举就宣称硬件支持 8x8。

## 4. 性能预算

IQ16 每复样点 4 byte，不计 64-byte 头和对齐开销：

| 模式 | 单方向 payload | 全双工 payload |
| --- | ---: | ---: |
| SISO @ 15.36 Msps | 61.44 MB/s | 122.88 MB/s |
| 2 通道 | 122.88 MB/s | 245.76 MB/s |
| 4 通道 | 245.76 MB/s | 491.52 MB/s |

`format:float` 若在应用边界使用 8 byte/复样点，主机内存带宽需求还会翻倍。
因此第一阶段必须记录 payload、帧头/对齐、CPU 转换和端到端吞吐，不能只看视频
码率判断 SDR 数据面是否稳定。

## 5. 错误处理原则

| 事件 | 适配器处理 |
| --- | --- |
| API timeout | 当前调用失败，记录厂商码并触发有界重试 |
| RX overflow | 标记 timestamp/sequence 不连续，清空分帧状态并重同步 |
| TX underflow | 丢弃当前发送计划，重新建立未来 timestamp |
| 短读/短写 | 不假设成功；按 SDK 契约补传或返回错误 |
| bad magic/格式错误 | 保存有限诊断样本，停止流并重新打开设备 |
| 设备断开 | 停止 TX/RX，释放句柄，禁止静默切换到仿真 |
| 退出请求 | 唤醒阻塞线程，2 秒内协作退出；必要时关闭设备句柄 |

## 6. 对主 OpenISAC 仓库的最小改动

P0～P3 不修改主仓库算法。P4/P5 预计只需要：

1. 允许本项目通过 CMake 链接 `cpp_phy/openisac_phy`；
2. 将视频 UDP 分片/重组中可复用部分从现有模拟桥抽成小型公共库；
3. 若 `DynamicLinkCaptureFrame` 仍只适配两路，在扩展 4x4 前改为通用通道数组；
4. P5 通过后才在主配置中放开 `radio_backend: libyunsdr`。

任何厂商头文件、DLL 加载和设备特定配置都保留在 `libyunsdr-isac`。

## 7. 依赖和阻塞项

开始 P1 前必须取得：

- 目标型号已确定为 YunSDR Y240；
- 对应 Windows x64 驱动、头文件、`.lib` 和 DLL；
- 与 `pcies:0.0` 配套的 `pcies.lib`/`libpcies.lib` 及运行时 DLL（如适用）；
- 厂商最小 Windows 同机 TX/RX 示例及成功日志；
- 固件/bitstream 版本；
- 最大 RX 输入与 TX 输出规格；
- 射频衰减器、线缆以及必要的 DC block；
- 实验中心频率和合法使用条件。

在这些条件齐备且没有驱动/ABI问题时，SISO VLC 硬件自回环的合理工作量约为
2～4 周。仅完成设备探测、IQ收发和单音回环通常需要 4～10 个工程日。

## 8. 下一项具体工作

下一步应执行 P0，不直接写流代码：

1. 将实际 SDK 放入外部固定目录或 `third_party/libyunsdr/`；
2. 记录 SDK/驱动/硬件版本和 SHA-256；
3. 对比实际 `yunsdr_api_ss.h` 与导入快照；
4. 根据差异生成最小必需 API 表；
5. 随后实现真正可运行的 `yunsdr_probe`。

当前 VS2019/CMake 实测和缺失项详见 `Y240_PCIES_BUILD_ASSESSMENT.md`。
