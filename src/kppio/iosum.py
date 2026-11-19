#!/usr/bin/env python3
# iosum.py kppio.log [t0_mono_ms t1_mono_ms]: bursts of file activity, most blocked time first (optionally only those
# overlapping a window on the CLOCK_MONOTONIC axis, e.g. rdprobe2's t0mono_us/1000 .. +2000), and totals per class.
import re, sys
bursts, cur = [], None
for l in open(sys.argv[1], errors='replace'):
    m = re.match(r'B t=([\d.]+) dur_ms=(\d+) blocked_ms=([\d.]+) calls=(\d+)', l)
    if m: cur = {'t': float(m[1]), 'dur': int(m[2]), 'blk': float(m[3]), 'calls': int(m[4]), 'rows': []}; bursts.append(cur); continue
    if l.startswith('  ') and cur: cur['rows'].append(l.rstrip()); continue
    if l.startswith(('kppio:', 'TOTAL')): print(l.rstrip())
if len(sys.argv) > 3:
    a, b = float(sys.argv[2]), float(sys.argv[3]); bursts = [x for x in bursts if x['t'] <= b and x['t'] + x['dur'] >= a]
tot = {}
for x in bursts:
    for r in x['rows']:
        m = re.match(r'\s+(\w+)\s+open=.*blocked_ms=([\d.]+) main_ms=([\d.]+)', r)
        if m: t = tot.setdefault(m[1], [0.0, 0.0, 0]); t[0] += float(m[2]); t[1] += float(m[3]); t[2] += 1
print(f'{len(bursts)} bursts, blocked {sum(x["blk"] for x in bursts):.1f} ms in total')
for k, v in sorted(tot.items(), key=lambda kv: -kv[1][0]): print(f'  class {k:9s} blocked {v[0]:8.1f} ms (main thread {v[1]:8.1f} ms) in {v[2]} bursts')
for x in sorted(bursts, key=lambda x: -x['blk'])[:12]:
    print(f'B t={x["t"]:.3f} dur={x["dur"]} ms blocked={x["blk"]} ms calls={x["calls"]}'); print('\n'.join(x['rows']))
