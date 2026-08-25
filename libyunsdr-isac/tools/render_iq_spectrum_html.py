#!/usr/bin/env python3
"""Render spectrum-analysis JSON as an inline Codex visualization fragment."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


TEMPLATE = r'''<div id="y240-iq-spectrum">
  <h2>Y240 有线回环接收 IQ 频谱</h2>
  <div class="spectrum-note" aria-live="polite"></div>
  <div class="spectrum-plot" data-plot="full"></div>
  <div class="spectrum-plot" data-plot="zoom"></div>
  <div class="tooltip" role="tooltip" hidden></div>
</div>
<style>
  #y240-iq-spectrum { width: 100%; color: var(--foreground); }
  #y240-iq-spectrum h2 { margin: 0 0 8px; font-weight: 500; }
  #y240-iq-spectrum .spectrum-note { margin-bottom: 12px; color: var(--foreground); }
  #y240-iq-spectrum .spectrum-plot { width: 100%; margin-top: 12px; }
  #y240-iq-spectrum svg { display: block; width: 100%; overflow: visible; }
  #y240-iq-spectrum .axis text,
  #y240-iq-spectrum .axis-title,
  #y240-iq-spectrum .plot-title,
  #y240-iq-spectrum .annotation { fill: var(--foreground); font-size: 12px; }
  #y240-iq-spectrum .axis path,
  #y240-iq-spectrum .axis line,
  #y240-iq-spectrum .grid line,
  #y240-iq-spectrum rect[data-chart-frame] { stroke: var(--border); }
  #y240-iq-spectrum .grid path { display: none; }
  #y240-iq-spectrum .grid line { stroke-opacity: 0.55; }
  #y240-iq-spectrum .spectrum-line { fill: none; stroke: var(--viz-series-1); stroke-width: 1.5; }
  #y240-iq-spectrum .expected-line { stroke: var(--viz-series-2); stroke-width: 1.5; stroke-dasharray: 5 4; }
  #y240-iq-spectrum .detected-line { stroke: var(--viz-series-3); stroke-width: 1.5; }
  #y240-iq-spectrum .tooltip { position: absolute; pointer-events: none; color: var(--popover-foreground); background: var(--popover); border: 1px solid var(--border); padding: 6px 8px; }
</style>
<script src="https://cdn.jsdelivr.net/npm/d3@7.9.0/dist/d3.min.js"></script>
<script>
(() => {
  const root = document.getElementById('y240-iq-spectrum');
  const data = __DATA__;
  const note = root.querySelector('.spectrum-note');
  const errorSign = data.frequency_error_hz >= 0 ? '+' : '';
  note.textContent = `检测峰值 ${(data.peak_estimated_hz / 1e6).toFixed(6)} MHz；相对 +1 MHz 误差 ${errorSign}${data.frequency_error_hz.toFixed(2)} Hz；峰值 ${data.peak_level_dbfs.toFixed(1)} dBFS；无削顶。`;

  const configs = [
    { key: 'full_spectrum', node: root.querySelector('[data-plot="full"]'), title: '全带宽频谱', xLabel: '基带频率 (MHz)' },
    { key: 'tone_zoom', node: root.querySelector('[data-plot="zoom"]'), title: '+1 MHz 单音局部频谱', xLabel: '基带频率 (MHz)' }
  ];
  const tooltip = root.querySelector('.tooltip');

  function draw(config) {
    const values = data[config.key].map(d => ({ x: d[0], y: d[1] }));
    const width = Math.max(320, Math.floor(config.node.getBoundingClientRect().width));
    const height = width < 520 ? 280 : 320;
    const margin = { top: 32, right: 20, bottom: 54, left: 72 };
    const innerWidth = width - margin.left - margin.right;
    const innerHeight = height - margin.top - margin.bottom;

    d3.select(config.node).selectAll('*').remove();
    const svg = d3.select(config.node).append('svg')
      .attr('viewBox', `0 0 ${width} ${height}`)
      .attr('role', 'img')
      .attr('aria-label', `${config.title}。检测到峰值 ${(data.peak_estimated_hz / 1e6).toFixed(6)} MHz。`);
    svg.append('title').text(config.title);
    svg.append('desc').text(`期望单音为 1 MHz，检测频率误差 ${data.frequency_error_hz.toFixed(2)} Hz。`);

    const plot = svg.append('g').attr('transform', `translate(${margin.left},${margin.top})`);
    const xExtent = d3.extent(values, d => d.x);
    const yExtent = d3.extent(values, d => d.y);
    const xPad = Math.max((xExtent[1] - xExtent[0]) * 0.005, 0.001);
    const yPad = Math.max((yExtent[1] - yExtent[0]) * 0.08, 3);
    const x = d3.scaleLinear().domain([xExtent[0] - xPad, xExtent[1] + xPad]).range([0, innerWidth]);
    const y = d3.scaleLinear().domain([yExtent[0] - yPad, Math.min(0, yExtent[1] + yPad)]).nice().range([innerHeight, 0]);

    plot.append('g').attr('class', 'grid')
      .call(d3.axisLeft(y).ticks(5).tickSize(-innerWidth).tickFormat(''));
    plot.append('rect').attr('data-chart-frame', '').attr('fill', 'none')
      .attr('width', innerWidth).attr('height', innerHeight);
    plot.append('path').datum(values).attr('class', 'spectrum-line')
      .attr('d', d3.line().x(d => x(d.x)).y(d => y(d.y)));

    const expectedMHz = data.expected_tone_hz / 1e6;
    const detectedMHz = data.peak_estimated_hz / 1e6;
    if (expectedMHz >= x.domain()[0] && expectedMHz <= x.domain()[1]) {
      plot.append('line').attr('class', 'expected-line')
        .attr('x1', x(expectedMHz)).attr('x2', x(expectedMHz)).attr('y1', 0).attr('y2', innerHeight);
    }
    if (detectedMHz >= x.domain()[0] && detectedMHz <= x.domain()[1]) {
      plot.append('line').attr('class', 'detected-line')
        .attr('x1', x(detectedMHz)).attr('x2', x(detectedMHz)).attr('y1', 0).attr('y2', innerHeight);
    }

    plot.append('g').attr('class', 'axis').attr('transform', `translate(0,${innerHeight})`)
      .call(d3.axisBottom(x).ticks(width < 520 ? 4 : 7).tickFormat(d => d.toFixed(config.key === 'tone_zoom' ? 3 : 1)));
    plot.append('g').attr('class', 'axis').call(d3.axisLeft(y).ticks(5));

    svg.append('text').attr('class', 'plot-title').attr('x', margin.left).attr('y', 18).text(config.title);
    svg.append('text').attr('class', 'axis-title').attr('data-axis', 'x')
      .attr('x', margin.left + innerWidth / 2).attr('y', height - 8).attr('text-anchor', 'middle').text(config.xLabel);
    svg.append('text').attr('class', 'axis-title').attr('data-axis', 'y')
      .attr('transform', `translate(16,${margin.top + innerHeight / 2}) rotate(-90)`).attr('text-anchor', 'middle').text('幅度 (dBFS)');

    const bisect = d3.bisector(d => d.x).center;
    const guide = plot.append('line').attr('data-chart-hover-guide', '').attr('y1', 0).attr('y2', innerHeight)
      .attr('stroke', 'var(--border)').attr('visibility', 'hidden');
    const marker = plot.append('circle').attr('data-chart-hover-marker', '').attr('r', 4)
      .attr('fill', 'var(--viz-series-1)').attr('visibility', 'hidden');
    plot.append('rect').attr('data-chart-hit', '').attr('data-chart-hover-overlay', 'cross-series')
      .attr('width', innerWidth).attr('height', innerHeight).attr('fill', 'transparent')
      .on('pointermove', event => {
        const [mx] = d3.pointer(event);
        const xv = x.invert(mx);
        const index = bisect(values, xv);
        const point = values[Math.max(0, Math.min(values.length - 1, index))];
        guide.attr('x1', mx).attr('x2', mx).attr('visibility', 'visible');
        marker.attr('cx', x(point.x)).attr('cy', y(point.y)).attr('visibility', 'visible');
        const bounds = root.getBoundingClientRect();
        tooltip.hidden = false;
        tooltip.textContent = `${point.x.toFixed(6)} MHz · ${point.y.toFixed(1)} dBFS`;
        tooltip.style.left = `${event.clientX - bounds.left + 12}px`;
        tooltip.style.top = `${event.clientY - bounds.top - 28}px`;
      })
      .on('pointerleave', () => {
        guide.attr('visibility', 'hidden');
        marker.attr('visibility', 'hidden');
        tooltip.hidden = true;
      });
  }

  function redraw() { configs.forEach(draw); }
  redraw();
  new ResizeObserver(redraw).observe(root);
})();
</script>'''


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("analysis_json", type=Path)
    parser.add_argument("output_html", type=Path)
    args = parser.parse_args()
    payload = json.loads(args.analysis_json.read_text(encoding="utf-8"))
    fragment = TEMPLATE.replace("__DATA__", json.dumps(payload, ensure_ascii=False, separators=(",", ":")))
    args.output_html.parent.mkdir(parents=True, exist_ok=True)
    args.output_html.write_text(fragment, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
