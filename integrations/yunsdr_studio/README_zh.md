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

实时视频使用双端口 STBC、64-QAM/FDM 和 15.36 MSa/s，每个 1,316 字节数据报占五个正式帧。双机的 Studio 收发端均需采用 816 载波版本。独立 Y240/VLC 和 CPU/CUDA 基线仍使用其原有帧布局。

Studio 72 项离线回归通过。实时视频在 30 dB AWGN 下以 1、1.5、2 Mbit/s 分别运行 30 秒，数据报全部恢复。新波形的真实射频视频和感知待上板验证。[基准 CSV](../../results/benchmarks/studio_816_live_windows.csv)
