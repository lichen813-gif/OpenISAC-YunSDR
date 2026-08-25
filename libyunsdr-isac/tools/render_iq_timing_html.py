#!/usr/bin/env python3
"""Render IQ threshold timing analysis as a responsive visualization fragment."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


TEMPLATE = r'''<div id="y240-iq-timing">
  <h2>Y240 接收单音起始点</h2>
  <div class="timing-note" aria-live="polite"></div>
  <div class="timing-legend" aria-label="时域波形图例">
    <button type="button" aria-pressed="true" data-series="i"><span data-swatch="i"></span>I</button>
    <button type="button" aria-pressed="true" data-series="q"><span data-swatch="q"></span>Q</button>
  </div>
  <div class="timing-plot" data-plot="iq"></div>
  <div class="timing-plot" data-plot="magnitude"></div>
  <div class="tooltip" role="tooltip" hidden></div>
</div>
<style>
  #y240-iq-timing { width: 100%; color: var(--foreground); }
  #y240-iq-timing h2 { margin: 0 0 8px; font-weight: 500; }
  #y240-iq-timing .timing-note { margin-bottom: 8px; color: var(--foreground); }
  #y240-iq-timing .timing-legend { display: flex; flex-wrap: wrap; gap: 14px; margin: 4px 0 0 64px; }
  #y240-iq-timing .timing-legend button { appearance: none; border: 0; background: transparent; color: var(--foreground); padding: 2px 0; font: inherit; cursor: pointer; }
  #y240-iq-timing .timing-legend button[aria-pressed="false"] { opacity: 0.5; }
  #y240-iq-timing .timing-legend span { display: inline-block; width: 18px; height: 2px; margin-right: 5px; vertical-align: middle; }
  #y240-iq-timing [data-swatch="i"] { background: var(--viz-series-1); }
  #y240-iq-timing [data-swatch="q"] { background: var(--viz-series-2); }
  #y240-iq-timing .timing-plot { width: 100%; margin-top: 8px; }
  #y240-iq-timing svg { display: block; width: 100%; overflow: visible; }
  #y240-iq-timing .axis text,
  #y240-iq-timing .axis-title,
  #y240-iq-timing .plot-title,
  #y240-iq-timing .annotation { fill: var(--foreground); font-size: 12px; }
  #y240-iq-timing .axis path,
  #y240-iq-timing .axis line,
  #y240-iq-timing .grid line,
  #y240-iq-timing rect[data-chart-frame] { stroke: var(--border); }
  #y240-iq-timing .grid path { display: none; }
  #y240-iq-timing .grid line { stroke-opacity: 0.55; }
  #y240-iq-timing .series-i { fill: none; stroke: var(--viz-series-1); stroke-width: 1.5; }
  #y240-iq-timing .series-q { fill: none; stroke: var(--viz-series-2); stroke-width: 1.5; }
  #y240-iq-timing .series-mag { fill: none; stroke: var(--viz-series-1); stroke-width: 1.5; }
  #y240-iq-timing .threshold-line { stroke: var(--viz-series-2); stroke-width: 1.5; stroke-dasharray: 5 4; }
  #y240-iq-timing .start-line { stroke: var(--viz-series-3); stroke-width: 1.5; }
  #y240-iq-timing .tooltip { position: absolute; pointer-events: none; color: var(--popover-foreground); background: var(--popover); border: 1px solid var(--border); padding: 6px 8px; }
</style>
<script src="https://cdn.jsdelivr.net/npm/d3@7.9.0/dist/d3.min.js"></script>
<script>
(() => {
  const root = document.getElementById('y240-iq-timing');
  const analysis = __DATA__;
  const values = analysis.samples.map(d => ({ index:d[0], timeUs:d[1], i:d[2], q:d[3], mag:d[4] }));
  const enabled = { i:true, q:true };
  const tooltip = root.querySelector('.tooltip');
  root.querySelector('.timing-note').textContent = `首个 |IQ| > ${analysis.threshold.toFixed(0)} 的样点是 ${analysis.first_crossing_index}，对应 ${analysis.first_crossing_time_us.toFixed(6)} μs；与程序打印一致。`;

  root.querySelectorAll('.timing-legend button').forEach(button => {
    button.addEventListener('click', () => {
      const key = button.dataset.series;
      enabled[key] = !enabled[key];
      button.setAttribute('aria-pressed', String(enabled[key]));
      redraw();
    });
  });

  function basePlot(node, title, yDomain, yLabel) {
    const width = Math.max(320, Math.floor(node.getBoundingClientRect().width));
    const height = width < 520 ? 270 : 300;
    const margin = { top:32, right:20, bottom:54, left:72 };
    const innerWidth = width - margin.left - margin.right;
    const innerHeight = height - margin.top - margin.bottom;
    d3.select(node).selectAll('*').remove();
    const svg = d3.select(node).append('svg').attr('viewBox', `0 0 ${width} ${height}`).attr('role','img').attr('aria-label', title);
    svg.append('title').text(title);
    svg.append('desc').text(`阈值 ${analysis.threshold}，检测起始样点 ${analysis.first_crossing_index}。`);
    const plot = svg.append('g').attr('transform', `translate(${margin.left},${margin.top})`);
    const xExtent = d3.extent(values, d => d.index);
    const xPad = Math.max((xExtent[1]-xExtent[0])*0.005, 1);
    const yPad = Math.max((yDomain[1]-yDomain[0])*0.08, 10);
    const x = d3.scaleLinear().domain([xExtent[0]-xPad,xExtent[1]+xPad]).range([0,innerWidth]);
    const y = d3.scaleLinear().domain([yDomain[0]-yPad,yDomain[1]+yPad]).nice().range([innerHeight,0]);
    plot.append('g').attr('class','grid').call(d3.axisLeft(y).ticks(5).tickSize(-innerWidth).tickFormat(''));
    plot.append('rect').attr('data-chart-frame','').attr('fill','none').attr('width',innerWidth).attr('height',innerHeight);
    plot.append('line').attr('class','start-line').attr('x1',x(analysis.first_crossing_index)).attr('x2',x(analysis.first_crossing_index)).attr('y1',0).attr('y2',innerHeight);
    plot.append('text').attr('class','annotation').attr('x',Math.min(innerWidth-4,x(analysis.first_crossing_index)+5)).attr('y',14).text(`起始 ${analysis.first_crossing_index}`);
    plot.append('g').attr('class','axis').attr('transform',`translate(0,${innerHeight})`).call(d3.axisBottom(x).ticks(width<520?4:7));
    plot.append('g').attr('class','axis').call(d3.axisLeft(y).ticks(5));
    svg.append('text').attr('class','plot-title').attr('x',margin.left).attr('y',18).text(title);
    svg.append('text').attr('class','axis-title').attr('data-axis','x').attr('x',margin.left+innerWidth/2).attr('y',height-8).attr('text-anchor','middle').text('样点索引');
    svg.append('text').attr('class','axis-title').attr('data-axis','y').attr('transform',`translate(16,${margin.top+innerHeight/2}) rotate(-90)`).attr('text-anchor','middle').text(yLabel);
    return {svg,plot,x,y,innerWidth,innerHeight};
  }

  function addHover(base, series) {
    const bisect = d3.bisector(d => d.index).center;
    const guide = base.plot.append('line').attr('data-chart-hover-guide','').attr('y1',0).attr('y2',base.innerHeight).attr('stroke','var(--border)').attr('visibility','hidden');
    const markers = series.map(s => base.plot.append('circle').attr('data-chart-hover-marker','').attr('r',4).attr('fill',s.color).attr('visibility','hidden'));
    base.plot.append('rect').attr('data-chart-hit','').attr('data-chart-hover-overlay','cross-series').attr('width',base.innerWidth).attr('height',base.innerHeight).attr('fill','transparent')
      .on('pointermove', event => {
        const [mx] = d3.pointer(event);
        const point = values[Math.max(0,Math.min(values.length-1,bisect(values,base.x.invert(mx))))];
        guide.attr('x1',mx).attr('x2',mx).attr('visibility','visible');
        const rows=[];
        series.forEach((s,index) => {
          const visible = s.visible();
          markers[index].attr('cx',base.x(point.index)).attr('cy',base.y(point[s.key])).attr('visibility',visible?'visible':'hidden');
          if (visible) rows.push(`${s.label} ${point[s.key].toFixed(1)}`);
        });
        const bounds=root.getBoundingClientRect();
        tooltip.hidden=false;
        tooltip.textContent=`样点 ${point.index} · ${rows.join(' · ')}`;
        tooltip.style.left=`${event.clientX-bounds.left+12}px`;
        tooltip.style.top=`${event.clientY-bounds.top-28}px`;
      }).on('pointerleave',()=>{guide.attr('visibility','hidden');markers.forEach(m=>m.attr('visibility','hidden'));tooltip.hidden=true;});
  }

  function drawIQ() {
    const node=root.querySelector('[data-plot="iq"]');
    const all=[...values.map(d=>d.i),...values.map(d=>d.q)];
    const base=basePlot(node,'I/Q 时域波形',d3.extent(all),'ADC 码值');
    if(enabled.i) base.plot.append('path').datum(values).attr('class','series-i').attr('d',d3.line().x(d=>base.x(d.index)).y(d=>base.y(d.i)));
    if(enabled.q) base.plot.append('path').datum(values).attr('class','series-q').attr('d',d3.line().x(d=>base.x(d.index)).y(d=>base.y(d.q)));
    addHover(base,[{key:'i',label:'I',color:'var(--viz-series-1)',visible:()=>enabled.i},{key:'q',label:'Q',color:'var(--viz-series-2)',visible:()=>enabled.q}]);
  }

  function drawMagnitude() {
    const node=root.querySelector('[data-plot="magnitude"]');
    const extent=d3.extent([...values.map(d=>d.mag),analysis.threshold]);
    const base=basePlot(node,'包络幅度与同步阈值',extent,'|IQ| (ADC 码值)');
    base.plot.append('path').datum(values).attr('class','series-mag').attr('d',d3.line().x(d=>base.x(d.index)).y(d=>base.y(d.mag)));
    base.plot.append('line').attr('class','threshold-line').attr('x1',0).attr('x2',base.innerWidth).attr('y1',base.y(analysis.threshold)).attr('y2',base.y(analysis.threshold));
    base.plot.append('text').attr('class','annotation').attr('x',base.innerWidth-4).attr('y',base.y(analysis.threshold)-5).attr('text-anchor','end').text(`阈值 ${analysis.threshold.toFixed(0)}`);
    addHover(base,[{key:'mag',label:'|IQ|',color:'var(--viz-series-1)',visible:()=>true}]);
  }

  function redraw(){drawIQ();drawMagnitude();}
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
