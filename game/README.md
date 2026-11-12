# game/ — your files go here (git-ignored)

Nothing in this folder is ever committed. Everything below comes from **your own copy** of inFamous 2,
dumped from your own PS3 / disc, and from your own PS3 firmware.

**With the launcher** (`inFamous2Launcher.exe`) you do not arrange anything by hand: on its first run it
asks for the four things below, checks them, and sets this folder up -- the disc's `PS3_GAME` is
**linked** here (a directory junction; the 15 GB stay where they are, and the game only reads them), the
update folder (~230 MB), the decrypted EBOOT and libsre are **copied** here. Your originals are never
changed. "Choose..." / "Change..." in the launcher points it at other files later.

| What | Where it comes from | Check |
|---|---|---|
| Disc folder (holds `PS3_GAME`) | your disc, dumped with your PS3 or a compatible Blu-ray drive (a decrypted ISO can be extracted with 7-Zip or mounted in Windows) | `PS3_GAME\PARAM.SFO` says BCUS98125 (BCES01143 / BCES01144: beta, see below) |
| Update 1.04 folder (`BCUS98125`, holds `PARAM.SFO` + `USRDIR`) | the title update installed in RPCS3 (`<RPCS3>\dev_hdd0\game\BCUS98125`) from your console / PSN packages | `PARAM.SFO` says APP_VER 01.04 |
| Decrypted update EBOOT (`EBOOT.elf`) | RPCS3: Utilities > Decrypt PS3 Binaries on that folder's `USRDIR\EBOOT.BIN` | SHA-1 `7814ff1a87d7d3d10bd0e9627dc05ac353ed4162` (US; 10,605,256 bytes) |
| Decrypted `libsre.prx` | RPCS3 with the official PS3 firmware (4.92 or newer) installed: Utilities > Decrypt PS3 Binaries on `<RPCS3>\dev_flash\sys\external\libsre.sprx` | a PRX named `cellSre_Library` |

**A European copy (beta).** The same four things, with your disc's title id in place of BCUS98125
(`<RPCS3>\dev_hdd0\game\BCES01143` for the update, and so on). Decrypt the **update's** `EBOOT.BIN` -- the
one in the update folder's `USRDIR`, not the disc's: the US update's EBOOT is 10,605,256 bytes, its disc
EBOOT 10,159,776, and step 2 (check) says when an EBOOT's code looks like a disc build. The build finds the
game's addresses in your EBOOT by pattern and logs what it found; after building (and after playing, if
something goes wrong) press **Save report** in the launcher and attach `logs\eu-report-<date>.txt` to
[issue #3](https://github.com/bhuiyah/infamous2-recomp/issues/3). README.md, *European copies (beta)*, says
what is different.

## The layout (what the launcher makes; build by hand to this)

```
game/
├── disc/
│   └── PS3_GAME/            ← the disc's PS3_GAME (linked or copied)
│       ├── PARAM.SFO
│       └── USRDIR/
│           ├── EBOOT.BIN    ← (or EBOOT.elf: needed decrypted only for a -Disc build)
│           └── …            ← everything else from the disc's USRDIR
├── hdd0/
│   └── game/
│       ├── BCUS98125/       ← the title update 1.04
│       │   ├── PARAM.SFO
│       │   └── USRDIR/
│       │       ├── EBOOT.elf ← DECRYPTED update EBOOT
│       │       └── …
│       └── BCUS98125_INSTALL/ ← made by the game on its first start (~2.4 GB)
└── firmware/
    └── libsre.prx           ← DECRYPTED from your firmware's libsre.sprx
```

Then `py tools\check_inputs.py` checks it all (the launcher's "Check your files" step runs it).

## Getting the decrypted files

Retail `EBOOT.BIN` and the firmware's `libsre.sprx` are encrypted SELFs. The recompiler needs the plain ELF.

1. Dump the disc with your PS3 (or a compatible Blu-ray drive). In **RPCS3**, install your PS3 firmware
   (File > Install Firmware, the official `PS3UPDAT.PUP`), then the game and its title updates
   (File > Install Packages, in order up to 1.04).
2. **Utilities > Decrypt PS3 Binaries**: select `dev_hdd0\game\BCUS98125\USRDIR\EBOOT.BIN`; keep the output
   as `EBOOT.elf`.
3. The same for `dev_flash\sys\external\libsre.sprx`; keep the output as `libsre.prx`.

**Why the update.** A title update replaces the disc EBOOT on real hardware, so `tools/check_inputs.py`
selects the update EBOOT when present. The assets still come from `disc/`; the update's own data (its
patch archives in `USRDIR/patch/psarc/`) is read from `hdd0/` like on a PS3. The title-specific code
carries values for the disc EBOOT and for update **1.04** (the last one; `src/if2_version.h`); 1.01-1.03
are refused.

**Why libsre.** inFamous 2 drives its SPU work through SPURS, Sony's SPU task system in the firmware
library libsre. `tools/lift_libsre.sh` recompiles your copy of it, so the game's SPURS calls run Sony's
own code; without it the game stalls while loading. Its output (`generated/prx`) is firmware-derived:
never share it.

**The HDD install.** On first boot the game copies part of the disc to `/dev_hdd0/game/BCUS98125_INSTALL`
(~2.4 GB): an update build keeps it in `hdd0/game/`, a disc build in `disc/game/`. If you switch to the
update after playing without it, link the existing one instead of letting the game install it again:
`mklink /J game\hdd0\game\BCUS98125_INSTALL game\disc\game\BCUS98125_INSTALL`.

**Playing 1.00 with the update installed.** `tools\build.ps1 -Disc` lifts the disc EBOOT (decrypted as
`disc\PS3_GAME\USRDIR\EBOOT.elf`) and leaves the update out (`check_inputs.py --disc`); that build runs
without `hdd0/`, as the disc EBOOT never runs with an update installed. Saves are shared: 1.00 saves load
in 1.04 (keep a copy of `gamedata/dev_hdd0/home/00000001/savedata/BCUS98125*` before switching back).

**DLC.** Copy a DLC package's `USRDIR` into `hdd0/game/BCUS98125/USRDIR` (inFamous 2's DLC is one small
`dlc/ulf/unlock_<item>.edat` per item). Licensed EDATs need the 16-byte RIF key of **your own** licence for
that content id, saved as raw bytes in `gamedata/dev_hdd0/home/00000001/exdata/<content id>.rifkey` (or
`PS3_RIF_KEY=<32 hex chars>`). Turning your `.rap` into that key is up to you; nothing in this repository
does it. Without the key the game treats the items as not owned, as a PS3 without the licence does.
