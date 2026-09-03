# Y240 OpenISAC hardware video test

## Implemented modes

| Command mode | RF ports | Spatial rank | Scheme | Detector/combiner |
|---|---:|---:|---|---|
| `siso` | 1Tx/1Rx | 1 | spatial | single-port equalizer |
| `mimo2` | 2Tx/2Rx | 2 | spatial multiplexing | 2x2 MMSE |
| `stbc` | 2Tx/2Rx | 1 | Alamouti STBC | Alamouti combining |

All modes use the formal 1024-subcarrier OFDM frame, 128-sample CP, ZC
preamble, CRC16, LDPC(1008,504), and either FDM pilots or NR-like DMRS. FDM is
the current hardware-video default because it uses three OFDM symbols per
frame instead of five.

At 64QAM/FDM the measured application payload capacities are:

- SISO: 439 bytes per PHY frame; 419 bytes of video fragment data.
- 2x2 two-layer: 880 bytes; 860 bytes of video fragment data.
- 2x2 STBC: 376 bytes; 356 bytes of video fragment data.

## Build

From the repository's `libyunsdr-isac` directory:

```powershell
.\build_vendor_y240_pcies_vs2019.cmd
.\build_hardware_vs2019.cmd
```

The VS2019 bundled CMake and Ninja are used. Hardware executables and required
DLLs are under `build\ninja-vs2019-hardware`.

## Deterministic PHY tests

The following commands use the user-confirmed cabled-loopback TX setting of
60 and do not raise it:

```powershell
build\ninja-vs2019-hardware\yunsdr_phy_loopback.exe --device pcies:0.0 --mode siso  --modulation 64qam --pilot fdm --frames 20 --frequency-mhz 1500 --tx-gain 60 --rx-gain 20
build\ninja-vs2019-hardware\yunsdr_phy_loopback.exe --device pcies:0.0 --mode mimo2 --modulation 64qam --pilot fdm --frames 20 --frequency-mhz 1500 --tx-gain 60 --rx-gain 24
build\ninja-vs2019-hardware\yunsdr_phy_loopback.exe --device pcies:0.0 --mode stbc  --modulation 64qam --pilot fdm --frames 20 --frequency-mhz 1500 --tx-gain 60 --rx-gain 18
```

For a video-sized fragmentation/reassembly test without VLC:

```powershell
build\ninja-vs2019-hardware\yunsdr_video_bridge.exe --device pcies:0.0 --mode siso --modulation 64qam --pilot fdm --frequency-mhz 1500 --tx-gain 60 --rx-gain 20 --self-test 64 --batch-packets 8 --lead-blocks 48 --retries 8
```

Replace `siso` with `mimo2` or `stbc` and use the recommended RX gains above.

## VLC video

The simple command is:

```powershell
.\run_y240_video.cmd "C:\path\to\video.mp4" siso 64qam fdm 1500 60 20 1000
```

Positional parameters are:

1. video file;
2. `siso`, `mimo2`, or `stbc`;
3. `qpsk`, `16qam`, `64qam`, or `256qam`;
4. `fdm` or `dmrs`;
5. center frequency in MHz;
6. TX gain;
7. RX gain;
8. VLC H.264 bitrate in kbit/s.

Advanced PowerShell example:

```powershell
.\run_y240_video.ps1 `
  -VideoFile "C:\path\to\video.mp4" `
  -Mode mimo2 -Modulation 64qam -Pilot fdm `
  -FrequencyMHz 1500 -TxGain 60 -RxGain 24 `
  -VideoBitrateKbps 1000 -BatchPackets 8 -LeadBlocks 48 -Retries 8
```

The script waits for a hardware warmup result before starting VLC. It retries
the complete Y240 session up to three times, opens the receiver VLC in the
foreground, starts the Python PHY/sensing monitor, batches input UDP packets,
drains the UDP queue at the end, saves the bridge log under
`out\hardware-video`, and closes every process it started.

If all three startup attempts report `hardware warmup batch failed`, VLC has
not started yet. Use the Chinese
[warmup troubleshooting guide](Y240_VIDEO_WARMUP_FAILURE_TROUBLESHOOTING_zh.md)
to interpret the non-`.err` bridge log and isolate RF, PHY, timestamp, or DMA
failures.

If SISO repeatedly fails specifically at
`fragment 3 timing=1 header=0 crc=0`, use the Chinese
[short-tail fragment analysis and solution](Y240_FRAGMENT3_SHORT_TAIL_FAILURE_SOLUTION_zh.md).
It explains why this pre-VLC failure is independent of the selected video and
defines the fixed-size padded-fragment implementation and acceptance tests.

## Live hardware monitor and sensing

The normal `run_y240_video.cmd` command now opens the existing OpenISAC live
monitor automatically. Hardware telemetry is written to
`out\hardware-video\live_phy_monitor` and refreshed every 0.8 seconds. The
window displays:

- per-layer decision-directed constellation and EVM;
- 4096 points of received time-domain IQ;
- SISO or 2x2 channel-estimate frequency responses and condition numbers;
- hardware CFO, residual SFO, timing, noise, CRC/FER and Y240 stream events;
- a range-Doppler heat map and CFAR detections from measured channel snapshots.

The sensing processor uses 16 consecutive over-air PHY frames within each
video burst. It noncoherently combines all four channel links in 2x2/STBC and
uses the single link in SISO. At 15.36 Msps the nominal range-bin spacing is
about 9.76 m. At 1500 MHz with FDM and 16 coherent frames, the velocity-bin
spacing is about 27.8 m/s, so the current view is useful for hardware closure,
static-path/range verification and large-Doppler checks; resolving 1-2 m/s
requires a later continuous-frame sensing scheduler with a much longer CPI.

PowerShell advanced controls:

```powershell
.\run_y240_video.ps1 -VideoFile "C:\path\to\video.mp4" `
  -Mode mimo2 -Modulation 64qam -Pilot fdm `
  -FrequencyMHz 1500 -TxGain 60 -RxGain 24 `
  -RefreshSeconds 0.8 -SensingCoherentFrames 16
```

Use `-NoMonitor` only when a headless run is required. The monitor, VLC sender,
VLC receiver, and bridge are all closed automatically at the end of a normal
test.

In a cabled loopback, the dominant path normally occupies range bin zero. The
CFAR stage deliberately excludes the protected near-range cells, so the heat
map can show a strong zero-range path while the final target count remains
zero. `sensing_raw_cfar_detection_count` records candidates before this
near-range filter; `sensing_detection_count` is the number plotted as valid
targets.

## Gain, EVM, and frequency findings

EVM on hardware captures is decision-directed: each equalized point is
compared with its nearest ideal QAM point. It is suitable for gain scans and
compression detection, but it can understate EVM when a symbol crosses a
decision boundary.

At 2.4 GHz, TX=60 and RX=5 was not saturated: the received peak was only about
0.028 of full scale, clipping was 0%, and 64QAM EVM was about 51.6%. Increasing
RX gain improved the link:

| Frequency | TX/RX gain | SISO 64QAM result | Decision EVM | RX peak |
|---:|---:|---|---:|---:|
| 2400 MHz | 60/25 | CRC pass | about 11.6% | 0.23 |
| 2400 MHz | 60/30 | CRC pass | about 8.0% | 0.41 |
| 2400 MHz | 60/35 | CRC pass | about 6.7-7.1% | 0.77 |
| 2400 MHz | 60/40 | CRC pass, near compression | about 9.8% | 1.01 |
| 1500 MHz | 60/20 | CRC pass | about 8.1% | 0.55 |
| 1500 MHz | 60/25 | CRC pass | about 6.6-6.9% | 0.90 |

At RX=25, 1500 MHz was markedly better than 2400 MHz. No valid burst was
received at 3000 MHz in the tested external path. The result describes the
complete cable/attenuator/port path, not only the Y240 RF IC.

## Verified results on 2026-08-25

- The working Y240 URI is `pcies:0.0`. Both hardware tools also accept an
  explicit `--device` argument; this value is now the project default.
- The reusable OpenISAC codec closed SISO, 2x2 two-layer, and STBC for FDM and
  DMRS with exact payload/CRC recovery.
- The vendor `yunsdr_ss_rate` test passed with `-a pcies:0.0`, 1500 MHz,
  RX=25, TX=60, masks `0x3/0x3`, 30720 samples, 15.36 Msps, and `-r 2`.
- The PHY loopback recovered 18/20 SISO frames at about 8.0-8.6% EVM,
  16/20 2x2 two-layer frames at about 8.5-9.2% EVM, and 18/20 STBC frames at
  about 4.1-4.5% EVM. All successful frames passed header, CRC, and payload
  comparison; clipping was 0%. Missing bursts coincided with TX timeouts.
- The 2x2 two-layer fixed-data video self-test recovered 64/64 packets with
  no retry, drop, overflow, underflow, or timestamp discontinuity.
- Real VLC tests used 1500 MHz, 64QAM/FDM, 1 Mbit/s, batch 8, lead 48 blocks,
  and 8 retries. SISO recovered 1246 packets, 2x2 two-layer recovered 1245,
  and STBC recovered 1245. All three reported zero dropped UDP packets.
  Logs: `bridge-20260825-130801-attempt1.log`,
  `bridge-20260825-130609-attempt2.log`, and
  `bridge-20260825-130708-attempt1.log` under `out\hardware-video`.
- The live-monitor hardware self-test recovered 64/64 2x2 packets. It produced
  all six monitor CSV files, including a 16-by-128 range-Doppler matrix. The
  measured timing index was 52, decision EVM was 7.19%, and the median channel
  condition number was 1.04.
- Separate sensing compatibility tests recovered 16/16 packets in both SISO
  and STBC. Both produced a valid 16-by-128 range-Doppler matrix; the final
  decision EVM was 9.35% for SISO and 1.26% for STBC, with timing indices 51
  and 52 respectively.
- A monitored 10-second 2x2 VLC run forwarded 1003 UDP packets and closed VLC,
  the Python monitor, and the bridge automatically. The bridge completed 1102
  packets with zero drops; the final monitor snapshot reported 8.58% EVM,
  timing index 51, median condition number 1.12, and a valid sensing matrix.
  The vendor stream reported 718 TX-timeout events, but bridge retries recovered
  them without application packet loss. Treat this counter as a scheduling or
  DMA-pressure diagnostic, not as the delivered-packet loss count.

The bridge now drains RX DMA continuously while waiting for VLC UDP input.
This prevents stale receive timestamps during VLC startup from scheduling TX
in the past, which previously caused TX timeouts and persistent CRC failures.
