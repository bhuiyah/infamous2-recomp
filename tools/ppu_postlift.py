#!/usr/bin/env python3
"""ppu_postlift.py -- call title code from inside lifted PPU functions.

Some fixes need host code to run at one point INSIDE a lifted guest function
-- between two of its instructions -- which neither an HLE override (firmware
imports only) nor a guest-memory write can do: lifted functions call each
other directly, so there is no table entry to replace. This script inserts
such calls into the lifted output (generated/ppu), at a line the lifter
emitted for a known guest address, and declares the hook. The hooks live in
src/ (extern "C" void hook(ppu_context*)) and decide at run time whether to
do anything.

tools/relift.sh runs it after every PPU lift. It is idempotent (a hook that
is already in place is left alone) and never fails the lift: if the lifted
code around an anchor changed, the hook is reported as NOT inserted and that
fix is simply absent. Run it by hand on an existing lift:

    py tools/ppu_postlift.py [generated/ppu] [--check]

--check reports without writing (exit status 1 if a hook is missing).
Line endings of the generated files are preserved.

A lift made with --cont-list (save states) also holds a second copy of some
lifted functions -- `contf_<addr>`, the function re-entered at a return
address, in ppu_recomp_cont.cpp. A hooked function that has such a copy gets
the same call in it, so a resumed thread runs the same code; a function with
no copy is simply skipped.

The anchors name lifted functions and guest addresses, so each EBOOT build
has its own table, made from tools/if2_resolve.py's analysis/resolve.json (the
functions, call sites and the frame offset it found in this EBOOT). HOOKS (the
disc EBOOT) and HOOKS_V104 (title update 1.04) are the US builds' tables: on
those EBOOTs the resolved table must be exactly theirs.
"""
import json
import os
import re
import sys

# (lifted function, anchor = the lifted line the call goes in front of, hook, why)
HOOKS = [
    ("func_002E7EDC",
     "ctx->lr = 0x002E8034; func_002D043C(ctx);",
     "if2_fmv_quad",
     "full-screen movie quad: fit instead of crop at a wide W (src/infamous2_hooks.cpp)"),
    # The game's signal dispatcher (source r3, id r4, args r5): first instruction.
    ("func_002F26B8",
     "ctx->gpr[9] = (uint64_t)ppc_rlwinm((uint32_t)ctx->gpr[4], 3, 0, 28);",
     "if2_signal_hook",
     "game signals (MenuActivate, WarpedToCheckpoint, ...) for the checkpoint boot driver (src/if2_checkpoint.cpp)"),
    # The physics step, right before its PPU narrowphase fallback (bl 0x47D438
    # at 0x479D5C). Diagnostic only, IF2_PHYSFB=1: why the SPU job's contacts
    # were not used for this body (no result header / job status).
    ("func_004796E0",
     "ctx->lr = 0x00479D60; func_0047D438(ctx);",
     "if2_physfb_diag",
     "physics: PPU narrowphase fallback reason, IF2_PHYSFB=1 (src/infamous2_hooks.cpp)"),
    # ...and where it has just loaded the body's result header into r0
    # (0x479CFC; the anchor is 0x479D00). Test only, IF2_PHYSFB_FORCE=1: every
    # body takes that fallback.
    ("func_004796E0",
     "ctx->gpr[25] = vm_read32(ctx->gpr[29] + 0x4);",
     "if2_physfb_force",
     "physics: IF2_PHYSFB_FORCE=1 sends every body to the PPU narrowphase (src/infamous2_hooks.cpp)"),
    # The mesh contact generator's body, right after `ld r31,0x448(r1)`
    # (0x4761FC; the anchor is the LR save at 0x476200, the same line with and
    # without the ps3recomp lifter fix). IF2_PHYSFB=1: r31 must be the spilled
    # side-A primitive; MISLIFT means the lift still has the old snapshot.
    ("func_004761F4",
     "vm_write64(ctx->gpr[1] + 0x600, ctx->gpr[0]);",
     "if2_physfb_side_a",
     "physics: side A of a PPU mesh contact must be the spilled argument, IF2_PHYSFB=1 (src/infamous2_hooks.cpp)"),
    # The user-made-mission (UGC) manager's idle state (its online mode, G+0x8F8, still -1), right before it
    # starts the online step by itself (state 5, the bl at 0x3D2BE4): GitHub #21, the step is taken offline
    # (state 18) with no connection and no message.
    ("func_003D2A04",
     "ctx->lr = 0x003D2BE8; func_003D270C(ctx);",
     "if2_ugc_offline",
     "UGC: the online step a finished user-made mission starts by itself is taken offline, #21 (src/infamous2_hooks.cpp)"),
]

# Title update 1.04 (tools/eboot_map.py, 1.00 -> 1.04): the same five sites. func_004761F4's frame grew by
# 0x10 there (stdu r1,-0x600; the LR save is at 0x610), the spill slot of side A is still r1+0x448.
HOOKS_V104 = [
    ("func_002FE8F8", "ctx->lr = 0x002FEA50; func_002E785C(ctx);", HOOKS[0][2], HOOKS[0][3]),
    ("func_0030A038", HOOKS[1][1], HOOKS[1][2], HOOKS[1][3]),
    ("func_004B90AC", "ctx->lr = 0x004B972C; func_004BCE44(ctx);", HOOKS[2][2], HOOKS[2][3]),
    ("func_004B90AC", HOOKS[3][1], HOOKS[3][2], HOOKS[3][3]),
    ("func_004B5BA0", "vm_write64(ctx->gpr[1] + 0x610, ctx->gpr[0]);", HOOKS[4][2], HOOKS[4][3]),
    # the UGC idle state: the online step is state 7 (the bl at 0x401158), offline is state 21
    ("func_00400F6C", "ctx->lr = 0x0040115C; func_00400BB4(ctx);", HOOKS[5][2], HOOKS[5][3]),
]


def hooks_from_resolve(res):
    """The hook table for this EBOOT from tools/if2_resolve.py's analysis/resolve.json: each hook whose items
    were found (a hook whose function or site was not is left out, and said so)."""
    it = res["items"]

    def ok(*names):
        return all(it.get(n, {}).get("status") == "found" for n in names)

    def fn(n):
        return "func_%08X" % it[n]["value"]
    out, off = [], []
    if ok("pl_fmv_fn", "pl_fmv_call", "pl_fmv_callee"):
        out.append((fn("pl_fmv_fn"), "ctx->lr = 0x%08X; func_%08X(ctx);" % (it["pl_fmv_call"]["value"] + 4,
                    it["pl_fmv_callee"]["value"]), HOOKS[0][2], HOOKS[0][3]))
    else:
        off.append(HOOKS[0][2])
    if ok("pl_signal_fn"):
        out.append((fn("pl_signal_fn"), HOOKS[1][1], HOOKS[1][2], HOOKS[1][3]))
    else:
        off.append(HOOKS[1][2])
    if ok("pl_phys_fn", "pl_phys_call", "pl_phys_callee"):
        out.append((fn("pl_phys_fn"), "ctx->lr = 0x%08X; func_%08X(ctx);" % (it["pl_phys_call"]["value"] + 4,
                    it["pl_phys_callee"]["value"]), HOOKS[2][2], HOOKS[2][3]))
        out.append((fn("pl_phys_fn"), HOOKS[3][1], HOOKS[3][2], HOOKS[3][3]))
    else:
        off += [HOOKS[2][2], HOOKS[3][2]]
    w = it.get("pl_mesh_lrsave", {}).get("word")
    if ok("pl_mesh_fn", "pl_mesh_lrsave") and w is not None and (w >> 16) == 0xF801 and (w & 3) == 0:
        out.append((fn("pl_mesh_fn"), "vm_write64(ctx->gpr[1] + 0x%X, ctx->gpr[0]);" % (w & 0xFFFC),
                    HOOKS[4][2], HOOKS[4][3]))
    else:
        off.append(HOOKS[4][2])
    if ok("pl_ugc_fn", "pl_ugc_call", "pl_ugc_callee"):
        out.append((fn("pl_ugc_fn"), "ctx->lr = 0x%08X; func_%08X(ctx);" % (it["pl_ugc_call"]["value"] + 4,
                    it["pl_ugc_callee"]["value"]), HOOKS[5][2], HOOKS[5][3]))
    else:
        off.append(HOOKS[5][2])
    return out, off


def hooks_for(gen_dir):
    """The hook table for the EBOOT this lift is of (analysis/resolve.json beside generated/). On the US builds
    it must be HOOKS / HOOKS_V104 exactly."""
    root = os.path.dirname(os.path.dirname(os.path.abspath(gen_dir)))
    try:
        res = json.load(open(os.path.join(root, "analysis", "resolve.json")))
    except (OSError, ValueError):
        return "no analysis/resolve.json (run tools/if2_resolve.py): no hooks", []
    hooks, off = hooks_from_resolve(res)
    known = res.get("known")
    want = {"us100": HOOKS, "us104": HOOKS_V104}.get(known)
    if want is not None and hooks != want:
        raise SystemExit("ppu_postlift: the resolved hook table for %s differs from the known one:\n  %s\n  %s"
                         % (known, hooks, want))
    name = {"us100": "disc EBOOT", "us104": "title update 1.04"}.get(known) or \
        "%s (resolved by pattern%s)" % (res.get("eboot", "EBOOT"), "; not found: " + ", ".join(off) if off else "")
    return name, hooks


TAG = "ppu_postlift"


def find_function(gen_dir, func, cont=False):
    if cont:     # the --cont-list copy: static void contf_<addr>(ppu_context* ctx, uint32_t ra) {
        name = func.replace("func_", "contf_", 1)
        head = re.compile(rb"^static void " + re.escape(name.encode())
                          + rb"\(ppu_context\* ctx, uint32_t ra\) \{\r?$", re.M)
    else:
        head = re.compile(rb"^void " + re.escape(func.encode()) + rb"\(ppu_context\* ctx\) \{\r?$", re.M)
    for name in sorted(os.listdir(gen_dir)):
        if not (name.endswith(".cpp") or name.endswith(".c")):
            continue
        path = os.path.join(gen_dir, name)
        with open(path, "rb") as f:
            data = f.read()
        m = head.search(data)
        if m:
            return path, data, m.start()
    return None, None, -1


def apply_hook(gen_dir, func, anchor, hook, why, check, cont=False):
    path, data, start = find_function(gen_dir, func, cont)
    if path is None:
        if cont:
            return "nocopy", ""
        return "MISSING", "%s not found in %s" % (func, gen_dir)
    eol = b"\r\n" if b"\r\n" in data[:4096] else b"\n"
    end = data.find(eol + b"}" + eol, start)
    if end < 0:
        return "MISSING", "end of %s not found" % func
    body = data[start:end]
    call = (hook + "(ctx);").encode()
    if call in body:
        return "present", os.path.basename(path)
    hits = [m.start() for m in re.finditer(re.escape(anchor.encode()), body)]
    if len(hits) != 1:
        return "MISSING", "anchor %r found %d times in %s (lifted code changed?)" % (anchor, len(hits), func)
    if check:
        return "MISSING", "not inserted yet (%s)" % os.path.basename(path)
    line_start = body.rfind(eol, 0, hits[0]) + len(eol)
    indent = re.match(rb"[ \t]*", body[line_start:]).group(0)
    ins_call = indent + call + b"   /* " + TAG.encode() + b": " + why.encode() + b" */" + eol
    linkage = b'extern "C" ' if path.endswith(".cpp") else b"extern "
    proto = linkage + b"void " + hook.encode() + b"(ppu_context* ctx);   /* " + TAG.encode() + b" */" + eol
    abs_line = start + line_start
    new = data[:start] + (proto if proto not in data else b"") + data[start:abs_line] + ins_call + data[abs_line:]
    with open(path, "wb") as f:
        f.write(new)
    return "inserted", os.path.basename(path)


def main(argv):
    check = "--check" in argv
    args = [a for a in argv if not a.startswith("--")]
    gen_dir = args[0] if args else os.path.join("generated", "ppu")
    if not os.path.isdir(gen_dir):
        print("ppu_postlift: no %s" % gen_dir)
        return 1 if check else 0
    missing = 0
    which, hooks = hooks_for(gen_dir)
    print("ppu_postlift: %s" % which)
    for func, anchor, hook, why in hooks:
        status, info = apply_hook(gen_dir, func, anchor, hook, why, check)
        if status == "MISSING":
            missing += 1
        print("ppu_postlift: %-14s %s in %s: %s" % (hook, status, func, info))
        status, info = apply_hook(gen_dir, func, anchor, hook, why, check, cont=True)
        if status != "nocopy":
            if status == "MISSING":
                missing += 1
            print("ppu_postlift: %-14s %s in the continuation copy of %s: %s"
                  % (hook, status, func, info))
    if missing and not check:
        print("ppu_postlift: WARNING %d hook(s) not inserted -- those fixes are absent from this build" % missing)
    return 1 if (check and missing) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
