"""Hermes bytecode v84 function-level patcher (research tool; not used by the shipped libraries).
Operates on a bytecode file you extract from your own device. Never publish patched or original bundles.

Reads a .hbc with hermes-dec (third_party, parse only) and writes a new file where selected functions get new
bytecode. New bodies + a large function header are appended after the debug info; the function's small header is
marked `overflowed` and points at the large header ((infoOffset << 16) | offset). Everything else stays byte-identical.
Header fileLength and the SHA-1 footer are recomputed.

Text assembly syntax (one instruction per line, '#' comments):
    L12:                       label (any name ending in ':')
    GetById r0, r1, 3, "props" string operands may be given as "literal" (must exist in the string table) or as s<ID>
    JmpTrue @L12, r0           jump operands are @label; the short/long form is chosen automatically
    CreateClosure r0, r1, f7841   function ids as f<ID>
    Ret r0
Registers: r<N>; plain integers for UInt/Imm; doubles as 1.5.
"""
import hashlib
import struct
import sys
from ctypes import sizeof
from pathlib import Path

# hermes-dec (https://github.com/P1sec/hermes-dec, parse only) must be importable: pip install it into a venv, or
# point HERMES_DEC_PATH at its site-packages directory.
import os  # noqa: E402
if os.environ.get("HERMES_DEC_PATH"):
    sys.path.insert(0, os.environ["HERMES_DEC_PATH"])
from hermes_dec.parsers.hbc_file_parser import HBCReader  # noqa: E402
from hermes_dec.parsers.hbc_bytecode_parser import parse_hbc_bytecode  # noqa: E402

JUMP_PAIRS = {}  # short name -> long name


class HBC:
    def __init__(self, path):
        self.path = Path(path)
        self.data = bytearray(self.path.read_bytes())
        self.r = HBCReader()
        with open(path, "rb") as f:
            self.r.read_whole_file(f)
            self.r.file_buffer = open(path, "rb")
        self.pm = self.r.parser_module
        self.ops = {i.name: i for i in self.pm._instructions}
        for n in self.ops:
            if n + "Long" in self.ops and any(o.operand_type.name == "Addr8" for o in self.ops[n].operands):
                JUMP_PAIRS[n] = n + "Long"
        hdr = self.r.get_header_reader(self.r.header.version)
        self.small_base = (sizeof(hdr) + 31) & ~31
        self.str_index = {}
        for i, s in enumerate(self.r.strings):
            self.str_index.setdefault(s, i)
        self.patches = {}

    # ---------- inspection ----------
    def fname(self, fid):
        return self.r.strings[self.r.function_headers[fid].functionName]

    def find(self, name):
        return [i for i, h in enumerate(self.r.function_headers) if self.r.strings[h.functionName] == name]

    def disasm(self, fid):
        """Return assembly text (labels for every jump target) that re-assembles to equivalent code."""
        h = self.r.function_headers[fid]
        insts = list(parse_hbc_bytecode(h, self.r))
        targets = set()
        for pi in insts:
            for k, o in enumerate(pi.inst.operands):
                if o.operand_type.name in ("Addr8", "Addr32"):
                    targets.add(pi.original_pos + getattr(pi, "arg%d" % (k + 1)))
            if pi.inst.name in ("SwitchImm", "UIntSwitchImm", "StringSwitchImm"):
                raise NotImplementedError("switch in function %d" % fid)
        for e in self.r.function_id_to_exc_handlers.get(fid, []):
            targets.update((e.start, e.end, e.target))
        out = []
        for pi in insts:
            if pi.original_pos in targets:
                out.append("L%d:" % pi.original_pos)
            args = []
            for k, o in enumerate(pi.inst.operands):
                v = getattr(pi, "arg%d" % (k + 1))
                tn = o.operand_type.name
                m = o.operand_meaning.name if o.operand_meaning else None
                if tn in ("Addr8", "Addr32"):
                    args.append("@L%d" % (pi.original_pos + v))
                elif tn.startswith("Reg"):
                    args.append("r%d" % v)
                elif m == "string_id":
                    args.append("s%d" % v)
                elif m == "function_id":
                    args.append("f%d" % v)
                else:
                    args.append(repr(v))
            cm = ""
            for k, o in enumerate(pi.inst.operands):
                if o.operand_meaning and o.operand_meaning.name == "string_id":
                    cm += "  # %r" % self.r.strings[getattr(pi, "arg%d" % (k + 1))]
                if o.operand_meaning and o.operand_meaning.name == "function_id":
                    cm += "  # fn %s" % self.fname(getattr(pi, "arg%d" % (k + 1)))
            out.append("    %s %s%s" % (pi.inst.name, ", ".join(args), cm))
        end = h.bytecodeSizeInBytes
        if end in targets:
            out.append("L%d:" % end)
        exc = ["L%d L%d L%d" % (e.start, e.end, e.target) for e in self.r.function_id_to_exc_handlers.get(fid, [])]
        return "\n".join(out) + "\n", exc

    # ---------- assembly ----------
    def _parse(self, text):
        items = []
        for line in text.splitlines():
            line = line.split("#", 1)[0].strip() if not line.strip().startswith('"') else line.strip()
            if not line:
                continue
            if line.endswith(":") and " " not in line:
                items.append(("label", line[:-1]))
                continue
            sp = line.split(None, 1)
            op = sp[0]
            args = []
            if len(sp) > 1:
                cur, q = "", False
                for ch in sp[1]:
                    if ch == '"':
                        q = not q
                    if ch == "," and not q:
                        args.append(cur.strip()); cur = ""
                    else:
                        cur += ch
                if cur.strip():
                    args.append(cur.strip())
            if op not in self.ops:
                raise ValueError("unknown op %s" % op)
            items.append(("inst", op, args))
        return items

    def _arg(self, a, o):
        tn = o.operand_type.name
        if tn.startswith("Reg"):
            assert a[0] == "r", a
            return int(a[1:])
        if a.startswith('"'):
            s = a[1:-1]
            if s not in self.str_index:
                raise KeyError("string not in table: %r" % s)
            return self.str_index[s]
        if a[0] in "sf" and a[1:].isdigit():
            return int(a[1:])
        if tn == "Double":
            return float(a)
        return int(a, 0)

    def assemble(self, text, exc=()):
        items = self._parse(text)
        longs = set()  # indices of items forced to long jump form
        while True:
            pos, labels, sizes = 0, {}, []
            for idx, it in enumerate(items):
                if it[0] == "label":
                    labels[it[1]] = pos
                    continue
                op = it[1]
                if idx in longs:
                    op = JUMP_PAIRS[op]
                pos += self.ops[op].binary_size
            code = bytearray(); grow = False
            for idx, it in enumerate(items):
                if it[0] == "label":
                    continue
                op = it[1]
                if idx in longs:
                    op = JUMP_PAIRS[op]
                ins = self.ops[op]
                here = len(code)
                vals = []
                for a, o in zip(it[2], ins.operands):
                    if a.startswith("@"):
                        vals.append(labels[a[1:]] - here)
                    else:
                        vals.append(self._arg(a, o))
                if len(vals) != len(ins.operands):
                    raise ValueError("operand count %s %s" % (op, it[2]))
                if ins.name in JUMP_PAIRS:
                    for v, o in zip(vals, ins.operands):
                        if o.operand_type.name == "Addr8" and not -128 <= v <= 127:
                            longs.add(idx); grow = True
                st = ins.structure()
                for k, v in enumerate(vals):
                    try:
                        setattr(st, "arg%d" % (k + 1), v)
                    except TypeError:
                        setattr(st, "arg%d" % (k + 1), int(v))
                code += bytes([ins.opcode]) + bytes(st)
            if not grow:
                break
        exc_t = []
        for e in exc:
            a, b, c = e.split()
            exc_t.append((labels[a], labels[b], labels[c]))
        return bytes(code), exc_t

    # ---------- patching ----------
    def replace(self, fid, code, exc=(), frame_size=None, read_cache=None, write_cache=None):
        self.patches[fid] = dict(code=code, exc=list(exc), frame=frame_size, rc=read_cache, wc=write_cache)

    def write(self, out):
        d = bytearray(self.data)
        flen = struct.unpack_from("<I", d, 32)[0]
        assert flen == len(d), (flen, len(d))
        body = d[:-20]
        for fid, p in sorted(self.patches.items()):
            h = self.r.function_headers[fid]
            while len(body) % 4:
                body.append(0)
            code_off = len(body)
            body += p["code"]
            while len(body) % 4:
                body.append(0)
            info_off = len(body)
            if p["exc"]:
                body += struct.pack("<I", len(p["exc"]))
                for s, e, t in p["exc"]:
                    body += struct.pack("<III", s, e, t)
            while len(body) % 4:
                body.append(0)
            large_off = len(body)
            flags = (h.prohibitInvoke | (h.strictMode << 2) | ((1 if p["exc"] else 0) << 3) | (0 << 4)
                     | (1 << 5) | (h.kind << 6))
            body += struct.pack("<IIIIIII", code_off, h.paramCount, len(p["code"]), h.functionName, info_off,
                                p["frame"] if p["frame"] is not None else h.frameSize, h.environmentSize)
            body += struct.pack("<BBB", p["rc"] if p["rc"] is not None else h.highestReadCacheIndex,
                                p["wc"] if p["wc"] is not None else h.highestWriteCacheIndex, flags)
            body.append(0)
            # small header: overflowed, offset = low 16 bits, infoOffset = high bits
            so = self.small_base + 16 * fid
            w0, w1, w2, w3 = struct.unpack_from("<IIII", body, so)
            w0 = (w0 & ~0x1FFFFFF) | (large_off & 0xFFFF)
            w2 = (w2 & ~0x1FFFFFF) | (large_off >> 16)
            w3 = w3 | (1 << 29)  # flags byte is the top byte of word 3: bit 5 = overflowed
            struct.pack_into("<IIII", body, so, w0, w1, w2, w3)
        total = len(body) + 20
        struct.pack_into("<I", body, 32, total)
        body += hashlib.sha1(bytes(body)).digest()
        Path(out).write_bytes(bytes(body))
        return total


if __name__ == "__main__":
    h = HBC(sys.argv[1])
    cmd = sys.argv[2]
    if cmd == "dis":
        for a in sys.argv[3:]:
            fids = [int(a)] if a.isdigit() else h.find(a)
            for fid in fids:
                fh = h.r.function_headers[fid]
                t, exc = h.disasm(fid)
                print("; fn %d %s params=%d frame=%d env=%d rc=%d wc=%d size=%d exc=%s" % (
                    fid, h.fname(fid), fh.paramCount, fh.frameSize, fh.environmentSize, fh.highestReadCacheIndex,
                    fh.highestWriteCacheIndex, fh.bytecodeSizeInBytes, exc))
                print(t)
    elif cmd == "roundtrip":  # re-assemble every listed function (or N first) and compare bytes when no jump changed
        n_ok = n_diff = n_skip = 0
        for fid in range(len(h.r.function_headers)):
            try:
                t, exc = h.disasm(fid)
            except NotImplementedError:
                n_skip += 1; continue
            code, _ = h.assemble(t, exc)
            fh = h.r.function_headers[fid]
            orig = bytes(h.data[fh.offset:fh.offset + fh.bytecodeSizeInBytes])
            if code == orig:
                n_ok += 1
            else:
                n_diff += 1
                if n_diff < 5:
                    print("diff fn", fid, h.fname(fid), len(code), len(orig))
        print("identical", n_ok, "different", n_diff, "skipped(switch)", n_skip)
