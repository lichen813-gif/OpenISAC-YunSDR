# 导入的 libyunsdr 经验评估

评估日期：2026-08-24
目标：判断迁移包能否作为 OpenISAC Windows SDR 接入的设计依据，并明确哪些
结论可以直接使用、哪些必须重新实测。

## 1. 输入材料与完整性

导入包位于：

```text
import/libyunsdr_original_analysis_handoff_20260824/
```

原 ZIP SHA-256：

```text
78F6C4E75359C3FFF00ABB9C7931C3F173801AAAFB33AE198631EAADD192D4D9
```

已按包内 `SHA256SUMS.txt` 复核 119 个文件，缺失 0、哈希不一致 0。

该迁移包的性质是“静态分析与证据快照”，不是可直接交付的 Windows SDK，也
不是已经在目标 SDR 上通过测试的运行版本。其上游版本、commit、PDF 与源码的
配套关系以及再分发许可仍未冻结。

本项目后续目标设备已确定为 Y240，目标 URI 为 `pcies:0.0`。这条路径使用
`ENABLE_PCIES` 和独立 `libpcies`，与迁移包重点分析的 `pciex` Windows backend
不是同一实现。迁移包的 PCIES 信息只够确认调用和构建依赖，不能证明其已经在
Y240/Windows 上运行。

## 2. 当前可以采用的结论

以下结论有迁移包内源码或文档直接支持，但仍需在目标硬件上做运行确认：

1. 公开 API 提供设备打开/关闭、收发采样率、本振、带宽、RX 增益、TX 衰减、
   参考时钟、FDD/TDD、时间戳、事件计数和多端口 IQ 读写。
2. 多端口 API 使用“每个通道一个连续 buffer”，与 OpenISAC 的多通道
   `complex<float>` 数组结构相容，不需要采用样点级通道交织。
3. 原始 IQ32 格式是低 16 bit I、高 16 bit Q；应用帧有 64-byte 头部。
4. `timestamp=0` 表示立即发送，非零 timestamp 表示按设备样点时钟定时发送。
5. Windows PCIEX 源码通过 XDMA 设备 GUID 枚举设备，并打开 `user`、`h2c_0`、
   `c2h_0` 端点；数据面使用阻塞 `ReadFile`/`WriteFile`。
6. Windows 控制事务存在约 2000 次、每次 1 ms 的等待上限；控制调用仍可能阻塞
   较长时间，因此不能放在实时 IQ 热路径中。
7. RX channel mask 改变会触发接收逻辑复位和约 1.2 秒等待；运行期间应固定通道
   mask，切换模式时重新创建流，而不是逐帧修改。
8. RX/TX timeout、underflow、overflow 和计数器可通过事件查询 API 读取，但原
   API 没有与 OpenISAC `recv_async_msg()` 完全等价的异步消息队列。

主要证据：

- `CURRENT_STATUS.md`
- `evidence/analysis_docs/PCIEX_API_AND_TEST_SOFTWARE_ANALYSIS.md`
- `reference/original_libyunsdr_api/src/yunsdr_ss/include/yunsdr_api_ss.h`
- `reference/original_libyunsdr_api/src/yunsdr_ss/src/interface_pciex_win.c`
- `reference/original_libyunsdr_api/src/yunsdr_ss/tests/yunsdr_ss_txrx_multiport.c`

## 3. 不能直接当作事实的事项

以下是导入分析阶段列出的待验证事项；当前硬件结论以
`Y240_HARDWARE_VIDEO.md` 为准：

- 目标硬件型号、射频端口数、固件/bitstream 与导入 API 是否匹配；
- Windows 驱动、DLL、导入库和 VS2019 ABI 是否完整兼容；
- 设备 URL 是否确实使用 `pciex:0`，以及是否支持 URI 中的 `format:float`；
- Y240 已验证使用 `pcies:0.0`；`libpcies`、驱动、固件是否
  与目标 API 版本配套；
- 15.36 Msps 是否可精确设置，时间戳 tick 是否严格等于采样时钟；
- 同一设备是否可以稳定地同时 TX/RX，并持续运行视频所需吞吐量；
- `complex<float>` 与 `format:float` 的 I/Q 顺序、幅度范围和内存布局；
- 64-byte 尾部对齐、短读/短写、bad magic 和设备重启后的恢复行为；
- 1/2/4/8 端口相干性、通道 mask 到物理射频口的实际映射；
- 最大允许 RX 输入功率、实际 TX 输出功率以及安全线缆衰减值；
- SDK 源码/PDF 的许可是否允许放入未来公开 GitHub 仓库。

原迁移包的主分析明确排除了 Windows `interface_pciex_win.c`，因此本项目可以把
该文件用于接口预研，但不能引用原结论声称“Windows 已验证”。

## 4. 对 OpenISAC 适配器的直接约束

### 4.1 统一使用 multiport API

SISO 也使用：

```text
yunsdr_read_samples_multiport(..., channel_mask=0x1, ...)
yunsdr_write_samples_multiport(..., channel_mask=0x1, ...)
```

这样 SISO、2x2 和 4x4 使用同一条代码路径，可避开旧单通道实现差异。

### 4.2 通道编号必须集中转换

- OpenISAC `StreamArgs.channels`：0 起始索引；
- multiport `channel_mask`：bit 0 对应物理逻辑通道 0；
-部分公共枚举和事件查询：TX1/RX1 从 1 开始。

适配器必须用一个经过单元测试的函数完成索引、mask 和事件通道号转换，业务层
不得自行加一或移位。

### 4.3 TX 增益不能直接照搬 RX 增益

参考 API 的 TX 主要配置量是毫分贝衰减，旧示例使用 `(90-gain)*1000`。这不是
所有硬件型号都成立的通用换算。`radio::IDevice::set_tx_gain()` 必须基于目标型号
的实际范围和校准定义进行转换，P1 阶段以前不得写死该公式。

### 4.4 控制面和数据面分离

- 设备配置只允许在启动、停止或明确重配置状态执行；
- RX/TX 实时线程只调用读写 API，不做 LO、增益或 channel mask 修改；
- 事件计数由低频监视线程轮询，再转换成 OpenISAC 的错误和遥测；
- 所有线程必须支持有界超时和协作退出，不能依赖永久阻塞。

### 4.5 固定设备传输块，独立做 PHY 分帧

OpenISAC 正式波形参数为 1024 FFT、128 CP、15.36 Msps：

- FDM 正式帧：3 个 OFDM symbol，共 3456 个复样点；
- NR-DMRS 正式帧：5 个 OFDM symbol，共 5760 个复样点。

设备 DMA block 不应等同于 PHY 帧。建议初始候选值为 7680 样点，RX 线程连续
写入固定容量环形缓冲，独立的同步/分帧器跨 DMA block 检测 ZC 前导并抽取
3456/5760 样点帧。最终 block 长度必须通过 SDK 和硬件吞吐测试确定。

### 4.6 样本格式策略

第一阶段优先尝试 `format:float`，验证其内存是否能直接解释为
`std::complex<float>`。如果布局或性能不满足要求，则切换到 IQ16，并使用独立的
SIMD 转换层。两种格式都必须用已知 I/Q 序列验证极性、顺序、幅度和饱和行为。

## 5. OpenISAC 能力映射初稿

| OpenISAC 接口 | libyunsdr 候选实现 | 初始状态 |
| --- | --- | --- |
| `IDevice::set_tx/rx_rate` | sampling frequency API | 需硬件验证 |
| `set_tx/rx_bandwidth` | RF bandwidth API | 需硬件验证 |
| `set_tx/rx_freq` | TX/RX LO API | 不支持独立 DSP tune |
| `set_rx_gain` | RX MGC + RF gain | 可直接适配后实测 |
| `set_tx_gain` | TX attenuation/功率转换 | 必须按型号定义 |
| `time_now` | `yunsdr_read_timestamp` | 需验证 tick 语义 |
| `ITxStream::send` | multiport write | 候选支持定时发送 |
| `IRxStream::recv` | multiport read | 需增加超时/退出封装 |
| `recv_async_msg` | 事件计数差分轮询 | 软件合成事件 |
| `StreamRestart` | timestamp enable/流重建 | 初期标记不支持 |
| `RfDspTune` | 无独立 DSP 频移证据 | 初期标记不支持 |

## 6. 当前就绪度

| 项目 | 状态 |
| --- | --- |
| 迁移包完整性 | 已验证，119/119 |
| API/Windows backend 源码 | 已导入，只读参考 |
| OpenISAC vendor-neutral radio contract | 已存在 |
| VS2019 子项目基线 | 已构建，测试通过 |
| 目标 Windows `.lib`/DLL | 尚未确认 |
| 匹配驱动与设备枚举 | 尚未确认 |
| 硬件型号和射频安全参数 | 型号 Y240 已确定；其余尚未提供 |
| Windows 实机日志 | 尚无 |
| 连续 SISO PHY/视频实测 | 尚未开始 |

补充状态：目标硬件型号现已确定为 Y240，但序列号、固件/bitstream、射频安全
参数以及 Windows 驱动版本仍待记录。VS2019 的最小 PCIES 配置已执行，工具链
正常；迁移快照缺少 `pcies/libpcies` 链接库及 5 个 CMake 模板，详细结果见
`Y240_PCIES_BUILD_ASSESSMENT.md`。
