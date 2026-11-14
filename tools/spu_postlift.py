#!/usr/bin/env python3
"""spu_postlift.py -- make listed TAIL branches of the lifted SPU job modules go through the dispatcher.

tools/lift_jobs.sh makes listed CALLS replaceable (spu_lifter.py --indirect-calls): a `brsl` to a listed address
goes through spu_indirect_branch, where spu_registry_replace can swap the callee (a native rewrite in src/). A
function a module reaches by a plain `br` (a tail branch) is lifted as a trampoline to its lifted body instead:

    { ctx->pc = 0x4030; g_spu_trampoline_fn = <module>_spu_func_00004030; goto ...; }

This rewrites that trampoline target to spu_indirect_branch, which looks ctx->pc up in the registry -- the same
function unless a native replaced it. Nothing else changes. Idempotent; a listed site that is not found is
reported and left alone (the lift then simply keeps the direct branch).

    SPU_JOBMOD_INDIRECT_TAILS="<module>:<addr>,<addr>;<module>:..."   (tools/lift_jobs.sh sets it)
    py tools/spu_postlift.py [generated/spu_jobs]
"""
import os
import re
import sys


def main(argv):
    gen = argv[1] if len(argv) > 1 else os.path.join("generated", "spu_jobs")
    spec = os.environ.get("SPU_JOBMOD_INDIRECT_TAILS", "")
    rc = 0
    for part in spec.split(";"):
        if ":" not in part:
            continue
        mod, addrs = part.split(":", 1)
        mod = mod.strip()
        path = os.path.join(gen, mod, "spu_recomp.c")
        if not os.path.exists(path):
            print("spu_postlift: %s: no %s (not lifted?)" % (mod, path))
            rc = 1
            continue
        data = open(path, "rb").read()
        for a in (x.strip() for x in addrs.split(",") if x.strip()):
            v = int(a, 0)
            direct = ("g_spu_trampoline_fn = %s_spu_func_%08X;" % (mod, v)).encode()
            site = ("ctx->pc = 0x%X; " % v).encode() + direct
            n = data.count(site)
            if n == 0:
                done = ("ctx->pc = 0x%X; g_spu_trampoline_fn = spu_indirect_branch;" % v).encode()
                print("spu_postlift: %s 0x%X: %s" % (mod, v, "already through the dispatcher" if done in data
                                                     else "no tail branch found (left alone)"))
                continue
            data = data.replace(site, ("ctx->pc = 0x%X; g_spu_trampoline_fn = spu_indirect_branch;" % v).encode())
            print("spu_postlift: %s 0x%X: %d tail branch(es) now through the dispatcher" % (mod, v, n))
        open(path, "wb").write(data)
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))
