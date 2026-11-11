#!/usr/bin/env python3
"""test_if2_resolve.py -- tools/if2_resolve.py against moved and changed copies of a US EBOOT.

Needs your decrypted US EBOOT (the one analysis/inputs.json names, with the lift's analysis/functions.json and
analysis/EBOOT.imports.json); without them it says so and passes. Nothing is written outside --work (a scratch
folder of your choice; default: a temporary one).

  1. the EBOOT itself: every item at its known value (tools/if2_items.py)
  2. SHIFTED: a copy with padding inserted into the code at five function boundaries (0x80 .. 0x4000 bytes, so
     every function after each point moves) and the data segment moved up by 0x20000 (the TOC with it); every
     branch, descriptor, pointer and lis/addi pair into code or data rewritten to the new addresses. Every item
     must be found at its moved address.
  3. CHANGED: a copy where, for every item, two of its pattern's windows (not the anchor's own) get one
     instruction changed (another destination register): a recompile that changed a few instructions. Every item
     must still be found at its address.
  4. UNIQUE: no window of any pattern matches anywhere else in the EBOOT, and no pattern matches twice.

    py tests/test_if2_resolve.py [--eboot E --functions F --imports I] [--work DIR] [--keep]
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import struct
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import if2_items  # noqa: E402
import if2_resolve as R  # noqa: E402

DATA_SHIFT = 0x20000
FUNCS_PATH = [""]
INSERTS = [(0.10, 0x80), (0.30, 0x800), (0.50, 0x2000), (0.70, 0x1000), (0.90, 0x4000)]


def known_values(build):
    out = {}
    for it in if2_items.ITEMS:
        v = it.get(build)
        if v is None and it["kind"] == "rel":
            b = if2_items.BY_NAME[it["base"]].get(build)
            v = None if b is None else (b + it["off"]) & 0xFFFFFFFF
        out[it["name"]] = v
    return out


def compare(res, want, label):
    bad = []
    for it in if2_items.ITEMS:
        n = it["name"]
        w = want.get(n)
        r = res[n]
        if w is None:
            continue
        if it["gates"] == ["menu_trace"] and r["status"] == "missing":
            continue
        if r["status"] != "found" or r["value"] != w:
            bad.append("%s: %s %s, want %s" % (n, r["status"], r["value"] if r["value"] is None else
                                               "0x%08X" % (r["value"] & 0xFFFFFFFF), "%#x" % w if w >= 0 else w))
    n = sum(1 for v in want.values() if v is not None)
    print("[%s] %d of %d items at the expected value%s" % (label, n - len(bad), n, "" if not bad else ":"))
    for b in bad:
        print("    " + b)
    return not bad


# ------------------------------------------------------------------------------------------- the moved copy
def shifted_copy(img: R.Image, funcs: dict, imports: dict, out_path: str):
    d = bytearray(img.d)
    phoff = struct.unpack_from(">Q", d, 0x20)[0]
    phentsz, phnum = struct.unpack_from(">HH", d, 0x36)
    phs = []
    for i in range(phnum):
        phs.append(list(struct.unpack_from(">IIQQQQQQ", d, phoff + i * phentsz)))
    code_i = next(i for i, p in enumerate(phs) if p[0] == 1 and p[1] & 1)
    data_i = next(i for i, p in enumerate(phs) if p[0] == 1 and not p[1] & 1)
    cva, coff, cfs = phs[code_i][3], phs[code_i][2], phs[code_i][5]
    dva, doff, dfs, dms = phs[data_i][3], phs[data_i][2], phs[data_i][5], phs[data_i][6]
    starts = sorted(f for f in funcs if cva <= f < cva + cfs)
    points = []
    for frac, size in INSERTS:
        points.append((starts[int(len(starts) * frac)], size))
    points.sort()

    def cmap(a):                      # an address in the code segment (code or rodata), moved
        s = 0
        for p, size in points:
            if a >= p:
                s += size
        return a + s

    def amap(a):
        if cva <= a < cva + cfs:
            return cmap(a)
        if dva <= a < dva + dms:
            return a + DATA_SHIFT
        return a

    code = bytearray(d[coff:coff + cfs])
    data = bytearray(d[doff:doff + dfs])
    nwords = cfs // 4
    words = list(struct.unpack(">%dI" % nwords, code[:nwords * 4]))
    ce = os.path.join(os.path.dirname(os.path.abspath(FUNCS_PATH[0])), "code_end.txt")
    code_end = int(open(ce).read().split()[-1], 16) if os.path.exists(ce) else max(funcs[f] for f in starts)
    new = list(words)
    # branches (relative), and lis/addi pairs into code or data
    for i, w in enumerate(words):
        a = cva + 4 * i
        if a >= code_end:
            break
        op = w >> 26
        if op == 18 and not w & 2:
            li = w & 0x03FFFFFC
            t = a + (li - 0x04000000 if li & 0x02000000 else li)
            nd = cmap(t) - cmap(a)
            new[i] = (w & 0xFC000003) | (nd & 0x03FFFFFC)
        elif op == 16 and not w & 2:
            bd = w & 0xFFFC
            t = a + (bd - 0x10000 if bd & 0x8000 else bd)
            nd = cmap(t) - cmap(a)
            assert -0x8000 <= nd < 0x8000
            new[i] = (w & 0xFFFF0003) | (nd & 0xFFFC)
        elif op == 15 and not (w >> 16) & 31:
            rt = (w >> 21) & 31
            for j in range(i + 1, min(i + 9, nwords)):
                w2 = words[j]
                o2 = w2 >> 26
                if (w2 >> 16) & 31 == rt and o2 in R._ADDR_IMM_OPS and o2 not in (15,):
                    lo = (w2 & 0xFFFF) if o2 == 24 else R.sx16(w2 & (0xFFFC if o2 in (58, 62) else 0xFFFF))
                    v = (((w & 0xFFFF) << 16) + lo) & 0xFFFFFFFF
                    nv = amap(v)
                    if nv != v:
                        if o2 == 24:
                            hi, nlo = nv >> 16, nv & 0xFFFF
                        else:
                            nlo = nv & 0xFFFF
                            hi = ((nv - R.sx16(nlo)) >> 16) & 0xFFFF
                        new[i] = (w & 0xFFFF0000) | hi
                        new[j] = (w2 & 0xFFFF0000) | (nlo & (0xFFFC if o2 in (58, 62) else 0xFFFF)) | \
                            (w2 & (3 if o2 in (58, 62) else 0))
                    break
    # pointers in the data segment (rodata is left as it is: it holds SPU code and other blobs whose words
    # would look like addresses)
    dwords = list(struct.unpack(">%dI" % (dfs // 4), data[:dfs // 4 * 4]))
    for i, w in enumerate(dwords):
        if (cva <= w < cva + cfs) and (w in funcs or w >= code_end) or dva <= w < dva + dms:
            dwords[i] = amap(w)
    data[:dfs // 4 * 4] = struct.pack(">%dI" % len(dwords), *dwords)
    # rebuild the code with the padding (nops) at each point
    out_code = bytearray()
    blob = struct.pack(">%dI" % nwords, *new) + bytes(code[nwords * 4:])
    prev = 0
    for p, size in points:
        o = p - cva
        out_code += blob[prev:o] + b"\x60\x00\x00\x00" * (size // 4)
        prev = o
    out_code += blob[prev:]
    grow = len(out_code) - cfs
    assert cva + len(out_code) <= dva + DATA_SHIFT, "the code would run into the moved data"
    new_doff = (coff + len(out_code) + 0xFFFF) & ~0xFFFF
    out = bytearray(d[:coff]) + out_code
    out += bytes(new_doff - len(out)) + data
    phs[code_i][5] += grow
    phs[code_i][6] += grow
    phs[data_i][2] = new_doff
    phs[data_i][3] += DATA_SHIFT
    phs[data_i][4] += DATA_SHIFT
    for i, p in enumerate(phs):         # other headers inside the data segment (TLS, process parameters)
        if i != data_i and p[0] != 1 and dva <= p[3] < dva + dms:
            p[3] += DATA_SHIFT
            p[4] += DATA_SHIFT
            p[2] = new_doff + (p[3] - DATA_SHIFT - dva)
    if coff == 0:                        # the headers are in the code segment's file image: write them there
        hdr = out
    else:
        hdr = out
    for i, p in enumerate(phs):
        struct.pack_into(">IIQQQQQQ", hdr, phoff + i * phentsz, *p)
    struct.pack_into(">Q", hdr, 0x28, 0)     # no section headers
    struct.pack_into(">HHH", hdr, 0x3A, 0, 0, 0)
    entry = struct.unpack_from(">Q", hdr, 0x18)[0]
    struct.pack_into(">Q", hdr, 0x18, amap(entry))
    with open(out_path, "wb") as f:
        f.write(out)
    nfuncs = {cmap(s): cmap(e) for s, e in funcs.items()}
    nimports = {k: cmap(v) for k, v in imports.items()}
    return amap, nfuncs, nimports, points


# ----------------------------------------------------------------------------------------- the changed copy
def changed_copy(img: R.Image, sigs, res, out_path):
    """Two windows of each item's patterns (not the anchor's own) get one instruction changed."""
    d = bytearray(img.d)
    s = next(s for s in img.segs if s[4] & 1)
    touched = 0
    anchors = set()
    for it in if2_items.ITEMS:
        for pat in sigs["items"].get(it["name"], []):
            st, a, _v = R.match_pattern(img, pat)
            if st == "found":
                anchors.update(range(a - 12, a + 16, 4))
    for it in if2_items.ITEMS:
        r = res.get(it["name"])
        if not r or r["status"] != "found":
            continue
        for pat in sigs["items"].get(it["name"], [])[:1]:
            st, a, _v = R.match_pattern(img, pat)
            if st != "found":
                continue
            far = sorted(pat["win"], key=lambda w: -abs(w["o"] + w["n"] // 2))[:2]
            for w in far:
                if w["o"] <= 0 < w["o"] + w["n"]:
                    continue
                for j in range(w["n"]):
                    ea = a + 4 * (w["o"] + j)
                    word = img.word(ea)
                    if word is None or ea in anchors:
                        continue
                    op = word >> 26
                    # a register-to-register instruction (no load, store, branch: nothing a decode follows)
                    if op == 31 and ((word >> 1) & 0x3FF) in (444, 266, 40, 8, 28, 316, 922, 954, 24, 536):
                        o = s[2] + ea - s[0]
                        struct.pack_into(">I", d, o, word ^ (1 << 21))   # another destination register
                        touched += 1
                        break
    with open(out_path, "wb") as f:
        f.write(d)
    return touched


def uniqueness(img, sigs):
    bad = 0
    for name, pats in sigs["items"].items():
        for pat in pats:
            if pat["from"] not in (if2_items.KNOWN.get(img.sha1),):
                continue
            for win in pat["win"]:
                t = bool(win.get("t"))
                hits = [p for p in img.positions(int(win["kw"], 16), t)
                        if img.nhash(p - 4 * win["k"], win["n"], t) == win["h"]]
                if len(hits) != 1:
                    print("    %s (%s): a window matches %d times" % (name, pat["from"], len(hits)))
                    bad += 1
            st, _a, _v = R.match_pattern(img, pat)
            if st != "found":
                print("    %s (%s): %s" % (name, pat["from"], st))
                bad += 1
    print("[unique] %s" % ("every window of this build's patterns matches once" if not bad else "%d problem(s)" % bad))
    return not bad


def main():
    ap = argparse.ArgumentParser()
    inputs = {}
    try:
        inputs = json.load(open(os.path.join(ROOT, "analysis", "inputs.json")))
    except (OSError, ValueError):
        pass
    ap.add_argument("--eboot", default=inputs.get("eboot"))
    ap.add_argument("--functions", default=os.path.join(ROOT, "analysis", "functions.json"))
    ap.add_argument("--imports", default=os.path.join(ROOT, "analysis", "EBOOT.imports.json"))
    ap.add_argument("--sigs", default=R.SIGS)
    ap.add_argument("--work")
    ap.add_argument("--keep", action="store_true")
    a = ap.parse_args()
    if not a.eboot or not os.path.exists(a.eboot) or not os.path.exists(a.functions):
        print("test_if2_resolve: no EBOOT / functions.json (analysis/) -- skipped")
        return 0
    img = R.Image(a.eboot)
    build = if2_items.KNOWN.get(img.sha1)
    if not build:
        print("test_if2_resolve: %s is not a US EBOOT -- skipped" % a.eboot)
        return 0
    sigs = json.load(open(a.sigs))
    funcs = R.load_functions(a.functions)
    FUNCS_PATH[0] = a.functions
    imports = R.load_imports(a.imports) or {}
    work = a.work or tempfile.mkdtemp(prefix="if2res_")
    os.makedirs(work, exist_ok=True)
    ok = True
    want = known_values(build)
    res = R.resolve(img, sigs, funcs, imports, keep_all=True)
    ok &= compare(res, want, "%s itself" % build)
    ok &= uniqueness(img, sigs)

    sp = os.path.join(work, "EBOOT.shifted.elf")
    amap, nf, ni, points = shifted_copy(img, funcs, imports, sp)
    print("[shifted] padding at %s; data +0x%X" % (", ".join("0x%X(+0x%X)" % p for p in points), DATA_SHIFT))
    simg = R.Image(sp)
    sres = R.resolve(simg, sigs, nf, ni, keep_all=True)
    swant = {}
    for n, v in want.items():
        it = if2_items.BY_NAME[n]
        swant[n] = v if v is None or it["kind"] == "tocdisp" else amap(v)
    moved = sum(1 for n, v in want.items() if v is not None and swant[n] != v)
    print("[shifted] %d of the items moved" % moved)
    ok &= compare(sres, swant, "shifted")

    cp = os.path.join(work, "EBOOT.changed.elf")
    n = changed_copy(img, sigs, res, cp)
    cimg = R.Image(cp)
    cres = R.resolve(cimg, sigs, funcs, imports, keep_all=True)
    print("[changed] %d instructions changed" % n)
    ok &= compare(cres, want, "changed")

    if not a.keep and not a.work:
        shutil.rmtree(work, ignore_errors=True)
    print("test_if2_resolve: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
