#!/usr/bin/env python3
"""if2_resolve.py -- find every inFamous 2 title-specific address in the EBOOT being lifted, by code patterns.

The title code (src/, tools/ppu_postlift.py, tools/extract_policy_modules.py) needs about two hundred addresses
of the game's EBOOT: functions it calls or hooks, globals it reads or patches, instructions it checks. They used
to be the two US builds' constants. This tool finds each one in whatever EBOOT is being lifted, from patterns
derived once from the US builds (config/if2_sigs.json), and writes:

    generated/if2_resolved.h      the values src/ compiles with (IF2_R_<NAME>, IF2_OK_<NAME>, IF2_HAVE_<FEATURE>)
    analysis/resolve.json         the same for the Python tools (post-lift hooks, policy modules)
    analysis/resolve_report.txt   one line per item: found / ambiguous / missing, where, and how

A pattern is a set of short instruction windows around an anchor instruction, each normalised so that what a
relink or a recompile with other addresses changes does not matter (branch displacements, the 16-bit halves of
address materialisation and every load/store/addi displacement -- a TOC offset, a field offset), and stored as
a hash plus its rarest normalised word, so no game code is kept in this repository. An item is found when
windows agree on one anchor (a few changed instructions only lose the windows they fall in); the value is then
decoded from the instructions at the anchor in THIS EBOOT (the TOC slot a load reads, the lis/addi pair, the
`bl` target, ...). Each item must match exactly once: two places = ambiguous, none = missing, and a feature
whose items are not all found is compiled off (src/ falls back to the game's own behaviour).

On the US EBOOTs (known by SHA-1) every value must equal the one in tools/if2_items.py, or this exits 1.

    py tools/if2_resolve.py resolve [--eboot E] [--functions F] [--imports I] [--probe]
    py tools/if2_resolve.py derive  --ref us104=EBOOT,FUNCS --ref us100=EBOOT,FUNCS [--only-from us104]
    py tools/if2_resolve.py selftest [--ref ...]          (every pattern unique in the references)

resolve reads analysis/inputs.json (the EBOOT) and analysis/functions.json + analysis/EBOOT.imports.json when
they exist (the lift step runs it right after find_functions); --probe (tools/check_inputs.py) skips those and
writes nothing but analysis/resolve_probe.json. Standard library only.
"""
from __future__ import annotations

import argparse
import array
import hashlib
import json
import os
import struct
import sys
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import if2_items  # noqa: E402

SIGS = os.path.join(ROOT, "config", "if2_sigs.json")
WIN = 8                 # words per window
TOL = 0x100             # bytes: windows within this of each other vote for one place
CTX = (-2, 3)           # the anchor's own context: words [A-2, A+3)

# ----------------------------------------------------------------------------------------------- normalise
_ADDR_IMM_OPS = {14, 15, 24, 25} | set(range(32, 56)) | {58, 62}   # addi addis ori oris, loads/stores, ld/std
MASK = [0xFFFFFFFF] * 64
for _op in _ADDR_IMM_OPS:
    MASK[_op] = 0xFFFF0000
MASK[18] = 0xFC000003       # b/bl: keep AA/LK
MASK[16] = 0xFFFF0003       # bc: keep BO/BI/AA/LK


def norm1(w: int) -> int:
    op = w >> 26
    if op == 14 and not (w >> 16) & 31:
        return w                     # li: a constant, not an address
    return w & MASK[op]


def tight_words(words):
    """Like norm1, but a load/store/addi keeps its displacement unless its base register holds an address
    (set by lis, or loaded / computed from r2 or from another such register since the last blr): field and
    stack offsets stay (they tell apart small functions of one shape), TOC and pool offsets go."""
    out = []
    addr = 0                                    # bit r: register r holds an address-like value
    for w in words:
        op = w >> 26
        rt, ra = (w >> 21) & 31, (w >> 16) & 31
        if op == 18 or op == 16:
            t = norm1(w)
        elif op in _ADDR_IMM_OPS:
            if op == 14 and ra == 0:
                t = w
            elif op in (15, 24, 25) or ra == 2 or (addr >> ra) & 1:
                t = w & 0xFFFF0000
            else:
                t = w
        else:
            t = w
        out.append(t)
        # what this instruction leaves in its destination register
        if w == 0x4E800020:
            addr = 0
            continue
        if op == 18 and (w & 1):
            addr &= ~0x1FF9                      # r0, r3..r12: volatile across a call
            continue
        if op in (14, 15, 32, 24):
            src = rt if op == 24 else ra
            dst = ra if op == 24 else rt
            if op == 15 and ra == 0:
                isaddr = True                    # lis
            elif op == 14 and ra == 0:
                isaddr = False                   # li
            else:
                isaddr = src == 2 or bool((addr >> src) & 1)
            if isaddr:
                addr |= 1 << dst
            else:
                addr &= ~(1 << dst)
            continue
        if op in (20, 21, 23, 26, 27, 28, 29, 30):
            addr &= ~(1 << ra)
        elif op in (7, 8, 12, 13, 34, 40, 42, 58):
            addr &= ~(1 << rt)
        elif op == 31:
            xo = (w >> 1) & 0x3FF
            if xo == 444 and rt == ((w >> 11) & 31):          # mr keeps the property
                if (addr >> rt) & 1:
                    addr |= 1 << ra
                else:
                    addr &= ~(1 << ra)
            elif xo in _LOGIC31:
                addr &= ~(1 << ra)
            elif xo not in _NOWRITE31:
                addr &= ~(1 << rt)
        elif op in (33, 35, 41, 43, 46):
            addr = 0
    return out


def sx16(v: int) -> int:
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def h8(b: bytes) -> str:
    return hashlib.blake2b(b, digest_size=8).hexdigest()


# --------------------------------------------------------------------------------------------------- image
class Image:
    """A decrypted PPU EBOOT: its segments, TOC, and its code as normalised words."""

    def __init__(self, path: str):
        d = open(path, "rb").read()
        if d[:4] != b"\x7fELF" or d[4] != 2 or d[5] != 2:
            raise ValueError("%s: not a 64-bit big-endian ELF" % path)
        self.path, self.d = path, d
        self.sha1 = hashlib.sha1(d).hexdigest()
        self.size = len(d)
        phoff = struct.unpack_from(">Q", d, 0x20)[0]
        phentsz, phnum = struct.unpack_from(">HH", d, 0x36)
        self.segs = []
        for i in range(phnum):
            t, fl, off, va, _pa, fs, ms, _al = struct.unpack_from(">IIQQQQQQ", d, phoff + i * phentsz)
            if t == 1 and ms:
                self.segs.append((va, ms, off, fs, fl))
        self.entry = struct.unpack_from(">Q", d, 0x18)[0]
        self.toc = self.u32(self.entry + 4) or 0
        code = [s for s in self.segs if s[4] & 1]
        if not code:
            raise ValueError("%s: no executable segment" % path)
        va, _ms, off, fs, _fl = code[0]
        self.cbase, self.cend = va, va + (fs & ~3)
        raw = array.array("I", d[off:off + (fs & ~3)])
        if sys.byteorder == "little":
            raw.byteswap()
        self.words = raw
        nw = array.array("I", [norm1(w) for w in raw])
        if sys.byteorder == "little":
            nw.byteswap()
        self.nb = nw.tobytes()         # normalised code, big-endian bytes
        self._tb = None                # the "tight" normalisation, made on first use
        self.fstarts = None            # sorted function starts, when the lift's functions.json is given
        self._pos = {}

    # memory
    def seg_of(self, a):
        for s in self.segs:
            if s[0] <= a < s[0] + s[1]:
                return s
        return None

    def bytes_at(self, a: int, n: int):
        s = self.seg_of(a)
        if not s or a + n > s[0] + s[3]:
            return None              # outside the file-backed part (bss) or the image
        o = s[2] + (a - s[0])
        return self.d[o:o + n]

    def u32(self, a: int):
        b = self.bytes_at(a, 4)
        return struct.unpack(">I", b)[0] if b else None

    def set_functions(self, funcs):
        self.fstarts = sorted(funcs) if funcs else None

    def func_start(self, a: int):
        """The start of the function holding a; a piece find_functions split off a function (no stdu r1 in its
        first instructions: e.g. after a large frame's stack probe) counts as part of the one before it."""
        if not self.fstarts:
            return None
        import bisect
        i = bisect.bisect_right(self.fstarts, a) - 1
        for _ in range(3):
            if i <= 0:
                break
            f = self.fstarts[i]
            if any(is_stdu_r1(self.word(f + 4 * k) or 0) or (self.word(f + 4 * k) or 0) >> 26 == 31 and
                   ((self.word(f + 4 * k) >> 1) & 0x3FF) == 181 for k in range(8)):
                break
            if not any((self.word(f + 4 * k) or 0) == 0x7C0802A6 for k in range(4)):
                break                        # a leaf (no mflr): really its own function
            i -= 1
        return self.fstarts[i] if i >= 0 else None

    def in_code(self, a: int) -> bool:
        return self.cbase <= a < self.cend and not a & 3

    def word(self, a: int):
        return self.words[(a - self.cbase) >> 2] if self.in_code(a) else None

    def nword(self, a: int):
        w = self.word(a)
        return None if w is None else norm1(w)

    @property
    def tb(self):
        if self._tb is None:
            tw = array.array("I", tight_words(self.words))
            if sys.byteorder == "little":
                tw.byteswap()
            self._tb = tw.tobytes()
        return self._tb

    def stream(self, tight: bool):
        return self.tb if tight else self.nb

    def tword(self, a: int):
        if not self.in_code(a):
            return None
        o = a - self.cbase
        return struct.unpack(">I", self.tb[o:o + 4])[0]

    def nhash(self, a: int, n: int, tight: bool = False):
        if a < self.cbase or a + 4 * n > self.cend:
            return None
        o = a - self.cbase
        return h8(self.stream(tight)[o:o + 4 * n])

    def positions(self, key: int, tight: bool = False):
        """Code addresses whose normalised word is key."""
        p = self._pos.get((key, tight))
        if p is None:
            kb = struct.pack(">I", key)
            p, i, nb = [], 0, self.stream(tight)
            while True:
                i = nb.find(kb, i)
                if i < 0:
                    break
                if not i & 3:
                    p.append(self.cbase + i)
                i += 1
            self._pos[(key, tight)] = p
        return p


# ------------------------------------------------------------------------------------------------- decode
# GPR writes, for the value tracker: which register an instruction writes (None: none, -1: several / unknown)
_LOGIC31 = {28, 60, 124, 284, 316, 412, 444, 476, 24, 536, 792, 824, 26, 922, 954, 986, 27, 539, 794, 58}
_NOWRITE31 = {0, 32, 4, 68, 86, 54, 278, 470, 1014, 598, 854, 982, 467, 144, 150, 214, 151, 183, 215, 247,
              407, 439, 662, 918, 660, 663, 695, 727, 759, 983, 231, 487, 135, 167, 199, 647, 679, 903, 935,
              149, 181, 725, 661}


def gpr_written(w: int):
    op = w >> 26
    rt, ra = (w >> 21) & 31, (w >> 16) & 31
    if op in (7, 8, 12, 13, 14, 15, 32, 34, 40, 42, 58):
        return rt
    if op in (33, 35, 41, 43):          # update forms write RT and RA
        return -1
    if op in (20, 21, 23, 24, 25, 26, 27, 28, 29, 30):
        return ra
    if op in (49, 51, 53, 55, 37, 39, 45):
        return ra                       # FP / store update forms: RA
    if op == 62:
        return ra if (w & 3) == 1 else None
    if op == 46:
        return -1                       # lmw
    if op == 31:
        xo = (w >> 1) & 0x3FF
        if xo in _NOWRITE31:
            return None
        if xo in _LOGIC31 or ((w >> 2) & 0x1FF) == 413:
            return ra
        return rt
    if op == 18 and (w & 1):
        return -2                       # a call: the volatile registers
    return None


def is_restore(w):
    """ld / lwz r14..r31, d(r1): an epilogue restoring a non-volatile register."""
    return (w >> 26) in (32, 58) and ((w >> 16) & 31) == 1 and ((w >> 21) & 31) >= 14 and         ((w >> 26) == 32 or (w & 3) == 0)


def is_stdu_r1(w):
    return (w >> 26) == 62 and ((w >> 16) & 31) == 1 and ((w >> 21) & 31) == 1 and (w & 3) == 1


def reg_value(img: Image, a: int, reg: int, depth: int = 0):
    """The value register reg holds at instruction a, from the nearest instruction before it (in a straight
    line back, inside this function) that writes it: a TOC load, a pool pointer, lis/addi/ori, mr. None when it
    is not a constant there. The same rules on every EBOOT: a pattern's decode means the same thing."""
    if reg == 2:
        return img.toc
    if reg == 0 or depth > 4:
        return None
    b = a - 4
    fs = img.func_start(a)
    lim = fs - 4 if fs is not None else a - 4 * 700
    while b >= img.cbase and b > lim:
        w = img.word(b)
        if w == 0x4E800020 and fs is None:   # blr, with no function bounds known: the previous function
            return None
        if is_restore(w):                # an epilogue's reload of a saved register: not this path's value
            b -= 4
            continue
        g = gpr_written(w)
        if g == -2:
            if reg <= 12:
                return None
        elif g == -1:
            if reg >= ((w >> 21) & 31) or reg == ((w >> 16) & 31):
                return None
        elif g == reg:
            op = w >> 26
            rs, ra = (w >> 21) & 31, (w >> 16) & 31
            if op == 14:
                if ra == 0:
                    return sx16(w)
                v = reg_value(img, b, ra, depth + 1)
                return None if v is None else (v + sx16(w)) & 0xFFFFFFFF
            if op == 15:
                hi = (w & 0xFFFF) << 16
                if ra == 0:
                    return hi
                v = reg_value(img, b, ra, depth + 1)
                return None if v is None else (v + hi) & 0xFFFFFFFF
            if op in (24, 25):
                v = reg_value(img, b, rs, depth + 1)
                return None if v is None else v | ((w & 0xFFFF) << (16 if op == 25 else 0))
            if op == 32:
                v = reg_value(img, b, ra, depth + 1)
                return None if v is None else img.u32((v + sx16(w)) & 0xFFFFFFFF)
            if op == 31 and ((w >> 1) & 0x3FF) == 444 and rs == ((w >> 11) & 31):
                return reg_value(img, b, rs, depth + 1)        # mr
            return None
        if is_stdu_r1(w) and fs is None:  # the prologue's frame: nothing before it is this function's
            return None
        b -= 4
    return None


def access_ea(img: Image, a: int):
    """The effective address of the D-form access (load, store, addi) at a, if its base is a constant."""
    w = img.word(a)
    if w is None:
        return None
    op, ra = w >> 26, (w >> 16) & 31
    if op not in _ADDR_IMM_OPS or op in (15, 24, 25) or ra == 0:
        return None
    base = reg_value(img, a, ra)
    if base is None:
        return None
    return (base + sx16(w & (0xFFFC if op in (58, 62) else 0xFFFF))) & 0xFFFFFFFF


def decode_value(img: Image, a: int, dec: dict):
    """The value the instruction at a (and the ones it depends on) materialises, per a pattern's decode rule:
    addr = a itself; ea = the address it accesses; ld = the word it loads (a pointer from a pool / the TOC).
    (value, why) or (None, why)."""
    t = dec["t"]
    if img.word(a) is None:
        return None, "anchor outside the code"
    if t == "addr":
        return a, ""
    if t == "bl":
        w = img.word(a)
        if (w >> 26) != 18 or (w & 3) != 1:
            return None, "0x%08X is not a bl" % a
        li = w & 0x03FFFFFC
        return (a + (li - 0x04000000 if li & 0x02000000 else li)) & 0xFFFFFFFF, ""
    ea = access_ea(img, a)
    if ea is None:
        return None, "the access at 0x%08X has no constant base here" % a
    if t == "ea":
        return ea, ""
    if t == "ld":
        if (img.word(a) >> 26) != 32:
            return None, "0x%08X is not a lwz" % a
        v = img.u32(ea)
        return (v, "") if v is not None else (None, "0x%08X is not in the file" % ea)
    return None, "unknown decode %r" % t


# ------------------------------------------------------------------------------------------------- match
def match_pattern(img: Image, pat: dict):
    """-> ("found", anchor, votes) | ("ambiguous", [anchors], votes) | ("missing", None, 0)"""
    votes = []                                  # (anchor, window index)
    for wi, win in enumerate(pat["win"]):
        key, tight = int(win["kw"], 16), bool(win.get("t"))
        for p in img.positions(key, tight):
            ws = p - 4 * win["k"]
            if img.nhash(ws, win.get("n", WIN), tight) == win["h"]:
                votes.append((ws - 4 * win["o"], wi))
    if not votes:
        return "missing", None, 0
    votes.sort()
    clusters = []                               # [first anchor, {window: anchor}]
    for a, wi in votes:
        if clusters and a - clusters[-1][0] <= TOL:
            clusters[-1][1].setdefault(wi, a)
        else:
            clusters.append([a, {wi: a}])
    need = pat["need"]
    good = sorted(((len(c[1]), c) for c in clusters if len(c[1]) >= need), key=lambda x: -x[0])
    if not good:
        best = max(len(c[1]) for c in clusters)
        return "missing", None, best
    if len(good) > 1:
        return "ambiguous", [g[1][0] for g in good], good[0][0]
    c = good[0][1][1]
    # the anchor from the window nearest to it (an insertion between farther windows and the anchor moves theirs)
    wi = min(c, key=lambda i: abs(pat["win"][i]["o"] + pat["win"][i].get("n", WIN) // 2))
    a = c[wi]
    anc = pat["anchor"]
    lo, n = CTX[0], CTX[1] - CTX[0]
    if img.nhash(a + 4 * lo, n) != anc["c"] and img.nword(a) != int(anc["w"], 16):
        # neither the anchor's few words nor the anchor itself are where the votes say: an instruction was
        # inserted or removed between the voting windows and the anchor. Its few words within 8 instructions,
        # once -- never a look-alike further away (a sibling site of the same shape)
        near = [b for b in range(a - 32, a + 36, 4) if img.nhash(b + 4 * lo, n) == anc["c"]]
        if len(near) != 1:
            return ("ambiguous" if near else "missing"), near or None, len(c)
        a = near[0]
    if img.nword(a) != int(anc["w"], 16):
        return "missing", None, len(c)
    return "found", a, len(c)


def load_functions(path):
    if not path or not os.path.exists(path):
        return None
    f = json.load(open(path))
    items = f if isinstance(f, list) else f.get("functions", [])
    return {int(it["start"], 16): int(it["end"], 16) for it in items}


def load_imports(path):
    if not path or not os.path.exists(path):
        return None
    out = {}
    for e in json.load(open(path)):
        out[(e["library"], int(e["nid"], 16))] = int(e["stub"], 16)
    return out


# ------------------------------------------------------------------------------------------------ resolve
def resolve(img: Image, sigs: dict, funcs=None, imports=None, keep_all=False):
    """-> {name: {status, value, how, ...}} for every item in if2_items.ITEMS."""
    res = {}
    if funcs is not None:
        img.set_functions(funcs)
    for it in if2_items.ITEMS:
        res[it["name"]] = resolve_item(img, it, sigs, funcs, imports, res)
    # A US build keeps exactly the features it had: an item that build was never mapped for is off there even
    # when a pattern finds it (1.00's code has most of the native menu's functions; they were never tried).
    build = None if keep_all else if2_items.KNOWN.get(img.sha1)
    if build:
        for it in if2_items.ITEMS:
            if it.get(build) is None and not _derived_known(it, build) and it["kind"] not in ("toc",):
                r = res[it["name"]]
                if r["status"] == "found":
                    r["how"] = "found at 0x%08X, but not mapped for %s: off as before" % (r["value"] & 0xFFFFFFFF, build)
                r["status"] = "n/a"
                r["value"] = None
    return res


def _derived_known(it, build):
    """A rel item whose base has a value in that build (quality rows on 1.04)."""
    return it["kind"] == "rel" and if2_items.BY_NAME[it["base"]].get(build) is not None


def _r(status, value=None, how="", **kw):
    d = dict(status=status, value=value, how=how)
    d.update(kw)
    return d


def resolve_item(img, it, sigs, funcs, imports, res):
    k = it["kind"]
    if k == "toc":
        return _r("found", img.toc, "the entry point's descriptor")
    if k == "import":
        if imports is None:
            return _r("unchecked", None, "no import table yet (analyze step)")
        v = imports.get((it["lib"], it["nid"]))
        return _r("found", v, "import %s 0x%08X" % (it["lib"], it["nid"])) if v else \
            _r("missing", None, "no import %s 0x%08X" % (it["lib"], it["nid"]))
    if k in ("rel", "deref", "callee", "tocdisp"):
        b = res.get(it["base"])
        if not b or b["status"] != "found":
            return _r("missing", None, "needs %s" % it["base"])
        if k == "rel":
            v = (b["value"] + it["off"]) & 0xFFFFFFFF
            if "expect" in it and img.u32(v) != it["expect"]:
                got = img.u32(v)
                return _r("missing", v, "%s + 0x%X = 0x%08X holds %s, not the stock 0x%08X" % (
                    it["base"], it["off"], v, "nothing" if got is None else "0x%08X" % got, it["expect"]))
            return _r("found", v, "%s + 0x%X" % (it["base"], it["off"]))
        if k == "deref":
            v = img.u32(b["value"] + it["off"])
            return _r("found", v, "*(%s + 0x%X)" % (it["base"], it["off"])) if v is not None else \
                _r("missing", None, "%s + 0x%X is not in the file" % (it["base"], it["off"]))
        w = img.word(b["value"])
        if k == "callee":
            if w is None or (w >> 26) != 18 or (w & 3) != 1:
                return _r("missing", None, "%s is not a bl" % it["base"])
            li = w & 0x03FFFFFC
            li = li - 0x04000000 if li & 0x02000000 else li
            v = (b["value"] + li) & 0xFFFFFFFF
            if funcs is not None and v not in funcs:
                return _r("missing", v, "bl target 0x%08X is not a function start" % v)
            return _r("found", v, "the bl at %s" % it["base"])
        if w is None or ((w >> 16) & 31) != 2:
            return _r("missing", None, "%s is not an r2-relative load" % it["base"])
        return _r("found", sx16(w), "the displacement of %s" % it["base"])
    if k == "pm":
        return resolve_pm(img, it, sigs)
    if k == "strtab":
        return resolve_strtab(img, it)
    pats = sigs.get("items", {}).get(it["name"], [])
    if not pats:
        return _r("missing", None, "no pattern (not in this tree's config/if2_sigs.json)")
    found, amb, how = {}, [], []
    for pat in pats:
        st, a, votes = match_pattern(img, pat)
        if st == "found":
            v, why = decode_value(img, a, pat["dec"])
            if v is None:
                how.append("%s: %s" % (pat["from"], why))
                continue
            found.setdefault(v, []).append("%s %d/%d windows at 0x%08X%s" % (
                pat["from"], votes, len(pat["win"]), a, "" if pat["dec"]["t"] == "addr" else " (%s)" % pat["dec"]["t"]))
        elif st == "ambiguous":
            amb.append("%s: %s" % (pat["from"], " ".join("0x%08X" % x for x in a)))
        else:
            how.append("%s: %s" % (pat["from"], "no match" if not votes else "best %d/%d windows (need %d)" % (
                votes, len(pat["win"]), pat["need"])))
    if len(found) > 1:
        return _r("ambiguous", None, "patterns disagree: " + "; ".join(
            "0x%08X by %s" % (v, ", ".join(w)) for v, w in found.items()))
    if not found:
        if amb:
            return _r("ambiguous", None, "matches at several places: " + "; ".join(amb))
        return _r("missing", None, "; ".join(how))
    v, ways = next(iter(found.items()))
    r = _r("found", v, "; ".join(ways))
    if k == "code":
        r["word"] = img.word(v)                         # (tools/ppu_postlift.py decodes some of these)
    if k == "func" and funcs is not None and v not in funcs:
        return _r("missing", v, "0x%08X is not a function start of this lift (%s)" % (v, r["how"]))
    if "expect" in it:
        got = img.u32(v)
        if got != it["expect"]:
            return _r("missing", v, "0x%08X holds %s, not the stock 0x%08X" % (
                v, "nothing (bss)" if got is None else "0x%08X" % got, it["expect"]))
    return r


def string_addrs(img: Image, text: str):
    """Addresses of the NUL-terminated string text (as a whole string) in the file-backed segments."""
    out, k = [], text.encode() + b"\0"
    for va, _ms, off, fs, _fl in img.segs:
        seg = img.d[off:off + fs]
        i = 0
        while True:
            i = seg.find(k, i)
            if i < 0:
                break
            if i == 0 or seg[i - 1] == 0:
                out.append(va + i)
            i += 1
    return out


def word_refs(img: Image, value: int):
    """Aligned 32-bit words equal to value in the non-code segments."""
    out, k = [], struct.pack(">I", value)
    for va, _ms, off, fs, fl in img.segs:
        if fl & 1:
            continue
        seg = img.d[off:off + fs]
        i = 0
        while True:
            i = seg.find(k, i)
            if i < 0:
                break
            if not i & 3:
                out.append(va + i)
            i += 1
    return out


def resolve_strtab(img: Image, it):
    strs = it["strings"]
    first = string_addrs(img, strs[0])
    hits = []
    for s0 in first:
        for t in word_refs(img, s0):
            ok = True
            for j, sj in enumerate(strs[1:], 1):
                p = img.u32(t + 4 * j)
                if p is None or img.bytes_at(p, len(sj) + 1) != sj.encode() + b"\0":
                    ok = False
                    break
            if ok:
                hits.append(t)
    if len(hits) == 1:
        return _r("found", hits[0], "the table whose entries point to %s" % ", ".join('"%s"' % x for x in strs))
    if hits:
        return _r("ambiguous", None, "%d tables point to %s" % (len(hits), ", ".join(strs)))
    return _r("missing", None, "no table of pointers to %s (strings found: %d)" % (", ".join(strs), len(first)))


# ------------------------------------------------------------------------------------- the policy module
PM_SIZE = 0x2E80
PM_SHA1 = "50dc1c2cb3f84e03ee2f62f56c203f34b5bbdad4"


def spu_plausible(b: bytes) -> float:
    """Share of the words that are SPU instructions this lifter knows the shape of (a code blob scores ~1)."""
    import re  # noqa: F401  (kept stdlib-only and cheap)
    ok = n = 0
    for (w,) in struct.iter_unpack(">I", b):
        n += 1
        if w == 0:
            continue
        o11 = w >> 21
        # RRR (4-bit op), RI18 (7-bit), RI10 (8-bit), RI16 (9-bit), RR/RI7 (11-bit): anything whose top bits are
        # a defined SPU opcode class. Data (ASCII, floats) mostly lands outside 0x000..0x7FF classes that exist.
        if (w >> 28) in (0x8, 0xB, 0xC, 0xE, 0xF):                 # selb shufb mpya fma fnms fms (RRR)
            ok += 1
        elif (w >> 25) in (0x08, 0x09, 0x21, 0x10, 0x11):           # hbra hbrr ila / RI18
            ok += 1
        elif (w >> 24) in range(0x04, 0x40) or (w >> 23) in range(0x40, 0x100) or o11 < 0x7FF:
            ok += 1
    return ok / max(1, n)


def pm_wellformed(img: Image, ea: int, size: int):
    b = img.bytes_at(ea, size)
    if b is None:
        return False, "0x%08X+0x%X is outside the file" % (ea, size)
    if ea & 0x7F or size & 0x7F:
        return False, "not 128-byte aligned (a SPURS policy module is DMA'd whole)"
    first = struct.unpack(">I", b[:4])[0]
    if not b.strip(b"\x00"):
        return False, "all zero"
    score = spu_plausible(b)
    if score < 0.97:
        return False, "%.0f%% of its words look like SPU code" % (100 * score)
    if (first >> 25) not in (0x21, 0x41 >> 1) and (first >> 23) not in (0x81, 0x83):
        return True, "SPU code (%.1f%%), first word 0x%08X" % (100 * score, first)
    return True, "SPU code (%.1f%%)" % (100 * score)


def resolve_pm(img: Image, it, sigs):
    """The Wws_Job policy module: the known bytes anywhere in the EBOOT, else the place its registering code
    names (a pattern) holding a well-formed module."""
    hits = []
    b = img.d
    # the known module, by content: at every 128-byte aligned address (a policy module is DMA'd whole, aligned),
    # the hash of its first 64 bytes, then the SHA-1 of the whole (no byte of it is kept in this repository)
    pm_head = sigs.get("pm_head_h")
    if pm_head:
        for va, _ms, off, fs, _fl in img.segs:
            a = (va + 127) & ~127
            while a + PM_SIZE <= va + fs:
                i = off + (a - va)
                if h8(b[i:i + 64]) == pm_head and hashlib.sha1(b[i:i + PM_SIZE]).hexdigest() == PM_SHA1:
                    hits.append(a)
                a += 128
    if len(hits) == 1:
        return _r("found", hits[0], "the known module (SHA-1 %s...) by content" % PM_SHA1[:12],
                  pm_size=PM_SIZE, pm_exact=True)
    if len(hits) > 1:
        return _r("ambiguous", None, "the known module at %s" % " ".join("0x%08X" % h for h in hits))
    # by its reference: the code that registers it
    pats = sigs.get("items", {}).get(it["name"], [])
    cands = {}
    for pat in pats:
        st, a, _v = match_pattern(img, pat)
        if st == "found":
            v, _why = decode_value(img, a, pat["dec"])
            if v is not None:
                cands.setdefault(v, pat["from"])
    if len(cands) != 1:
        return _r("missing" if not cands else "ambiguous", None,
                  "the known module is not in this EBOOT, and its registering code %s" % (
                      "was not found" if not cands else "names several places"))
    ea = next(iter(cands))
    ok, why = pm_wellformed(img, ea, PM_SIZE)
    if not ok:
        return _r("missing", ea, "the registering code names 0x%08X, which is not a well-formed module: %s" % (ea, why))
    return _r("found", ea, "named by its registering code (%s); NOT the known bytes (SHA-1 %s): %s" % (
        cands[ea], hashlib.sha1(img.bytes_at(ea, PM_SIZE)).hexdigest()[:12], why), pm_size=PM_SIZE, pm_exact=False)


# --------------------------------------------------------------------------------------------------- derive
def code_funcs(funcs):
    starts = sorted(funcs)
    return starts


def func_of(starts, funcs, a):
    import bisect
    i = bisect.bisect_right(starts, a) - 1
    if i >= 0 and starts[i] <= a < funcs[starts[i]]:
        return starts[i], funcs[starts[i]]
    return None


def materialisations(img: Image, wanted: set, funcs: dict):
    """Every access in the code whose address (ea) or loaded word (ld) is one of the wanted values: value ->
    [(anchor, decode)]. A cheap forward pass proposes them; each one is confirmed with decode_value (the rules
    the resolver uses on the target), so only what decodes the same way there is kept."""
    out = defaultdict(list)
    regs = {2: img.toc}
    for fs in sorted(funcs):
        fe = funcs[fs]
        if not (img.in_code(fs) and fe <= img.cend):
            continue
        if img.func_start(fs) == fs:          # (a split-off piece goes on with what its function knew)
            regs = {2: img.toc}
        for a in range(fs, fe, 4):
            w = img.word(a)
            op = w >> 26
            rt, ra = (w >> 21) & 31, (w >> 16) & 31
            if op in _ADDR_IMM_OPS and op not in (15, 24, 25) and ra and ra in regs:
                ea = (regs[ra] + sx16(w & (0xFFFC if op in (58, 62) else 0xFFFF))) & 0xFFFFFFFF
                if ea in wanted:
                    out[ea].append((a, {"t": "ea"}))
                if op == 32:
                    v = img.u32(ea)
                    if v is not None and v in wanted:
                        out[v].append((a, {"t": "ld"}))
            if is_restore(w):
                continue
            g = gpr_written(w)
            if g == -2:
                for r in range(13):
                    if r != 2:
                        regs.pop(r, None)
                continue
            if g is None:
                continue
            if g == -1:
                regs.clear()
                regs[2] = img.toc
                continue
            nv = None
            if op == 14:
                nv = sx16(w) if ra == 0 else (regs[ra] + sx16(w)) & 0xFFFFFFFF if ra in regs else None
            elif op == 15:
                nv = (w & 0xFFFF) << 16 if ra == 0 else (regs[ra] + ((w & 0xFFFF) << 16)) & 0xFFFFFFFF \
                    if ra in regs else None
            elif op in (24, 25) and rt in regs:
                nv = regs[rt] | ((w & 0xFFFF) << (16 if op == 25 else 0))
            elif op == 32 and ra in regs:
                nv = img.u32((regs[ra] + sx16(w)) & 0xFFFFFFFF)
            elif op == 31 and ((w >> 1) & 0x3FF) == 444 and rt == ((w >> 11) & 31) and rt in regs:
                nv = regs[rt]
            if g == 2:
                continue
            if nv is None:
                regs.pop(g, None)
            else:
                regs[g] = nv
    return out


def make_pattern(img: Image, a: int, dec: dict, counts: dict, name: str, frm: str):
    """Windows of WIN words around anchor a, each unique in img, as a pattern -- checked by matching it."""
    cands = []
    base = a - 4 * 3                                     # the anchor sits 4th in its own window
    for reach in (6, 16):
        for tight in (False, True):
            for k in range(-reach, reach + 1):
                if reach > 6 and abs(k) <= 6:
                    continue
                ws = base + 4 * WIN * k
                if ws < img.cbase or ws + 4 * WIN > img.cend:
                    continue
                h = img.nhash(ws, WIN, tight)
                words = [(img.tword if tight else img.nword)(ws + 4 * j) for j in range(WIN)]
                cnt = counts[tight]
                key_i = min(range(WIN), key=lambda j: (cnt[words[j]], j))
                hits = [p - 4 * key_i for p in img.positions(words[key_i], tight)]
                n = sum(1 for p in hits if img.nhash(p, WIN, tight) == h)
                if n == 1:
                    win = dict(o=(ws - a) // 4, k=key_i, kw="%08x" % words[key_i], h=h, n=WIN)
                    if tight:
                        win["t"] = 1
                    cands.append((tight, abs(k), k, win))
        if len(cands) >= 4:
            break
    # nearest first, loose before tight at the same distance; up to 6, at most 3 of them tight
    cands.sort(key=lambda c: (c[1], c[0]))
    wins, nt = [], 0
    for tight, _d, _k, w in cands:
        if tight and nt >= 3:
            continue
        wins.append(w)
        nt += tight
        if len(wins) >= 6:
            break
    if not wins:              # (one window: still unique, checked below -- just less robust)
        return None, "only %d unique window(s) near 0x%08X" % (len(wins), a)
    lo, n = CTX[0], CTX[1] - CTX[0]
    pat = dict(name=name, **{"from": frm}, anchor=dict(w="%08x" % img.nword(a), c=img.nhash(a + 4 * lo, n)),
               win=wins, need=2 if len(wins) >= 3 else len(wins), dec=dec)
    st, got, _v = match_pattern(img, pat)
    if st != "found" or got != a:
        return None, "the windows near 0x%08X do not single it out (%s)" % (a, st)
    return pat, ""


def callers(img: Image, target: int, code_end: int):
    """Every bl to target."""
    out = []
    for i in range(0, (min(code_end, img.cend) - img.cbase) // 4):
        w = img.words[i]
        if (w >> 26) == 18 and (w & 3) == 1:
            a = img.cbase + 4 * i
            li = w & 0x03FFFFFC
            if (a + (li - 0x04000000 if li & 0x02000000 else li)) & 0xFFFFFFFF == target:
                out.append(a)
    return out


def derive(refs: dict, only_from=None, out_path=SIGS, quiet=False):
    """refs: {"us104": (Image, funcs, code_end), ...} -> writes config/if2_sigs.json."""
    sigs = {"version": 1, "win": WIN, "items": {}, "notes": {}}
    for frm, (img, funcs, code_end) in refs.items():
        if only_from and frm not in only_from:
            continue
        counts = {False: Counter(norm1(w) for w in img.words),
                  True: Counter(struct.unpack(">%dI" % (len(img.tb) // 4), img.tb))}
        wanted = {it[frm] for it in if2_items.ITEMS if it["kind"] in ("data", "pm") and it.get(frm) is not None}
        mats = materialisations(img, wanted, funcs)
        starts = sorted(funcs)
        for it in if2_items.ITEMS:
            v = it.get(frm)
            if v is None or it["kind"] not in ("func", "code", "data", "pm"):
                continue
            if it["kind"] in ("func", "code"):
                pat, why = make_pattern(img, v, {"t": "addr"}, counts, it["name"], frm)
                cands = [pat] if pat else []
                if it["kind"] == "func":
                    # also (or only, for a function too small or too common to single out) through its callers
                    for c in callers(img, v, code_end)[:12]:
                        p2, why2 = make_pattern(img, c, {"t": "bl"}, counts, it["name"], frm)
                        if p2:
                            cands.append(p2)
                            if len(cands) >= 3:
                                break
                        elif not cands:
                            why = why + "; caller 0x%08X: %s" % (c, why2)
            else:
                cands, why = [], "never materialised in the code"
                sites = mats.get(v, [])
                # prefer a direct access, then the fewest instructions between base and field; inside a function
                sites = sorted(set((a, json.dumps(d, sort_keys=True)) for a, d in sites))
                for a, dj in sites:
                    dec = json.loads(dj)
                    if func_of(starts, funcs, a) is None:
                        continue
                    pat, why = make_pattern(img, a, dec, counts, it["name"], frm)
                    if pat:
                        got, _ = decode_value(img, a, dec)
                        if got != v:
                            why = "decode check failed at 0x%08X" % a
                            continue
                        cands.append(pat)
                        if len(cands) >= 2:      # two independent references: either one may survive a change
                            break
            if not cands:
                sigs["notes"].setdefault(it["name"], []).append("%s: %s" % (frm, why))
                if not quiet:
                    print("  [%s] %-24s NO PATTERN: %s" % (frm, it["name"], why))
                continue
            sigs["items"].setdefault(it["name"], []).extend(cands)
    # the known Wws_Job module: the hash of its first 64 bytes, how resolve_pm finds it by content (then the
    # SHA-1 of the whole, PM_SHA1; no byte of the module is kept here)
    for frm, (img, _f, _c) in refs.items():
        v = if2_items.BY_NAME["wws_pm"].get(frm)
        if v is not None:
            b = img.bytes_at(v, PM_SIZE)
            if b and hashlib.sha1(b).hexdigest() == PM_SHA1:
                sigs["pm_head_h"] = h8(b[:64])
                break
    with open(out_path, "w", newline="\n") as f:
        json.dump(sigs, f, indent=1, sort_keys=True)
        f.write("\n")
    return sigs


# --------------------------------------------------------------------------------------------------- output
def features(res):
    """feature -> (on, [items that are not found])"""
    out = {}
    for f in if2_items.FEATURES:
        bad = [it["name"] for it in if2_items.ITEMS if f in it["gates"] and res[it["name"]]["status"] != "found"]
        out[f] = (not bad, bad)
    # the trace natives only drop out one by one
    out["menu_trace"] = (True, out["menu_trace"][1])
    return out


def family(res):
    """Which US build this EBOOT's code resembles: 104 when code only 1.04 has is there (its dynamic-quality table:
    1.00 has none -- most other 1.04 items, the native menu's functions among them, are in 1.00's code too),
    else 100."""
    return 104 if res["quality_table"]["status"] == "found" else 100


def check_known(img, res):
    """On a US EBOOT: every value as in if2_items. -> list of differences"""
    build = if2_items.KNOWN.get(img.sha1)
    if not build:
        return None, []
    bad = []
    for it in if2_items.ITEMS:
        want = it.get(build)
        if it["kind"] in ("rel",) and want is None:
            b = if2_items.BY_NAME[it["base"]].get(build)
            want = None if b is None else (b + it["off"]) & 0xFFFFFFFF
        r = res[it["name"]]
        if want is None:
            continue                                    # (n/a: resolve() turned it off)
        if r["status"] == "unchecked" or (it["gates"] == ["menu_trace"] and r["status"] == "missing"):
            continue                                    # (a trace native not found is only not traced)
        if r["status"] != "found" or r["value"] != want:
            bad.append("%s: %s %s, expected 0x%08X" % (it["name"], r["status"],
                       "-" if r["value"] is None else "0x%08X" % (r["value"] & 0xFFFFFFFF), want & 0xFFFFFFFF))
    return build, bad


def cname(n):
    return n.upper()


def write_header(path, img, res, build, fam, title_id, edition, from_update=True):
    feats = features(res)
    L = ["/* if2_resolved.h -- GENERATED by tools/if2_resolve.py from the EBOOT being lifted. Do not edit.",
         " *   EBOOT    %s" % os.path.basename(img.path),
         " *   SHA-1    %s (%s)" % (img.sha1, build or "not a US build: resolved by pattern"),
         " *   layout   like the US %s" % ("update 1.04" if fam == 104 else "disc EBOOT (1.00)"),
         " * Every value below was found in this EBOOT (analysis/resolve_report.txt says how). IF2_OK_<item> is 0",
         " * for an item that was not found; IF2_HAVE_<feature> is 0 when any item it needs was not, and that",
         " * feature is then off at run time (the game's own behaviour). */",
         "#pragma once",
         "#define IF2_RESOLVED 1",
         "#define IF2_EBOOT_SHA1 \"%s\"" % img.sha1,
         "#define IF2_EBOOT_KNOWN %d   /* 1: a US build whose values are cross-checked */" % (1 if build else 0),
         "#define IF2_EBOOT_V104 %d   /* the layout family: 1.04's (1) or the disc EBOOT's (0) */" % (1 if fam == 104 else 0),
         "#define IF2_TITLE_ID \"%s\"" % title_id,
         "#define IF2_EDITION \"%s\"" % edition,
         "#define IF2_EDITION_US %d" % (1 if title_id == "BCUS98125" else 0),
         "#define IF2_EBOOT_FROM_UPDATE %d   /* lifted from the title update (game/hdd0), not the disc */" % (
             1 if from_update else 0),
         "#define IF2_EDITION_BETA %d" % (0 if title_id == "BCUS98125" and build else 1),
         ]
    name = ("BCUS98125 update 1.04" if build == "us104" else "BCUS98125 disc (v02.00)" if build == "us100" else
            "%s %s (beta)" % (title_id, "update 1.04 layout" if fam == 104 else "disc layout"))
    L.append("#define IF2_EBOOT_NAME \"%s\"" % name)
    L.append("")
    for it in if2_items.ITEMS:
        if it["gates"] == ["menu_trace"]:
            continue
        r = res[it["name"]]
        ok = r["status"] == "found"
        v = r["value"] if ok else 0
        n = cname(it["name"])
        if it["kind"] == "tocdisp":
            L.append("#define IF2_R_%-26s (%d)" % (n, v))
        else:
            L.append("#define IF2_R_%-26s 0x%08Xu   /* %s */" % (n, (v or 0) & 0xFFFFFFFF, it["desc"][:70]))
        L.append("#define IF2_OK_%-25s %d" % (n, 1 if ok else 0))
        if it["kind"] in ("func", "callee", "import"):
            L.append("#define IF2_FN_%-25s %s" % (n, "func_%08X" % v if ok else "if2_unresolved_fn"))
    L.append("")
    for f, (on, bad) in feats.items():
        L.append("#define IF2_HAVE_%-23s %d%s" % (cname(f), 1 if on else 0,
                                                  "" if on else "   /* not found: %s */" % ", ".join(bad)[:150]))
    off = [f for f, (on, _b) in feats.items() if not on]
    L.append("#define IF2_FEATURES_OFF \"%s\"" % (", ".join(off) if off else ""))
    L.append("")
    # MENU_TRACE natives that were found (diagnostics)
    for acc, mac in ((0, "IF2_TRACE_NATIVES"), (1, "IF2_TRACE_ACCESSORS")):
        rows = []
        for it in if2_items.ITEMS:
            if it["gates"] == ["menu_trace"] and it["accessor"] == acc and res[it["name"]]["status"] == "found":
                rows.append("X(%08X, \"%s\")" % (res[it["name"]]["value"], it["trace"]))
        L.append("#define %s(X) \\" % mac)
        for i in range(0, len(rows), 4):
            L.append("    " + " ".join(rows[i:i + 4]) + " \\")
        L.append("    /* end */")
    L.append("")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    data = "\n".join(L) + "\n"
    if not os.path.exists(path) or open(path).read() != data:   # unchanged: no rebuild of every title file
        with open(path, "w", newline="\n") as f:
            f.write(data)


def report_lines(img, res, build, fam, bad, title_id):
    st = Counter(r["status"] for r in res.values())
    L = ["inFamous 2 title items resolved in %s" % os.path.basename(img.path),
         "  EBOOT sha1 %s, size %d bytes, TOC 0x%08X, code 0x%08X..0x%08X" % (
             img.sha1, img.size, img.toc, img.cbase, img.cend),
         "  title %s; %s; layout like the US %s" % (
             title_id, ("known US build %s: every value cross-checked" % build) if build else "not a US build (beta)",
             "update 1.04" if fam == 104 else "disc EBOOT (1.00)"),
         "  %d items: %d found, %d ambiguous, %d missing, %d not mapped for this build%s" % (
             len(res), st["found"], st["ambiguous"], st["missing"], st["n/a"],
             ", %d unchecked" % st["unchecked"] if st["unchecked"] else "")]
    for it in if2_items.ITEMS:
        r = res[it["name"]]
        v = r["value"]
        vs = "-" if v is None else ("%d" % v if it["kind"] == "tocdisp" else "0x%08X" % (v & 0xFFFFFFFF))
        L.append("  %-9s %-24s %-11s %-18s %s" % (r["status"], it["name"], vs, ",".join(it["gates"])[:18], r["how"]))
    L.append("  features:")
    for f, (on, b) in features(res).items():
        L.append("    %-17s %s%s" % (f, "on " if on else "OFF", "" if on else "  (not found: %s)" % ", ".join(b)))
    if bad:
        L.append("  CROSS-CHECK FAILED against the US values (tools/if2_items.py):")
        L += ["    " + x for x in bad]
    return L


# ------------------------------------------------------------------------------- save-state continuations
# config/ss_cont_list*.txt name return addresses (and function starts) of one US build. For another build each
# one is carried over through its function: the function's whole body, normalised (relocation-insensitive), is
# hashed; wherever this EBOOT has a function with the same hash, the same offsets are its continuations.
CONTDB = os.path.join(ROOT, "config", "if2_contdb.json")
_CONT_TOK = __import__("re").compile(
    r"(?:[A-Za-z0-9]+_)?func_(?P<f>[0-9A-Fa-f]{8})(?:\+(?P<off>0x[0-9A-Fa-f]+|\d+))?"
    r"|fn:(?P<fn>(?:0x)?[0-9A-Fa-f]{1,8})"
    r"|(?:ra:)?(?P<ra>0x[0-9A-Fa-f]{1,8}|[0-9A-Fa-f]{8})")


def parse_cont(text):
    """(return addresses, function starts) as ppu_lifter.py's parse_cont_list reads them."""
    ras, fns = set(), set()
    for line in text.splitlines():
        for tok in __import__("re").split(r"[\s,;]+", line.split("#", 1)[0]):
            m = _CONT_TOK.fullmatch(tok) if tok else None
            if not m:
                continue
            if m.group("f"):
                v = int(m.group("f"), 16)
                if m.group("off") is None:
                    fns.add(v)
                    continue
                v += int(m.group("off"), 0)
            elif m.group("fn"):
                fns.add(int(m.group("fn"), 16))
                continue
            else:
                v = int(m.group("ra"), 16)
            if not v & 3 and v >= 4:
                ras.add(v)
    return ras, fns


def func_hash(img: Image, s: int, e: int):
    if not (img.in_code(s) and e <= img.cend and e > s):
        return None
    return h8(img.nb[s - img.cbase:e - img.cbase]) + "%x" % ((e - s) // 4)


def contdb_derive(refs, lists):
    """refs as derive's; lists: {build: path of its cont list} -> {hash: {"r": [offsets], "f": 0|1}}"""
    db = {}
    stats = {}
    for build, path in lists.items():
        img, funcs, _ce = refs[build]
        starts = sorted(funcs)
        ras, fns = parse_cont(open(path).read())
        n = kept = 0
        for v, isf in [(x, 0) for x in sorted(ras)] + [(x, 1) for x in sorted(fns)]:
            if not img.in_code(v):
                continue                                 # libsre's (0x3000xxxx): its own lift
            n += 1
            fr = func_of(starts, funcs, v - (0 if isf else 4)) if not isf else (v, funcs.get(v)) if v in funcs else None
            if not fr or fr[1] is None:
                continue
            h = func_hash(img, fr[0], fr[1])
            if not h:
                continue
            e = db.setdefault(h, {"r": [], "f": 0})
            if isf:
                e["f"] = 1
            elif v - fr[0] not in e["r"]:
                e["r"].append(v - fr[0])
            kept += 1
        stats[build] = (n, kept)
    for e in db.values():
        e["r"].sort()
    return db, stats


def contdb_apply(img: Image, funcs, db):
    """-> (sorted return addresses, sorted function starts) for this EBOOT"""
    ras, fns = set(), set()
    for s, e in funcs.items():
        h = func_hash(img, s, e)
        d = db.get(h) if h else None
        if not d:
            continue
        if d["f"]:
            fns.add(s)
        for o in d["r"]:
            ras.add(s + o)
    return sorted(ras), sorted(fns)


def write_cont_list(path, img, ras, fns, note):
    L = ["# GENERATED by tools/if2_resolve.py: the save-state continuation list for this EBOOT (%s)," % img.sha1[:12],
         "# carried over from the US builds' lists (config/ss_cont_list*.txt) function by function (%s)." % note,
         "# Format: ppu_lifter.py parse_cont_list."]
    for i in range(0, len(fns), 8):
        L.append(" ".join("fn:0x%08X" % f for f in fns[i:i + 8]))
    for i in range(0, len(ras), 8):
        L.append(" ".join("0x%08X" % r for r in ras[i:i + 8]))
    with open(path, "w", newline="\n") as f:
        f.write("\n".join(L) + "\n")


# ------------------------------------------------------------------------------------------------------ main
def read_inputs(path):
    try:
        return json.load(open(path))
    except (OSError, ValueError):
        return {}


def cmd_resolve(a):
    inputs = read_inputs(a.inputs)
    eboot = a.eboot or inputs.get("eboot")
    if not eboot or not os.path.exists(eboot):
        print("if2_resolve: no EBOOT (analysis/inputs.json) -- run tools/check_inputs.py first")
        return 1
    title_id = a.title_id or inputs.get("title_id") or "BCUS98125"
    edition = inputs.get("edition") or ("North America" if title_id == "BCUS98125" else title_id)
    sigs = json.load(open(a.sigs))
    img = Image(eboot)
    funcs = None if a.probe else load_functions(a.functions)
    img.set_functions(funcs)
    imports = None if a.probe else load_imports(a.imports)
    res = resolve(img, sigs, funcs, imports)
    fam = family(res)
    build, bad = check_known(img, res)
    lines = report_lines(img, res, build, fam, bad, title_id)
    if a.probe:
        out = dict(sha1=img.sha1, family=fam, known=build,
                   counts=dict(Counter(r["status"] for r in res.values())),
                   features={f: on for f, (on, _b) in features(res).items()},
                   missing=[n for n, r in res.items() if r["status"] not in ("found", "unchecked")])
        # the core: what both US builds have (the display, clock, task graph, signals, WAIT_EVENTS, post-lift
        # hooks, language table, the policy module) -- what any build of the game should have
        core = [it["name"] for it in if2_items.ITEMS if it.get("us100") is not None and it.get("us104") is not None]
        out["core"] = dict(total=len(core), found=sum(res[c]["status"] == "found" for c in core),
                           ambiguous=sum(res[c]["status"] == "ambiguous" for c in core))
        if a.out_json:
            with open(a.out_json, "w") as f:
                json.dump(out, f, indent=1)
        for l in lines[:4]:
            print(l)
        return 0
    for l in lines:
        print(l)
    os.makedirs(os.path.dirname(a.report), exist_ok=True)
    with open(a.report, "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    js = dict(sha1=img.sha1, eboot=os.path.basename(eboot), family=fam, known=build, title_id=title_id,
              toc=img.toc, items={n: r for n, r in res.items()},
              features={f: dict(on=on, missing=b) for f, (on, b) in features(res).items()})
    with open(a.json, "w", newline="\n") as f:
        json.dump(js, f, indent=1, sort_keys=True)
    write_header(a.header, img, res, build, fam, title_id, edition,
                 from_update=(inputs.get("eboot_source") == "update") if inputs else fam == 104)
    if funcs is not None and os.path.exists(a.contdb):
        db = json.load(open(a.contdb))
        ras, fns = contdb_apply(img, funcs, db)
        total = db.get("_total", {})
        write_cont_list(a.cont_out, img, ras, fns, "%d return addresses, %d function starts" % (len(ras), len(fns)))
        msg = ("save-state continuations: %d return addresses and %d function starts carried over (the US lists: %s)"
               % (len(ras), len(fns), ", ".join("%s %d" % kv for kv in sorted(total.items()))))
        print("  " + msg)
        with open(a.report, "a", newline="\n") as f:
            f.write("  " + msg + "\n")
    print("if2_resolve: wrote %s, %s, %s" % (os.path.relpath(a.header, ROOT), os.path.relpath(a.json, ROOT),
                                             os.path.relpath(a.report, ROOT)))
    if bad:
        print("if2_resolve: ERROR -- this is a US EBOOT (%s) and %d value(s) differ from the known ones: a pattern "
              "is wrong (tools/if2_items.py, config/if2_sigs.json)." % (build, len(bad)))
        return 1
    return 0


def parse_refs(refs):
    out = {}
    for r in refs or []:
        name, rest = r.split("=", 1)
        eboot, funcs = rest.split(",", 1)
        img = Image(eboot)
        fn = load_functions(funcs)
        img.set_functions(fn)
        ce = os.path.join(os.path.dirname(funcs), "code_end.txt")
        code_end = int(open(ce).read().split()[-1], 16) if os.path.exists(ce) else img.cend
        out[name] = (img, fn, code_end)
    return out


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("resolve")
    r.add_argument("--inputs", default=os.path.join(ROOT, "analysis", "inputs.json"))
    r.add_argument("--eboot")
    r.add_argument("--title-id")
    r.add_argument("--functions", default=os.path.join(ROOT, "analysis", "functions.json"))
    r.add_argument("--imports", default=os.path.join(ROOT, "analysis", "EBOOT.imports.json"))
    r.add_argument("--sigs", default=SIGS)
    r.add_argument("--header", default=os.path.join(ROOT, "generated", "if2_resolved.h"))
    r.add_argument("--json", default=os.path.join(ROOT, "analysis", "resolve.json"))
    r.add_argument("--report", default=os.path.join(ROOT, "analysis", "resolve_report.txt"))
    r.add_argument("--probe", action="store_true")
    r.add_argument("--contdb", default=CONTDB)
    r.add_argument("--cont-out", default=os.path.join(ROOT, "analysis", "ss_cont_list.resolved.txt"))
    r.add_argument("--out-json")
    d = sub.add_parser("derive")
    d.add_argument("--ref", action="append", required=True, help="NAME=EBOOT,functions.json (NAME: us104, us100)")
    d.add_argument("--only-from", action="append")
    d.add_argument("--out", default=SIGS)
    d.add_argument("--cont", action="append", default=[], help="NAME=cont list (config/ss_cont_list*.txt)")
    d.add_argument("--contdb", default=CONTDB)
    a = ap.parse_args(argv)
    if a.cmd == "resolve":
        return cmd_resolve(a)
    if a.cmd == "derive":
        refs = parse_refs(a.ref)
        sigs = derive(refs, a.only_from, a.out)
        n = sum(len(v) for v in sigs["items"].values())
        print("derive: %d pattern(s) for %d item(s) -> %s" % (n, len(sigs["items"]), a.out))
        if a.cont:
            lists = dict(c.split("=", 1) for c in a.cont)
            db, stats = contdb_derive(refs, lists)
            db = dict(sorted(db.items()))
            db["_total"] = {b: k for b, (_n, k) in stats.items()}
            with open(a.contdb, "w", newline="\n") as f:
                json.dump(db, f, indent=0, sort_keys=True)
                f.write("\n")
            for b, (n, k) in stats.items():
                print("contdb: %s: %d of %d entries are inside a function (carried by its hash)" % (b, k, n))
        return 0
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
