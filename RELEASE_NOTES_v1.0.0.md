# OpenISAC-YunSDR v1.0.0

[中文](#中文) | [English](#english)

## 中文

这是 OpenISAC-YunSDR 的首个公开版本，汇总了当前通信感知一体化平台的实现、设计资料与中英文说明。

### 本版内容

- 实时 OFDM 通信、单站感知与双站感知基础实现。
- C++ PHY、Python PHY 验证链路及信道仿真工具。
- SISO 高阶调制、2x2 与可扩展 MIMO、链路自适应相关设计和实现路线。
- Windows C++ PHY 正式帧结构、VLC 视频信道仿真与运行说明。
- 英文和中文 README、文档站源码以及构建后的静态手册。
- 成对的基础、双工、eRTM 与资源映射仿真配置。
- 可复现 C++ 正式帧测试所需的非视频金向量。

### 说明

该版本面向科研原型与算法验证，不以 Wi-Fi/5G NR 标准兼容或生产部署为目标。当前版本仅支持 ChannelSimulator；原 UHD/USRP 实现与配置已移除，`libyunsdr` 接口将在真实后端完成并验证后更新。

## English

This is the first public release of OpenISAC-YunSDR. It packages the current integrated sensing and communication implementation together with its design material and bilingual documentation.

### Included in this release

- Baseline real-time OFDM communications, monostatic sensing, and bistatic sensing.
- A C++ PHY, Python PHY validation paths, and channel-simulation tools.
- Design and implementation roadmaps for higher-order SISO modulation, 2x2 and scalable MIMO, and link adaptation.
- Windows C++ PHY framing, VLC video-channel simulation, and operating guides.
- English and Chinese READMEs, documentation-site sources, and generated static manuals.
- Paired baseline, duplex, eRTM, and resource-map simulation configurations.
- Non-video golden vectors required to reproduce the formal C++ frame tests.

### Notes

This release targets research prototyping and algorithm validation. It does not aim for Wi-Fi/5G NR standards compliance or production deployment. It currently supports ChannelSimulator only; the former UHD/USRP implementation and presets are removed, and the `libyunsdr` interface will be documented after a real backend is implemented and verified.
