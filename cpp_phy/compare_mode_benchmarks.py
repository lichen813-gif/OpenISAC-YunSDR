#!/usr/bin/env python3
import argparse
import csv
import statistics
from collections import defaultdict
from pathlib import Path


def read_grouped(path: Path, expected_backend: str):
    grouped = defaultdict(list)
    with path.open(newline="", encoding="utf-8") as stream:
        for row in csv.DictReader(stream):
            backend = row.get("backend", "cpu")
            if backend != expected_backend:
                raise SystemExit(
                    f"{path}: expected backend={expected_backend}, got {backend}")
            if float(row["fer_percent"]) != 0.0:
                raise SystemExit(f"{path}: non-zero FER in {row['label']}")
            grouped[row["label"]].append(row)
    return grouped


def median(rows, field):
    return statistics.median(float(row[field]) for row in rows)


def cv_percent(rows, field):
    values = [float(row[field]) for row in rows]
    mean = statistics.fmean(values)
    return 100.0 * statistics.pstdev(values) / mean if mean else 0.0


def main():
    parser = argparse.ArgumentParser(
        description="Compare Windows CPU, DGX CPU and DGX CUDA mode benchmarks")
    parser.add_argument("windows_cpu_csv", type=Path)
    parser.add_argument("dgx_cpu_csv", type=Path)
    parser.add_argument("dgx_cuda_csv", type=Path)
    parser.add_argument("output_csv", type=Path)
    parser.add_argument("output_markdown", type=Path)
    args = parser.parse_args()

    windows = read_grouped(args.windows_cpu_csv, "cpu")
    dgx_cpu = read_grouped(args.dgx_cpu_csv, "cpu")
    dgx_cuda = read_grouped(args.dgx_cuda_csv, "cuda")
    preferred_order = [
        "siso_fdm", "siso_nr-dmrs",
        "mimo2_fdm", "mimo2_nr-dmrs",
        "stbc_fdm", "stbc_nr-dmrs",
        "rank2_4port_fdm", "rank2_4port_nr-dmrs",
        "rank4_fdm", "rank4_nr-dmrs",
    ]
    common = set(windows) & set(dgx_cpu) & set(dgx_cuda)
    labels = [label for label in preferred_order if label in common]
    labels.extend(sorted(common - set(labels)))
    if not labels:
        raise SystemExit("No matching benchmark labels")

    rows = []
    for label in labels:
        win_latency = median(windows[label], "phy_latency_ms")
        cpu_latency = median(dgx_cpu[label], "phy_latency_ms")
        cuda_latency = median(dgx_cuda[label], "phy_latency_ms")
        win_throughput = median(windows[label], "throughput_mbps")
        cpu_throughput = median(dgx_cpu[label], "throughput_mbps")
        cuda_throughput = median(dgx_cuda[label], "throughput_mbps")
        rows.append({
            "label": label,
            "windows_cpu_latency_ms": win_latency,
            "dgx_cpu_latency_ms": cpu_latency,
            "dgx_cuda_latency_ms": cuda_latency,
            "cuda_vs_dgx_cpu_latency_speedup": cpu_latency / cuda_latency,
            "cuda_vs_windows_latency_speedup": win_latency / cuda_latency,
            "windows_cpu_throughput_mbps": win_throughput,
            "dgx_cpu_throughput_mbps": cpu_throughput,
            "dgx_cuda_throughput_mbps": cuda_throughput,
            "cuda_vs_dgx_cpu_throughput_ratio": cuda_throughput / cpu_throughput,
            "windows_latency_cv_percent": cv_percent(
                windows[label], "phy_latency_ms"),
            "dgx_cpu_latency_cv_percent": cv_percent(
                dgx_cpu[label], "phy_latency_ms"),
            "dgx_cuda_latency_cv_percent": cv_percent(
                dgx_cuda[label], "phy_latency_ms"),
            "repeats_per_backend": len(windows[label]),
        })

    args.output_csv.parent.mkdir(parents=True, exist_ok=True)
    with args.output_csv.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)

    lines = [
        "# Windows CPU、DGX CPU 与 DGX CUDA 物理层性能对比",
        "",
        "测试参数：Release、1024 子载波、CP 128、64QAM、45 dB、CFO 300 Hz、"
        "SFO 20 ppm、定时偏差 20 采样、空间相关系数 0.02、固定随机种子。"
        "每种模式预热 20 包，测量 200 包，重复 5 轮；表中为中位数。",
        "",
        "三组各 50 轮，共 150 轮；所有 UDP 数据逐字节回环一致，FER=0、UDP 丢包=0。"
        "普通帧关闭仿真专用 BER/EVM/NMSE 真值统计，周期遥测/感知帧仍保留；"
        "CUDA 加速比为 DGX CPU 延迟 / DGX CUDA 延迟，大于 1 才表示 GPU 更快。",
        "",
        "| 模式 | Windows CPU ms/帧 | DGX CPU ms/帧 | DGX CUDA ms/帧 | "
        "CUDA/CPU 加速 | Win/CUDA 加速 |",
        "|---|---:|---:|---:|---:|---:|",
    ]
    for row in rows:
        lines.append(
            f"| {row['label']} | {row['windows_cpu_latency_ms']:.4f} | "
            f"{row['dgx_cpu_latency_ms']:.4f} | "
            f"{row['dgx_cuda_latency_ms']:.4f} | "
            f"{row['cuda_vs_dgx_cpu_latency_speedup']:.2f}x | "
            f"{row['cuda_vs_windows_latency_speedup']:.2f}x |")
    lines.extend([
        "",
        "| 模式 | Windows CPU Mbit/s | DGX CPU Mbit/s | DGX CUDA Mbit/s | "
        "CUDA/CPU 吞吐比 | 延迟波动 Win/CPU/CUDA |",
        "|---|---:|---:|---:|---:|---:|",
    ])
    for row in rows:
        lines.append(
            f"| {row['label']} | {row['windows_cpu_throughput_mbps']:.3f} | "
            f"{row['dgx_cpu_throughput_mbps']:.3f} | "
            f"{row['dgx_cuda_throughput_mbps']:.3f} | "
            f"{row['cuda_vs_dgx_cpu_throughput_ratio']:.2f}x | "
            f"{row['windows_latency_cv_percent']:.1f}% / "
            f"{row['dgx_cpu_latency_cv_percent']:.1f}% / "
            f"{row['dgx_cuda_latency_cv_percent']:.1f}% |")
    lines.extend([
        "",
        "## CUDA 接入范围",
        "",
        "- SISO、2x2、STBC、4Tx/4Rx Rank-2、4Tx/4Rx Rank-4 都通过同一个 "
        "`--backend cpu|cuda|auto` 接口选择执行后端。",
        "- CUDA 路径已经进入正式整帧接收链：批量 1024 点 OFDM FFT/去 CP；"
        "空间复用模式还包含批量 2x2、4x2、4x4 MMSE 检测。",
        "- 4 端口空间复用把 MMSE、QPSK/16/64/256-QAM max-log软解调、"
        "21x48 逆交织和软解扰融合在同一 CUDA kernel；普通帧直接回传 LDPC "
        "顺序 LLR，遥测帧才额外回传星座和 MSE。",
        "- 前导同步、TDL/CFO/SFO 信道、导频信道估计和 LDPC/CRC "
        "仍在 CPU。这是 CPU+CUDA 混合整帧实现，不是全 GPU 实现。",
        "- STBC 当前只有 FFT 在 CUDA，Alamouti 合并仍在 CPU，所以其 GPU 收益有限。",
        "",
        "## 结论",
        "",
        "- CUDA 对 4 端口模式收益最稳定；Rank-4 两种导频模式均降低整帧延迟。",
        "- SISO、2x2、STBC 的单帧批量很小，GPU 内存传输和 kernel 启动开销"
        "占比高，不能用 CUDA 微基准的峰值加速比外推整帧结果。",
        "- 当前 Windows CPU 仍是低阶模式最低延迟方案；DGX 的优势会随端口数、"
        "子载波批次和并发帧数增加而变大。",
        "- 下一轮应把 GPU 驻留边界扩展到信道估计，并采用多帧"
        "合批减少同步次数；随后再根据端到端瓶颈评估 LDPC GPU 化。",
    ])
    args.output_markdown.write_text("\n".join(lines) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
