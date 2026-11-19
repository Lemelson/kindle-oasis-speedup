#!/usr/bin/env python3
"""Analyze libkppprof samples, restricted to page-turn windows from a kpp-bench run.
   usage: analyze.py PROFDIR BENCHDIR [SYMDIR]
     PROFDIR  has <pid>.bin/.maps/.threads (copied from /tmp/kppprof)
     BENCHDIR has turns.jsonl (t0_ms, settle_ms in CLOCK_MONOTONIC, same clock as the profiler)
     SYMDIR   has <lib-basename>.sym files (nm -D --defined-only output) for symbolization"""
import bisect, collections, glob, json, os, struct, sys

prof, bench = sys.argv[1], sys.argv[2]
symdir = sys.argv[3] if len(sys.argv) > 3 else prof

binf = max(glob.glob(os.path.join(prof, "*.bin")), key=os.path.getsize)
pid = os.path.basename(binf)[:-4]
raw = open(binf, "rb").read()
S = [struct.unpack_from("<IIII", raw, i) for i in range(0, len(raw) - 15, 16)]

maps = []
for l in open(os.path.join(prof, pid + ".maps")):
    p = l.split()
    a, b = (int(x, 16) for x in p[0].split("-"))
    maps.append((a, b, int(p[2], 16), p[5] if len(p) > 5 else "[anon]", "x" in p[1]))
maps.sort()
starts = [m[0] for m in maps]
names = {}
for l in open(os.path.join(prof, pid + ".threads")):
    t, *c = l.split(); names[int(t)] = " ".join(c)

_sym = {}
def syms(lib):
    if lib not in _sym:
        f = os.path.join(symdir, lib + ".sym"); s = []
        if os.path.exists(f):
            for l in open(f):
                p = l.split()
                if len(p) >= 3 and p[1] in "TtWwiI":
                    s.append((int(p[0], 16), p[2]))
            s.sort()
        _sym[lib] = (s, [x[0] for x in s])
    return _sym[lib]

def where(addr):
    i = bisect.bisect_right(starts, addr) - 1
    if i < 0 or not (maps[i][0] <= addr < maps[i][1]):
        return "?", "?"
    a, b, off, path, x = maps[i]
    lib = os.path.basename(path)
    # file offset -> vaddr: for these ARM libs the text segment is mapped at offset 0 with vaddr == offset
    o = addr - a + off
    s, k = syms(lib)
    j = bisect.bisect_right(k, o) - 1
    fn = s[j][1] if j >= 0 and o - s[j][0] < 0x40000 else "0x%x" % o
    if len(fn) > 60: fn = fn[:57] + "..."
    return lib, fn

turns = [json.loads(l) for l in open(os.path.join(bench, "turns.jsonl")) if l.strip()]
win = []
for t in turns:
    ph = t["label"].split(":")[1]
    if ph == "open" or t["settle_ms"] <= 0: continue
    a = (t["t0_ms"] * 1000) % 4294967296; b = a + (t["settle_ms"] + 30) * 1000
    win.append((a, b, ph))
win.sort()
wst = [w[0] for w in win]

def phase(tus):
    i = bisect.bisect_right(wst, tus) - 1
    return win[i][2] if i >= 0 and tus < win[i][1] else None

sel = [(s, phase(s[0])) for s in S]
sel = [(s, p) for s, p in sel if p]
nturn = len(win)
print(f"samples total {len(S)}, in {nturn} turn windows: {len(sel)}  (~{len(sel)*10/nturn:.0f} ms CPU per turn at 10 ms/sample)")

thr = collections.Counter(names.get(s[1], str(s[1])) for s, _ in sel)
print("\nper thread (ms CPU per turn):")
for k, v in thr.most_common(12): print(f"  {v*10/nturn:7.1f}  {k}")

def top(filter_thread, n=25):
    libc = collections.Counter(); fn = collections.Counter(); caller = collections.Counter()
    for s, _ in sel:
        if names.get(s[1], "") not in filter_thread: continue
        lib, f = where(s[2]); libc[lib] += 1; fn[(lib, f)] += 1
        if lib.startswith(("libc-", "libpthread", "libm-")) or f.startswith(("mem", "__")):
            cl, cf = where(s[3]); caller[(f, cl, cf)] += 1
    tot = sum(libc.values()) or 1
    print(f"\n== threads {filter_thread}: {tot} samples = {tot*10/nturn:.0f} ms/turn")
    print("  by library:")
    for k, v in libc.most_common(12): print(f"    {100*v/tot:5.1f}%  {v*10/nturn:6.1f} ms  {k}")
    print("  by function:")
    for (l, f), v in fn.most_common(n): print(f"    {100*v/tot:5.1f}%  {v*10/nturn:6.1f} ms  {l}:{f}")
    print("  libc/libm leaf -> caller (from LR):")
    for (f, cl, cf), v in caller.most_common(12): print(f"    {100*v/tot:5.1f}%  {f}  <-  {cl}:{cf}")

top({"KPPMainApp"})
top({"JSQueue"})
top({"NativeQueue", "KUIContext"}, 12)
others = set(thr) - {"KPPMainApp", "JSQueue", "NativeQueue", "KUIContext"}
if others: top(others, 15)
