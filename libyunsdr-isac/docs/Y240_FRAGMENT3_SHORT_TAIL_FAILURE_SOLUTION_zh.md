# Y240 SISO `fragment 3` 短尾帧失败分析与解决方案

## 1. 文档目的

本文记录 Y240 有线回环启动视频桥时，固定重复出现以下错误的分析结论、验证方法和
工程修复方案：

```text
PHY payload=439 bytes, fragment data=419 bytes, frame samples=3456
Batch decode failed; retry 1/8 (fragment 3 timing=1 header=0 crc=0)
...
Batch decode failed; retry 8/8 (fragment 3 timing=1 header=0 crc=0)
FAIL: hardware video self-test payload mismatch
```

本文针对当前 SISO、64QAM、FDM、1500 MHz、TX gain 60、RX gain 20 的截图和
日志。其他模式出现同样的固定尾分片失败时，也可按相同方法检查。

## 2. 结论摘要

1. **本次错误基本不是更换视频造成的。** 报错发生在视频桥的硬件预热或
   `--self-test` 阶段，此时 VLC 尚未启动，也没有读取视频文件。
2. `timing=1` 表示接收端已经检测到前导并获得定时位置，链路并非完全无信号；
   `header=0 crc=0` 表示该分片的控制头和有效载荷没有可靠恢复。
3. 失败始终固定在 `fragment 3`，而不是随机落在不同分片，说明问题优先指向最后一个
   **短尾 PHY 帧**的变长编码、资源占用或突发边界处理，而不是随机视频内容。
4. 推荐的正式修复是：所有视频分片都按所选模式的最大 PHY payload 定长发送，最后
   一个分片用零填充；接收端依据分片头中的原始 UDP 包长度，只拷贝真实有效字节并
   丢弃填充。这样不修改空口分片头格式，同时使 LDPC 块数、OFDM 资源占用和帧功率
   形态保持一致。
5. 上述根因目前是依据稳定日志和源码得到的高概率判断；完成代码修改后仍须经过
   软件回归和 Y240 实机回环才能标记为最终确认。

## 3. 为什么固定失败在 `fragment 3`

SISO 64QAM/FDM 当前参数为：

| 项目 | 数值 |
| --- | ---: |
| 每个 PHY 帧最大 payload | 439 bytes |
| 视频分片头 | 20 bytes |
| 每个分片最大视频数据 | 419 bytes |
| 预热测试 UDP 包 | 1316 bytes |

1316 字节测试包的分片过程为：

| 分片编号 | 视频数据 | 分片头 | 当前送入 PHY 的总字节数 |
| ---: | ---: | ---: | ---: |
| 0 | 419 | 20 | 439 |
| 1 | 419 | 20 | 439 |
| 2 | 419 | 20 | 439 |
| 3 | 59 | 20 | 79 |

计算如下：

```text
1316 = 419 * 3 + 59
最后一帧 PHY payload = 20 + 59 = 79 bytes
```

前三个分片都是最大长度 439 字节，最后的 `fragment 3` 只有 79 字节。日志中前三帧
能够继续处理、最后一帧稳定失败，与这个长度边界完全吻合。

当前发送端按真实尾部长度创建 payload：

```text
fragment_header_bytes + bytes
```

其中 `bytes` 在最后一个分片小于 `fragment_data_bytes`。因此最后一个 PHY 帧可能与
前三帧具有不同的 LDPC 码块数量、有效资源占用、填充方式或突发末尾功率形态。任何
变长路径或硬件突发边界问题都会集中暴露在尾分片。

## 4. 为什么不是视频文件问题

`run_y240_video.ps1` 的启动顺序是：

```text
启动 Y240 PHY bridge
        |
        v
执行固定数据 warmup/self-test
        |
        +-- 失败：关闭 bridge，重试；不会启动 VLC
        |
        +-- 通过：才启动监视器、VLC 接收端和 VLC 发送端
```

预热数据由 `yunsdr_video_bridge.exe` 内部生成。当前 warmup 使用固定的 1316 字节
测试包，`--self-test` 的第一个包也固定为 1316 字节。因此：

- MP4 文件名、分辨率、编码格式和内容不会影响当前失败；
- `1000 kbit/s` VLC 码率不会影响预热；
- 将 2 Mbps 视频换成 5 Mbps 视频也不会改变 `fragment 3` 的预热失败；
- 只有预热已经通过、VLC 已开始发送 UDP 后，不同视频导致的 UDP 包长、码率和突发
  分布差异才可能影响运行期稳定性。

## 5. 无需视频的确认命令

在已经确认安全的外部衰减有线回环条件下，从 `libyunsdr-isac` 目录运行：

```powershell
.\build\ninja-vs2019-hardware\yunsdr_video_bridge.exe `
  --device pcies:0.0 `
  --mode siso `
  --modulation 64qam `
  --pilot fdm `
  --frequency-mhz 1500 `
  --tx-gain 60 `
  --rx-gain 20 `
  --self-test 1 `
  --batch-packets 1 `
  --warmup-packets 0 `
  --retries 2
```

该命令不读取视频文件，不启动 VLC，且第一个测试包仍为 1316 字节。如果仍固定报告：

```text
fragment 3 timing=1 header=0 crc=0
```

即可排除“换视频导致预热失败”。

再运行普通 PHY 回环作为对照：

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

若固定 200 字节 PHY 回环能够通过，而 1316 字节视频桥自检仍稳定失败在
`fragment 3`，则进一步支持“尾分片长度路径”判断。普通回环通过并不能代替短尾
分片测试，因为二者的 payload 长度不同。

## 6. 正式工程修复方案

### 6.1 发送端定长填充

对每个视频分片都分配最大 PHY payload，而不是按最后一个分片的真实长度分配：

```text
payload_size = maximum_payload_bytes
```

处理步骤：

1. 整个 payload 初始化为 0；
2. 前 20 字节继续写现有分片头；
3. 从偏移 20 开始拷贝本分片的真实视频数据；
4. 最后一个分片剩余位置保持为 0；
5. CRC 和 LDPC 对完整的定长 payload 进行处理。

以本次 SISO 为例，四个分片均以 439 字节进入 PHY。`fragment 3` 的前 79 字节包含
分片头和真实数据，后 360 字节为零填充。

### 6.2 接收端按原始长度截断

现有 20 字节分片头已经包含：

- UDP 包序号；
- 分片编号；
- 分片总数；
- 原始 UDP 包总字节数。

因此无需修改空口头格式。接收端按以下方式计算本分片的真实数据长度：

```text
offset = fragment_index * fragment_data_bytes
valid_bytes = min(fragment_data_bytes, packet_bytes - offset)
```

只将 `valid_bytes` 拷贝到重组缓冲区，忽略后面的零填充。

### 6.3 必须增加的边界校验

接收端在拷贝前必须验证：

1. `packet_bytes` 在 UDP 合法范围内；
2. `fragment_count` 等于根据 `packet_bytes` 计算出的期望分片数；
3. `fragment_index < fragment_count`；
4. `offset < packet_bytes`；
5. 解码后的 payload 至少包含 20 字节分片头和 `valid_bytes`；
6. 同一个分片重复到达时不重复累计已接收分片数；
7. 所有分片接收完成后，重组结果长度严格等于 `packet_bytes`。

这些检查可防止错误头部造成越界写入，也避免把填充字节送给 VLC。

## 7. 方案影响

### 7.1 优点

- 消除最后一个分片的变长 PHY 编码路径；
- 每个同模式 PHY 帧具有一致的 LDPC/OFDM 处理规模；
- 保持现有 20 字节分片头兼容，不增加协议版本；
- SISO、2x2 MIMO 和 STBC 可共用同一逻辑；
- 对后续 C++ 实时实现友好，只增加零初始化和少量边界计算。

### 7.2 代价

- 尾分片会发送额外填充字节，空口效率略有降低；
- 小 UDP 包的相对开销更明显；
- 接收端必须严格按原始长度截断，不能直接把解码 payload 尾部全部写入重组包。

视频传输通常以接近 MTU 的 UDP 包为主，稳定性优先于最后一个分片的少量空口效率。

## 8. 回归测试矩阵

### 8.1 软件边界用例

SISO `fragment_data_bytes=419` 时至少覆盖：

| UDP 包长度 | 期望分片数 | 用途 |
| ---: | ---: | --- |
| 1 | 1 | 极短尾分片 |
| 418 | 1 | 满载前一字节 |
| 419 | 1 | 单分片边界 |
| 420 | 2 | 第二分片仅 1 字节 |
| 838 | 2 | 两个满载分片 |
| 839 | 3 | 第三分片仅 1 字节 |
| 1316 | 4 | 本次故障复现长度，尾部 59 字节 |
| 65507 | 按公式计算 | 最大合法 UDP payload |

每个用例必须验证：分片数、定长 PHY payload、CRC、重组长度和逐字节一致性。

### 8.2 各模式软件与硬件测试

修复后依次执行：

1. 无硬件 CTest 全部通过；
2. 硬件构建的 CTest 全部通过；
3. SISO QPSK/64QAM 固定 PHY 回环；
4. SISO `--self-test 1`，确认 1316 字节包通过；
5. SISO `--self-test 64 --batch-packets 8`，确认无固定尾分片失败；
6. 2x2 双层 MIMO 自检；
7. 2x2 Alamouti STBC 自检；
8. 两个测试视频分别运行，检查播放、FER、EVM、队列和进程清理；
9. 监视器验证星座图、时域、频偏、信道估计和感知输出持续刷新。

硬件验收应保留每种模式的成功包数、重试数、EVM、峰值、削顶率、TX timeout、
RX overflow 和 timestamp discontinuity。不能仅以 VLC 画面是否出现作为通过标准。

## 9. 临时绕过与禁止事项

- 可以使用本文件第 5 节的最小自检命令定位问题；
- 可以先用 QPSK 判断是否存在基础链路，再恢复 64QAM；
- 不建议关闭 warmup 后直接启动 VLC。实际视频 UDP 包同样可能产生短尾分片，跳过
  warmup 只会把错误推迟到播放阶段；
- 不应通过无限增加重试次数掩盖稳定的 `fragment 3` 失败；
- 不应在未知外部衰减下提高 TX gain。增益调整只能处理链路电平问题，不能修复确定
  性的分片长度逻辑。

## 10. 完成判据

满足以下条件后，才可把本问题标记为修复完成：

1. 发送端所有尾分片均按最大 PHY payload 定长编码；
2. 接收端仅重组真实 UDP 数据并通过全部边界校验；
3. 1316 字节自检不再固定失败在 `fragment 3`；
4. 软件回归、SISO、MIMO2、STBC 硬件自检全部通过；
5. 更换两个视频文件均能运行，且 warmup 结果与视频文件无关；
6. 文档记录测试命令、设备连接、频率、增益和完整结果。

## 11. 相关资料

- [Y240 视频预热失败排查说明](Y240_VIDEO_WARMUP_FAILURE_TROUBLESHOOTING_zh.md)
- [Y240 硬件视频测试](Y240_HARDWARE_VIDEO.md)
- [Y240 Windows 快速搭建](Y240_WINDOWS_QUICKSTART_zh.md)
- [视频桥实现](../tools/yunsdr_video_bridge.cpp)

