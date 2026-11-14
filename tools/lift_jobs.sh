#!/usr/bin/env bash
# SPU side of SPURS for inFamous 2 (needs tools/lift_libsre.sh first):
#   1. the 27 WWS job modules linked into the EBOOT  -> generated/spu_jobs/
#   2. policy modules captured at LS 0xA00            -> generated/spu_pm/
#      (analysis/pm/ovl_<ea>.bin, from run.ps1 -DumpSpu / SPU_DUMP_OVL=<dir>,0xA00)
set -euo pipefail
# SPU lifts group basic blocks into call-graph regions (in-region branches are
# gotos instead of dispatcher hops); SPU_LIFT_REGIONS=0 restores the old lift.
export SPU_LIFT_REGIONS="${SPU_LIFT_REGIONS:-1}"
cd "$(dirname "$0")/.."
if [ -z "${PYTHON:-}" ]; then
    if python3 -c "" >/dev/null 2>&1; then PYTHON=python3; else PYTHON=python; fi
fi
PY="$PYTHON"
SDK="${PS3RECOMP:-external/ps3recomp}"

"$PY" tools/extract_eboot_jobs.py analysis/inputs.json --out analysis/jobmods
rm -rf generated/spu_jobs
# Edge Geometry (job 222): its stage calls go through the dispatcher so src/if2_edge.cpp can
# replace them at run time (spu_lifter.py --indirect-calls; other modules unchanged).
# The BSP clipper of job 216 (0x68D8: src/if2_bsp_native.c), the render-list builder's main of job 225
# (0x12C60) and the scene walker's worker of job 209 (0x4358: src/if2_scene_native.c) likewise.
export SPU_JOBMOD_INDIRECT_CALLS="${SPU_JOBMOD_INDIRECT_CALLS-jobmod_d0d15ec83992_ea2a0:0x5740,0x4448,0xAD48,0x5490,0xD0A8,0xD030,0xC7B8,0xA798,0xA3C8;jobmod_9cb47b0cc0eb_e60c0:0x68D8;jobmod_ed956f3d8d84_e14ff0:0x12C60;jobmod_867e5d2348b3_e2898:0x4358}"
# The job modules (and the JobChain binaries: lift_jobchain.py sets it itself)
# keep SPU registers in C locals (spu_lifter.py --reg-cache); SPU_LIFT_RCACHE=0
# restores the ctx->gpr lift. Not used for the policy modules below.
SPU_LIFT_RCACHE="${SPU_LIFT_RCACHE:-1}" \
"$PY" "$SDK/tools/lift_jobmods.py" --jobmods analysis/jobmods --out generated/spu_jobs \
    --register generated/spu_jobs/jobmods_register.c --symbol-prefix infamous2
# lifted sources include "../../runtime/spu/x.h"; make them bare like the rest
"$PY" - <<'PYEOF'
import glob, sys
sys.path.insert(0, "external/ps3recomp/tools")
from build_spu_workloads import debase_includes
for p in glob.glob("generated/spu_jobs/*/spu_recomp.[ch]"):
    debase_includes(p)
PYEOF
# Tail branches a native rewrite replaces (a plain br, not a brsl: the culler's worker of job 207 at 0x4030,
# src/if2_cull_native.c): through the dispatcher too (tools/spu_postlift.py).
export SPU_JOBMOD_INDIRECT_TAILS="${SPU_JOBMOD_INDIRECT_TAILS-jobmod_7393701cbd39_e1390:0x4030}"
"$PY" tools/spu_postlift.py generated/spu_jobs

if ls analysis/pm/*.bin >/dev/null 2>&1; then
    rm -rf generated/spu_pm
    # register caching is verified for spu_jobs/spu_jobchain only
    SPU_LIFT_RCACHE=0 "$PY" tools/lift_policy.py --dumps analysis/pm --out generated/spu_pm
else
    echo "(no analysis/pm/*.bin yet -- run tools/run.ps1 -DumpSpu, then re-run this)"
fi

# SPURS JobChain job binaries (post-processing etc.), lifted position-independent:
# the JobChain policy module loads them at varying LS addresses.
rm -rf generated/spu_jobchain
"$PY" tools/lift_jobchain.py analysis/inputs.json --out generated/spu_jobchain
echo "Rebuild: powershell -ExecutionPolicy Bypass -File tools\\build.ps1 -BuildOnly"
