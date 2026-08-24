# OpenISAC-YunSDR v1.1.0

[中文](#中文) | [English](#english)

## 中文

`v1.1.0` 是当前完整的仿真后端版本，包含最新程序源码、配置、设计文档和中英文使用说明，不包含视频文件。

### 主要更新

- 当前仅支持 `ChannelSimulator` 软件仿真后端。
- 完整移除 UHD/USRP 后端源码、编译依赖、设备/PPS 参数以及 X310/B210 配置。
- Docker、性能测试模板、CPU 隔离工具和 Web 配置编辑器均已切换为仿真配置。
- 重写中英文首页，补充 Linux 编译、三进程启动、运行时 PHY 参数、正式帧参数和配置约束。
- 新增 Rank-4 时域与正式帧链路、NR 风格双符号 DM-RS、信道估计、TDL 信道、采样偏差处理及 4×4 诊断/基准工具。
- 包含完整 Python PHY 算法源码、21 个实验脚本、21 份配置和 22 个 pytest 测试文件。
- 根目录依赖已包含 `pytest>=7.0`；Python 测试和专项验证命令见 `python_phy/README.md`。
- 补齐 C++ 正式帧测试所需的非视频黄金向量。

### libyunsdr 状态

本版本尚未实现或声明 `libyunsdr` API 兼容性。仓库保留厂商无关的 `RadioBackend` 边界，实际后端、硬件配置和接口说明将在 SDK 接入并完成实测后更新。

### 验证结果

- Python PHY：114 项测试通过；包含工具测试时共 119 项通过。
- Windows C++ 正式 PHY：干净构建成功，CTest 4/4 通过（含 Rank-1/2/4 兼容自检）。
- 18 份 YAML 解析和 8 份 Web 配置渲染通过。
- 源码、配置、Docker 和工具目录中的 UHD/USRP 实现残留扫描为零。

## English

`v1.1.0` is the current complete simulator-backend release. It includes the latest program source, configurations, design documents, and bilingual usage guides, with no video files.

### Highlights

- The current release supports the software `ChannelSimulator` backend only.
- The UHD/USRP backend source, build dependencies, device/PPS parameters, and X310/B210 presets have been removed.
- Docker, benchmark templates, CPU-isolation tools, and the web configuration editor now use simulator configurations.
- The English and Chinese homepages now document Linux builds, three-process startup, runtime PHY controls, the formal-frame profile, and configuration constraints.
- Rank-4 time-domain and formal-frame links, NR-style two-symbol DM-RS, channel estimation, TDL channels, sampling-offset processing, and 4x4 diagnostics/benchmarks are included.
- The complete Python PHY source is included with 21 experiment scripts, 21 configurations, and 22 pytest files.
- Root dependencies now include `pytest>=7.0`; Python tests and explicit validation commands are documented in `python_phy/README.md`.
- Non-video golden vectors required by the formal C++ frame tests are included.

### libyunsdr status

This release does not implement or claim compatibility with a concrete `libyunsdr` API. The vendor-neutral `RadioBackend` boundary remains for a future verified SDK backend, hardware presets, and interface documentation.

### Validation

- Python PHY: 114 tests passed; 119 passed when utility tests are included.
- Windows formal C++ PHY: a clean build succeeded and CTest passed 4/4, including Rank-1/2/4 compatibility checks.
- 18 YAML files parsed and 8 web-editor configurations rendered successfully.
- No UHD/USRP implementation residue remains in source, configuration, Docker, or tool directories.
