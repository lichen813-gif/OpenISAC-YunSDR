"""OpenISAC sensing, burst playback and real-time video panels."""

from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from queue import Empty, SimpleQueue
from threading import Event
from time import perf_counter, perf_counter_ns

import numpy as np
import pyqtgraph as pg
from PySide6.QtCore import QRectF, Qt, QTimer, QUrl
from PySide6.QtGui import QImage
from PySide6.QtMultimedia import (QAudioOutput, QCamera, QMediaCaptureSession, QMediaDevices, QMediaPlayer,
                                  QVideoFrame, QVideoSink)
from PySide6.QtMultimediaWidgets import QVideoWidget
from PySide6.QtWidgets import (QCheckBox, QComboBox, QFileDialog, QHBoxLayout, QLineEdit, QProgressBar,
                               QSlider, QStackedWidget, QTabWidget, QVBoxLayout, QWidget)

from .iq import complex_iq
from .openisac import DEFAULT_CARRIERS, RATE, TOPOLOGIES, OpenIsacConfig, sense_openisac, sensing_demo
from .openisac_air import run_live_video
from .openisac_live import HEIGHT, LIVE_RATES, WIDTH, LatestFrame, LiveStats
from .openisac_video import video_loopback
from .widgets import button, card, field, float_box, label, plot_card


def _choice(items):
    result = QComboBox()
    for name, value in items:
        result.addItem(name, value)
    return result


class OpenIsacLab:
    def __init__(self, workbench):
        self.window = workbench
        self.executor = ThreadPoolExecutor(max_workers=1, thread_name_prefix="studio-openisac")
        self.future = None
        self.cancel = Event()
        self.progress_events = SimpleQueue()
        self.player = None
        self.recovered = None
        self.live_future = None
        self.live_role = "loop"
        self.live_cancel = Event()
        self.camera = None
        self.capture = None
        self.camera_sink = None
        self.camera_device = QMediaDevices()

        self.left = QWidget()
        left = QVBoxLayout(self.left)
        left.setContentsMargins(0, 0, 0, 0)
        self.video_modes = QTabWidget()
        self.video_modes.setStyleSheet("QTabBar::tab { padding: 7px 13px; }")
        left.addWidget(self.video_modes)
        left.addStretch()
        sensing = QWidget()
        self.sensing_tab = self.video_modes.addTab(sensing, "感知")
        sense_layout = QVBoxLayout(sensing)
        self.topology = _choice(TOPOLOGIES)
        self.pilot = _choice([("FDM", False), ("前置 DM-RS", True)])
        self.modulation = _choice([("QPSK", 4), ("16-QAM", 16), ("64-QAM", 64), ("256-QAM", 256)])
        self.modulation.setCurrentIndex(2)
        sense_layout.addWidget(field("收发结构", self.topology))
        sense_layout.addWidget(label(f"{DEFAULT_CARRIERS} 载波 · 80% 限带", "muted"))
        sense_layout.addWidget(field("导频", self.pilot))
        sense_layout.addWidget(field("发射调制", self.modulation))
        self.center = float_box(1, 100_000, 5800, 3)
        self.coherent = _choice([(f"{n} 帧", n) for n in (16, 32, 64, 128)])
        self.coherent.setCurrentIndex(2)
        sense_layout.addWidget(field("感知中心频率 (MHz)", self.center))
        sense_layout.addWidget(field("相干积累", self.coherent))
        self.static = QCheckBox("抑制静态杂波")
        self.static.setChecked(True)
        sense_layout.addWidget(self.static)
        sense_layout.addWidget(button("离线运动目标演示", lambda: self._sense(True), "primary"))
        sense_layout.addWidget(button("用当前 IQ 做感知", lambda: self._sense(False)))
        sense_layout.addStretch()

        clip = QWidget()
        clip_layout = QVBoxLayout(clip)
        self.clip_tab = self.video_modes.addTab(clip, "短片回环")
        clip_layout.addWidget(button("设置收发与感知", lambda: self.video_modes.setCurrentIndex(self.sensing_tab)))
        self.source = QLineEdit(readOnly=True, placeholderText="MP4 / H.264 或 MPEG-TS")
        choose = QHBoxLayout()
        choose.addWidget(self.source, 1)
        choose.addWidget(button("浏览", self._pick_video))
        clip_layout.addWidget(label("视频源", "muted"))
        clip_layout.addLayout(choose)
        self.snr = float_box(0, 80, 45, 1, " dB")
        clip_layout.addWidget(field("视频仿真 SNR", self.snr))
        self.video_sensing = QCheckBox("对视频 PHY 帧同时感知")
        self.video_sensing.setChecked(True)
        clip_layout.addWidget(self.video_sensing)
        clip_layout.addWidget(button("传输并回放", self._video, "primary"))
        self.stop = button("停止视频传输", self.cancel.set)
        self.stop.setEnabled(False)
        clip_layout.addWidget(self.stop)
        self.video_heatmap = button("视频感知热图", self._show_video_sensing)
        self.video_heatmap.setEnabled(False)
        clip_layout.addWidget(self.video_heatmap)

        self.right, content = card()
        self.pages = QStackedWidget()
        content.addWidget(self.pages, 1)
        scene = QWidget()
        layout = QVBoxLayout(scene)
        layout.setContentsMargins(0, 0, 0, 0)
        plot, self.heatmap, self.caption = plot_card("距离–多普勒 / CFAR", "距离 (m)", "径向速度 (m/s)")
        self.image = pg.ImageItem(axisOrder="row-major")
        self.image.setColorMap(pg.colormap.get("viridis"))
        self.heatmap.addItem(self.image)
        self.markers = pg.ScatterPlotItem(symbol="+", size=18, pen=pg.mkPen("#f7df91", width=2), brush=None)
        self.heatmap.addItem(self.markers)
        self.sensing_status = label("选择离线运动目标演示，或分析一段连续 OpenISAC IQ。", "muted")
        self.sensing_status.setWordWrap(True)
        layout.addWidget(plot, 1)
        layout.addWidget(self.sensing_status)
        self.pages.addWidget(scene)

        video = QWidget()
        layout = QVBoxLayout(video)
        layout.setContentsMargins(0, 0, 0, 0)
        self.video_status = label("选择视频开始离线回环。", "section")
        self.video_status.setWordWrap(True)
        layout.addWidget(self.video_status)
        self.bar = QProgressBar()
        self.bar.setRange(0, 100)
        layout.addWidget(self.bar)
        self.screen = QVideoWidget()
        self.screen.setStyleSheet("background:#10263a")
        layout.addWidget(self.screen, 1)
        playback = QHBoxLayout()
        self.play = button("播放 / 暂停", self._play)
        self.play.setEnabled(False)
        playback.addWidget(self.play)
        self.seek = QSlider(Qt.Orientation.Horizontal)
        self.seek.setAccessibleName("视频播放位置")
        playback.addWidget(self.seek, 1)
        layout.addLayout(playback)
        self.pages.addWidget(video)
        self.return_to_video = button("返回视频回放", lambda: self.pages.setCurrentIndex(1))
        self.return_to_video.setEnabled(False)
        clip_layout.addWidget(self.return_to_video)
        clip_layout.addStretch()
        self.timer = QTimer(self.right)
        self.timer.setInterval(80)
        self.timer.timeout.connect(self._poll)
        self._setup_live()

    def _setup_live(self):
        controls = QWidget()
        layout = QVBoxLayout(controls)
        self.live_tab = self.video_modes.addTab(controls, "实时视频")
        self.live_mode = _choice([("本机仿真回环", "loop"), ("本机射频回环", "rf"),
                                  ("设备发送", "tx"), ("设备接收", "rx")])
        layout.addWidget(field("连接方式", self.live_mode))
        self.live_radio_hint = label("两端设为 15.36 MSa/s · CH 0 / CH 1\n发送端 TX LO 与接收端 RX LO 设为相同频率", "muted")
        self.live_radio_hint.setWordWrap(True)
        self.live_apply_rate = button("应用视频采样率", self._apply_live_rate)
        layout.addWidget(self.live_radio_hint)
        layout.addWidget(self.live_apply_rate)
        self.live_input = _choice([("本地视频", "file"), ("摄像头", "camera")])
        self.live_input_field = field("输入", self.live_input)
        layout.addWidget(self.live_input_field)
        self.live_inputs = QStackedWidget()
        file_page = QWidget()
        pick = QHBoxLayout(file_page)
        pick.setContentsMargins(0, 0, 0, 0)
        self.live_file = QLineEdit(readOnly=True, placeholderText="选择本地视频")
        pick.addWidget(self.live_file, 1)
        pick.addWidget(button("浏览", self._pick_live_file))
        self.live_inputs.addWidget(file_page)
        self.live_cameras = QComboBox()
        self.live_inputs.addWidget(self.live_cameras)
        layout.addWidget(self.live_inputs)
        self.live_loop = QCheckBox("循环播放源文件")
        layout.addWidget(self.live_loop)
        self.live_input.currentIndexChanged.connect(self.live_inputs.setCurrentIndex)
        self.live_input.currentIndexChanged.connect(self._live_controls)
        self.live_rate = _choice([(f"{bps/1e6:g} Mbit/s", bps) for bps in LIVE_RATES])
        self.live_snr = float_box(0, 80, 45, 1, " dB")
        self.live_rate_field = field("链路总码率", self.live_rate)
        self.live_snr_field = field("仿真 SNR", self.live_snr)
        layout.addWidget(self.live_rate_field)
        layout.addWidget(self.live_snr_field)
        self.live_start = button("开始仿真回环", self._live_video, "primary")
        self.live_stop = button("停止实时回环", self.stop_live)
        self.live_stop.setEnabled(False)
        layout.addWidget(self.live_start)
        layout.addWidget(self.live_stop)
        layout.addStretch()
        self.camera_device.videoInputsChanged.connect(self._refresh_cameras)
        self.live_mode.currentIndexChanged.connect(self._live_controls)
        self._refresh_cameras()
        self._live_controls()

        live = QWidget()
        view = QVBoxLayout(live)
        view.setContentsMargins(0, 0, 0, 0)
        self.live_status = label(f"2×2 Alamouti STBC · 64-QAM／FDM · {DEFAULT_CARRIERS} 载波 · 15.36 MSa/s",
                                 "section")
        self.live_status.setWordWrap(True)
        view.addWidget(self.live_status)
        self.live_screen = QVideoWidget()
        self.live_screen.setStyleSheet("background:#10263a")
        view.addWidget(self.live_screen, 1)
        self.live_counters = label("", "muted")
        self.live_counters.setWordWrap(True)
        view.addWidget(self.live_counters)
        self.live_page = self.pages.addWidget(live)
        self.live_timer = QTimer(live)
        self.live_timer.setTimerType(Qt.TimerType.PreciseTimer)
        self.live_timer.setInterval(5)
        self.live_timer.timeout.connect(self._poll_live)
        self.video_modes.currentChanged.connect(
            lambda index: self.pages.setCurrentIndex(self.live_page if index == self.live_tab else
                                                      1 if index == self.clip_tab else 0))

    def _live_controls(self, *_):
        mode = self.live_mode.currentData()
        sending = mode != "rx"
        for item in (self.live_input_field, self.live_inputs, self.live_rate_field):
            item.setVisible(sending)
        self.live_loop.setVisible(sending and self.live_input.currentData() == "file")
        self.live_snr_field.setVisible(mode == "loop")
        self.live_radio_hint.setVisible(mode != "loop")
        self.live_radio_hint.setText(
            "15.36 MSa/s · CH 0 / CH 1\nTX LO 与 RX LO 相同\n"
            "两路 TX 各经匹配衰减器接到对应 RX" if mode == "rf" else
            "两端 15.36 MSa/s · CH 0 / CH 1\n两端的 TX LO 与 RX LO 对准")
        self.live_apply_rate.setVisible(mode != "loop")
        self.live_start.setText({"loop": "开始仿真回环", "rf": "开始射频回环",
                                 "tx": "开始设备发送", "rx": "开始设备接收"}[mode])

    def _apply_live_rate(self):
        window = self.window
        group = window.rf_group.currentData()
        window._submit("apply_video_rate", group, callback=lambda settings: window._read_back(group, settings),
                       error_callback=lambda error: self.live_status.setText(str(error)))

    def _refresh_cameras(self):
        selected = self.live_cameras.currentData()
        self.live_cameras.clear()
        for device in self.camera_device.videoInputs():
            self.live_cameras.addItem(device.description(), device)
            if selected is not None and device.id() == selected.id():
                self.live_cameras.setCurrentIndex(self.live_cameras.count()-1)
        if not self.live_cameras.count():
            self.live_cameras.addItem("未检测到摄像头", None)

    def _pick_live_file(self):
        path, _ = QFileDialog.getOpenFileName(self.left, "选择实时发送视频", str(self.window.last_dir),
                                             "视频 (*.mp4 *.ts *.mts *.mkv *.mov *.avi);;所有文件 (*)")
        if path:
            self.live_file.setText(path)
            self.window.last_dir = Path(path).parent

    def _live_video(self):
        if self.future is not None or self.live_future is not None:
            return
        self.pages.setCurrentIndex(self.live_page)
        role = self.live_mode.currentData()
        if role != "loop":
            info = self.window.session.status.info
            if info is None or info.connection.transport not in ("pcies", "pciex"):
                self.live_status.setText("请先在设备设置中连接本机的 PCIe YunSDR")
                return
            if self.window.settings.sample_rate != RATE:
                self.live_status.setText("请先应用视频采样率 15.36 MSa/s")
                return
        if role == "rx":
            self.start_live(role=role)
            return
        if self.live_input.currentData() == "file":
            if not self.live_file.text():
                self._pick_live_file()
            if self.live_file.text():
                self.start_live(source=Path(self.live_file.text()), loop=self.live_loop.isChecked(), role=role)
        elif (device := self.live_cameras.currentData()) is not None:
            self.start_live(device=device, role=role)
        else:
            self.live_status.setText("未检测到摄像头。连接后可重新选择输入。")

    def start_live(self, *, source=None, device=None, camera_feed=None, loop=False, role="loop"):
        if self.future is not None or self.live_future is not None or self.window.pending:
            return
        self.live_role = role
        self.live_mode.setCurrentIndex(self.live_mode.findData(role))
        self.video_modes.setCurrentIndex(self.live_tab)
        self.pages.setCurrentIndex(self.live_page)
        self.live_screen.videoSink().setVideoFrame(QVideoFrame())
        if self.player is not None:
            self.player.stop()
        self.live_stats = LiveStats(role)
        self.live_output = LatestFrame()
        self.live_camera = LatestFrame() if device is not None else camera_feed
        self.live_cancel.clear()
        self.live_start.setEnabled(False)
        for control in (self.live_mode, self.live_apply_rate, self.live_input_field, self.live_inputs,
                        self.live_loop, self.live_rate_field, self.live_snr_field):
            control.setEnabled(False)
        self.live_stop.setEnabled(True)
        self.next_live_status = 0
        self.live_failure = ""
        self.live_counters.clear()
        self.live_status.setText("正在启动 · 2×2 STBC · 64-QAM／FDM")
        rate, snr = self.live_rate.currentData(), self.live_snr.value()
        if device is not None:
            self.camera = QCamera(device, self.live_screen)
            self.capture = QMediaCaptureSession(self.live_screen)
            self.camera_sink = QVideoSink(self.capture)
            self.camera_sink.videoFrameChanged.connect(self._camera_frame)
            self.camera.errorOccurred.connect(self._camera_error)
            self.capture.setCamera(self.camera)
            self.capture.setVideoSink(self.camera_sink)
        options = dict(source=source, camera=self.live_camera, output=self.live_output, stats=self.live_stats,
                       cancel=self.live_cancel, wire_rate=rate, loop=loop)
        if role == "loop":
            self.live_future = self.executor.submit(run_live_video, **options, snr=snr)
        else:
            self.live_future = self.window.session.submit("run_air_video", dict(options, role=role))
        self.live_timer.start()
        self.window._update_state()
        if self.camera is not None:
            self.camera.start()

    def _camera_frame(self, frame):
        if self.live_cancel.is_set() or not frame.isValid():
            return
        stamp = perf_counter_ns()
        image = frame.toImage()
        if image.isNull():
            return
        image = image.scaled(WIDTH, HEIGHT, Qt.AspectRatioMode.KeepAspectRatio,
                             Qt.TransformationMode.FastTransformation).convertToFormat(QImage.Format.Format_RGB888)
        pixels = np.frombuffer(image.constBits(), dtype=np.uint8).reshape(image.height(), image.bytesPerLine())
        rgb = pixels[:, :image.width()*3].reshape(image.height(), image.width(), 3).copy()
        self.live_camera.put(rgb, stamp)

    def _camera_error(self, _, message):
        self.live_failure = f"摄像头 · {message}"
        self.stop_live()

    @property
    def live_running(self):
        return self.live_future is not None and not self.live_cancel.is_set()

    @property
    def owns_radio(self):
        return self.live_future is not None and self.live_role in ("tx", "rx", "rf")

    def stop_live(self):
        self.live_cancel.set()
        if self.camera is not None:
            camera, capture = self.camera, self.capture
            self.camera = self.capture = self.camera_sink = None
            capture.setVideoSink(None)
            camera.stop()
            camera.deleteLater()
            capture.deleteLater()

    def _poll_live(self):
        if sample := self.live_output.take():
            rgb, stamp = sample
            h, w = rgb.shape[:2]
            image = QImage(rgb.data, w, h, rgb.strides[0], QImage.Format.Format_RGB888).copy()
            self.live_screen.videoSink().setVideoFrame(QVideoFrame(image))
            self.live_stats.show(stamp)
        active = not self.live_future.done()
        if active and perf_counter() < self.next_live_status:
            return
        self.next_live_status = perf_counter()+.25
        stats = self.live_stats.snapshot(detail=not active)
        wire = stats["offered_mbps"]
        drops = str(stats["lost"]) if not active else "待排空"
        display_drop = self.live_output.dropped
        rx_delay = stats.get("last_display_ms")
        if not active:
            rx_delay = stats["display_latency"].get("p95_ms")
        delay = f" · 显示 {'最近' if active else 'P95'} {rx_delay:.1f} ms" if rx_delay is not None else ""
        if self.live_role == "tx":
            counters = (f"{wire:.2f} Mbit/s · 本机 SDR 已提交 {stats['air_packets']:,}/{stats['sent']:,} 包 · "
                        f"发送缓冲丢包 {stats['air_dropped']:,} · 编码丢帧 {stats['encode_drop']:,}")
            phase = "设备发送中" if active else "发送结束"
        elif self.live_role == "rx":
            counters = (f"收到 {stats['received']:,} 包 · 序号缺口 {stats['lost']:,} · 显示 {stats['displayed']:,} 帧 · "
                        f"IQ 时间戳间断 / 主机丢块 {stats['iq_gaps']} / {stats['iq_dropped']} · 显示跳帧 {display_drop}")
            phase = ("设备接收中" if stats["displayed"] else "等待有效 RF 视频") if active else "接收结束"
        elif self.live_role == "rf":
            counters = (f"{wire:.2f} Mbit/s · SDR 已提交 {stats['air_packets']:,} 包 · "
                        f"已回传 {stats['received']:,}/{stats['sent']:,} 包 · 丢包 {drops} · "
                        f"IQ 时间戳间断 / 主机丢块 {stats['iq_gaps']} / {stats['iq_dropped']} · "
                        f"显示 {stats['displayed']:,} 帧{delay}")
            phase = ("射频回环中" if stats["displayed"] else "等待有效 RF 视频") if active else "射频回环结束"
        else:
            counters = (f"{wire:.2f} Mbit/s · 已回传 {stats['received']:,}/{stats['sent']:,} 包 · 丢包 {drops} · "
                        f"PHY 队列 {stats['phy_queue']}/{stats['phy_queue_peak']} · "
                        f"编码队列 {stats['source_queue']}/{stats['source_queue_peak']} · 在途 {stats['in_flight']} · "
                        f"显示 {stats['displayed']:,} 帧 · 编码 / 显示丢帧 {stats['encode_drop']} / {display_drop}{delay}")
            phase = ("实时接收" if stats["displayed"] else "等待首帧") if active else "回环结束"
        self.live_counters.setText(counters)
        self.live_status.setText(f"{phase} · 2×2 STBC · 64-QAM／FDM · {DEFAULT_CARRIERS} 载波")
        if not active:
            self.live_timer.stop()
            self.stop_live()
            try:
                self.live_future.result()
            except Exception as exc:
                self.live_failure = str(exc)
            if self.live_failure:
                self.live_status.setText(self.live_failure)
            self.live_future = None
            self.live_start.setEnabled(True)
            for control in (self.live_mode, self.live_apply_rate, self.live_input_field, self.live_inputs,
                            self.live_loop, self.live_rate_field, self.live_snr_field):
                control.setEnabled(True)
            self.live_stop.setEnabled(False)
            self.window._update_state()

    def config(self):
        return OpenIsacConfig.from_topology(self.topology.currentData(), self.modulation.currentData(),
                                            self.pilot.currentData())

    def _sense(self, demo):
        if self.future is not None or self.live_future is not None:
            return
        config = self.config()
        hz, frames, reject = self.center.value()*1e6, self.coherent.currentData(), self.static.isChecked()
        self.video_modes.setCurrentIndex(self.sensing_tab)
        self.pages.setCurrentIndex(0)
        if demo:
            rate = self.window.settings.sample_rate
            task = lambda: sensing_demo(config, hz, frames, reject_static=reject, sample_rate=rate)
        else:
            frame = self.window.session.last_frame
            if frame is None:
                self.sensing_status.setText("请先采集或回放包含连续 OpenISAC 帧的 IQ。")
                return
            try:
                config.validate_capture(frame.channels, frame.sample_rate, frame.samples)
            except ValueError as exc:
                self.sensing_status.setText(str(exc))
                return
            task = lambda: sense_openisac(complex_iq(frame.iq), config, hz, frames, reject_static=reject,
                                          sample_rate=frame.sample_rate)
            rate = frame.sample_rate
        self.sensing_status.setText(f"计算中 · {config.mode}")
        source = "合成运动目标" if demo else "当前 IQ"
        heading = f"{source} · {rate/1e6:g} MSa/s"
        self._start(task, lambda result: self._show_sensing(result, heading), self.sensing_status)

    def _show_sensing(self, result, heading):
        # Show 128 bins by default and expand as needed for distant detections.
        stop = max(128, max((int(hit[0]/result.ranges[1])+12 for hit in result.detections), default=0))
        ranges = result.ranges[:stop]
        dx, dy = ranges[1], result.velocity[1]-result.velocity[0]
        self.image.setImage(result.power[:, :len(ranges)], autoLevels=False, levels=(-55, 0))
        self.image.setRect(QRectF(-dx/2, result.velocity[0]-dy/2, ranges.size*dx, result.velocity.size*dy))
        self.heatmap.setRange(xRange=(0, ranges[-1]), yRange=(result.velocity[0], result.velocity[-1]), padding=0)
        self.markers.setData(x=[hit[0] for hit in result.detections], y=[hit[2] for hit in result.detections])
        self.caption.setText(f"{result.power.shape[0]} 帧 · {len(result.detections)} 个 CFAR 目标")
        details = [f"{i+1}. {r:.2f} m  ·  {fd:+.2f} Hz  ·  {v:+.2f} m/s  ·  高于门限 {db:.1f} dB"
                   for i, (r, fd, v, db) in enumerate(result.detections[:6])]
        self.sensing_status.setText("\n".join([f"{heading} · 单站距离", *(details or ["当前参数下未检出 CFAR 目标"])]))

    def _show_video_sensing(self):
        self.pages.setCurrentIndex(0)
        self._show_sensing(self.recovered.sensing, "视频回环 · 同帧感知")

    def _pick_video(self):
        path, _ = QFileDialog.getOpenFileName(self.left, "选择离线视频", str(self.window.last_dir),
                                             "视频 (*.mp4 *.ts *.mts *.mkv *.mov)")
        if path:
            self.source.setText(path)
            self.window.last_dir = Path(path).parent

    def _video(self):
        if self.future is not None or self.live_future is not None:
            return
        if not self.source.text():
            self._pick_video()
        if not self.source.text():
            return
        src = Path(self.source.text())
        dst, _ = QFileDialog.getSaveFileName(self.left, "保存回传视频", str(src.with_name(src.stem+"_openisac.ts")),
                                            "MPEG-TS (*.ts)")
        if dst:
            path = Path(dst)
            self.start_video(src, path if path.suffix.lower() == ".ts" else path.with_suffix(".ts"))

    def start_video(self, src, dst):
        if self.future is not None or self.live_future is not None:
            return
        config, snr = self.config(), self.snr.value()
        coherent = self.coherent.currentData() if self.video_sensing.isChecked() else 0
        rf_hz = self.center.value()*1e6
        self.video_modes.setCurrentIndex(self.clip_tab)
        self.pages.setCurrentIndex(1)
        if self.player is not None:
            self.player.stop()
            self.player.setSource(QUrl())
        self.recovered = None
        self.play.setEnabled(False)
        self.video_heatmap.setEnabled(False)
        self.return_to_video.setEnabled(False)
        self.stop.setEnabled(True)
        self.bar.setRange(0, 0)
        self.video_status.setText(f"PHY 视频突发回环 · {config.mode}")
        self._start(lambda: video_loopback(src, dst, config, snr=snr, coherent=coherent, rf_hz=rf_hz, cancel=self.cancel,
                                           progress=lambda done, total: self.progress_events.put((done, total))),
                    lambda result: self._video_ready(result, coherent), self.video_status)

    def _start(self, task, callback, status):
        self.cancel.clear()
        self.future = self.executor.submit(task), callback, status
        self.timer.start()

    def _poll(self):
        try:
            while True:
                done, total = self.progress_events.get_nowait()
                self.bar.setRange(0, 100)
                self.bar.setValue(int(done*100/total))
        except Empty:
            pass
        future, callback, status = self.future
        if not future.done():
            return
        self.timer.stop()
        self.future = None
        self.stop.setEnabled(False)
        try:
            callback(future.result())
        except Exception as exc:
            self.bar.setRange(0, 100)
            self.bar.setValue(0)
            status.setText(str(exc))
            self.window._error(exc)

    def _video_ready(self, result, coherent):
        self.recovered = result
        self.bar.setRange(0, 100)
        self.bar.setValue(100)
        sensing = ""
        if coherent:
            sensing = " · 同帧感知已完成" if result.sensing else f" · 未生成 {coherent} 帧感知图"
        self.video_status.setText(f"{result.datagrams:,}/{result.datagrams:,} 包逐字节一致 · "
                                  f"{result.bytes:,} 字节 · 离线处理 {result.mbps:.2f} Mbit/s · {result.path.name}{sensing}")
        self.video_heatmap.setEnabled(result.sensing is not None)
        self.return_to_video.setEnabled(True)
        if self.player is None:
            self.player = QMediaPlayer(self.right)
            self.audio = QAudioOutput(self.right)
            self.player.setAudioOutput(self.audio)
            self.player.setVideoOutput(self.screen)
            self.player.durationChanged.connect(lambda duration: self.seek.setRange(0, duration))
            self.player.positionChanged.connect(self.seek.setValue)
            self.player.errorOccurred.connect(lambda _, message: self.window._error(f"视频回放 · {message}"))
            self.seek.sliderMoved.connect(self.player.setPosition)
        self.player.setSource(QUrl.fromLocalFile(str(result.path)))
        self.play.setEnabled(True)
        self.player.play()

    def _play(self):
        if self.player.playbackState() == QMediaPlayer.PlaybackState.PlayingState:
            self.player.pause()
        else:
            self.player.play()

    def close(self):
        self.live_timer.stop()
        self.stop_live()
        self.timer.stop()
        self.cancel.set()
        if self.player is not None:
            self.player.stop()
        self.executor.shutdown(wait=False, cancel_futures=True)
