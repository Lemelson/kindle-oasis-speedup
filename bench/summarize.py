#!/usr/bin/env python3
"""Summarise kpp-bench turns.jsonl: median/mean/sd per phase (fwd1 = first visit, fwd2 = forward revisit, back).
   python3 summarize.py RUN_DIR [RUN_DIR2]   (two dirs: also prints B vs A median deltas)
   --skip N  drop the first N turns of every phase (default 0)  --json  machine-readable"""
import json, statistics as st, sys, os

def load(d):
    rows = [json.loads(l) for l in open(os.path.join(d, 'turns.jsonl')) if l.strip()]
    for r in rows:
        rep, ph, i = r['label'].split(':'); r['rep'], r['phase'], r['i'] = rep, ph, int(i)
    return rows

METRICS = [('settle_ms', 'settle ms'), ('main_ticks', 'main tk'), ('js_ticks', 'JS tk'), ('proc_ticks', 'proc tk'),
           ('x_ticks', 'X tk'), ('sys_busy_ticks', 'sys tk'), ('main_ns', 'main ms(ns)'), ('js_ns', 'JS ms(ns)'), ('last_irq_ms', 'irq-last ms')]

def stats(v):
    v = [x for x in v if x is not None]
    if not v: return None
    return dict(n=len(v), med=st.median(v), mean=st.mean(v), sd=st.stdev(v) if len(v) > 1 else 0.0, min=min(v), max=max(v))

def summarise(rows, skip):
    out = {}
    for ph in ['open', 'fwd1', 'fwd2', 'back']:
        sel = [r for r in rows if r['phase'] == ph and r['i'] > (skip if ph != 'open' else -1)]
        if not sel: continue
        out[ph] = {}
        for k, _ in METRICS:
            vals = []
            for r in sel:
                x = r.get(k)
                if x is None or x == -1: continue
                if k in ('main_ns', 'js_ns'): x = x / 1e6
                vals.append(x)
            s = stats(vals)
            if s: out[ph][k] = s
        out[ph]['timeouts'] = sum(r['timeout'] for r in sel)
        out[ph]['noop'] = sum(1 for r in sel if r['settle_ms'] < 0)
        out[ph]['reps'] = sorted(set(r['rep'] for r in sel))
        out[ph]['rep_med_settle'] = {rp: st.median([r['settle_ms'] for r in sel if r['rep'] == rp and r['settle_ms'] >= 0] or [-1]) for rp in out[ph]['reps']}
        out[ph]['rep_med_main'] = {rp: st.median([r['main_ticks'] for r in sel if r['rep'] == rp]) for rp in out[ph]['reps']}
    return out

def show(d, s):
    print(f"\n== {d}")
    names = {'open': 'open Library', 'fwd1': 'forward, first visit', 'fwd2': 'forward, revisit', 'back': 'back (revisit)'}
    for ph, m in s.items():
        print(f"-- {names[ph]}  (reps {','.join(m['reps'])}; timeouts {m['timeouts']}, no-activity {m['noop']})")
        for k, lab in METRICS:
            if k in m:
                x = m[k]; print(f"   {lab:12s} n={x['n']:3d}  med {x['med']:8.1f}  mean {x['mean']:8.1f}  sd {x['sd']:7.1f}  [{x['min']:.0f}..{x['max']:.0f}]")
        print('   per-rep median settle ms: ' + '  '.join(f'{k}={v:.0f}' for k, v in m['rep_med_settle'].items()) + '   main tk: ' + '  '.join(f'{k}={v:.0f}' for k, v in m['rep_med_main'].items()))

def main():
    a = [x for x in sys.argv[1:] if not x.startswith('--')]
    skip = 0
    if '--skip' in sys.argv: skip = int(sys.argv[sys.argv.index('--skip') + 1]); a = [x for x in a if x != str(skip)]
    res = [(d, summarise(load(d), skip)) for d in a]
    if '--json' in sys.argv: print(json.dumps({d: s for d, s in res}, indent=1)); return
    for d, s in res: show(d, s)
    if len(res) == 2:
        (da, sa), (db, sb) = res
        print(f"\n== B vs A medians  (A={os.path.basename(da)}  B={os.path.basename(db)})")
        for ph in sa:
            if ph not in sb: continue
            parts = []
            for k in ('settle_ms', 'main_ticks', 'js_ticks', 'proc_ticks', 'x_ticks'):
                if k in sa[ph] and k in sb[ph] and sa[ph][k]['med']:
                    A, B = sa[ph][k]['med'], sb[ph][k]['med']; parts.append(f"{k} {A:.0f}->{B:.0f} ({100*(B-A)/A:+.0f}%)")
            print(f"   {ph:5s} " + '  '.join(parts))

main()
