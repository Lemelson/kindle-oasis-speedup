#!/usr/bin/env python3
"""Compare kpp-bench runs: one row per run dir, medians per phase (settle ms, main/JS/proc ticks).
   usage: compare.py DIR [DIR...]   (dirs = runs/kpp-bench-<mode>-<stamp>)"""
import json, os, statistics, sys

PH = [("open", "open"), ("fwd1", "fwd new"), ("fwd2", "fwd again"), ("back", "back")]


def load(d):
    rows = [json.loads(l) for l in open(os.path.join(d, "turns.jsonl")) if l.strip()]
    out = {}
    for key, _ in PH:
        r = [x for x in rows if x["label"].split(":")[1] == key and x["settle_ms"] > 0]
        if not r:
            continue
        out[key] = dict(n=len(r), settle=statistics.median(x["settle_ms"] for x in r),
                        main=statistics.median(x["main_ticks"] for x in r),
                        js=statistics.median(x["js_ticks"] for x in r),
                        proc=statistics.median(x["proc_ticks"] for x in r))
    return out


def checks(d):
    """page-change check (pages.txt, written by kpp-bench.sh since 19.11) and low-tick turns per phase."""
    rows = [json.loads(l) for l in open(os.path.join(d, "turns.jsonl")) if l.strip()]
    out = []
    pp = os.path.join(d, "pages.txt")
    if os.path.exists(pp):
        pg = [l.split() for l in open(pp) if l.strip()]
        same = [pg[i][0] for i in range(1, len(pg)) if pg[i][1] == pg[i - 1][1]]
        out.append(f"page check: {len(pg) - 1} turns, {len(same)} without a page change" + (f" ({' '.join(same[:6])})" if same else ""))
    else:
        out.append("page check: no pages.txt")
    aft = []
    for key, name in PH[1:]:
        r = [x for x in rows if x["label"].split(":")[1] == key and x["settle_ms"] > 0 and "inject_ms" in x]
        if r: aft.append(f"{statistics.median(x['settle_ms'] - x['inject_ms'] for x in r):.0f}")
    if aft: out.append("settle after the key was delivered (settle - inject), fwd new/again/back: " + "/".join(aft) + " ms")
    for key, name in PH[1:]:
        r = [x for x in rows if x["label"].split(":")[1] == key and x["settle_ms"] > 0]
        if not r: continue
        med = statistics.median(x["main_ticks"] for x in r)
        hi = [x for x in r if x["main_ticks"] >= 0.6 * med]
        out.append(f"{name}: {len(r) - len(hi)} low-tick turns (< 60 % of median {med:.0f} main ticks)" +
                   (f", median of the other {len(hi)}: {statistics.median(x['settle_ms'] for x in hi):.0f} ms" if len(hi) != len(r) else ""))
    return "; ".join(out)


def main():
    print(f"{'run':38} " + " ".join(f"{t:>22}" for _, t in PH))
    print(f"{'':38} " + " ".join(f"{'ms  main js proc':>22}" for _ in PH))
    for d in sys.argv[1:]:
        r = load(d)
        cells = []
        for k, _ in PH:
            v = r.get(k)
            cells.append(f"{v['settle']:6.0f} {v['main']:4.0f} {v['js']:3.0f} {v['proc']:4.0f} n{v['n']:<3}" if v else f"{'-':>22}")
        print(f"{os.path.basename(d.rstrip('/'))[10:]:38} " + " ".join(f"{c:>22}" for c in cells))
        print("    " + checks(d))


if __name__ == "__main__":
    main()
