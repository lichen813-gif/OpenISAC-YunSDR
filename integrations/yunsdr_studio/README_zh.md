# YunSDR Studio OpenISAC 集成

[English](README.md) · [帧结构](FRAME_V2.md) · [接板步骤](Y240_LIVE_VALIDATION.md)

正式帧采用 FFT 1024、CP 128 和 816 个活动子载波，索引为 −408…−1 和 +1…+408。标称带宽为采样率的 817/1024，在 122.88 MSa/s 下为 98.04 MHz。支持 SISO、2/4/8 层空间复用和双端口 Alamouti STBC，以及 FDM 和前置 DM-RS。同步前导采用同一频域范围，载荷区的空余位置用伪随机 QPSK 填充以降低瞬时峰值。星座和 EVM 使用实际承载 LDPC 编码比特的位置。

从仓库根目录执行以下命令。独立 CTest 可在没有 Studio 的环境中验证 40 种收发配置。

```powershell
cmake -S integrations/yunsdr_studio/native -B build/studio-phy -G "Visual Studio 16 2019" -A x64
cmake --build build/studio-phy --config Release --parallel
ctest --test-dir build/studio-phy -C Release --output-on-failure
cmake --install build/studio-phy --config Release --prefix build/studio-runtime
```

`overlay/yunsdr_studio` 下是 Studio 0.16 OpenISAC 的 Python 绑定、感知、短片回环、实时视频和面板源码。把它们覆盖到已接入 OpenISAC 工作台入口的 Studio 软件目录。面板依赖 Studio 宿主的 IQ 转换、设备会话和 PySide6 组件。用 `YUNSDR_OPENISAC_RUNTIME` 指向安装目录，也可安装到 Studio 的 `output/openisac_runtime`。运行库 ABI 为 7。独立 CTest 无需厂商 SDK。

实时视频可选 15.36、30.72 或 61.44 MSa/s，使用 816 载波和 2×2 STBC。可选 QPSK 至 256-QAM 和 FDM／前置 DM-RS。双机两端使用相同的采样率、调制和导频。实时视频的四个面板可双击全屏。

如果宿主仍是上一版视频诊断工作台，先在 Studio 根目录用 `git apply --check` 检查 [宿主更新补丁](host-update.patch)，再用 `git apply` 应用。该补丁补齐设备采样率、DMA 调用、接收 CSI 和四图布局所需的配套修改。

Studio 74 项离线测试通过。61.44 MSa/s 的模拟时间戳 DMA 回环，以 64-QAM／前置 DM-RS 在 1、1.5、2 Mbit/s 各运行 30 秒，均完整恢复。Y790 8T8R 平台已录得 30.72 MSa/s 的双通道本机射频视频回环。61.44 MSa/s 和双机实物链路仍待验证。[61.44 基准 CSV](../../results/benchmarks/studio_6144_timed_mock.csv)

独立 Y240/VLC 和 CPU/CUDA 基线使用各自发布的帧布局。
