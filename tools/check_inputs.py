#!/usr/bin/env python3
"""Validate the game files YOU supplied before anything gets lifted.

Expected layout (see game/README.md):

    game/disc/PS3_GAME/PARAM.SFO
    game/disc/PS3_GAME/USRDIR/EBOOT.elf      <- decrypted (RPCS3 "Decrypt PS3 Binaries")
    game/disc/PS3_GAME/USRDIR/...            <- the rest of the disc's USRDIR
    game/hdd0/game/<TITLE_ID>/...            <- optional: installed title update
    game/firmware/libsre.prx                 <- decrypted SPURS library from your PS3 firmware

Checks: the SFO parses and names a known inFamous 2 release; the EBOOT is a
decrypted big-endian PPC64 ELF and not a still-encrypted SELF, and whether its
SHA-1 is one of the known decrypted retail EBOOTs; whether a title update is
present and which EBOOT the pipeline will therefore lift; and the decrypted
libsre.prx that tools/lift_libsre.sh lifts.

Editions: the US disc (BCUS98125) with update 1.04 is the supported one. The
European discs (BCES01143 Europe, BCES01144 UK) are BETA: their EBOOT's title
addresses are found by pattern (tools/if2_resolve.py), and an EBOOT this script
does not know by SHA-1 is let through only when that resolver finds the game's
structure in it (a probe, printed here). Everything else stops, unless
IF2_ALLOW_UNKNOWN_EBOOT=1 (development). Writes the result (with the EBOOT's SHA-1, so
lifts are reproducible) to analysis/inputs.json, which tools/relift.sh reads.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Retail inFamous 2 releases seen in public databases. Others may exist
# (Collection / Greatest Hits re-releases). Only BCUS98125 is supported: see SUPPORT below.
KNOWN_IDS = {
    "BCUS98125": "inFamous 2 (North America, disc) -- reference target",
    "BCES01143": "inFamous 2 (Europe, disc)",
    "BCES01144": "inFamous 2 (UK, disc)",
}
# Every edition the game itself knows (its SKU list, 1.04 EBOOT 0x744B02), for the message below.
EDITIONS = {
    "BCUS98125": "North America", "BCES01143": "Europe", "BCES01144": "UK", "BCES01229": "Turkey",
    "BCAS20181": "Asia", "BCJS30075": "Japan", "NPUA70156": "North America, PSN demo",
    "NPEA90095": "Europe, PSN demo", "NPEA90096": "Europe, PSN demo", "NPHA80154": "Asia, PSN demo",
    "NPJA90193": "Japan, PSN demo", "NPUA70154": "North America, PSN demo", "NPUA70146": "North America, PSN demo",
}
# The European discs: BETA (issue #3). Their title addresses are found by pattern; see the docstring.
BETA_EDITIONS = {"BCES01143": "Europe", "BCES01144": "UK"}
# An EBOOT that is not one of KNOWN_EBOOTS (and not a beta edition's whose structure resolves) is an error: it
# would fail later and less clearly. IF2_ALLOW_UNKNOWN_EBOOT=1 makes it a warning (development).
SUPPORT = ("This build supports the US edition, BCUS98125, with title update 1.04 (or its 1.00 disc EBOOT), and, "
           "as a beta, the European editions BCES01143 (Europe) and BCES01144 (UK). Russian, German, Italian, "
           "Polish and the other European languages are also available on a US copy: PC Settings > System > "
           "Language edition: Europe.")
ALLOW_UNKNOWN_ENV = "IF2_ALLOW_UNKNOWN_EBOOT"


def edition_of(title_id: str) -> str:
    name = EDITIONS.get(title_id)
    return f"{title_id} ({name})" if name else f"{title_id} (not an edition this port knows)"

EM_PPC64 = 21
ET_SCE_PPURELEXEC = 0xFFA4   # a PRX

# SHA-1 of the decrypted EBOOT.elf ("Decrypt PS3 Binaries" in RPCS3) of the builds this port
# targets. Another hash is a warning: a different region or build, or a damaged dump.
KNOWN_EBOOTS = {
    "f1c20717d6532dfb27e292a5db6cdf5a3218760b": "BCUS98125 disc EBOOT (v02.00, APP_VER 01.00)",
    "7814ff1a87d7d3d10bd0e9627dc05ac353ed4162": "BCUS98125 title update 1.04 EBOOT",
}
# Beta: EBOOTs of the European editions players have reported (issue #3), by SHA-1. Their addresses are found
# by pattern like any other's; this only names them.
BETA_EBOOTS = {
    "4bfff4699fb607fe6864bc54d564c7ecb2324364": ("BCES01143", "Europe, beta (the EBOOT reported as update 1.04's)"),
}
# The US builds' sizes, to tell a player which one an unknown EBOOT resembles
US_SIZES = {10159776: "the US disc EBOOT (1.00)", 10605256: "the US update 1.04 EBOOT"}
# The decrypted libsre.prx the port was developed with. Other firmware versions may work.
KNOWN_LIBSRE = {
    "c2f5d2dbf1c58eadcc3367dc34b28f80c4251a67": "libsre.prx (cellSre_Library) the port was developed with",
}
# APP_VERs the title hooks have values for (src/if2_version.h); None = the disc EBOOT.
SUPPORTED_UPDATES = (None, "01.04")


def parse_sfo(path: Path) -> dict[str, object]:
    data = path.read_bytes()
    if data[:4] != b"\x00PSF":
        raise ValueError(f"{path} is not a PARAM.SFO (bad magic)")
    _ver, key_tab, data_tab, count = struct.unpack_from("<IIII", data, 4)
    out: dict[str, object] = {}
    for i in range(count):
        k_off, fmt, length, _maxlen, d_off = struct.unpack_from("<HHIII", data, 20 + i * 16)
        k_start = key_tab + k_off
        key = data[k_start:data.index(b"\x00", k_start)].decode("ascii", "replace")
        raw = data[data_tab + d_off:data_tab + d_off + length]
        if fmt == 0x0404:  # int32
            out[key] = struct.unpack("<I", raw[:4])[0]
        else:              # utf-8 (0x0204 null-terminated / 0x0004 special)
            out[key] = raw.rstrip(b"\x00").decode("utf-8", "replace")
    return out


def classify_exe(path: Path) -> tuple[str, str]:
    head = path.read_bytes()[:64]
    if head[:4] == b"SCE\x00":
        return "self", ("still an encrypted SELF. Decrypt it first: RPCS3 -> Utilities -> "
                        "Decrypt PS3 Binaries (needs RPCS3 to have the game/update installed "
                        "with its licence), then save the result as EBOOT.elf.")
    if head[:4] != b"\x7fELF":
        return "unknown", "not an ELF or SELF -- wrong file?"
    if head[4] != 2 or head[5] != 2:
        return "bad", "ELF but not 64-bit big-endian; this is not a PS3 PPU executable"
    machine = struct.unpack_from(">H", head, 18)[0]
    if machine != EM_PPC64:
        return "bad", f"ELF e_machine={machine}, expected {EM_PPC64} (PPC64)"
    return "ok", "decrypted PPC64 ELF"


def check_libsre(path: Path) -> tuple[bool, str]:
    data = path.read_bytes()
    if data[:4] == b"SCE\x00":
        return False, ("still encrypted (libsre.sprx). Decrypt it: RPCS3 -> Utilities -> Decrypt PS3 "
                       "Binaries on <RPCS3>/dev_flash/sys/external/libsre.sprx, and keep the output as libsre.prx")
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 2:
        return False, "not a decrypted PS3 PRX (a 64-bit big-endian ELF)"
    if struct.unpack_from(">H", data, 16)[0] != ET_SCE_PPURELEXEC:
        return False, "an ELF, but not a PRX module"
    if b"cellSre_Library" not in data:
        return False, "a PRX, but not libsre (cellSre_Library)"
    return True, "decrypted libsre PRX"


def resolver_probe(eboot: Path, title_id: str):
    """tools/if2_resolve.py --probe on an EBOOT this script does not know: does the game's structure resolve in
    it? -> {"ok", "summary", "family", ...} or None when the resolver cannot run."""
    import subprocess
    import tempfile
    out = Path(tempfile.gettempdir()) / f"if2_probe_{os.getpid()}.json"
    try:
        r = subprocess.run([sys.executable, str(ROOT / "tools" / "if2_resolve.py"), "resolve", "--probe",
                            "--eboot", str(eboot), "--title-id", title_id, "--out-json", str(out)],
                           capture_output=True, text=True, timeout=600)
        for line in r.stdout.splitlines():
            print("    " + line)
        p = json.loads(out.read_text())
    except (OSError, ValueError, subprocess.SubprocessError) as e:
        print(f"[!] the structure probe did not run ({e})")
        return None
    finally:
        try:
            out.unlink()
        except OSError:
            pass
    c = p.get("core", {})
    found, amb, total = c.get("found", 0), c.get("ambiguous", 0), c.get("total", 1)
    # The game's structure resolves: most of the core items (what both US builds have) are found, none of them in
    # two places (an ambiguous core item means the patterns do not fit this code well). The probe decodes
    # without the lift's function list, so a few values that need it are only checked in the lift step.
    p["ok"] = found >= 0.75 * total and amb == 0
    p["summary"] = (f"{found} of {total} core title items found, {amb} ambiguous; layout like the US "
                    f"{'update 1.04' if p.get('family') == 104 else 'disc EBOOT (1.00)'}")
    return p


def sha1(path: Path) -> str:
    h = hashlib.sha1()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def find_eboot(usrdir: Path) -> Path | None:
    for name in ("EBOOT.elf", "EBOOT.ELF", "EBOOT.BIN"):
        if (usrdir / name).exists():
            return usrdir / name
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--game", type=Path, default=ROOT / "game",
                    help="directory holding disc/ and (optionally) hdd0/ (default: game/)")
    ap.add_argument("--disc", action="store_true",
                    help="lift the disc EBOOT even when a title update is installed (game/hdd0); "
                         "the build then runs without the update (tools/run.ps1, play mode)")
    ap.add_argument("--libsre", type=Path, default=None,
                    help="the decrypted libsre.prx (default: game/firmware/libsre.prx)")
    args = ap.parse_args()

    errors, warnings = [], []
    disc = args.game / "disc" / "PS3_GAME"
    sfo_path = disc / "PARAM.SFO"
    if not sfo_path.exists():
        print(f"[x] {sfo_path} not found -- copy PS3_GAME from your disc dump into game/disc/")
        return 1

    sfo = parse_sfo(sfo_path)
    title_id = str(sfo.get("TITLE_ID", "?"))
    print(f"[i] PARAM.SFO: {sfo.get('TITLE', '?')}  id={title_id}  "
          f"disc version={sfo.get('VERSION', '?')}  app_ver={sfo.get('APP_VER', '?')}")
    allow_unknown = os.environ.get(ALLOW_UNKNOWN_ENV, "") == "1"
    beta = title_id in BETA_EDITIONS
    if title_id == "BCUS98125":
        print(f"[ok] {KNOWN_IDS[title_id]}")
    elif beta:
        print(f"[ok] {KNOWN_IDS[title_id]} -- BETA: European support is new and tested only on players' copies; "
              f"the build finds the game's addresses in your EBOOT by pattern and logs what it found")
    else:
        (warnings if allow_unknown else errors).append(
            f"your disc is {edition_of(title_id)}. {SUPPORT}"
            + (f" ({ALLOW_UNKNOWN_ENV}=1: going on anyway -- the pipeline will likely fail later.)" if allow_unknown
               else f" (Developers: {ALLOW_UNKNOWN_ENV}=1 makes this a warning.)"))

    disc_eboot = find_eboot(disc / "USRDIR")
    if not disc_eboot:
        errors.append("no EBOOT.elf in game/disc/PS3_GAME/USRDIR")

    # A title update replaces the disc EBOOT at runtime on real hardware. Lift
    # the newest one or the code you recompile is not the code the game ships.
    upd_dir = args.game / "hdd0" / "game" / title_id
    upd_eboot = find_eboot(upd_dir / "USRDIR") if upd_dir.exists() else None
    upd_ver = None
    if (upd_dir / "PARAM.SFO").exists():
        upd_ver = parse_sfo(upd_dir / "PARAM.SFO").get("APP_VER")
    if upd_dir.exists() and not upd_eboot:
        warnings.append(f"{upd_dir} exists but has no USRDIR/EBOOT -- update ignored")

    if args.disc and upd_eboot:
        print(f"[i] --disc: the title update in {upd_dir.relative_to(args.game)} is left out of this build")
        upd_eboot, upd_ver = None, None
    target = upd_eboot or disc_eboot
    result: dict[str, object] = {"title_id": title_id, "sfo": sfo}
    if target:
        kind, msg = classify_exe(target)
        label = "update" if target is upd_eboot else "disc"
        print(f"[{'ok' if kind == 'ok' else 'x'}] {label} EBOOT {target.relative_to(args.game)}: {msg}")
        if kind != "ok":
            errors.append(f"{target.name}: {msg}")
        else:
            digest = sha1(target)
            size = target.stat().st_size
            print(f"[i] sha1 {digest}  size {size:,} bytes")
            probe = None
            if digest not in KNOWN_EBOOTS:
                probe = resolver_probe(target, title_id)
                if probe is not None:
                    result["resolve_probe"] = probe
            if probe is not None and probe["ok"] and label == "update" and probe.get("family") == 100:
                warnings.append(
                    f"the update's EBOOT ({size:,} bytes) looks like a disc (1.00) build, not 1.04: its code matches "
                    f"the US disc EBOOT's layout" + (f" (and it is the size of {US_SIZES[size]})" if size in US_SIZES
                                                     else "") +
                    ". If you meant the update, decrypt game/hdd0/game/" + title_id + "/USRDIR/EBOOT.BIN (the "
                    "update's own) and put that EBOOT.elf there. The build goes on with this one, and the game then runs "
                    "as the disc version, without the update's files (as a PS3 runs the disc EBOOT).")
            if digest in KNOWN_EBOOTS:
                print(f"[ok] {KNOWN_EBOOTS[digest]}")
            elif beta and probe is not None and probe["ok"]:
                name = BETA_EBOOTS[digest][1] if digest in BETA_EBOOTS else f"{edition_of(title_id)}, beta"
                print(f"[ok] {name}: the game's structure resolves in it ({probe['summary']}) -- BETA")
            elif beta and probe is not None:
                (warnings if allow_unknown else errors).append(
                    f"the {label} EBOOT (SHA-1 {digest}) is the {edition_of(title_id)} edition's, but the game's "
                    f"structure does not resolve in it ({probe['summary']}): it is not an inFamous 2 EBOOT this "
                    f"build can use (a damaged or wrongly decrypted dump also lands here). Send the launcher's "
                    f"report (Save report) to the beta thread."
                    + (f" ({ALLOW_UNKNOWN_ENV}=1: going on anyway.)" if allow_unknown
                       else f" (Developers: {ALLOW_UNKNOWN_ENV}=1 makes this a warning.)"))
            else:
                (warnings if allow_unknown else errors).append(
                    f"the {label} EBOOT {target.relative_to(args.game)} (SHA-1 {digest}) is not a supported "
                    f"build: the title hooks have addresses for the BCUS98125 disc EBOOT and update 1.04 only "
                    f"(the disc here: {edition_of(title_id)}; a damaged or wrongly decrypted dump also lands "
                    f"here). {SUPPORT}"
                    + (f" ({ALLOW_UNKNOWN_ENV}=1: going on anyway.)" if allow_unknown
                       else f" (Developers: {ALLOW_UNKNOWN_ENV}=1 makes this a warning.)"))
            result.update(eboot=str(target.resolve()), eboot_source=label,
                          eboot_sha1=digest, update_app_ver=upd_ver, eboot_size=size,
                          edition=("North America" if title_id == "BCUS98125" else
                                   BETA_EDITIONS.get(title_id, EDITIONS.get(title_id, title_id))),
                          beta=title_id != "BCUS98125" or digest not in KNOWN_EBOOTS)
        disc_layout = (result.get("resolve_probe") or {}).get("family") == 100
        if target is upd_eboot and disc_layout:
            print(f"[i] lifting the EBOOT in the update folder (APP_VER {upd_ver}) -- a disc-layout build: it runs "
                  "without the update's files (see the [!] below)")
        elif target is upd_eboot:
            print(f"[i] lifting the title-update EBOOT (APP_VER {upd_ver}); disc assets still "
                  "come from game/disc. Set PS3_HDD0_ROOT=game/hdd0 at runtime.")
            if upd_ver not in SUPPORTED_UPDATES and not beta:
                errors.append(f"title update APP_VER {upd_ver}: the title hooks have values for the disc "
                              "EBOOT and update 1.04 only -- install every update up to 1.04 "
                              "(game/README.md), or build the disc EBOOT with tools\\build.ps1 -Disc")
        elif not upd_dir.exists():
            warnings.append("no title update found -- you will be lifting the 1.00 disc EBOOT. "
                            "Fine for bring-up, but install and decrypt update 1.04 (the target "
                            "of this port) before playing.")

    # The real SPURS library (tools/lift_libsre.sh): without it the game stalls loading.
    libsre = args.libsre or (args.game / "firmware" / "libsre.prx")
    if libsre.exists():
        ok, msg = check_libsre(libsre)
        print(f"[{'ok' if ok else 'x'}] {libsre.name}: {msg}")
        if ok:
            digest = sha1(libsre)
            print(f"[i] sha1 {digest}")
            if digest in KNOWN_LIBSRE:
                print(f"[ok] {KNOWN_LIBSRE[digest]}")
            else:
                # (issue #4: a player read this note as the cause of a failed build. It never stops anything.)
                print("[ok] libsre.prx: decrypted; from another firmware build than the tested one -- this is fine "
                      "and does not stop the build")
            result.update(libsre=str(libsre.resolve()), libsre_sha1=digest)
        else:
            errors.append(f"{libsre.name}: {msg}")
    elif (ROOT / "generated" / "prx" / "libsre.linked.bin").exists():
        print("[i] no libsre.prx given; the libsre lifted earlier (generated/prx) is kept")
    else:
        errors.append(f"{libsre} not found -- the game needs your decrypted libsre.prx (game/README.md)")

    for w in warnings:
        print(f"[!] {w}")
    for e in errors:
        print(f"[x] {e}")

    if errors:
        return 1
    out = ROOT / "analysis" / "inputs.json"
    out.parent.mkdir(exist_ok=True)
    out.write_text(json.dumps(result, indent=2, default=str))
    print(f"[ok] wrote {out.relative_to(ROOT)} -- next: tools/analyze.sh")
    return 0


if __name__ == "__main__":
    sys.exit(main())
