"""Race-window averages of the report lines of night runs (VBlank intervals ending in [LO, HI]).

usage: cpu_summary.py <tag> ...   (the tags of night-run.sh or series.sh, under GT4_BENCH_OUT). It averages the
600-VBlank reports, so a run needs GT4_MTVU_REPORT_EVERY=600 GT4_HW_PROFILE_EVERY=600 (series.sh sets them).
"""
import os
import re
import sys
from pathlib import Path

LO, HI = 7800, 11400
ROOT = Path(os.environ.get('GT4_BENCH_OUT', Path(__file__).resolve().parents[2] / 'build' / 'evidence'))


def num(s):
    return float(s)


def summarise(name):
    t = (ROOT / f'night-{name}' / 'log.txt').read_text(encoding='utf-8', errors='replace')
    out = {}

    def avg(key, values):
        if values:
            out[key] = sum(values) / len(values)

    # [hw] cost / transport lines carry their vsync
    cost = [(int(m.group(1)), num(m.group(2)), num(m.group(3))) for m in
            re.finditer(r'cost vsync=(\d+) gs_thread_cpu_ms=([\d.]+) process_cpu_core_ms=([\d.]+)', t)]
    cost = [c for c in cost if LO <= c[0] <= HI]
    avg('gs_cpu', [c[1] for c in cost])
    avg('process_cpu', [c[2] for c in cost])
    tr = [m for m in re.finditer(r'transport vsync=(\d+) ee: (.*?) \| worker: (.*?max_depth=\d+)', t)
          if LO <= int(m.group(1)) <= HI]
    for field in ('gif_cmds', 'gif_kib', 'gif_ms', 'copy_ms', 'drain_wait_ms', 'read_fifo_ms', 'wake_pushes',
                  'vsync_wait_ms'):
        avg('ee_' + field, [num(re.search(field + r'=([\d.]+)', m.group(2)).group(1)) for m in tr])
    for field in ('gif_ms', 'idle_ms', 'wakeups'):
        avg('worker_' + field, [num(re.search(field + r'=([\d.]+)', m.group(3)).group(1)) for m in tr])
    # [core2] lines have no vsync: take the ones after the matching [core2] vblank= counter line
    blocks = []
    for m in re.finditer(r'core2\] vblank=(\d+) ', t):
        vb = int(m.group(1))
        rest = t[m.end():m.end() + 20000]
        w = re.search(r'core2\] wall ms/vblank=([\d.]+)', rest)
        x = re.search(r'core2\] exclusive ms/vblank (.*?)\n', rest)
        blocks.append((vb, w, x))
    blocks = [b for b in blocks if LO <= b[0] <= HI]
    avg('wall', [num(b[1].group(1)) for b in blocks if b[1]])
    ex = {}
    for b in blocks:
        if b[2]:
            for k, v in re.findall(r'(\w+)=([\d.]+)', b[2].group(1)):
                ex.setdefault(k, []).append(num(v))
    for k, v in ex.items():
        avg('x_' + k, v)
    # EE-side MTVU and perf lines (no vsync on them; take the race-time ones by position after vsync LO)
    start = t.find(f'vsync={LO}')
    tail = t[start:] if start >= 0 else t
    avg('ee_gs_drain', [num(v) for v in re.findall(r'EE gs_drain ms/frame=([\d.]+)', tail)])
    avg('ee_mtvu_wait_download', [num(v) for v in re.findall(r'EE wait ms/frame: vblank=[\d.]+ download=([\d.]+)', tail)])
    avg('ee_critical_path', [num(v) for v in re.findall(r'critical path=([\d.]+)', tail)])
    avg('frame_p50', [num(a) for a, b in re.findall(r'frame ms: median ([\d.]+) p95 ([\d.]+)', tail)])
    avg('frame_p95', [num(b) for a, b in re.findall(r'frame ms: median ([\d.]+) p95 ([\d.]+)', tail)])
    gw, gb, gd = [], [], []
    for m in re.finditer(r'core2\] vblank=(\d+) ', t):
        if LO <= int(m.group(1)) <= HI:
            g = re.search(r'gpu_wait=([\d.]+)', t[m.end():m.end() + 20000])
            if g: gw.append(num(g.group(1)))
            b = re.search(r'gpu_busy=([\d.]+) \([\d.]+ submits\) gpu_draw_passes=([\d.]+)', t[m.end():m.end() + 20000])
            if b:
                gb.append(num(b.group(1)))
                gd.append(num(b.group(2)))
    avg('gpu_wait', gw)
    avg('gpu_busy', gb)
    avg('gpu_draw_passes', gd)
    avg('mtvu_worker_busy', [num(v) for v in re.findall(r'worker ms/frame: vu1_program=[\d.]+ output_path12=[\d.]+ busy=([\d.]+)', tail)])
    avg('mtvu_vu1_program', [num(v) for v in re.findall(r'worker ms/frame: vu1_program=([\d.]+)', tail)])
    avg('mtvu_output_path12', [num(v) for v in re.findall(r'worker ms/frame: vu1_program=[\d.]+ output_path12=([\d.]+)', tail)])
    avg('w2ee_wakeups', [num(v) for v in re.findall(r'worker_to_ee rings=[\d.]+ waits=[\d.]+ wakeups=([\d.]+)', tail)])
    avg('w2ee_notifies', [num(v) for v in re.findall(r'worker_to_ee rings=[\d.]+ waits=[\d.]+ wakeups=[\d.]+ notifies=([\d.]+)', tail)])
    return out


names = sys.argv[1:]
rows = {n: summarise(n) for n in names}
keys = []
for r in rows.values():
    for k in r:
        if k not in keys:
            keys.append(k)
print('metric'.ljust(26) + ''.join(n[:14].rjust(15) for n in names))
for k in keys:
    print(k.ljust(26) + ''.join((f'{rows[n][k]:.3f}' if k in rows[n] else '-').rjust(15) for n in names))
