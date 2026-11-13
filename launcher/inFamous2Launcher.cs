// inFamous2Launcher -- a small Windows front end for this repository's own build and run scripts.
//
// First run: asks for YOUR game files (disc, title update 1.04, the decrypted EBOOT.elf and the decrypted
// firmware libsre.prx), checks them, checks the build prerequisites (offering a winget install for each
// missing one, only after you confirm), then runs tools\build.ps1 one step at a time -- setup, check, analyze,
// lift, libsre, jobs, build -- and starts the game the way tools\run.ps1 does once the build succeeds.
// Later runs: Play when the build is up to date with the sources, the toolkit pin and patch, and your files.
//
// It contains no game code (setup.ps1 clones the ps3recomp toolkit from GitHub; winget installs only what you
// confirm). At startup it asks GitHub, in the background, whether a newer release of this port exists (one
// request to api.github.com; nothing about the player is sent) and offers it; it downloads a release only when
// the player presses Update now. Build it with tools\build_launcher.cmd (the .NET Framework's own csc.exe, C# 5).
// Every path is relative to the folder the launcher sits in.
//
// Command line (all optional; for shortcuts and scripted tests):
//   --disc <dir> --update <dir> --eboot <EBOOT.elf> --libsre <libsre.prx | RPCS3 folder>   prefill the files dialog
//   --detect-out <file>     write the prerequisite / game-file / build-state report to <file> and exit
//   --report-out <file>     write the support report (what "Save report" saves: the EBOOT / libsre identity, the
//                           resolver's report, the build state, the newest logs' tails; personal paths replaced by
//                           %USERPROFILE%) to <file> and exit
//   --update-check-out <file>  ask GitHub for the latest release, write what was understood to <file> and exit
//   --winget-dry-run        Install buttons log the winget command instead of running it
//   --mark-built            (scripted tests) record every step whose output exists as done in this folder, as a
//                           build by the launcher would, and exit
//   --pretend-missing <ids> show these prerequisites as missing (git,python,cmake,ninja,vs,llvm): UI tests
// Headless (no window; for other launchers such as RecompLauncher, and scripts). Any of --prepare, --build, --play
// runs without the window, in that order, writing progress lines to stdout (from a console: start /wait, or pipe it):
//   --prepare               check the game files and set up game\ exactly as the files dialog's "Use these files"
//                           does (the disc linked, the update, EBOOT and libsre copied). The files come from
//                           --game <folder> (searched for the disc, the update and libsre) and/or --disc, --update,
//                           --eboot, --libsre (which win); libsre is also looked for in the RPCS3 folder the update
//                           sits in, and an earlier game\firmware\libsre.prx is kept
//   --build                 run the build steps that are not up to date (tools\build.ps1 -Step <id>, as the window)
//   --yes                   allow the first build's toolkit download (without it --build stops and says what it is)
//   --play                  start the game (tools\run.ps1) once the build is up to date; waits for it to close
//   Output lines: "PROGRESS <step> <n>/<total>", "progress <0..1> <text>" (RecompLauncher's prepare format),
//   "INFO ...", "WARN ...", "ERROR <message>", "OK <what>", the build steps' own output, and last "EXIT <code>".
//   Exit codes: 0 done, 1 bad command line, 2 game files missing or wrong, 3 setting up game\ failed, 4 build tools
//   missing, 5 the toolkit download needs --yes, 6 a build step failed (or was stopped), 7 cannot play (not built,
//   not up to date, the game is running), 8 the folder cannot be used; once --play started the game, the exit code
//   of tools\run.ps1 (as the window's Play).
//   Options also take the form --name=value (--eboot=C:\dumps\EBOOT.elf).
// Used by the update itself: --apply-update <plan> (the updater: a temp copy of this exe), --updated <old version>,
// --update-failed, --update-log <file> (the restarted launcher).
// Environment: IF2_TOOLKIT_URL=<git url or folder> clones the toolkit from there instead of GitHub (tests).
// INF2L_NO_UPDATE_CHECK=1 turns the startup check off. For tests only: INF2L_UPDATE_API=<url> asks
// that URL instead of the GitHub API (and then allows downloads from anywhere it names), INF2L_UPDATE_TEST_FAIL=
// <release-relative path> makes the updater fail right after writing that file (to exercise the undo).
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Net;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Windows.Forms;
using Microsoft.Win32;

[assembly: System.Reflection.AssemblyTitle("inFamous 2 recomp launcher")]
[assembly: System.Reflection.AssemblyProduct("infamous2-recomp")]
[assembly: System.Reflection.AssemblyDescription("Builds and starts the inFamous 2 static recompilation from your own game files")]
[assembly: System.Reflection.AssemblyCopyright("bhuiyah and infamous2-recomp contributors, MIT")]
[assembly: System.Reflection.AssemblyVersion(Infamous2Launcher.AppInfo.Version + ".0")]
[assembly: System.Reflection.AssemblyFileVersion(Infamous2Launcher.AppInfo.Version + ".0")]
[assembly: System.Reflection.AssemblyInformationalVersion(Infamous2Launcher.AppInfo.Version)]

namespace Infamous2Launcher
{
    static class AppInfo
    {
        // The release this launcher belongs to: tools\make_release.ps1 refuses a tag that does not match it.
        public const string Version = "0.1.35";
        public const string Repo = "bhuiyah/infamous2-recomp";
        public const string ApiLatest = "https://api.github.com/repos/" + Repo + "/releases/latest";
        public const string ReleasesPage = "https://github.com/" + Repo + "/releases";
        public static string UserAgent { get { return "inFamous2Recomp-launcher/" + Version; } }
    }

    // ------------------------------------------------------------------ options
    class Options
    {
        public string Disc, Update, Eboot, Libsre, DetectOut, ReportOut, UpdateCheckOut, ApplyUpdate, Updated, UpdateLog;
        public bool WingetDryRun, UpdateFailed, MarkBuilt;
        public HashSet<string> PretendMissing = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        // headless (Headless below)
        public string Game;
        public bool Prepare, Build, Play, Yes, Help;
        public List<string> Unknown = new List<string>();
        public bool IsHeadless { get { return Prepare || Build || Play; } }

        public static Options Parse(string[] a)
        {
            var o = new Options();
            for (int i = 0; i < a.Length; i++)
            {
                string k = a[i], inline = null;
                // --name=value: what RecompLauncher passes for an optional input ("--eboot={input:eboot}")
                int eq = k.StartsWith("--") ? k.IndexOf('=') : -1;
                if (eq > 2) { inline = k.Substring(eq + 1); k = k.Substring(0, eq); }
                k = k.ToLowerInvariant();
                Func<string> next = () => inline != null ? inline : (i + 1 < a.Length) ? a[++i] : "";
                if (k == "--game") o.Game = next();
                else if (k == "--prepare") o.Prepare = true;
                else if (k == "--build") o.Build = true;
                else if (k == "--play") o.Play = true;
                else if (k == "--yes") o.Yes = true;
                else if (k == "--help" || k == "-h" || k == "/?") o.Help = true;
                else if (k == "--disc") o.Disc = next();
                else if (k == "--update") o.Update = next();
                else if (k == "--eboot") o.Eboot = next();
                else if (k == "--libsre") o.Libsre = next();
                else if (k == "--detect-out") o.DetectOut = next();
                else if (k == "--report-out") o.ReportOut = next();
                else if (k == "--update-check-out") o.UpdateCheckOut = next();
                else if (k == "--apply-update") o.ApplyUpdate = next();
                else if (k == "--updated") o.Updated = next();
                else if (k == "--update-log") o.UpdateLog = next();
                else if (k == "--update-failed") o.UpdateFailed = true;
                else if (k == "--winget-dry-run") o.WingetDryRun = true;
                else if (k == "--mark-built") o.MarkBuilt = true;
                else if (k == "--pretend-missing")
                {
                    foreach (var s in next().Split(',')) if (s.Trim().Length > 0) o.PretendMissing.Add(s.Trim());
                }
                else o.Unknown.Add(a[i]);
            }
            if (Environment.GetEnvironmentVariable("INF2L_WINGET_DRYRUN") == "1") o.WingetDryRun = true;
            return o;
        }
    }

    // ------------------------------------------------------------------ helpers
    static class Util
    {
        public static string Sha1File(string path)
        {
            using (var s = File.OpenRead(path)) using (var h = SHA1.Create())
                return Hex(h.ComputeHash(s));
        }
        public static string Sha1(string text)
        {
            using (var h = SHA1.Create()) return Hex(h.ComputeHash(Encoding.UTF8.GetBytes(text)));
        }
        public static string Hex(byte[] b)
        {
            var sb = new StringBuilder(b.Length * 2);
            foreach (var x in b) sb.Append(x.ToString("x2"));
            return sb.ToString();
        }
        // path, size and write time: cheap identity for big files (the EBOOT, the PRX)
        public static string Identity(string path)
        {
            if (path == null || !File.Exists(path)) return "missing:" + (path ?? "");
            var fi = new FileInfo(path);
            return fi.FullName.ToLowerInvariant() + "|" + fi.Length + "|" + fi.LastWriteTimeUtc.Ticks;
        }
        // Runs a short command and returns (exit code, stdout+stderr); -1 when it cannot start or times out.
        public static int Run(string exe, string args, out string output, int timeoutMs = 20000, string cwd = null)
        {
            output = "";
            try
            {
                var psi = new ProcessStartInfo(exe, args)
                {
                    UseShellExecute = false, CreateNoWindow = true,
                    RedirectStandardOutput = true, RedirectStandardError = true
                };
                if (cwd != null) psi.WorkingDirectory = cwd;
                using (var p = Process.Start(psi))
                {
                    var err = p.StandardError.ReadToEndAsync();
                    string o = p.StandardOutput.ReadToEnd();
                    if (!p.WaitForExit(timeoutMs)) { try { p.Kill(); } catch { } return -1; }
                    output = o + err.Result;
                    return p.ExitCode;
                }
            }
            catch { return -1; }
        }
        public static string FindOnPath(string file, string pathVar)
        {
            foreach (var d in (pathVar ?? "").Split(';'))
            {
                if (d.Trim().Length == 0) continue;
                try
                {
                    var p = Path.Combine(Environment.ExpandEnvironmentVariables(d.Trim().Trim('"')), file);
                    if (File.Exists(p)) return p;
                }
                catch { }
            }
            return null;
        }
        // PATH as a new process would see it (after an install), plus this process's own.
        public static string FreshPath()
        {
            string m = (string)Registry.GetValue(@"HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Session Manager\Environment", "Path", "");
            string u = (string)Registry.GetValue(@"HKEY_CURRENT_USER\Environment", "Path", "");
            var parts = new List<string>();
            foreach (var s in new[] { m, u, Environment.GetEnvironmentVariable("PATH") })
                foreach (var d in (s ?? "").Split(';'))
                {
                    var e = Environment.ExpandEnvironmentVariables(d.Trim());
                    if (e.Length > 0 && !parts.Contains(e, StringComparer.OrdinalIgnoreCase)) parts.Add(e);
                }
            return string.Join(";", parts);
        }
        public static string Pf { get { return Environment.GetEnvironmentVariable("ProgramFiles") ?? @"C:\Program Files"; } }
        public static string Pf86 { get { return Environment.GetEnvironmentVariable("ProgramFiles(x86)") ?? @"C:\Program Files (x86)"; } }
        public static string LocalAppData { get { return Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData); } }
        public static void KillTree(int pid)
        {
            string o;
            Run(Path.Combine(Environment.SystemDirectory, "taskkill.exe"), "/PID " + pid + " /T /F", out o, 15000);
        }
        public static string Elapsed(TimeSpan t)
        {
            return t.TotalHours >= 1 ? string.Format("{0}:{1:00}:{2:00}", (int)t.TotalHours, t.Minutes, t.Seconds)
                                     : string.Format("{0}:{1:00}", t.Minutes, t.Seconds);
        }
    }

    // ------------------------------------------------------------------ PS3 file checks (the same rules as tools\check_inputs.py)
    static class Ps3Files
    {
        // Issue #9: an encrypted PS3 disc image (a Redump-style .iso) unpacked with 7-Zip or WinRAR keeps its file
        // system and PARAM.SFO readable but its data encrypted with the disc key, and the game then sits on a black
        // screen after the warning. Every game archive of a decrypted copy starts with "PSAR". The game's first run
        // copies some of them into hdd0\game\<title>_INSTALL, so an install made from encrypted files is encrypted too.
        // Issue #4: the port uses three SPURS policy modules from libsre (tools/extract_policy_modules.py) and the runtime
        // knows them by their addresses in the 4.92 libsre. Firmware 4.80's libsre has other ones there, and the build
        // stops at step 6. Checked here in the unrelocated PRX, where those bytes sit unchanged: address in the image,
        // size, SHA-1 (the same table as the script).
        static readonly Tuple<uint, int, string>[] LibsreModules =
        {
            Tuple.Create(0x21480u, 0x2200, "f41f5ec45be04d84f9d279af5fcfd478542b0240"),
            Tuple.Create(0x23680u, 0x1E40, "dd993498889ae02aa471e3bfaa0623d104b67ff4"),
            Tuple.Create(0x2A180u, 0x3E80, "0b30c7042a95059084c3e460817c7f0a5e68306b"),
        };
        public const string OldLibsreHelp =
            "this libsre.prx is from an older PS3 firmware (its SPURS modules differ from the ones the port uses, so the build " +
            "would stop at step 6). Install the latest official PS3 firmware (4.92 or newer) in RPCS3, decrypt " +
            "dev_flash\\sys\\external\\libsre.sprx again (Utilities > Decrypt PS3 Binaries) and choose the new libsre.prx.";
        public static bool LibsreModulesMatch(string prx)
        {
            try
            {
                byte[] d = File.ReadAllBytes(prx);
                Func<int, ulong> u64 = o => { ulong v = 0; for (int k = 0; k < 8; k++) v = (v << 8) | d[o + k]; return v; };
                Func<int, uint> u32 = o => (uint)(d[o] << 24 | d[o + 1] << 16 | d[o + 2] << 8 | d[o + 3]);
                int phoff = (int)u64(0x20), phentsize = d[0x36] << 8 | d[0x37], phnum = d[0x38] << 8 | d[0x39];
                foreach (var m in LibsreModules)
                {
                    bool found = false;
                    for (int i = 0; i < phnum && !found; i++)
                    {
                        int p = phoff + i * phentsize;
                        ulong off = u64(p + 8), va = u64(p + 16), fsz = u64(p + 32);
                        if (u32(p) != 1 || m.Item1 < va || m.Item1 + (ulong)m.Item2 > va + fsz) continue;
                        int at = (int)(off + (m.Item1 - va));
                        if (at + m.Item2 > d.Length) return false;
                        using (var sha = System.Security.Cryptography.SHA1.Create())
                            if (BitConverter.ToString(sha.ComputeHash(d, at, m.Item2)).Replace("-", "").ToLowerInvariant() != m.Item3) return false;
                        found = true;
                    }
                    if (!found) return false;
                }
                return true;
            }
            catch { return false; }
        }

        public static bool? IsPsar(string path)
        {
            try
            {
                if (!File.Exists(path)) return null;
                var b = new byte[4];
                using (var s = File.OpenRead(path)) if (s.Read(b, 0, 4) < 4) return false;
                return b[0] == 'P' && b[1] == 'S' && b[2] == 'A' && b[3] == 'R';
            }
            catch { return null; }
        }
        public static bool DiscDataEncrypted(string discGame)
        {
            return discGame != null && IsPsar(Path.Combine(discGame, @"USRDIR\cache\psarc\core.psarc")) == false;
        }
        public static string InstallDir(string game, string titleId)
        {
            return Path.Combine(game, @"hdd0\game\" + titleId + "_INSTALL");
        }
        public static bool InstallEncrypted(string installDir)
        {
            return IsPsar(Path.Combine(installDir, @"USRDIR\cache\psarc\install1\infamous1.psarc_s")) == false ||
                   IsPsar(Path.Combine(installDir, @"USRDIR\cache\psarc\install2\infamous2.psarc_s")) == false;
        }
        public const string EncryptedDiscHelp =
            "Your disc files are still encrypted: the game archives (USRDIR\\cache\\psarc\\*.psarc) don't start with \"PSAR\". " +
            "This happens when an encrypted disc image (.iso) is unpacked with 7-Zip or WinRAR: the folders look right but the data " +
            "inside stays encrypted, and the game shows only a black screen after the warning.\r\n\r\n" +
            "Decrypt the .iso first (PS3Dec with the disc's key, or dump the disc in decrypted / folder form), extract it again, " +
            "then choose that folder with Choose... under 1. Your game files.";
        public static readonly Dictionary<string, string> KnownEboots = new Dictionary<string, string>
        {
            { "f1c20717d6532dfb27e292a5db6cdf5a3218760b", "BCUS98125 disc EBOOT (1.00)" },
            { "7814ff1a87d7d3d10bd0e9627dc05ac353ed4162", "BCUS98125 update 1.04 EBOOT" },
            { "4bfff4699fb607fe6864bc54d564c7ecb2324364", "BCES01143 (Europe) EBOOT -- beta" },
        };
        public const string UsUpdateSha1 = "7814ff1a87d7d3d10bd0e9627dc05ac353ed4162";
        // The European discs: beta (issue #3). The build finds the game's addresses in their EBOOT by pattern
        // (tools\if2_resolve.py); an EBOOT not listed above is let through by tools\check_inputs.py when the
        // game's structure resolves in it.
        public static readonly Dictionary<string, string> BetaEditions = new Dictionary<string, string>
        {
            { "BCES01143", "Europe" }, { "BCES01144", "UK" },
        };
        public static bool IsBeta(string id) { return id != null && BetaEditions.ContainsKey(id); }
        // The editions the game knows (its SKU list) -- for the message when a disc is not BCUS98125 (issue #3).
        public static readonly Dictionary<string, string> Editions = new Dictionary<string, string>
        {
            { "BCUS98125", "North America" }, { "BCES01143", "Europe" }, { "BCES01144", "UK" }, { "BCES01229", "Turkey" },
            { "BCAS20181", "Asia" }, { "BCJS30075", "Japan" },
        };
        public static string EditionOf(string id)
        {
            string n;
            return (id ?? "?") + (id != null && Editions.TryGetValue(id, out n) ? " (" + n + ")" : " (not an edition this port knows)");
        }
        public const string Support = "This build supports the US edition, BCUS98125, with title update 1.04, and as a beta " +
            "the European editions BCES01143 (Europe) and BCES01144 (UK). " +
            "Russian, German, Italian, Polish and the other European languages are also available on a US copy: " +
            "PC Settings > System > Language edition: Europe.";
        // tools\check_inputs.py's developer override: an unknown edition / EBOOT is a warning instead.
        public static bool AllowUnknown { get { return Environment.GetEnvironmentVariable("IF2_ALLOW_UNKNOWN_EBOOT") == "1"; } }
        public static readonly Dictionary<string, string> KnownLibsre = new Dictionary<string, string>
        {
            { "c2f5d2dbf1c58eadcc3367dc34b28f80c4251a67", "libsre.prx the port was developed with" },
        };

        public static Dictionary<string, string> ParseSfo(string path)
        {
            var r = new Dictionary<string, string>();
            byte[] d = File.ReadAllBytes(path);
            if (d.Length < 20 || d[0] != 0 || d[1] != 'P' || d[2] != 'S' || d[3] != 'F') throw new InvalidDataException("not a PARAM.SFO");
            int keyTab = BitConverter.ToInt32(d, 8), dataTab = BitConverter.ToInt32(d, 12), count = BitConverter.ToInt32(d, 16);
            for (int i = 0; i < count; i++)
            {
                int e = 20 + i * 16;
                int kOff = BitConverter.ToUInt16(d, e), fmt = BitConverter.ToUInt16(d, e + 2);
                int len = BitConverter.ToInt32(d, e + 4), dOff = BitConverter.ToInt32(d, e + 12);
                int ks = keyTab + kOff, ke = ks;
                while (ke < d.Length && d[ke] != 0) ke++;
                string key = Encoding.ASCII.GetString(d, ks, ke - ks);
                if (fmt == 0x0404) r[key] = BitConverter.ToUInt32(d, dataTab + dOff).ToString();
                else r[key] = Encoding.UTF8.GetString(d, dataTab + dOff, len).TrimEnd('\0');
            }
            return r;
        }
        // "ok" | "self" | "bad"
        public static string ElfKind(string path, out string why)
        {
            byte[] h = new byte[64];
            using (var s = File.OpenRead(path)) s.Read(h, 0, 64);
            if (h[0] == 'S' && h[1] == 'C' && h[2] == 'E' && h[3] == 0)
            { why = "still encrypted (a SELF): decrypt it in RPCS3 (Utilities > Decrypt PS3 Binaries)"; return "self"; }
            if (h[0] != 0x7F || h[1] != 'E' || h[2] != 'L' || h[3] != 'F' || h[4] != 2 || h[5] != 2)
            { why = "not a decrypted PS3 executable"; return "bad"; }
            why = "decrypted"; return "ok";
        }
        public static bool IsLibsre(string path, out string why)
        {
            string k = ElfKind(path, out why);
            if (k != "ok") return false;
            byte[] d = File.ReadAllBytes(path);
            if (d[16] != 0xFF || d[17] != 0xA4) { why = "an ELF, but not a PRX module"; return false; }
            if (Encoding.ASCII.GetString(d).IndexOf("cellSre_Library", StringComparison.Ordinal) < 0)
            { why = "a PRX, but not libsre (cellSre_Library)"; return false; }
            why = "decrypted libsre"; return true;
        }
        public static string FindEboot(string usrdir)
        {
            foreach (var n in new[] { "EBOOT.elf", "EBOOT.ELF" })
                if (File.Exists(Path.Combine(usrdir, n))) return Path.Combine(usrdir, n);
            return null;
        }
    }

    // ------------------------------------------------------------------ the player's files in game\
    class GameFiles
    {
        public readonly string Root;
        public GameFiles(string root) { Root = root; }
        public string Game { get { return Path.Combine(Root, "game"); } }
        public string DiscGame { get { return Path.Combine(Game, @"disc\PS3_GAME"); } }
        public string TitleId
        {
            get
            {
                try { string v; return ParseSfo(Path.Combine(DiscGame, "PARAM.SFO")).TryGetValue("TITLE_ID", out v) ? v : "BCUS98125"; }
                catch { return "BCUS98125"; }
            }
        }
        static Dictionary<string, string> ParseSfo(string p) { return Ps3Files.ParseSfo(p); }
        public string UpdateDir { get { return Path.Combine(Game, @"hdd0\game\" + TitleId); } }
        public string LibsrePath { get { return Path.Combine(Game, @"firmware\libsre.prx"); } }
        // the EBOOT check_inputs.py selects: the update's when there is one
        public string TargetEboot
        {
            get
            {
                string u = Directory.Exists(UpdateDir) ? Ps3Files.FindEboot(Path.Combine(UpdateDir, "USRDIR")) : null;
                return u ?? Ps3Files.FindEboot(Path.Combine(DiscGame, "USRDIR"));
            }
        }
        public bool Present
        {
            get
            {
                return File.Exists(Path.Combine(DiscGame, "PARAM.SFO")) && TargetEboot != null &&
                       (File.Exists(LibsrePath) || File.Exists(Path.Combine(Root, @"generated\prx\libsre.linked.bin")));
            }
        }
        // (relative to the folder: a moved or copied install keeps its fingerprints -- issue #5; the steps whose
        // output names the folder run again through Pipeline.MovedFrom)
        public string Identity()
        {
            string r = Path.GetFullPath(Root).TrimEnd('\\').ToLowerInvariant() + "\\";
            return string.Join("\n", new[] {
                Util.Identity(Path.Combine(DiscGame, "PARAM.SFO")), Util.Identity(Path.Combine(UpdateDir, "PARAM.SFO")),
                Util.Identity(TargetEboot), Util.Identity(LibsrePath) }.Select(x => x.Replace(r, "")));
        }

        public class Finding { public int Level; public string Text; public Finding(int l, string t) { Level = l; Text = t; } }   // 0 ok, 1 warn, 2 error

        // Checks a selection: disc folder (holding PS3_GAME, or PS3_GAME itself), the update folder, an EBOOT.elf
        // given separately (or null), and libsre.prx (or an RPCS3 folder holding the decrypted one).
        public static List<Finding> Check(string disc, string update, string eboot, string libsre,
                                          out string discGame, out string ebootPath, out string libsrePath)
        {
            var f = new List<Finding>();
            discGame = ebootPath = libsrePath = null;
            // disc
            if (string.IsNullOrEmpty(disc)) f.Add(new Finding(2, "Disc folder: not chosen."));
            else
            {
                string g = Directory.Exists(Path.Combine(disc, "PS3_GAME")) ? Path.Combine(disc, "PS3_GAME") : disc;
                string sfo = Path.Combine(g, "PARAM.SFO");
                if (!File.Exists(sfo)) f.Add(new Finding(2, "Disc folder: no PS3_GAME\\PARAM.SFO under " + disc));
                else
                {
                    try
                    {
                        var s = Ps3Files.ParseSfo(sfo);
                        string id; s.TryGetValue("TITLE_ID", out id);
                        if (id == "BCUS98125") f.Add(new Finding(0, "Disc: inFamous 2, BCUS98125 (North America)."));
                        else if (Ps3Files.IsBeta(id))
                            f.Add(new Finding(0, "Disc: inFamous 2, " + id + " (" + Ps3Files.BetaEditions[id] + ") -- BETA: European support is new. " +
                                                 "If anything goes wrong, press Save report and send the file it saves."));
                        else f.Add(new Finding(Ps3Files.AllowUnknown ? 1 : 2, "Disc: this is " + Ps3Files.EditionOf(id) + ". " + Ps3Files.Support));
                        if (!Directory.Exists(Path.Combine(g, "USRDIR"))) f.Add(new Finding(2, "Disc: PS3_GAME has no USRDIR folder."));
                        else
                        {
                            discGame = g;
                            if (Ps3Files.DiscDataEncrypted(g)) f.Add(new Finding(2, "Disc: " + Ps3Files.EncryptedDiscHelp.Replace("\r\n\r\n", " ")));
                        }
                    }
                    catch (Exception e) { f.Add(new Finding(2, "Disc: PARAM.SFO unreadable (" + e.Message + ").")); }
                }
            }
            // update
            string updEboot = null;
            if (string.IsNullOrEmpty(update)) f.Add(new Finding(2, "Update 1.04 folder: not chosen (this port runs update 1.04)."));
            else
            {
                string sfo = Path.Combine(update, "PARAM.SFO");
                if (!File.Exists(sfo)) f.Add(new Finding(2, "Update folder: no PARAM.SFO in " + update + " (choose the update's folder, named after the disc's title id -- BCUS98125, BCES01143, ... -- that holds PARAM.SFO and USRDIR)."));
                else
                {
                    try
                    {
                        var s = Ps3Files.ParseSfo(sfo);
                        string v; s.TryGetValue("APP_VER", out v);
                        if (v == "01.04") f.Add(new Finding(0, "Update: APP_VER 01.04."));
                        else f.Add(new Finding(2, "Update: APP_VER " + v + " -- the port needs update 1.04 (install every update up to 1.04)."));
                    }
                    catch (Exception e) { f.Add(new Finding(2, "Update: PARAM.SFO unreadable (" + e.Message + ").")); }
                    if (!Directory.Exists(Path.Combine(update, "USRDIR"))) f.Add(new Finding(2, "Update folder has no USRDIR."));
                    else updEboot = Ps3Files.FindEboot(Path.Combine(update, "USRDIR"));
                }
            }
            // EBOOT
            string e2 = !string.IsNullOrEmpty(eboot) ? eboot : updEboot;
            if (e2 == null) f.Add(new Finding(2, "Decrypted EBOOT.elf: not found in the update's USRDIR -- choose the EBOOT.elf you decrypted with RPCS3."));
            else if (!File.Exists(e2)) f.Add(new Finding(2, "Decrypted EBOOT.elf: " + e2 + " does not exist."));
            else
            {
                string why;
                if (Ps3Files.ElfKind(e2, out why) != "ok") f.Add(new Finding(2, "EBOOT: " + why + "."));
                else
                {
                    string h = Util.Sha1File(e2), name;
                    long size = new FileInfo(e2).Length;
                    string discId = null;
                    try { if (discGame != null) Ps3Files.ParseSfo(Path.Combine(discGame, "PARAM.SFO")).TryGetValue("TITLE_ID", out discId); } catch { }
                    if (Ps3Files.KnownEboots.TryGetValue(h, out name))
                    {
                        if (h == Ps3Files.UsUpdateSha1) f.Add(new Finding(0, "EBOOT: " + name + " (SHA-1 matches)."));
                        else if (name.Contains("beta")) f.Add(new Finding(0, "EBOOT: " + name + " (SHA-1 matches); the build finds its addresses by pattern and logs what it found."));
                        // (a Reddit player: "the launcher says my game version is 1.0, RPCS3 says 1.04" -- the update was
                        // installed, the EBOOT.elf chosen was decrypted from the disc's EBOOT.BIN)
                        else f.Add(new Finding(2, "EBOOT: this is the " + name + ", decrypted from the DISC's EBOOT.BIN -- even " +
                            "with update 1.04 installed, the port needs the UPDATE's EBOOT. In RPCS3: Utilities > Decrypt PS3 Binaries, " +
                            "and pick " + (!string.IsNullOrEmpty(update) ? Path.Combine(update, @"USRDIR\EBOOT.BIN") : @"dev_hdd0\game\BCUS98125\USRDIR\EBOOT.BIN") +
                            " (the update's, not the disc's PS3_GAME\\USRDIR\\EBOOT.BIN); it writes EBOOT.elf next to it. Choose that one here."));
                    }
                    else if (Ps3Files.IsBeta(discId))
                        f.Add(new Finding(1, "EBOOT: SHA-1 " + h.Substring(0, 12) + "... (" + size.ToString("N0", CultureInfo.InvariantCulture) +
                            " bytes) is not one this launcher knows: step 2 (check) tests whether the game's structure resolves in it, " +
                            "and stops if it does not (beta)."));
                    else f.Add(new Finding(Ps3Files.AllowUnknown ? 1 : 2, "EBOOT: SHA-1 " + h.Substring(0, 12) + "... is not the BCUS98125 " +
                        "update 1.04 EBOOT (another edition, or a damaged or wrongly decrypted dump): the build's title hooks would not " +
                        "match it. " + Ps3Files.Support));
                    if (size == 10159776 && h != "f1c20717d6532dfb27e292a5db6cdf5a3218760b")
                        f.Add(new Finding(1, "EBOOT: it is exactly the size of the US disc EBOOT (1.00), not of update 1.04 (10,605,256 bytes): " +
                            "if you meant the update's, decrypt the EBOOT.BIN of the update folder's USRDIR (step 2 says which layout its code has)."));
                    ebootPath = e2;
                }
            }
            // libsre
            string l = libsre;
            if (!string.IsNullOrEmpty(l) && Directory.Exists(l))
            {
                string c = Path.Combine(l, @"dev_flash\sys\external\libsre.prx");
                string enc = Path.Combine(l, @"dev_flash\sys\external\libsre.sprx");
                if (File.Exists(c)) l = c;
                else if (File.Exists(enc)) { f.Add(new Finding(2, "libsre: " + l + " has only the encrypted libsre.sprx -- decrypt it in RPCS3 (Utilities > Decrypt PS3 Binaries) first.")); l = null; }
                else { f.Add(new Finding(2, "libsre: no dev_flash\\sys\\external\\libsre.prx under " + l + ".")); l = null; }
            }
            if (l == null) { }
            else if (string.IsNullOrEmpty(l)) f.Add(new Finding(2, "Decrypted libsre.prx: not chosen."));
            else if (!File.Exists(l)) f.Add(new Finding(2, "libsre: " + l + " does not exist."));
            else
            {
                string why;
                if (!Ps3Files.IsLibsre(l, out why)) f.Add(new Finding(2, "libsre: " + why + "."));
                else
                {
                    string h = Util.Sha1File(l);
                    if (Ps3Files.KnownLibsre.ContainsKey(h)) { f.Add(new Finding(0, "libsre.prx: decrypted, the known version.")); libsrePath = l; }
                    else if (Ps3Files.LibsreModulesMatch(l))
                    {
                        // (issue #4: this note was read as the cause of a failed build. It is not an error and never stops anything.)
                        f.Add(new Finding(0, "libsre.prx: decrypted, from another firmware than the tested 4.92; its SPURS modules match (fine)."));
                        libsrePath = l;
                    }
                    else f.Add(new Finding(2, "libsre: " + Ps3Files.OldLibsreHelp));
                }
            }
            return f;
        }
    }

    // ------------------------------------------------------------------ prerequisites
    class Prereq
    {
        public string Id, Name, Why, WingetId, WingetArgs, Found, Note, Url;   // Url: the download page, when winget fails
        public bool Ok, Outdated;   // Outdated: installed, but too old (Install becomes Update)
    }

    static class Prereqs
    {
        public static List<Prereq> Detect(Options o)
        {
            string path = Util.FreshPath();
            var list = new List<Prereq>();
            string o1;

            // Git for Windows: build.ps1 runs Git Bash from Program Files; setup.ps1 runs git.
            var git = new Prereq { Id = "git", Name = "Git for Windows (with Git Bash)", Why = "fetches the toolkit, runs the lift scripts", WingetId = "Git.Git", Url = "https://git-scm.com/download/win" };
            foreach (var b in new[] { Path.Combine(Util.Pf, @"Git\bin\bash.exe"), Path.Combine(Util.Pf86, @"Git\bin\bash.exe") })
                if (File.Exists(b)) { git.Ok = true; git.Found = b; break; }
            if (!git.Ok) git.Note = "Git Bash must be in Program Files\\Git (the installer's default).";
            list.Add(git);

            // Python 3.10+
            var py = new Prereq { Id = "python", Name = "Python 3.10 or newer", Why = "runs the recompiler (ps3recomp)", WingetId = "Python.Python.3.12", Url = "https://www.python.org/downloads/windows/" };
            foreach (var c in new[] { new[] { "py", "-3" }, new[] { "python", "" }, new[] { "python3", "" } })
            {
                string exe = Util.FindOnPath(c[0] + ".exe", path);
                if (exe == null) continue;
                if (Util.Run(exe, (c[1] + " -c \"import sys;print(sys.executable);print('%d.%d'%sys.version_info[:2])\"").Trim(), out o1, 15000) != 0) continue;
                var lines = o1.Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries);
                if (lines.Length < 2) continue;
                Version v;
                if (Version.TryParse(lines[1].Trim(), out v) && v >= new Version(3, 10)) { py.Ok = true; py.Found = lines[0].Trim() + " (" + v + ")"; break; }
                py.Note = "found Python " + lines[1].Trim() + "; 3.10 or newer is needed";
            }
            list.Add(py);

            // Visual Studio 2022 Build Tools with the C++ tools
            var vs = new Prereq
            {
                Id = "vs", Name = "Visual Studio 2022 Build Tools (C++)", Why = "the C++ libraries and linker", WingetId = "Microsoft.VisualStudio.2022.BuildTools", Url = "https://visualstudio.microsoft.com/visual-cpp-build-tools/",
                WingetArgs = "--override \"--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended\""
            };
            string vswhere = Path.Combine(Util.Pf86, @"Microsoft Visual Studio\Installer\vswhere.exe");
            string vsDir = null;
            if (File.Exists(vswhere) &&
                Util.Run(vswhere, "-latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath", out o1) == 0 &&
                o1.Trim().Length > 0)
            { vsDir = o1.Trim().Split('\n')[0].Trim(); vs.Ok = true; vs.Found = vsDir; }
            else if (File.Exists(vswhere)) vs.Note = "Visual Studio is installed without the C++ tools: open Visual Studio Installer, Modify, and add \"Desktop development with C++\".";
            list.Add(vs);

            // The Windows SDK the build uses (vcvars picks the newest installed). Its d3d12.h must declare the C
            // binding of Get{CPU,GPU}DescriptorHandleForHeapStart with the RetVal out-parameter; 10.0.19041 and
            // older still have the one-argument form, and the renderer fails to compile ("too many arguments to
            // function call, expected 1, have 2", issue #18).
            var sdk = new Prereq
            {
                Id = "sdk", Name = "Windows SDK 10.0.20348 or newer", Why = "the Direct3D 12 headers", WingetId = "Microsoft.WindowsSDK.10.0.26100",
                Url = "https://developer.microsoft.com/windows/downloads/windows-sdk/"
            };
            string sdkVer, sdkD3d = NewestSdkD3d12(out sdkVer);
            if (sdkD3d == null) { sdk.Ok = true; sdk.Found = "(not found separately: Visual Studio's)"; }
            else
            {
                string h = "";
                try { h = File.ReadAllText(sdkD3d); } catch { }
                bool retval = h.Length == 0 || Regex.IsMatch(h, @"GetCPUDescriptorHandleForHeapStart\s*\)\s*\(\s*ID3D12DescriptorHeap\s*\*\s*This\s*,\s*D3D12_CPU_DESCRIPTOR_HANDLE\s*\*\s*RetVal");
                sdk.Found = Path.GetDirectoryName(Path.GetDirectoryName(sdkD3d)) + " (" + sdkVer + ")";
                if (retval) sdk.Ok = true;
                else
                {
                    sdk.Outdated = true;
                    sdk.Note = "found Windows SDK " + sdkVer + "; its Direct3D 12 header is too old for the game's renderer (10.0.20348 or newer is needed)";
                }
            }
            list.Add(sdk);

            // CMake and Ninja: on PATH, in their usual folders, or the copies Visual Studio's C++ CMake tools bring.
            var cm = new Prereq { Id = "cmake", Name = "CMake 3.20 or newer", Why = "configures the build", WingetId = "Kitware.CMake", Url = "https://cmake.org/download/" };
            cm.Found = Util.FindOnPath("cmake.exe", path);
            if (cm.Found == null && File.Exists(Path.Combine(Util.Pf, @"CMake\bin\cmake.exe"))) cm.Found = Path.Combine(Util.Pf, @"CMake\bin\cmake.exe");
            if (cm.Found == null && vsDir != null)
            {
                string c = Path.Combine(vsDir, @"Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe");
                if (File.Exists(c)) cm.Found = c;
            }
            cm.Ok = cm.Found != null;
            list.Add(cm);

            var nj = new Prereq { Id = "ninja", Name = "Ninja", Why = "runs the compiler on every core", WingetId = "Ninja-build.Ninja", Url = "https://github.com/ninja-build/ninja/releases/latest" };
            nj.Found = Util.FindOnPath("ninja.exe", path);
            if (nj.Found == null)
            {
                string l = Path.Combine(Util.LocalAppData, @"Microsoft\WinGet\Links\ninja.exe");
                if (File.Exists(l)) nj.Found = l;
            }
            if (nj.Found == null && vsDir != null)
            {
                string c = Path.Combine(vsDir, @"Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe");
                if (File.Exists(c)) nj.Found = c;
            }
            nj.Ok = nj.Found != null;
            list.Add(nj);

            // LLVM 19+: clang-cl 18 and older leave the AVX2 / AVX-512 intrinsic headers out of <immintrin.h>
            // in MSVC mode unless the whole file is built for that CPU, so the runtime's per-function
            // target("avx512...") paths fail with "unknown type name '__m512i'" (issue #8, clang 18.1.8).
            var llvm = new Prereq { Id = "llvm", Name = "LLVM 19 or newer (clang-cl)", Why = "the compiler", WingetId = "LLVM.LLVM", Url = "https://github.com/llvm/llvm-project/releases/latest" };
            string cl = Path.Combine(Util.Pf, @"LLVM\bin\clang-cl.exe");
            if (File.Exists(cl))
            {
                Version cv = ClangVersion(cl);
                if (cv != null && cv.Major < MinClangMajor)
                {
                    llvm.Outdated = true; llvm.Found = cl + " (" + cv + ")";
                    llvm.Note = "found LLVM " + cv + "; " + MinClangMajor + " or newer is needed (older clang-cl cannot compile the game's AVX2 / AVX-512 code)";
                }
                else { llvm.Ok = true; llvm.Found = cl + (cv != null ? " (" + cv + ")" : ""); }
            }
            else llvm.Note = "clang-cl must be in Program Files\\LLVM (the installer's default).";
            list.Add(llvm);

            foreach (var p in list) if (o.PretendMissing.Contains(p.Id)) { p.Ok = false; p.Found = null; p.Note = "(shown as missing: --pretend-missing)"; }
            return list;
        }

        // Folders the build's processes need on PATH: where CMake, Ninja, Git and Python were found.
        public static string ChildPath(List<Prereq> list)
        {
            var dirs = new List<string>();
            foreach (var p in list)
            {
                if (!p.Ok || p.Found == null) continue;
                string f = p.Found;
                int paren = f.IndexOf(" (", StringComparison.Ordinal);
                if (paren > 0) f = f.Substring(0, paren);
                if (p.Id == "cmake" || p.Id == "ninja" || p.Id == "python") dirs.Add(Path.GetDirectoryName(f));
                if (p.Id == "git") dirs.Add(Path.Combine(Path.GetDirectoryName(Path.GetDirectoryName(f)), "cmd"));
            }
            dirs.Add(Util.FreshPath());
            return string.Join(";", dirs.Where(d => !string.IsNullOrEmpty(d)).Distinct(StringComparer.OrdinalIgnoreCase));
        }

        public static string WingetExe()
        {
            string w = Path.Combine(Util.LocalAppData, @"Microsoft\WindowsApps\winget.exe");
            if (File.Exists(w)) return w;
            return Util.FindOnPath("winget.exe", Util.FreshPath());
        }
        public static string WingetCommand(Prereq p)
        {
            // too old: --force installs the current version over it (winget upgrade misses copies it did not install)
            return "winget install --id " + p.WingetId + " --exact --source winget" + (p.Outdated ? " --force" : "") +
                   (p.WingetArgs != null ? " " + p.WingetArgs : "");
        }

        public const int MinClangMajor = 19;

        // um\d3d12.h of the newest Windows 10/11 SDK installed (the one vcvars picks), or null.
        static string NewestSdkD3d12(out string version)
        {
            version = null;
            var roots = new List<string>();
            try
            {
                using (var k = Microsoft.Win32.RegistryKey.OpenBaseKey(Microsoft.Win32.RegistryHive.LocalMachine, Microsoft.Win32.RegistryView.Registry32)
                                   .OpenSubKey(@"SOFTWARE\Microsoft\Windows Kits\Installed Roots"))
                { var r = k != null ? k.GetValue("KitsRoot10") as string : null; if (!string.IsNullOrEmpty(r)) roots.Add(r); }
            }
            catch { }
            roots.Add(Path.Combine(Util.Pf86, @"Windows Kits\10"));
            Version best = null; string bestPath = null;
            foreach (var root in roots)
            {
                string inc = Path.Combine(root, "Include");
                if (!Directory.Exists(inc)) continue;
                foreach (var d in Directory.GetDirectories(inc))
                {
                    Version v;
                    string f = Path.Combine(d, @"um\d3d12.h");
                    if (!Version.TryParse(Path.GetFileName(d), out v) || !File.Exists(f)) continue;
                    if (best == null || v > best) { best = v; bestPath = f; }
                }
            }
            if (best != null) version = best.ToString();
            return bestPath;
        }

        // "clang version 18.1.8 (...)" -> 18.1.8; null when it cannot be read (then it is not held against it).
        public static Version ClangVersion(string clangCl)
        {
            string o;
            try { if (Util.Run(clangCl, "--version", out o, 15000) != 0) return null; }
            catch { return null; }
            var m = Regex.Match(o ?? "", @"clang version (\d+)\.(\d+)(?:\.(\d+))?");
            if (!m.Success) return null;
            return new Version(int.Parse(m.Groups[1].Value), int.Parse(m.Groups[2].Value),
                               m.Groups[3].Success ? int.Parse(m.Groups[3].Value) : 0);
        }
    }

    // ------------------------------------------------------------------ the pipeline (tools\build.ps1 -Step <id>)
    class StepDef
    {
        public string Id, Title, Marker; public int Weight; public string[] Inputs;
        public StepDef(string id, string title, int weight, string marker, params string[] inputs)
        { Id = id; Title = title; Weight = weight; Marker = marker; Inputs = inputs; }
    }

    class Pipeline
    {
        public static readonly StepDef[] Steps =
        {
            new StepDef("setup",   "Fetch and patch the ps3recomp toolkit", 5, @"external\ps3recomp\.git",
                        "ps3recomp.pin", @"patches\ps3recomp\*", @"tools\setup.ps1", @"tools\python.ps1"),
            new StepDef("check",   "Check your game files", 2, @"analysis\inputs.json", @"tools\check_inputs.py"),
            new StepDef("analyze", "Analyze the game's executable", 5, @"analysis\EBOOT.functions.json",
                        @"tools\analyze.sh", @"tools\import_coverage.py", @"tools\code_end.py"),
            new StepDef("lift",    "Translate the PowerPC and SPU code to C++", 18, @"generated\ppu",
                        @"tools\relift.sh", @"tools\ppu_postlift.py", @"config\ss_cont_list.txt", @"config\ss_cont_list.104.txt",
                        @"tools\if2_resolve.py", @"tools\if2_items.py", @"config\if2_sigs.json", @"config\if2_contdb.json"),
            new StepDef("libsre",  "Translate the SPURS library (libsre)", 5, @"generated\prx\libsre.linked.bin",
                        @"tools\lift_libsre.sh", @"config\ss_cont_list_libsre.txt"),
            new StepDef("jobs",    "Translate the SPURS jobs", 18, @"generated\spu_jobs",
                        @"tools\lift_jobs.sh", @"tools\extract_eboot_jobs.py", @"tools\lift_policy.py", @"tools\lift_jobchain.py",
                        @"tools\extract_policy_modules.py"),
            new StepDef("build",   "Compile the game (the long step)", 47, @"build\infamous2.exe",
                        "CMakeLists.txt", @"src\*", @"tools\build.ps1"),
        };

        readonly string root;
        readonly GameFiles files;
        public Pipeline(string root, GameFiles files) { this.root = root; this.files = files; }

        IEnumerable<string> Expand(string pattern)
        {
            if (pattern.EndsWith(@"\*"))
            {
                string d = Path.Combine(root, pattern.Substring(0, pattern.Length - 2));
                if (!Directory.Exists(d)) return new string[0];
                return Directory.GetFiles(d, "*", SearchOption.AllDirectories).OrderBy(x => x, StringComparer.OrdinalIgnoreCase);
            }
            return new[] { Path.Combine(root, pattern) };
        }

        // Each step's fingerprint covers its own inputs and the step before it: a change re-runs that step and every later one.
        public string[] Fingerprints()
        {
            var fp = new string[Steps.Length];
            string prev = "";
            for (int i = 0; i < Steps.Length; i++)
            {
                var sb = new StringBuilder(prev).Append('\n').Append(Steps[i].Id);
                foreach (var pat in Steps[i].Inputs)
                    foreach (var f in Expand(pat))
                        sb.Append('\n').Append(f.Substring(root.Length)).Append('=').Append(File.Exists(f) ? Util.Sha1File(f) : "missing");
                if (Steps[i].Id == "check") sb.Append('\n').Append(files.Identity());
                prev = fp[i] = Util.Sha1(sb.ToString());
            }
            return fp;
        }

        public bool MarkerOk(int i)
        {
            string m = Path.Combine(root, Steps[i].Marker);
            return File.Exists(m) || Directory.Exists(m);
        }

        // A moved / renamed folder (issue #3): the game itself (build\infamous2.exe) finds everything relative to where
        // it is, so the build step, up to date but for the folder, does not compile it again -- only the build
        // directory's configuration names the old folder. That is dropped the way tools\build.ps1 drops it for a moved
        // folder (CMakeCache.txt and CMakeFiles; never the shader cache), so the next real compile configures it here.
        // Returns true when skipped; the caller records the new folder for the step.
        public bool SkipMovedBuild(int i, string movedFrom, Action<string> say)
        {
            if (Steps[i].Id != "build" || movedFrom == null || !File.Exists(Path.Combine(root, @"build\infamous2.exe"))) return false;
            say("the folder moved (the game was built in " + movedFrom + "): it does not depend on its folder, so it is not compiled again");
            try
            {
                File.Delete(Path.Combine(root, @"build\CMakeCache.txt"));
                string cf = Path.Combine(root, @"build\CMakeFiles");
                if (Directory.Exists(cf)) Directory.Delete(cf, true);
            }
            catch (Exception ex) { say("(could not clear the old build configuration: " + ex.Message + "; the next compile does it)"); }
            return true;
        }

        // Issue #5: a folder moved or copied after a build. Two steps leave this folder's absolute path in what they
        // write -- check (analysis\inputs.json: the EBOOT and libsre) and build (build\CMakeCache.txt, build.ninja) --
        // so they run again when it changed; the steps between keep their results (nothing they wrote names the
        // folder). Returns why, or null. recorded = the folder state.ini says the step ran in (null: not recorded).
        public string MovedFrom(int i, string recorded)
        {
            string id = Steps[i].Id;
            if (id != "check" && id != "build") return null;
            string here = Path.GetFullPath(root).TrimEnd('\\');
            // recorded: the step's last run says where it ran, and that settles it (its outputs are from that run). The
            // files are read only for a state without it: a path in them can point outside the folder without a move
            // (a linked game\ folder), and reading them after a rerun here would run the step again and again.
            if (recorded != null)
                return string.Equals(recorded.TrimEnd('\\'), here, StringComparison.OrdinalIgnoreCase) ? null : recorded;
            try
            {
                if (id == "check")
                {
                    string j = Path.Combine(root, @"analysis\inputs.json");
                    if (!File.Exists(j)) return null;
                    var m = Regex.Match(File.ReadAllText(j), "\"eboot\":\\s*\"((?:[^\"\\\\]|\\\\.)*)\"");
                    if (!m.Success) return null;
                    string e = Regex.Unescape(m.Groups[1].Value);
                    int g = e.IndexOf(@"\game\", StringComparison.OrdinalIgnoreCase);
                    if (g > 0 && !string.Equals(e.Substring(0, g), here, StringComparison.OrdinalIgnoreCase)) return e.Substring(0, g);
                }
                else
                {
                    string c = Path.Combine(root, @"build\CMakeCache.txt");
                    if (!File.Exists(c)) return null;
                    foreach (var l in File.ReadLines(c))
                        if (l.StartsWith("CMAKE_HOME_DIRECTORY:", StringComparison.Ordinal))
                        {
                            string d = l.Substring(l.IndexOf('=') + 1).Replace('/', '\\').TrimEnd('\\');
                            if (!string.Equals(d, here, StringComparison.OrdinalIgnoreCase)) return d;
                        }
                }
            }
            catch { }
            return null;
        }
    }

    // ------------------------------------------------------------------ state (.launcher\state.ini: key=value)
    class State
    {
        readonly string path;
        public Dictionary<string, string> V = new Dictionary<string, string>();
        public State(string root)
        {
            path = Path.Combine(root, @".launcher\state.ini");
            try
            {
                if (File.Exists(path))
                    foreach (var l in File.ReadAllLines(path))
                    {
                        int k = l.IndexOf('=');
                        if (k > 0) V[l.Substring(0, k)] = l.Substring(k + 1);
                    }
            }
            catch { }
        }
        public string Get(string k) { string v; return V.TryGetValue(k, out v) ? v : null; }
        public void Set(string k, string v) { if (v == null) V.Remove(k); else V[k] = v; Save(); }
        public void Save()
        {
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(path));
                File.WriteAllLines(path, V.OrderBy(kv => kv.Key).Select(kv => kv.Key + "=" + kv.Value));
            }
            catch { }
        }
    }

    // ------------------------------------------------------------------ the game files dialog
    class GameFilesForm : Form
    {
        readonly TextBox tDisc = new TextBox(), tUpd = new TextBox(), tEboot = new TextBox(), tLib = new TextBox();
        readonly TextBox tResult = new TextBox();
        readonly Button bUse = new Button(), bCheck = new Button(), bCancel = new Button();
        readonly GameFiles files;
        public string DiscGame, UpdateDir, EbootPath, LibsrePath;
        bool lastOk;

        public GameFilesForm(GameFiles files, Options o)
        {
            this.files = files;
            Text = "inFamous 2 -- your game files";
            Font = new Font("Segoe UI", 9f);
            ClientSize = new Size(820, 560);
            FormBorderStyle = FormBorderStyle.FixedDialog; MaximizeBox = false; MinimizeBox = false;
            StartPosition = FormStartPosition.CenterParent;

            var intro = new Label
            {
                Left = 16, Top = 12, Width = 790, Height = 92,
                Text = "This port runs YOUR copy of inFamous 2 (BCUS98125; the European BCES01143 / BCES01144 as a beta) with title update 1.04. Nothing from the game is included.\r\n" +
                       "From your own console and dumps you need: the disc's files (the folder that holds PS3_GAME), the 1.04 update folder " +
                       "(named after the title id, holding PARAM.SFO and USRDIR), the update's EBOOT.BIN decrypted with RPCS3 (Utilities > Decrypt PS3 Binaries) " +
                       "as EBOOT.elf, and libsre.prx decrypted from your PS3 firmware in RPCS3 (dev_flash\\sys\\external\\libsre.sprx).\r\n" +
                       "The disc folder is linked, not copied; the update (about 230 MB), the EBOOT and libsre are copied into this folder's game\\. " +
                       "Your originals are never changed. See game\\README.md for details."
            };
            Controls.Add(intro);
            int y = 112;
            Row("Disc folder (holds PS3_GAME):", tDisc, true, ref y, o.Disc);
            Row("Update 1.04 folder (BCUS98125 / BCES01143 ... with PARAM.SFO + USRDIR):", tUpd, true, ref y, o.Update);
            Row("Decrypted update EBOOT.elf (leave empty if it is in the update's USRDIR):", tEboot, false, ref y, o.Eboot);
            Row("Decrypted libsre.prx (or type the path of your RPCS3 folder, which holds dev_flash):", tLib, false, ref y, o.Libsre);

            tResult.Multiline = true; tResult.ReadOnly = true; tResult.ScrollBars = ScrollBars.Vertical;
            tResult.SetBounds(16, y + 6, 788, 560 - y - 60);
            tResult.Font = new Font("Consolas", 9f);
            Controls.Add(tResult);

            bCheck.Text = "Check"; bCheck.SetBounds(16, 518, 110, 30); bCheck.Click += (s, e) => DoCheck();
            bUse.Text = "Use these files"; bUse.SetBounds(574, 518, 130, 30); bUse.Click += (s, e) => DoUse();
            bCancel.Text = "Cancel"; bCancel.SetBounds(714, 518, 90, 30); bCancel.DialogResult = DialogResult.Cancel;
            Controls.AddRange(new Control[] { bCheck, bUse, bCancel });
            CancelButton = bCancel; AcceptButton = bUse;
            Shown += (s, e) => { if (tDisc.Text.Length > 0) DoCheck(); };
        }

        void Row(string label, TextBox t, bool folder, ref int y, string init)
        {
            Controls.Add(new Label { Text = label, Left = 16, Top = y, Width = 700, Height = 18 });
            t.SetBounds(16, y + 20, 690, 24);
            if (!string.IsNullOrEmpty(init)) t.Text = init;
            t.TextChanged += (s, e) => { lastOk = false; };
            Controls.Add(t);
            var b = new Button { Text = "Browse...", Left = 714, Top = y + 19, Width = 90, Height = 26 };
            b.Click += (s, e) =>
            {
                if (folder)
                {
                    using (var d = new FolderBrowserDialog { Description = label, ShowNewFolderButton = false })
                        if (d.ShowDialog(this) == DialogResult.OK) t.Text = d.SelectedPath;
                }
                else
                {
                    using (var d = new OpenFileDialog { Title = label, Filter = t == tLib ? "libsre.prx|libsre.prx|PRX (*.prx)|*.prx|All files|*.*" : "EBOOT.elf|EBOOT.elf;*.elf|All files|*.*" })
                        if (d.ShowDialog(this) == DialogResult.OK) t.Text = d.FileName;
                }
                lastOk = false;
            };
            Controls.Add(b);
            y += 54;
        }

        bool DoCheck()
        {
            Cursor = Cursors.WaitCursor;
            string dg, eb, lp;
            var f = GameFiles.Check(tDisc.Text.Trim(), tUpd.Text.Trim(), tEboot.Text.Trim(), tLib.Text.Trim(), out dg, out eb, out lp);
            Cursor = Cursors.Default;
            var sb = new StringBuilder();
            foreach (var x in f) sb.Append(x.Level == 0 ? "[ok] " : x.Level == 1 ? "[!]  " : "[x]  ").Append(x.Text).Append("\r\n");
            lastOk = !f.Any(x => x.Level == 2);
            sb.Append(lastOk ? "\r\nReady: press \"Use these files\"." : "\r\nFix the [x] lines, then Check again.");
            tResult.Text = sb.ToString();
            DiscGame = dg; UpdateDir = tUpd.Text.Trim(); EbootPath = eb; LibsrePath = lp;
            return lastOk;
        }

        void DoUse()
        {
            if (!DoCheck()) { MessageBox.Show(this, "Some files are missing or wrong: see the [x] lines.", Text); return; }
            DialogResult = DialogResult.OK;
            Close();
        }
    }

    // ------------------------------------------------------------------ updates: the latest GitHub release
    static class SemVer
    {
        // "v1.2.3", "1.2", "1.2.3-rc1": up to four numbers and a pre-release tag; false when it is not a version
        public static bool TryParse(string s, out int[] nums, out string pre)
        {
            nums = null; pre = "";
            if (s == null) return false;
            s = s.Trim();
            if (s.StartsWith("v") || s.StartsWith("V")) s = s.Substring(1);
            int plus = s.IndexOf('+');
            if (plus >= 0) s = s.Substring(0, plus);
            int dash = s.IndexOf('-');
            if (dash >= 0) { pre = s.Substring(dash + 1); s = s.Substring(0, dash); }
            var parts = s.Split('.');
            if (parts.Length > 4) return false;
            var n = new int[4];
            for (int i = 0; i < parts.Length; i++)
                if (!int.TryParse(parts[i], NumberStyles.None, CultureInfo.InvariantCulture, out n[i])) return false;
            nums = n;
            return true;
        }
        public static bool Valid(string s) { int[] n; string p; return TryParse(s, out n, out p); }
        // < 0 when a is older than b (1.2.3-rc1 is older than 1.2.3); 0 when equal or either is not a version
        public static int Compare(string a, string b)
        {
            int[] x, y; string px, py;
            if (!TryParse(a, out x, out px) || !TryParse(b, out y, out py)) return 0;
            for (int i = 0; i < 4; i++) if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
            if (px.Length == 0 || py.Length == 0) return px.Length == py.Length ? 0 : px.Length == 0 ? 1 : -1;
            return Math.Sign(string.CompareOrdinal(px, py));
        }
        public static string Clean(string tag)
        {
            if (tag == null) return null;
            tag = tag.Trim();
            return tag.StartsWith("v") || tag.StartsWith("V") ? tag.Substring(1) : tag;
        }
    }

    class ReleaseAsset { public string Name, Url, Digest; public long Size; }

    class ReleaseInfo
    {
        public string Tag, Name, Body, HtmlUrl;
        public bool Draft, Prerelease;
        public List<ReleaseAsset> Assets = new List<ReleaseAsset>();
        public string Version { get { return SemVer.Clean(Tag); } }
        // the ready-to-run zip: inFamous2Recomp-<tag>-windows.zip (found by its name's pattern)
        public ReleaseAsset WindowsZip
        {
            get { return Assets.FirstOrDefault(a => a.Name != null && Regex.IsMatch(a.Name, @"^inFamous2Recomp-.+-windows\.zip$", RegexOptions.IgnoreCase)); }
        }

        public static ReleaseInfo Parse(string json)
        {
            var js = new System.Web.Script.Serialization.JavaScriptSerializer { MaxJsonLength = 16 << 20 };
            var d = js.DeserializeObject(json) as Dictionary<string, object>;
            if (d == null) throw new InvalidDataException("the answer is not a release");
            var r = new ReleaseInfo
            {
                Tag = Str(d, "tag_name"), Name = Str(d, "name"), Body = Str(d, "body"), HtmlUrl = Str(d, "html_url"),
                Draft = Bool(d, "draft"), Prerelease = Bool(d, "prerelease")
            };
            if (string.IsNullOrEmpty(r.Tag)) throw new InvalidDataException("the answer has no tag_name" + (d.ContainsKey("message") ? " (GitHub: " + Str(d, "message") + ")" : ""));
            object a;
            if (d.TryGetValue("assets", out a) && a is object[])
                foreach (var o in (object[])a)
                {
                    var ad = o as Dictionary<string, object>;
                    if (ad == null) continue;
                    long size = 0;
                    object sv;
                    if (ad.TryGetValue("size", out sv) && sv != null) try { size = Convert.ToInt64(sv, CultureInfo.InvariantCulture); } catch { }
                    r.Assets.Add(new ReleaseAsset { Name = Str(ad, "name"), Url = Str(ad, "browser_download_url"), Digest = Str(ad, "digest"), Size = size });
                }
            return r;
        }
        static string Str(Dictionary<string, object> d, string k) { object v; return d.TryGetValue(k, out v) && v != null ? Convert.ToString(v, CultureInfo.InvariantCulture) : null; }
        static bool Bool(Dictionary<string, object> d, string k) { object v; return d.TryGetValue(k, out v) && v is bool && (bool)v; }
    }

    static class Updates
    {
        static Updates()
        {
            // the .NET Framework may default to TLS 1.0 for an exe built like this one; GitHub needs 1.2
            try { ServicePointManager.SecurityProtocol |= SecurityProtocolType.Tls12; } catch { }
        }
        public static string Overridden { get { string e = Environment.GetEnvironmentVariable("INF2L_UPDATE_API"); return string.IsNullOrEmpty(e) ? null : e; } }
        public static string ApiUrl { get { return Overridden ?? AppInfo.ApiLatest; } }
        public static bool OffByEnv { get { return Environment.GetEnvironmentVariable("INF2L_NO_UPDATE_CHECK") == "1"; } }
        public static string TempRoot { get { return Path.Combine(Path.GetTempPath(), "inFamous2Recomp-update"); } }

        public static HttpWebRequest Request(string url, int timeoutMs)
        {
            var req = (HttpWebRequest)WebRequest.Create(url);
            req.UserAgent = AppInfo.UserAgent;
            req.Timeout = timeoutMs; req.ReadWriteTimeout = timeoutMs;
            req.AllowAutoRedirect = true;
            return req;
        }

        // One GET of releases/latest (GitHub leaves drafts and pre-releases out of it). Throws on any failure.
        public static ReleaseInfo FetchLatest(int timeoutMs)
        {
            var req = Request(ApiUrl, timeoutMs);
            req.Accept = "application/vnd.github+json";
            req.Headers["X-GitHub-Api-Version"] = "2022-11-28";
            var ar = req.BeginGetResponse(null, null);
            if (!ar.AsyncWaitHandle.WaitOne(timeoutMs)) { req.Abort(); throw new TimeoutException("no answer within " + timeoutMs / 1000 + " s"); }
            using (var resp = (HttpWebResponse)req.EndGetResponse(ar))
            using (var rd = new StreamReader(resp.GetResponseStream(), Encoding.UTF8))
                return ReleaseInfo.Parse(rd.ReadToEnd());
        }

        // "v0.1.3 -- <title>", or the title alone when it already starts with the tag
        public static string Title(ReleaseInfo r)
        {
            if (string.IsNullOrEmpty(r.Name) || r.Name == r.Tag) return r.Tag;
            return r.Name.StartsWith(r.Tag, StringComparison.OrdinalIgnoreCase) ? r.Name : r.Tag + " -- " + r.Name;
        }

        public static bool IsNewer(ReleaseInfo r)
        {
            return r != null && !r.Draft && !r.Prerelease && SemVer.Valid(r.Tag) && SemVer.Compare(AppInfo.Version, r.Tag) < 0;
        }

        public static string Describe(Exception e)
        {
            var we = e as WebException;
            if (we != null && we.Response is HttpWebResponse)
                return "HTTP " + (int)((HttpWebResponse)we.Response).StatusCode + " " + ((HttpWebResponse)we.Response).StatusDescription;
            return e.GetType().Name + ": " + e.Message;
        }

        // "sha256:<64 hex>" -> the hex; null when the asset lists no SHA-256
        public static string DigestSha256(string digest)
        {
            if (string.IsNullOrEmpty(digest) || !digest.StartsWith("sha256:", StringComparison.OrdinalIgnoreCase)) return null;
            string h = digest.Substring(7).Trim().ToLowerInvariant();
            return Regex.IsMatch(h, "^[0-9a-f]{64}$") ? h : null;
        }

        // Only this repository's release downloads over https (the tests' INF2L_UPDATE_API lifts that).
        public static bool DownloadAllowed(string url)
        {
            if (Overridden != null) return url != null && (url.StartsWith("http://") || url.StartsWith("https://"));
            return url != null && url.StartsWith("https://github.com/" + AppInfo.Repo + "/releases/download/", StringComparison.OrdinalIgnoreCase);
        }
        public static string ReleasePage(ReleaseInfo r)
        {
            if (r != null && r.HtmlUrl != null &&
                (r.HtmlUrl.StartsWith("https://github.com/" + AppInfo.Repo + "/", StringComparison.OrdinalIgnoreCase) ||
                 (Overridden != null && (r.HtmlUrl.StartsWith("http://") || r.HtmlUrl.StartsWith("https://")))))
                return r.HtmlUrl;
            return AppInfo.ReleasesPage;
        }

        // The start of the release notes, without the heaviest markdown, for the dialog.
        public static string Excerpt(string body, int max)
        {
            if (string.IsNullOrEmpty(body)) return "(no release notes)";
            var sb = new StringBuilder();
            foreach (var raw in body.Replace("\r\n", "\n").Split('\n'))
            {
                string l = raw.TrimEnd().Replace("**", "").Replace("`", "");
                if (l.StartsWith("#")) l = l.TrimStart('#').Trim();
                if (sb.Length + l.Length > max)
                {
                    int room = max - sb.Length;
                    if (room > 40) sb.Append(l.Substring(0, room)).Append("...");
                    else sb.Append("...");
                    break;
                }
                sb.Append(l).Append("\r\n");
            }
            return Regex.Replace(sb.ToString(), @"(\r\n){3,}", "\r\n\r\n").Trim();
        }

        // Old work folders of earlier updates (the updater's copy, the backup of a clean undo); never one marked KEEP.
        public static void CleanTemp()
        {
            try
            {
                if (!Directory.Exists(TempRoot)) return;
                foreach (var d in Directory.GetDirectories(TempRoot))
                {
                    try
                    {
                        if (File.Exists(Path.Combine(d, "KEEP.txt"))) continue;
                        if (DateTime.UtcNow - Directory.GetLastWriteTimeUtc(d) < TimeSpan.FromHours(1)) continue;
                        Directory.Delete(d, true);
                    }
                    catch { }
                }
                foreach (var f in Directory.GetFiles(TempRoot, "update-*.log"))
                    try { if (DateTime.UtcNow - File.GetLastWriteTimeUtc(f) > TimeSpan.FromDays(30)) File.Delete(f); } catch { }
            }
            catch { }
        }

        public static string Report(Options o)
        {
            var sb = new StringBuilder();
            sb.AppendLine("launcher version: " + AppInfo.Version);
            sb.AppendLine("api: " + ApiUrl);
            try
            {
                var t0 = DateTime.Now;
                var r = FetchLatest(10000);
                sb.AppendLine("answer after: " + (int)(DateTime.Now - t0).TotalMilliseconds + " ms");
                sb.AppendLine("tag: " + r.Tag + "  (version " + r.Version + ", valid " + SemVer.Valid(r.Tag) + ")");
                sb.AppendLine("name: " + r.Name);
                sb.AppendLine("draft: " + r.Draft + "  prerelease: " + r.Prerelease);
                sb.AppendLine("newer than this launcher: " + IsNewer(r));
                sb.AppendLine("release page: " + ReleasePage(r));
                foreach (var a in r.Assets)
                    sb.AppendLine("asset: " + a.Name + "  size " + a.Size + "  digest " + (a.Digest ?? "(none)") + "  sha256 " + (DigestSha256(a.Digest) ?? "(none)") +
                                  "  download allowed " + DownloadAllowed(a.Url) + "  " + a.Url);
                var z = r.WindowsZip;
                sb.AppendLine("windows zip: " + (z != null ? z.Name : "(none)"));
                sb.AppendLine("excerpt:");
                sb.AppendLine(Excerpt(r.Body, 700));
            }
            catch (Exception e) { sb.AppendLine("check failed: " + Describe(e)); }
            return sb.ToString();
        }
    }

    // ------------------------------------------------------------------ updates: staging and installing a release zip
    class UpdatePlan
    {
        public string Root, Stage, Backup, Restart, FromVersion, ToVersion, Log;
        public int Pid;
        public List<string> Write = new List<string>(), Delete = new List<string>(), Skipped = new List<string>();

        public void Save(string path)
        {
            var l = new List<string>
            {
                "root=" + Root, "stage=" + Stage, "backup=" + Backup, "restart=" + Restart, "from=" + FromVersion,
                "to=" + ToVersion, "log=" + Log, "pid=" + Pid
            };
            l.AddRange(Write.Select(f => "write=" + f));
            l.AddRange(Delete.Select(f => "delete=" + f));
            l.AddRange(Skipped.Select(f => "skipped=" + f));
            File.WriteAllLines(path, l, new UTF8Encoding(false));
        }
        public static UpdatePlan Load(string path)
        {
            var p = new UpdatePlan();
            foreach (var line in File.ReadAllLines(path))
            {
                int k = line.IndexOf('=');
                if (k <= 0) continue;
                string key = line.Substring(0, k), v = line.Substring(k + 1);
                if (key == "root") p.Root = v; else if (key == "stage") p.Stage = v; else if (key == "backup") p.Backup = v;
                else if (key == "restart") p.Restart = v; else if (key == "from") p.FromVersion = v; else if (key == "to") p.ToVersion = v;
                else if (key == "log") p.Log = v; else if (key == "pid") int.TryParse(v, out p.Pid);
                else if (key == "write") p.Write.Add(Installer.Normalize(v));
                else if (key == "delete") p.Delete.Add(Installer.Normalize(v));
                else if (key == "skipped") p.Skipped.Add(v);
            }
            if (p.Root == null || p.Stage == null || p.Backup == null) throw new InvalidDataException("incomplete update plan " + path);
            return p;
        }
    }

    class UpdateFailed : Exception
    {
        public readonly bool Clean;   // true: every change was undone (or none was made)
        public UpdateFailed(string m, bool clean) : base(m) { Clean = clean; }
    }

    static class Installer
    {
        public const string Manifest = "release-files.txt";
        // The player's own data: never written, never deleted, whatever a release lists.
        static readonly string[] PlayerDirs =
        {
            "game", "build", "generated", "analysis", "external", "logs", "savestates", "checkpoints", "gamedata", ".launcher",
            "pgo", "screenshots", "textures", "texdump", "texpacks", "out", ".git", ".vs"
        };
        static readonly string[] PlayerFiles = { @"config\options.ini", @"config\options.ini.tmp", "options.ini" };

        // a release-relative path: backslashes, no root, no "..", no empty part
        public static string Normalize(string rel)
        {
            string r = (rel ?? "").Trim().Replace('/', '\\');
            if (r.Length == 0 || r.StartsWith("\\") || r.IndexOf(':') >= 0)
                throw new InvalidDataException("bad path in the release: \"" + rel + "\"");
            foreach (var part in r.Split('\\'))
                if (part.Length == 0 || part == "." || part == ".." || part.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0)
                    throw new InvalidDataException("bad path in the release: \"" + rel + "\"");
            return r;
        }

        public static bool IsPlayerData(string rel)
        {
            string top = rel.Split('\\')[0];
            if (PlayerDirs.Contains(top, StringComparer.OrdinalIgnoreCase)) return true;
            if (top.StartsWith("build-", StringComparison.OrdinalIgnoreCase)) return true;
            return PlayerFiles.Contains(rel, StringComparer.OrdinalIgnoreCase);
        }

        // Every existing part of root\rel below root must be a plain folder or file: the update never writes to
        // or deletes through a junction or a symbolic link.
        public static void CheckNoLinks(string root, string rel)
        {
            string p = root;
            foreach (var part in rel.Split('\\'))
            {
                p = Path.Combine(p, part);
                FileAttributes a;
                try { a = File.GetAttributes(p); }
                catch (FileNotFoundException) { return; }
                catch (DirectoryNotFoundException) { return; }
                if ((a & FileAttributes.ReparsePoint) != 0)
                    throw new IOException(p + " is a link (junction or symbolic link). The update never writes through links: move it away and try again.");
            }
        }

        // Unpacks the release zip into stageDir and returns its one top folder (inFamous2Recomp\).
        public static string Extract(string zip, string stageDir)
        {
            if (Directory.Exists(stageDir)) Directory.Delete(stageDir, true);
            Directory.CreateDirectory(stageDir);
            string full = Path.GetFullPath(stageDir).TrimEnd('\\') + "\\";
            string prefix = null;
            using (var fs = File.OpenRead(zip))
            using (var z = new ZipArchive(fs, ZipArchiveMode.Read))
                foreach (var e in z.Entries)
                {
                    if (e.FullName.EndsWith("/") || e.FullName.EndsWith("\\")) continue;     // folder entries
                    string rel = Normalize(e.FullName);
                    int cut = rel.IndexOf('\\');
                    if (cut < 0) throw new InvalidDataException("the zip holds " + rel + " outside its top folder");
                    string top = rel.Substring(0, cut);
                    if (prefix == null) prefix = top;
                    else if (!string.Equals(prefix, top, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("the zip has more than one top folder");
                    string dst = Path.GetFullPath(Path.Combine(stageDir, rel));
                    if (!dst.StartsWith(full, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("bad path in the zip: " + e.FullName);
                    Directory.CreateDirectory(Path.GetDirectoryName(dst));
                    using (var i = e.Open()) using (var o = File.Create(dst)) i.CopyTo(o);
                }
            if (prefix == null) throw new InvalidDataException("the zip is empty");
            string r = Path.Combine(stageDir, prefix);
            foreach (var need in new[] { "inFamous2Launcher.exe", @"tools\build.ps1", "ps3recomp.pin" })
                if (!File.Exists(Path.Combine(r, need))) throw new InvalidDataException("the zip has no " + prefix + "\\" + need + ": not an inFamous2Recomp release");
            return r;
        }

        static List<string> ReadManifest(string path)
        {
            return File.ReadAllLines(path).Select(l => l.Trim()).Where(l => l.Length > 0 && !l.StartsWith("#")).Select(Normalize).ToList();
        }

        // What the update writes (the release's files, but never player data), and what it deletes (files the
        // installed release listed and the new one does not; nothing when either manifest is missing).
        public static UpdatePlan MakePlan(string root, string staged, Action<string> log)
        {
            var files = Directory.GetFiles(staged, "*", SearchOption.AllDirectories)
                                 .Select(f => f.Substring(staged.Length + 1)).OrderBy(f => f, StringComparer.OrdinalIgnoreCase).ToList();
            var inZip = new HashSet<string>(files, StringComparer.OrdinalIgnoreCase);
            HashSet<string> listed = null;
            string nm = Path.Combine(staged, Manifest);
            if (File.Exists(nm))
            {
                listed = new HashSet<string>(ReadManifest(nm), StringComparer.OrdinalIgnoreCase);
                foreach (var f in files) if (!listed.Contains(f)) throw new InvalidDataException("the release zip holds " + f + ", which its " + Manifest + " does not list");
                foreach (var f in listed) if (!inZip.Contains(f)) throw new InvalidDataException("the release's " + Manifest + " lists " + f + ", which its zip does not hold");
            }
            else log("the new release has no " + Manifest + ": files it no longer has are left in place");

            var p = new UpdatePlan { Root = root, Stage = staged };
            int same = 0;
            foreach (var f in files)
            {
                if (IsPlayerData(f)) { p.Skipped.Add(f); continue; }
                CheckNoLinks(root, f);
                string t = Path.Combine(root, f), s = Path.Combine(staged, f);
                // a file that is already identical is left as it is (fewer writes, nothing to undo)
                if (File.Exists(t) && new FileInfo(t).Length == new FileInfo(s).Length && Util.Sha1File(t) == Util.Sha1File(s)) { same++; continue; }
                p.Write.Add(f);
            }
            log(p.Write.Count + " files new or changed, " + same + " already up to date");
            string om = Path.Combine(root, Manifest);
            if (!File.Exists(om)) log("this folder has no " + Manifest + " (an install from before v0.1.2): nothing is deleted");
            else if (listed != null)
                foreach (var f in ReadManifest(om))
                {
                    if (listed.Contains(f) || IsPlayerData(f)) continue;
                    CheckNoLinks(root, f);
                    if (File.Exists(Path.Combine(root, f))) p.Delete.Add(f);
                }
            foreach (var f in p.Skipped) log("left alone (player data folder): " + f);
            foreach (var f in p.Delete) log("to delete (no longer in the release): " + f);
            return p;
        }

        // Nothing may hold a file the update changes: checked for every file before the first one is touched.
        static void EnsureFree(string path, string rel)
        {
            try
            {
                bool ro = (File.GetAttributes(path) & FileAttributes.ReadOnly) != 0;
                using (new FileStream(path, FileMode.Open, ro ? FileAccess.Read : FileAccess.ReadWrite, FileShare.None)) { }
            }
            catch (Exception e)
            {
                if (!(e is IOException) && !(e is UnauthorizedAccessException)) throw;
                throw new IOException(rel + " is in use or cannot be written (" + e.Message.Trim() + ")\r\nClose what holds it " +
                                      "(another launcher window, an editor, the game) and try the update again.");
            }
        }

        // Runs in the updater, after the launcher has closed. All checks first; then every file that is replaced or
        // deleted is backed up, and any failure puts back every file already changed.
        public static void Apply(UpdatePlan p, Action<string> log, Action<int, int> progress)
        {
            try
            {
                foreach (var f in p.Write.Concat(p.Delete))
                {
                    if (IsPlayerData(f)) throw new IOException("the plan touches player data: " + f);
                    CheckNoLinks(p.Root, f);
                }
                foreach (var f in p.Write)
                    if (!File.Exists(Path.Combine(p.Stage, f))) throw new IOException("the staged file " + f + " is missing");
                foreach (var f in p.Write.Concat(p.Delete))
                {
                    string t = Path.Combine(p.Root, f);
                    if (File.Exists(t)) EnsureFree(t, f);
                    else if (Directory.Exists(t)) throw new IOException(f + " is a folder in this install, but a file in the release");
                }
            }
            catch (Exception e) { throw new UpdateFailed(e.Message, true); }
            log("checks passed: " + p.Write.Count + " files to write, " + p.Delete.Count + " to delete; nothing is in use");

            string fail = Environment.GetEnvironmentVariable("INF2L_UPDATE_TEST_FAIL");
            var done = new List<KeyValuePair<string, bool>>();     // file, existed before (then the backup holds it)
            var madeDirs = new List<string>();
            int n = 0, total = p.Write.Count + p.Delete.Count;
            string current = null;
            try
            {
                foreach (var f in p.Write)
                {
                    current = f;
                    string t = Path.Combine(p.Root, f);
                    bool existed = File.Exists(t);
                    if (existed) BackUp(p, f);
                    done.Add(new KeyValuePair<string, bool>(f, existed));
                    MakeDirs(p.Root, Path.GetDirectoryName(f), madeDirs);
                    if (existed) File.SetAttributes(t, File.GetAttributes(t) & ~FileAttributes.ReadOnly);
                    File.Copy(Path.Combine(p.Stage, f), t, true);
                    if (fail != null && string.Equals(Normalize(fail), f, StringComparison.OrdinalIgnoreCase))
                        throw new IOException("test failure after writing " + f + " (INF2L_UPDATE_TEST_FAIL)");
                    progress(++n, total);
                }
                foreach (var f in p.Delete)
                {
                    current = f;
                    string t = Path.Combine(p.Root, f);
                    BackUp(p, f);
                    done.Add(new KeyValuePair<string, bool>(f, true));
                    File.SetAttributes(t, FileAttributes.Normal);
                    File.Delete(t);
                    log("deleted " + f);
                    progress(++n, total);
                }
            }
            catch (Exception e)
            {
                log("FAILED at " + current + ": " + e.Message);
                log("undoing the " + done.Count + " file(s) already changed");
                var bad = new List<string>();
                for (int i = done.Count - 1; i >= 0; i--)
                {
                    string f = done[i].Key, t = Path.Combine(p.Root, f);
                    try
                    {
                        if (done[i].Value)
                        {
                            Directory.CreateDirectory(Path.GetDirectoryName(t));
                            if (File.Exists(t)) File.SetAttributes(t, FileAttributes.Normal);
                            File.Copy(Path.Combine(p.Backup, f), t, true);
                        }
                        else if (File.Exists(t)) { File.SetAttributes(t, FileAttributes.Normal); File.Delete(t); }
                    }
                    catch (Exception e2) { bad.Add(f + " (" + e2.Message + ")"); }
                }
                for (int i = madeDirs.Count - 1; i >= 0; i--) try { Directory.Delete(madeDirs[i], false); } catch { }
                if (bad.Count > 0)
                {
                    foreach (var b in bad) log("UNDO FAILED: " + b);
                    throw new UpdateFailed(current + ": " + e.Message + "\r\n\r\nThe undo failed for " + bad.Count + " file(s): " + string.Join(", ", bad), false);
                }
                log("undo complete: the folder is as it was before the update");
                throw new UpdateFailed(current + ": " + e.Message, true);
            }
        }

        static void BackUp(UpdatePlan p, string f)
        {
            string b = Path.Combine(p.Backup, f);
            Directory.CreateDirectory(Path.GetDirectoryName(b));
            File.Copy(Path.Combine(p.Root, f), b, true);
        }

        static void MakeDirs(string root, string relDir, List<string> made)
        {
            if (string.IsNullOrEmpty(relDir)) return;
            string d = root;
            foreach (var part in relDir.Split('\\'))
            {
                d = Path.Combine(d, part);
                if (!Directory.Exists(d)) { Directory.CreateDirectory(d); made.Add(d); }
            }
        }
    }

    // ------------------------------------------------------------------ updates: the windows
    class UpdateForm : Form
    {
        public UpdateForm(ReleaseInfo r, bool gitClone)
        {
            Text = "inFamous 2 -- update available";
            Font = new Font("Segoe UI", 9f);
            ClientSize = new Size(620, 430);
            FormBorderStyle = FormBorderStyle.FixedDialog; MaximizeBox = false; MinimizeBox = false;
            StartPosition = FormStartPosition.CenterParent;
            ShowInTaskbar = false;

            Controls.Add(new Label
            {
                Text = "A new version is available: " + r.Tag + "   (you have v" + AppInfo.Version + ")",
                Font = new Font("Segoe UI", 11.5f, FontStyle.Bold), Left = 16, Top = 12, Width = 590, Height = 26
            });
            Controls.Add(new Label { Text = r.Name ?? r.Tag, Left = 16, Top = 40, Width = 590, Height = 20, Font = new Font("Segoe UI", 9.5f, FontStyle.Bold) });
            var notes = new TextBox { Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical, Text = Updates.Excerpt(r.Body, 900) };
            notes.SetBounds(16, 64, 588, 196);
            Controls.Add(notes);
            var z = r.WindowsZip;
            string info;
            if (gitClone)
                info = "This folder is a git clone: update it with git pull, then build the launcher with tools\\build_launcher.cmd. " +
                       "Your game files, build, saves and settings are not part of the repository and stay as they are.";
            else if (z == null)
                info = "This release has no Windows zip to install: see its release notes.";
            else
                info = "Update now downloads " + z.Name + (z.Size > 0 ? " (" + (z.Size / 1048576.0).ToString("0.0", CultureInfo.InvariantCulture) + " MB)" : "") +
                       " from GitHub, checks it, closes the launcher, replaces the port's files and the launcher, and starts it again.\r\n\r\n" +
                       "Your game files, build, saves, settings and logs are kept. The game then needs a rebuild of a few minutes.";
            Controls.Add(new Label { Text = info, Left = 16, Top = 270, Width = 590, Height = 100 });

            var bUpd = new Button { Text = "Update now", Left = 16, Top = 384, Width = 120, Height = 32, DialogResult = DialogResult.Yes, Enabled = z != null && !gitClone };
            var bNotes = new Button { Text = "Release notes", Left = 144, Top = 384, Width = 120, Height = 32 };
            bNotes.Click += (s, e) => MainForm.OpenUrl(Updates.ReleasePage(r));
            var bSkip = new Button { Text = "Skip this version", Left = 364, Top = 384, Width = 130, Height = 32, DialogResult = DialogResult.Ignore };
            var bLater = new Button { Text = "Later", Left = 502, Top = 384, Width = 102, Height = 32, DialogResult = DialogResult.Cancel };
            Controls.AddRange(new Control[] { bUpd, bNotes, bSkip, bLater });
            AcceptButton = bUpd.Enabled ? (IButtonControl)bUpd : bLater;
            CancelButton = bLater;
            notes.TabStop = false;
            ActiveControl = bUpd.Enabled ? (Control)bUpd : bLater;     // not the notes (their text would show selected)
        }
    }

    // Downloads the release zip to a file, hashing it on the way; Cancel aborts.
    class DownloadForm : Form
    {
        readonly ReleaseAsset asset;
        readonly string dest;
        readonly ProgressBar bar = new ProgressBar();
        readonly Label lbl = new Label();
        readonly Button btn = new Button();
        volatile bool cancelled, running;
        HttpWebRequest req;
        int shown = -1;
        public string Error, Sha256;
        public long Bytes;

        public DownloadForm(ReleaseAsset asset, string dest)
        {
            this.asset = asset; this.dest = dest;
            Text = "inFamous 2 -- downloading the update";
            Font = new Font("Segoe UI", 9f);
            ClientSize = new Size(500, 120);
            FormBorderStyle = FormBorderStyle.FixedDialog; MaximizeBox = false; MinimizeBox = false;
            StartPosition = FormStartPosition.CenterParent; ShowInTaskbar = false;
            lbl.SetBounds(16, 14, 468, 20); lbl.Text = "Downloading " + asset.Name + " ...";
            bar.SetBounds(16, 40, 468, 22); bar.Maximum = 1000;
            btn.Text = "Cancel"; btn.SetBounds(394, 78, 90, 30);
            btn.Click += (s, e) => DoCancel();
            Controls.AddRange(new Control[] { lbl, bar, btn });
            FormClosing += (s, e) => { if (running) { e.Cancel = true; DoCancel(); } };
            Shown += (s, e) => { running = true; new Thread(Work) { IsBackground = true, Name = "update download" }.Start(); };
        }

        void DoCancel()
        {
            if (!running || cancelled) return;
            cancelled = true; btn.Enabled = false; lbl.Text = "Cancelling...";
            try { var r = req; if (r != null) r.Abort(); } catch { }
        }

        void Work()
        {
            try
            {
                req = Updates.Request(asset.Url, 30000);
                using (var resp = (HttpWebResponse)req.GetResponse())
                using (var s = resp.GetResponseStream())
                using (var o = new FileStream(dest, FileMode.Create, FileAccess.Write, FileShare.None))
                using (var h = SHA256.Create())
                {
                    long total = asset.Size > 0 ? asset.Size : resp.ContentLength, got = 0;
                    var buf = new byte[81920];
                    int n;
                    while ((n = s.Read(buf, 0, buf.Length)) > 0)
                    {
                        if (cancelled) throw new OperationCanceledException();
                        o.Write(buf, 0, n);
                        h.TransformBlock(buf, 0, n, null, 0);
                        got += n;
                        int pm = total > 0 ? (int)Math.Min(1000, got * 1000 / total) : 0;
                        if (pm != shown) { shown = pm; long g = got, t = total; BeginInvoke(new Action(() => Progress(g, t))); }
                    }
                    if (cancelled) throw new OperationCanceledException();
                    h.TransformFinalBlock(buf, 0, 0);
                    if (asset.Size > 0 && got != asset.Size) throw new IOException("downloaded " + got + " bytes; the release lists " + asset.Size);
                    Bytes = got;
                    Sha256 = Util.Hex(h.Hash);
                }
            }
            catch (Exception e) { if (!cancelled) Error = Updates.Describe(e); }
            BeginInvoke(new Action(() =>
            {
                running = false;
                DialogResult = Sha256 != null && Error == null && !cancelled ? DialogResult.OK : DialogResult.Cancel;
                Close();
            }));
        }

        void Progress(long got, long total)
        {
            bar.Value = total > 0 ? (int)Math.Min(1000, got * 1000 / total) : 0;
            if (!cancelled)
                lbl.Text = "Downloading " + asset.Name + ": " + (got / 1048576.0).ToString("0.0", CultureInfo.InvariantCulture) +
                           (total > 0 ? " of " + (total / 1048576.0).ToString("0.0", CultureInfo.InvariantCulture) : "") + " MB";
        }
    }

    // The updater: a temp copy of the launcher started with --apply-update. Waits for the launcher to close,
    // installs the staged release, and starts the launcher again (the new one, or the old one after an undo).
    class ApplyForm : Form
    {
        readonly string planPath;
        readonly Label lbl = new Label();
        readonly ProgressBar bar = new ProgressBar();
        UpdatePlan plan;
        StreamWriter log;

        public ApplyForm(string planPath)
        {
            this.planPath = planPath;
            Text = "inFamous 2 -- installing the update";
            Font = new Font("Segoe UI", 9f);
            ClientSize = new Size(480, 90);
            FormBorderStyle = FormBorderStyle.FixedDialog; MaximizeBox = false; MinimizeBox = false; ControlBox = false;
            StartPosition = FormStartPosition.CenterScreen;
            lbl.SetBounds(16, 14, 448, 20); lbl.Text = "Waiting for the launcher to close...";
            bar.SetBounds(16, 44, 448, 22); bar.Maximum = 1000;
            Controls.AddRange(new Control[] { lbl, bar });
            Shown += (s, e) => new Thread(Work) { IsBackground = true, Name = "apply update" }.Start();
        }

        void Log(string s)
        {
            try { if (log != null) { log.WriteLine(DateTime.Now.ToString("HH:mm:ss ", CultureInfo.InvariantCulture) + s); log.Flush(); } } catch { }
        }

        void Work()
        {
            string error = null;
            bool clean = true, restart = true;
            try
            {
                plan = UpdatePlan.Load(planPath);
                try { log = new StreamWriter(plan.Log, true, new UTF8Encoding(false)); } catch { }
                Log("updater: v" + plan.FromVersion + " -> v" + plan.ToVersion + " in " + plan.Root);
                try
                {
                    using (var parent = Process.GetProcessById(plan.Pid))
                        if (!parent.WaitForExit(60000)) { restart = false; throw new UpdateFailed("the launcher (process " + plan.Pid + ") did not close", true); }
                }
                catch (ArgumentException) { }       // already gone
                BeginInvoke(new Action(() => lbl.Text = "Installing v" + plan.ToVersion + " ..."));
                Installer.Apply(plan, Log, (n, t) => BeginInvoke(new Action(() => bar.Value = t > 0 ? n * 1000 / t : 1000)));
                Log("installed v" + plan.ToVersion + ": " + plan.Write.Count + " files written, " + plan.Delete.Count + " deleted, " + plan.Skipped.Count + " left alone");
            }
            catch (UpdateFailed e) { error = e.Message; clean = e.Clean; }
            catch (Exception e) { error = e.Message; }
            if (error != null) Log("update NOT installed: " + error + (clean ? "" : " -- the folder is partly updated; the backup is kept"));
            // clean up the staged copy and the backup (kept when the undo was incomplete)
            try
            {
                if (plan != null)
                {
                    string work = Path.GetDirectoryName(plan.Backup);
                    if (!clean) File.WriteAllText(Path.Combine(work, "KEEP.txt"), "backup of the files an incomplete update changed: " + plan.Backup + "\r\n");
                    else
                    {
                        if (Directory.Exists(plan.Backup)) Directory.Delete(plan.Backup, true);
                        string stage = Path.GetDirectoryName(plan.Stage);
                        if (Directory.Exists(stage)) Directory.Delete(stage, true);
                        foreach (var z in Directory.GetFiles(work, "*.zip")) File.Delete(z);
                    }
                }
            }
            catch { }
            BeginInvoke(new Action(() => Done(error, clean, restart)));
        }

        void Done(string error, bool clean, bool restart)
        {
            string logPath = plan != null ? plan.Log : null;
            if (error != null)
            {
                MessageBox.Show(this, clean
                    ? "The update could not be installed:\r\n\r\n" + error + "\r\n\r\nYour folder is as it was before the update (anything already replaced was put back)" +
                      (restart ? "; the launcher starts again." : ".")
                    : "The update failed and could not be fully undone:\r\n\r\n" + error + "\r\n\r\nThe changed files' backup is in " + (plan != null ? plan.Backup : "?") +
                      ". To repair the folder, extract the release zip over it (your game files, saves and settings are not in the zip).",
                    "inFamous 2 -- update", MessageBoxButtons.OK, clean ? MessageBoxIcon.Warning : MessageBoxIcon.Error);
                Log("updater: error shown");
            }
            if (restart && plan != null && File.Exists(plan.Restart))
            {
                string args = (error == null ? "--updated \"" + plan.FromVersion + "\"" : "--update-failed") + (logPath != null ? " --update-log \"" + logPath + "\"" : "");
                try { Process.Start(new ProcessStartInfo(plan.Restart, args) { UseShellExecute = false, WorkingDirectory = plan.Root }); Log("started " + plan.Restart + " " + args); }
                catch (Exception e) { Log("could not start the launcher: " + e.Message); }
            }
            try { if (log != null) log.Dispose(); } catch { }
            Close();
        }
    }

    // ------------------------------------------------------------------ main window
    partial class MainForm : Form
    {
        readonly string root;
        readonly Options opts;
        readonly GameFiles files;
        readonly Pipeline pipe;
        readonly State state;
        List<Prereq> prereqs = new List<Prereq>();

        readonly Label lblFiles = new Label(), lblStep = new Label(), lblElapsed = new Label(), lblHint = new Label();
        readonly Button btnFiles = new Button(), btnMain = new Button(), btnCancel = new Button(), btnRebuild = new Button(),
                        btnLogs = new Button(), btnFolder = new Button(), btnRecheck = new Button(), btnReport = new Button();
        readonly LinkLabel lnkOldBuild = new LinkLabel();
        readonly ProgressBar bar = new ProgressBar();
        readonly TextBox txtLog = new TextBox();
        readonly TableLayoutPanel tblPre = new TableLayoutPanel();
        readonly System.Windows.Forms.Timer tick = new System.Windows.Forms.Timer();

        // running state
        Process proc;              // the current step's powershell
        int stepIndex = -1, runFrom = 0;
        bool cancelling, building;
        DateTime runStart, stepStart;
        double stepFrac;
        string[] fps;
        StreamWriter logFile;
        string logPath;
        readonly object qLock = new object();
        readonly List<string> queue = new List<string>();
        readonly List<string> tail = new List<string>();
        Process game;              // the run.ps1 powershell while the game runs
        DateTime gameStart;

        // updates
        readonly Panel content = new Panel(), banner = new Panel();
        readonly Label lblBanner = new Label(), lblVersion = new Label();
        readonly Button btnCheckUpd = new Button();
        readonly CheckBox chkUpd = new CheckBox();
        ReleaseInfo latest;        // a newer release than this launcher, once the check found one
        bool checking, pendingDialog, bannerGrown;

        [DllImport("user32.dll")] static extern bool IsWindowEnabled(IntPtr hWnd);

        public MainForm(string root, Options opts)
        {
            this.root = root; this.opts = opts;
            files = new GameFiles(root);
            pipe = new Pipeline(root, files);
            state = new State(root);

            Text = "inFamous 2 -- PC launcher v" + AppInfo.Version;
            Font = new Font("Segoe UI", 9f);
            ClientSize = new Size(1000, 760);
            MinimumSize = new Size(820, 640);
            StartPosition = FormStartPosition.CenterScreen;
            try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }

            // everything below the update banner lives in `content` (the banner, when shown, pushes it down)
            content.Bounds = new Rectangle(0, 0, 1000, 760);
            var Controls = content.Controls;

            var title = new Label { Text = "inFamous 2 -- native PC build (static recompilation)", Font = new Font("Segoe UI", 15f, FontStyle.Bold), Left = 14, Top = 10, Width = 780, Height = 32 };
            var sub = new Label { Text = "Builds a Windows executable from your own copy of the game (BCUS98125, update 1.04; European copies: beta) and starts it. No emulator; no game files included.", Left = 16, Top = 44, Width = 830, Height = 20, ForeColor = SystemColors.GrayText };
            Controls.Add(title); Controls.Add(sub);

            lblVersion.Text = "launcher v" + AppInfo.Version; lblVersion.SetBounds(800, 4, 188, 16); lblVersion.TextAlign = ContentAlignment.TopRight;
            lblVersion.ForeColor = SystemColors.GrayText; lblVersion.Anchor = AnchorStyles.Top | AnchorStyles.Right;
            btnCheckUpd.Text = "Check for updates"; btnCheckUpd.SetBounds(852, 20, 136, 26); btnCheckUpd.Anchor = AnchorStyles.Top | AnchorStyles.Right;
            btnCheckUpd.Click += (s, e) => CheckForUpdates(true);
            chkUpd.Text = "Check at startup"; chkUpd.SetBounds(856, 48, 132, 20); chkUpd.Anchor = AnchorStyles.Top | AnchorStyles.Right;
            chkUpd.Checked = !Updates.OffByEnv && state.Get("update.check") != "0";
            if (Updates.OffByEnv) chkUpd.Enabled = false;
            new ToolTip().SetToolTip(chkUpd, Updates.OffByEnv ? "Off: INF2L_NO_UPDATE_CHECK=1 is set." :
                                     "At startup, ask GitHub whether a newer release exists (one request to api.github.com; nothing about you is sent).");
            chkUpd.CheckedChanged += (s, e) =>
            {
                state.Set("update.check", chkUpd.Checked ? null : "0");
                Append("update check at startup: " + (chkUpd.Checked ? "on" : "off"));
            };
            Controls.Add(lblVersion); Controls.Add(btnCheckUpd); Controls.Add(chkUpd);

            var gFiles = new GroupBox { Text = "1. Your game files", Left = 12, Top = 70, Width = 976, Height = 64, Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right };
            lblFiles.SetBounds(12, 22, 820, 36); lblFiles.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
            btnFiles.Text = "Choose..."; btnFiles.SetBounds(860, 22, 104, 28); btnFiles.Anchor = AnchorStyles.Top | AnchorStyles.Right;
            btnFiles.Click += (s, e) => ChooseFiles();
            gFiles.Controls.Add(lblFiles); gFiles.Controls.Add(btnFiles);
            Controls.Add(gFiles);

            var gPre = new GroupBox { Text = "2. Build tools", Left = 12, Top = 140, Width = 976, Height = 196, Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right };
            tblPre.SetBounds(10, 20, 954, 140); tblPre.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
            tblPre.ColumnCount = 3;
            tblPre.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 300));
            tblPre.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            tblPre.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 90));
            btnRecheck.Text = "Check again"; btnRecheck.SetBounds(10, 162, 104, 26);
            btnRecheck.Click += (s, e) => { DetectPrereqs(); RefreshUi(); };
            gPre.Controls.Add(tblPre); gPre.Controls.Add(btnRecheck);
            Controls.Add(gPre);

            var gBuild = new GroupBox { Text = "3. Build and play", Left = 12, Top = 342, Width = 976, Height = 360, Anchor = AnchorStyles.Top | AnchorStyles.Bottom | AnchorStyles.Left | AnchorStyles.Right };
            lblStep.SetBounds(12, 22, 700, 20); lblStep.Font = new Font("Segoe UI", 9.5f, FontStyle.Bold); lblStep.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
            lblElapsed.SetBounds(720, 22, 244, 20); lblElapsed.TextAlign = ContentAlignment.TopRight; lblElapsed.Anchor = AnchorStyles.Top | AnchorStyles.Right;
            bar.SetBounds(12, 46, 952, 20); bar.Maximum = 1000; bar.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
            txtLog.Multiline = true; txtLog.ReadOnly = true; txtLog.WordWrap = false; txtLog.ScrollBars = ScrollBars.Both;
            txtLog.Font = new Font("Consolas", 8.5f); txtLog.BackColor = Color.FromArgb(24, 24, 24); txtLog.ForeColor = Color.Gainsboro;
            txtLog.SetBounds(12, 72, 952, 278); txtLog.Anchor = AnchorStyles.Top | AnchorStyles.Bottom | AnchorStyles.Left | AnchorStyles.Right;
            gBuild.Controls.AddRange(new Control[] { lblStep, lblElapsed, bar, txtLog });
            Controls.Add(gBuild);

            int by = 712;
            btnMain.SetBounds(808, by, 180, 38); btnMain.Font = new Font("Segoe UI", 11f, FontStyle.Bold); btnMain.Anchor = AnchorStyles.Bottom | AnchorStyles.Right;
            btnMain.Click += (s, e) => MainAction();
            btnCancel.Text = "Cancel"; btnCancel.SetBounds(712, by + 6, 90, 30); btnCancel.Anchor = AnchorStyles.Bottom | AnchorStyles.Right;
            btnCancel.Click += (s, e) => CancelRun();
            btnRebuild.Text = "Rebuild"; btnRebuild.SetBounds(12, by + 6, 90, 30); btnRebuild.Anchor = AnchorStyles.Bottom | AnchorStyles.Left;
            btnRebuild.Click += (s, e) => Rebuild();
            btnLogs.Text = "Open logs"; btnLogs.SetBounds(108, by + 6, 90, 30); btnLogs.Anchor = AnchorStyles.Bottom | AnchorStyles.Left;
            btnLogs.Click += (s, e) => OpenFolder(Path.Combine(root, "logs"), null);
            btnFolder.Text = "Open game folder"; btnFolder.SetBounds(204, by + 6, 120, 30); btnFolder.Anchor = AnchorStyles.Bottom | AnchorStyles.Left;
            btnFolder.Click += (s, e) => OpenFolder(root, File.Exists(ExePath) ? ExePath : null);
            btnReport.Text = "Save report"; btnReport.SetBounds(330, by + 6, 96, 30); btnReport.Anchor = AnchorStyles.Bottom | AnchorStyles.Left;
            btnReport.Click += (s, e) => SaveReport();
            new ToolTip().SetToolTip(btnReport, "Saves logs\\" + "report-<date>.txt and a .zip of it with your newest logs (for GitHub) (eu-report-... for a European copy): your EBOOT and libsre identity, " +
                                     "what the build found in your EBOOT, the build state and the newest logs' last lines, with your user folder " +
                                     "replaced by %USERPROFILE%. Attach it to your issue or beta report.");
            lnkOldBuild.Text = "Play the existing build without updating it"; lnkOldBuild.SetBounds(434, by + 12, 270, 20); lnkOldBuild.Anchor = AnchorStyles.Bottom | AnchorStyles.Left;
            lnkOldBuild.LinkClicked += (s, e) => LaunchGame();
            lblHint.SetBounds(434, by + 12, 272, 20); lblHint.Anchor = AnchorStyles.Bottom | AnchorStyles.Left; lblHint.ForeColor = SystemColors.GrayText;
            Controls.AddRange(new Control[] { btnMain, btnCancel, btnRebuild, btnLogs, btnFolder, btnReport, lnkOldBuild, lblHint });
            AcceptButton = btnMain;

            // the update banner (hidden until the check finds a newer release)
            banner.Bounds = new Rectangle(0, 0, 1000, 44);
            banner.BackColor = Color.FromArgb(255, 236, 170);
            banner.Visible = false;
            lblBanner.SetBounds(14, 0, 520, 44); lblBanner.TextAlign = ContentAlignment.MiddleLeft; lblBanner.AutoEllipsis = true;
            lblBanner.Font = new Font("Segoe UI", 9.5f, FontStyle.Bold); lblBanner.ForeColor = Color.FromArgb(60, 40, 0);
            lblBanner.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
            banner.Controls.Add(lblBanner);
            int bx = 1000 - 12;
            foreach (var b in new[] { new { T = "Later", W = 70 }, new { T = "Skip this version", W = 124 }, new { T = "Release notes", W = 106 }, new { T = "Update now", W = 100 } })
            {
                bx -= b.W;
                var btn = new Button { Text = b.T, Left = bx, Top = 8, Width = b.W, Height = 28, Anchor = AnchorStyles.Top | AnchorStyles.Right, BackColor = SystemColors.Control, UseVisualStyleBackColor = true };
                string which = b.T;
                btn.Click += (s, e) => BannerAction(which);
                banner.Controls.Add(btn);
                bx -= 6;
            }
            this.Controls.Add(banner);
            this.Controls.Add(content);
            banner.Dock = DockStyle.Top;
            content.Dock = DockStyle.Fill;
            content.BringToFront();
            Activated += (s, e) => { if (pendingDialog && CanShowUpdateDialog()) { pendingDialog = false; BeginInvoke(new Action(ShowUpdateDialog)); } };

            tick.Interval = 150;
            tick.Tick += (s, e) => OnTick();
            tick.Start();
            FormClosing += OnClosing;
            Shown += (s, e) => OnShown();
        }

        string ExePath { get { return Path.Combine(root, @"build\infamous2.exe"); } }

        void OnShown()
        {
            Append("launcher v" + AppInfo.Version + ": " + root);
            // the update check runs in the background: it never holds up the window
            if (Updates.OffByEnv) Append("update check: off (INF2L_NO_UPDATE_CHECK=1)");
            else if (state.Get("update.check") == "0") Append("update check: off (\"Check at startup\" is unticked)");
            else CheckForUpdates(false);
            ThreadPool.QueueUserWorkItem(_ => Updates.CleanTemp());
            if (opts.Updated != null) state.Set("update.installed", AppInfo.Version);
            ShowUpdateLog();
            DetectPrereqs();
            RefreshUi();
            if (!files.Present) BeginInvoke(new Action(ChooseFiles));
            else if (opts.Updated != null && File.Exists(ExePath) && FirstStale() < Pipeline.Steps.Length)
                BeginInvoke(new Action(() => MessageBox.Show(this,
                    "The launcher was updated to v" + AppInfo.Version + " (from v" + opts.Updated + ").\r\n\r\n" +
                    "The update changed the port's sources, so the game needs a rebuild: it takes a few minutes. " +
                    "Press \"Continue build\" (or Rebuild).\r\n\r\nYour game files, saves, settings and logs are kept.",
                    "inFamous 2 -- updated", MessageBoxButtons.OK, MessageBoxIcon.Information)));
        }

        // ---------------------------------------------------------- updates
        void ShowUpdateLog()
        {
            if (opts.Updated != null) Append("updated from v" + opts.Updated + " to v" + AppInfo.Version);
            if (opts.UpdateFailed) Append("the update was NOT installed (see below); this is still v" + AppInfo.Version);
            if (opts.UpdateLog == null) return;
            Append("update log: " + opts.UpdateLog);
            try
            {
                var lines = File.ReadAllLines(opts.UpdateLog);
                foreach (var l in lines.Skip(Math.Max(0, lines.Length - 30))) Append("  | " + l);
            }
            catch (Exception e) { Append("  (could not read it: " + e.Message + ")"); }
        }

        void CheckForUpdates(bool manual)
        {
            if (checking) return;
            checking = true;
            btnCheckUpd.Enabled = false;
            Append("update check: asking " + Updates.ApiUrl);
            var t = new Thread(() =>
            {
                ReleaseInfo r = null; string err = null;
                try { r = Updates.FetchLatest(10000); }
                catch (Exception ex) { err = Updates.Describe(ex); }
                try { if (!IsDisposed && IsHandleCreated) BeginInvoke(new Action(() => CheckDone(r, err, manual))); } catch { }
            }) { IsBackground = true, Name = "update check" };
            t.Start();
        }

        void CheckDone(ReleaseInfo r, string err, bool manual)
        {
            checking = false;
            btnCheckUpd.Enabled = true;
            if (err != null)
            {
                Append("update check failed: " + err);
                if (manual) MessageBox.Show(this, "Could not reach GitHub to check for updates:\r\n\r\n" + err, Text, MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            if (!Updates.IsNewer(r))
            {
                Append("update check: v" + AppInfo.Version + " is up to date (latest release: " + r.Tag +
                       (r.Draft || r.Prerelease ? ", a draft or pre-release: ignored" : !SemVer.Valid(r.Tag) ? ", not a version number: ignored" : "") + ")");
                if (manual) MessageBox.Show(this, "You have the latest version (v" + AppInfo.Version + ").", Text, MessageBoxButtons.OK, MessageBoxIcon.Information);
                latest = null;
                HideBanner();
                return;
            }
            latest = r;
            Append("update available: " + Updates.Title(r) + " (you have v" + AppInfo.Version + ")");
            if (!manual && state.Get("update.skip") == r.Version)
            {
                Append("  you chose to skip " + r.Tag + "; \"Check for updates\" shows it again");
                return;
            }
            ShowBanner();
            // the dialog: once per version at startup (the banner stays), every time on "Check for updates"
            if (manual || state.Get("update.notified") != r.Version)
            {
                if (CanShowUpdateDialog()) ShowUpdateDialog();
                else pendingDialog = true;
            }
        }

        bool CanShowUpdateDialog()
        {
            return latest != null && !building && game == null && WindowState != FormWindowState.Minimized &&
                   IsHandleCreated && IsWindowEnabled(Handle);      // not over another dialog
        }

        void ShowUpdateDialog()
        {
            if (latest == null) return;
            state.Set("update.notified", latest.Version);
            DialogResult res;
            using (var f = new UpdateForm(latest, IsGitClone)) res = f.ShowDialog(this);
            if (res == DialogResult.Yes) StartUpdate();
            else if (res == DialogResult.Ignore) SkipVersion();
            else Append("update " + latest.Tag + ": later (the banner stays until you skip or install it)");
        }

        void ShowBanner()
        {
            lblBanner.Text = "Update available: " + Updates.Title(latest) + "   (you have v" + AppInfo.Version + ")";
            if (banner.Visible) return;
            banner.Visible = true;
            // grow the window by the banner's height so the build area keeps its size
            if (WindowState == FormWindowState.Normal) { Height += banner.Height; bannerGrown = true; }
        }

        void HideBanner()
        {
            if (!banner.Visible) return;
            banner.Visible = false;
            if (bannerGrown && WindowState == FormWindowState.Normal) Height -= banner.Height;
            bannerGrown = false;
        }

        void BannerAction(string which)
        {
            if (latest == null) { HideBanner(); return; }
            if (which == "Update now") StartUpdate();
            else if (which == "Release notes") OpenUrl(Updates.ReleasePage(latest));
            else if (which == "Skip this version") SkipVersion();
            else { HideBanner(); Append("update " + latest.Tag + ": later (shown again at the next start)"); }
        }

        void SkipVersion()
        {
            state.Set("update.skip", latest.Version);
            Append("update " + latest.Tag + " skipped: not offered again at startup (\"Check for updates\" still shows it)");
            HideBanner();
        }

        public static void OpenUrl(string url)
        {
            try { Process.Start(new ProcessStartInfo(url) { UseShellExecute = true }); } catch { }
        }

        bool IsGitClone { get { return Directory.Exists(Path.Combine(root, ".git")) || File.Exists(Path.Combine(root, ".git")); } }

        void StartUpdate()
        {
            if (latest == null) return;
            if (building) { MessageBox.Show(this, "Finish or cancel the build first, then update.", Text); return; }
            if (game != null) { MessageBox.Show(this, "Close the game first, then update.", Text); return; }
            if (IsGitClone)
            {
                MessageBox.Show(this, "This folder is a git clone: update it with git pull, then build the launcher with tools\\build_launcher.cmd.\r\n\r\n" +
                                "(Update now installs release zips; it would leave your clone with uncommitted changes.)", Text);
                return;
            }
            var asset = latest.WindowsZip;
            if (asset == null || !Updates.DownloadAllowed(asset.Url))
            {
                Append("update: " + (asset == null ? "the release has no inFamous2Recomp-*-windows.zip" : "refusing the download address " + asset.Url));
                MessageBox.Show(this, "This release has no Windows zip the launcher can install. Get it from the release page.", Text);
                OpenUrl(Updates.ReleasePage(latest));
                return;
            }
            string stamp = DateTime.Now.ToString("yyyyMMdd-HHmmss");
            string work = Path.Combine(Updates.TempRoot, latest.Version + "-" + stamp);
            string zip = Path.Combine(work, asset.Name);
            Directory.CreateDirectory(work);
            Append("update: downloading " + asset.Url);
            string sha;
            long bytes;
            using (var d = new DownloadForm(asset, zip))
            {
                if (d.ShowDialog(this) != DialogResult.OK)
                {
                    Append(d.Error != null ? "update: download failed: " + d.Error : "update: download cancelled");
                    if (d.Error != null) MessageBox.Show(this, "The download failed:\r\n\r\n" + d.Error, Text, MessageBoxButtons.OK, MessageBoxIcon.Warning);
                    TryDeleteDir(work);
                    return;
                }
                sha = d.Sha256; bytes = d.Bytes;
            }
            Append("update: downloaded " + bytes + " bytes, SHA-256 " + sha);
            string want = Updates.DigestSha256(asset.Digest);
            if (want != null)
            {
                if (want != sha)
                {
                    Append("update: SHA-256 MISMATCH -- GitHub lists " + want + "; the download is refused and deleted");
                    MessageBox.Show(this, "The download does not match the checksum GitHub lists for it, so it was not installed.\r\n\r\n" +
                                    "Expected SHA-256 " + want + "\r\nDownloaded " + sha + "\r\n\r\nTry again later.", Text, MessageBoxButtons.OK, MessageBoxIcon.Error);
                    TryDeleteDir(work);
                    return;
                }
                Append("update: SHA-256 matches the digest GitHub lists for " + asset.Name);
            }
            else
            {
                Append("update: GitHub lists no SHA-256 digest for " + asset.Name + ": the download cannot be verified");
                if (MessageBox.Show(this, "GitHub lists no checksum for " + asset.Name + ", so the download cannot be verified.\r\n\r\nSHA-256 of the download: " + sha +
                                    "\r\n\r\nInstall it anyway?", Text, MessageBoxButtons.YesNo, MessageBoxIcon.Warning, MessageBoxDefaultButton.Button2) != DialogResult.Yes)
                {
                    Append("update: not installed (unverified download declined)");
                    TryDeleteDir(work);
                    return;
                }
                Append("update: installing the unverified download (you confirmed)");
            }
            UpdatePlan plan;
            try
            {
                Cursor = Cursors.WaitCursor;
                string staged = Installer.Extract(zip, Path.Combine(work, "stage"));
                plan = Installer.MakePlan(root, staged, s => Append("update: " + s));
            }
            catch (Exception ex)
            {
                Cursor = Cursors.Default;
                Append("update: refused: " + ex.Message);
                MessageBox.Show(this, "The update was not installed:\r\n\r\n" + ex.Message + "\r\n\r\nNothing was changed.", Text, MessageBoxButtons.OK, MessageBoxIcon.Warning);
                TryDeleteDir(work);
                return;
            }
            Cursor = Cursors.Default;
            plan.Backup = Path.Combine(work, "backup");
            plan.Restart = Path.Combine(root, "inFamous2Launcher.exe");
            plan.FromVersion = AppInfo.Version; plan.ToVersion = latest.Version;
            plan.Pid = Process.GetCurrentProcess().Id;
            plan.Log = Path.Combine(Updates.TempRoot, "update-" + stamp + ".log");
            string planPath = Path.Combine(work, "plan.txt");
            string updater = Path.Combine(work, "inFamous2Launcher-updater.exe");
            try
            {
                plan.Save(planPath);
                File.Copy(Application.ExecutablePath, updater, true);
                Process.Start(new ProcessStartInfo(updater, "--apply-update \"" + planPath + "\"") { UseShellExecute = false, WorkingDirectory = work });
            }
            catch (Exception ex)
            {
                Append("update: could not start the updater: " + ex.Message);
                MessageBox.Show(this, "Could not start the updater: " + ex.Message + "\r\n\r\nNothing was changed.", Text, MessageBoxButtons.OK, MessageBoxIcon.Warning);
                TryDeleteDir(work);
                return;
            }
            Append("update: " + plan.Write.Count + " files to write, " + plan.Delete.Count + " to delete; the launcher closes now and restarts as " + latest.Tag);
            Flush();
            Close();
        }

        static void TryDeleteDir(string d) { try { if (Directory.Exists(d)) Directory.Delete(d, true); } catch { } }

        // ---------------------------------------------------------- game files
        void ChooseFiles()
        {
            if (building) return;
            using (var f = new GameFilesForm(files, opts))
            {
                if (f.ShowDialog(this) != DialogResult.OK) { RefreshUi(); return; }
                string err = null;
                Cursor = Cursors.WaitCursor;
                lblStep.Text = "Copying your update files into game\\ ...";
                Refresh();
                try { Import(f.DiscGame, f.UpdateDir, f.EbootPath, f.LibsrePath); }
                catch (Exception ex) { err = ex.Message; }
                Cursor = Cursors.Default;
                if (err != null) { Append("import failed: " + err); MessageBox.Show(this, "Could not set up game\\:\r\n\r\n" + err, Text, MessageBoxButtons.OK, MessageBoxIcon.Error); }
                else Append("game files set up in game\\ (disc linked from " + f.DiscGame + ")");
            }
            RefreshUi();
        }

        void Import(string discGame, string updDir, string eboot, string libsre)
        {
            GameSetup.Import(files, state, discGame, updDir, eboot, libsre);
        }
    }

    // ------------------------------------------------------------------ setting up game\ (the files dialog's "Use these files", --prepare)
    static class GameSetup
    {
        // libsre null: keep what is there (headless, when the libsre lifted earlier is kept)
        public static void Import(GameFiles files, State state, string discGame, string updDir, string eboot, string libsre)
        {
            string game = files.Game;
            Directory.CreateDirectory(Path.Combine(game, "disc"));
            // disc: a junction game\disc\PS3_GAME -> the player's PS3_GAME (read-only use; 15+ GB stays where it is)
            string link = files.DiscGame;
            string full = Path.GetFullPath(discGame).TrimEnd('\\');
            if (!string.Equals(full, Path.GetFullPath(link).TrimEnd('\\'), StringComparison.OrdinalIgnoreCase))
            {
                if (Directory.Exists(link))
                {
                    var a = File.GetAttributes(link);
                    if ((a & FileAttributes.ReparsePoint) == 0)
                        throw new IOException(link + " is a real folder. Move it away (or choose it as the disc folder) and try again.");
                    Directory.Delete(link, false);         // the link only, never its target
                }
                string o;
                int rc = Util.Run(Path.Combine(Environment.SystemDirectory, "cmd.exe"), "/c mklink /J \"" + link + "\" \"" + full + "\"", out o, 20000);
                if (rc != 0 || !Directory.Exists(link)) throw new IOException("mklink /J failed: " + o.Trim());
            }
            // update: a copy (small), so nothing the game writes ever lands in the original
            string title = files.TitleId;
            string dst = Path.Combine(game, @"hdd0\game\" + title);
            string src = Path.GetFullPath(updDir).TrimEnd('\\');
            if (!string.Equals(src, Path.GetFullPath(dst).TrimEnd('\\'), StringComparison.OrdinalIgnoreCase))
                CopyTree(src, dst);
            if (!string.IsNullOrEmpty(eboot))
            {
                string to = Path.Combine(dst, @"USRDIR\EBOOT.elf");
                if (!string.Equals(Path.GetFullPath(eboot), Path.GetFullPath(to), StringComparison.OrdinalIgnoreCase))
                    File.Copy(eboot, to, true);
            }
            // libsre
            string lto = files.LibsrePath;
            if (libsre != null)
            {
                Directory.CreateDirectory(Path.GetDirectoryName(lto));
                if (!string.Equals(Path.GetFullPath(libsre), Path.GetFullPath(lto), StringComparison.OrdinalIgnoreCase))
                    File.Copy(libsre, lto, true);
            }
            state.Set("files.disc", full);
            state.Set("files.update", src);
        }

        public static void CopyTree(string src, string dst)
        {
            Directory.CreateDirectory(dst);
            foreach (var d in Directory.GetDirectories(src, "*", SearchOption.AllDirectories))
                Directory.CreateDirectory(Path.Combine(dst, d.Substring(src.Length + 1)));
            foreach (var f in Directory.GetFiles(src, "*", SearchOption.AllDirectories))
            {
                string t = Path.Combine(dst, f.Substring(src.Length + 1));
                var fi = new FileInfo(f);
                if (File.Exists(t))
                {
                    var ti = new FileInfo(t);
                    if (ti.Length == fi.Length && ti.LastWriteTimeUtc == fi.LastWriteTimeUtc) continue;
                }
                File.Copy(f, t, true);
                File.SetAttributes(t, FileAttributes.Normal);
            }
        }
    }

    partial class MainForm
    {
        // ---------------------------------------------------------- prerequisites
        void DetectPrereqs()
        {
            prereqs = Prereqs.Detect(opts);
            tblPre.SuspendLayout();
            tblPre.Controls.Clear();
            tblPre.RowStyles.Clear();
            tblPre.RowCount = prereqs.Count;
            int r = 0;
            foreach (var p in prereqs)
            {
                tblPre.RowStyles.Add(new RowStyle(SizeType.Absolute, 23));
                tblPre.Controls.Add(new Label { Text = p.Name, AutoSize = false, Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft }, 0, r);
                string st = p.Ok ? "OK  " + p.Found : p.Outdated ? "TOO OLD -- " + p.Note : "MISSING -- " + p.Why + (p.Note != null ? ". " + p.Note : "");
                tblPre.Controls.Add(new Label
                {
                    Text = st, AutoSize = false, Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft, AutoEllipsis = true,
                    ForeColor = p.Ok ? Color.DarkGreen : Color.Firebrick
                }, 1, r);
                if (!p.Ok && p.WingetId != null)
                {
                    var b = new Button { Text = p.Outdated ? "Update" : "Install", Dock = DockStyle.Fill, Margin = new Padding(2, 0, 2, 1), Tag = p };
                    b.Click += (s, e) => InstallPrereq((Prereq)((Button)s).Tag);
                    tblPre.Controls.Add(b, 2, r);
                }
                r++;
            }
            tblPre.ResumeLayout();
            foreach (var p in prereqs) Append("prerequisite " + p.Id + ": " + (p.Ok ? "ok (" + p.Found + ")" : "missing" + (p.Note != null ? " -- " + p.Note : "")));
        }

        void InstallPrereq(Prereq p)
        {
            string cmd = Prereqs.WingetCommand(p);
            string winget = Prereqs.WingetExe();
            if (winget == null)
            {
                OfferManualInstall(p, "winget (App Installer) is not available on this PC.");
                return;
            }
            var ok = MessageBox.Show(this,
                (p.Outdated ? p.Name + " is too old: " + p.Note + ".\r\n\r\nInstall the current version over it with winget? This runs"
                            : p.Name + " is missing.\r\n\r\nInstall it with winget? This runs") + ", in a console window you can watch:\r\n\r\n    " + cmd +
                "\r\n\r\nwinget downloads it from its publisher and may ask you to accept its licence and to allow the installer (UAC)." +
                (opts.WingetDryRun ? "\r\n\r\n(dry run: the command is only written to the log)" : ""),
                "Install " + p.Name, MessageBoxButtons.OKCancel, MessageBoxIcon.Question);
            if (ok != DialogResult.OK) { Append("install of " + p.Id + " declined"); return; }
            if (opts.WingetDryRun) { Append("[dry run] would run: " + cmd); return; }
            Append("running: " + cmd);
            string how = null;
            try
            {
                // /v:on: !ERRORLEVEL! is winget's code (%ERRORLEVEL% would be read before winget runs), and cmd
                // exits with it after the pause, so the log has the real code.
                var psi = new ProcessStartInfo(Path.Combine(Environment.SystemDirectory, "cmd.exe"),
                    "/v:on /c \"" + cmd + " & set \"E=!ERRORLEVEL!\" & echo. & echo winget finished with exit code !E!. & pause & exit !E!\"") { UseShellExecute = false };
                using (var pr = Process.Start(psi))
                {
                    Enabled = false;
                    while (!pr.WaitForExit(200)) Application.DoEvents();
                    Enabled = true;
                    Append("winget exited with " + pr.ExitCode + (pr.ExitCode != 0 ? " (0x" + pr.ExitCode.ToString("X8") + ")" : ""));
                    if (pr.ExitCode != 0) how = "winget failed (exit code 0x" + pr.ExitCode.ToString("X8") + ").";
                }
            }
            catch (Exception ex) { Enabled = true; Append("winget failed to start: " + ex.Message); how = "winget could not be started (" + ex.Message + ")."; }
            DetectPrereqs();
            RefreshUi();
            var now = prereqs.FirstOrDefault(q => q.Id == p.Id);
            if (now != null && !now.Ok)
                OfferManualInstall(now, how ?? (now.Outdated ? "winget finished, but the old version is still the one found." : "winget finished, but it is still not found."));
        }

        // winget is missing or failed (a broken winget source, a policy, no Store): point the player at the download page.
        void OfferManualInstall(Prereq p, string why)
        {
            Append("manual install offered for " + p.Id + ": " + why);
            if (p.Url == null) { MessageBox.Show(this, why + "\r\n\r\nInstall " + p.Name + " yourself, then press \"Check again\".", Text); return; }
            var r = MessageBox.Show(this, why + "\r\n\r\nYou can install " + p.Name + " yourself instead:\r\n\r\n    " + p.Url +
                (p.Id == "llvm" ? "\r\n\r\nPick LLVM-<version>-win64.exe (version 19 or newer) and keep the default folder (Program Files\\LLVM)." :
                 p.Id == "git" ? "\r\n\r\nKeep the default folder (Program Files\\Git)." :
                 p.Id == "vs" ? "\r\n\r\nIn the installer, tick \"Desktop development with C++\"." : "") +
                "\r\n\r\nThen press \"Check again\". Open that page now?", "Install " + p.Name, MessageBoxButtons.YesNo, MessageBoxIcon.Information);
            if (r == DialogResult.Yes) OpenUrl(p.Url);
        }

        // ---------------------------------------------------------- UI state
        int FirstStale()
        {
            fps = pipe.Fingerprints();
            for (int i = 0; i < Pipeline.Steps.Length; i++)
                if (state.Get("done." + Pipeline.Steps[i].Id) != fps[i] || !pipe.MarkerOk(i) || Moved(i) != null) return i;
            return Pipeline.Steps.Length;
        }

        // the step is up to date but ran in another folder (issue #5): only it runs again, not the ones after it
        string Moved(int i) { return pipe.MovedFrom(i, state.Get("root." + Pipeline.Steps[i].Id)); }
        bool OnlyMoved(int i)
        {
            return state.Get("done." + Pipeline.Steps[i].Id) == fps[i] && pipe.MarkerOk(i) && Moved(i) != null;
        }

        void RefreshUi()
        {
            // files
            if (files.Present)
            {
                string e = files.TargetEboot, label = "unknown EBOOT";
                try { string n; if (Ps3Files.KnownEboots.TryGetValue(StateHash(e), out n)) label = n; } catch { }
                if (label == "unknown EBOOT" && Ps3Files.IsBeta(files.TitleId)) label = files.TitleId + " EBOOT (not known by SHA-1) -- beta";
                lblFiles.Text = "Disc: " + (state.Get("files.disc") ?? files.DiscGame) + "\r\nEBOOT: " + label +
                                "    libsre: " + (File.Exists(files.LibsrePath) ? "game\\firmware\\libsre.prx" : "lifted earlier");
                btnFiles.Text = "Change...";
            }
            else { lblFiles.Text = "Not set up yet: choose your disc, update 1.04, decrypted EBOOT.elf and libsre.prx."; btnFiles.Text = "Choose..."; }

            bool toolsOk = prereqs.Count > 0 && prereqs.All(p => p.Ok);
            bool running = game != null;
            btnCancel.Enabled = building;
            btnRebuild.Enabled = !building && !running && files.Present && toolsOk;
            btnFiles.Enabled = !building && !running;
            btnRecheck.Enabled = !building;
            lnkOldBuild.Visible = false; lblHint.Text = "";
            if (building) { btnMain.Text = "Building..."; btnMain.Enabled = false; return; }
            if (running) { btnMain.Text = "Running"; btnMain.Enabled = false; lblHint.Text = "The game is running. Close it to come back here."; return; }
            int stale = files.Present ? FirstStale() : 0;
            if (!files.Present) { btnMain.Text = "Build and play"; btnMain.Enabled = false; lblHint.Text = "Choose your game files first (1)."; }
            else if (stale == Pipeline.Steps.Length && File.Exists(ExePath))
            {
                btnMain.Text = "Play"; btnMain.Enabled = true;
                lblStep.Text = "Ready: the build is up to date.";
                if (state.Get("update.installed") != null) state.Set("update.installed", null);
                bar.Value = 1000;
                lblHint.Text = "Built " + File.GetLastWriteTime(ExePath).ToString("g", CultureInfo.CurrentCulture) + ".";
            }
            else
            {
                btnMain.Text = state.Get("failed") != null ? "Retry" : stale > 0 ? "Continue build" : "Build and play";
                btnMain.Enabled = toolsOk;
                if (!toolsOk) lblHint.Text = "Install the missing build tools first (2).";
                else if (File.Exists(ExePath)) lnkOldBuild.Visible = true;
                lblStep.Text = stale == 0 ? "Not built yet (" + Pipeline.Steps.Length + " steps; the first build takes a while)."
                                          : "Next: step " + (stale + 1) + " of " + Pipeline.Steps.Length + ", " + Pipeline.Steps[stale].Title + ".";
                // after an update the sources' fingerprints changed: say why a build is due and what is kept
                if (state.Get("update.installed") != null && File.Exists(ExePath))
                    lblStep.Text = "Updated to v" + state.Get("update.installed") + ": the game needs a rebuild (a few minutes). Your saves and settings are kept.";
                bar.Value = (int)(1000 * DoneWeight(stale) / TotalWeight());
            }
        }

        string StateHash(string path)
        {
            // the EBOOT's SHA-1, cached by identity (it is 10 MB)
            string id = Util.Identity(path);
            if (state.Get("eboot.id") == id && state.Get("eboot.sha1") != null) return state.Get("eboot.sha1");
            string h = Util.Sha1File(path);
            state.V["eboot.id"] = id; state.Set("eboot.sha1", h);
            return h;
        }

        static double TotalWeight() { return Pipeline.Steps.Sum(s => s.Weight); }
        static double DoneWeight(int upTo) { double w = 0; for (int i = 0; i < upTo; i++) w += Pipeline.Steps[i].Weight; return w; }

        void MainAction()
        {
            if (btnMain.Text == "Play") { LaunchGame(); return; }
            StartBuild(false);
        }

        void Rebuild()
        {
            if (MessageBox.Show(this, "Run every step again, from fetching the toolkit to compiling? (The first build's time again.)",
                                "Rebuild", MessageBoxButtons.OKCancel, MessageBoxIcon.Question) != DialogResult.OK) return;
            StartBuild(true);
        }

        // ---------------------------------------------------------- running the steps
        // The folder the port lives in. Spaces are fine everywhere (issue #2). What is not: letters the Windows
        // ANSI code page cannot spell (the game's runtime opens files through the ANSI API, so such a folder is
        // not found at all), and a folder so deep that the build's longest paths (about 105 characters below the
        // folder: build\ps3recomp_sdk\CMakeFiles\...\atrac3plusdsp.cpp.obj) pass Windows' 260-character limit.
        public const int MaxRootLength = 150;
        public static string FolderProblem(string root)
        {
            var problems = new List<string>();
            try
            {
                var ansi = Encoding.GetEncoding(Encoding.Default.CodePage, EncoderFallback.ExceptionFallback, DecoderFallback.ExceptionFallback);
                ansi.GetBytes(root);
            }
            catch (EncoderFallbackException)
            {
                problems.Add("Its path has letters that Windows' code page for programs that are not Unicode (" + Encoding.Default.WebName +
                             ") cannot spell, and the game opens its files through that code page: it would not find them.");
            }
            // PowerShell reads [ and ] in a path as a wildcard pattern, so build.ps1 cannot even enter such a folder
            // (Set-Location "...\[PS3] inFamous 2\tools\.." -> path not found; issue #3).
            if (root.IndexOfAny(new[] { '[', ']' }) >= 0)
                problems.Add("Its path has square brackets [ ]; the build's PowerShell scripts read them as a wildcard pattern " +
                             "and cannot find the folder. Use round brackets ( ) or none.");
            if (root.Length > MaxRootLength)
                problems.Add("Its path is " + root.Length + " characters long; above " + MaxRootLength + " the build's deepest files pass " +
                             "Windows' 260-character path limit and the compiler fails.");
            if (problems.Count == 0) return null;
            string suggest = Path.Combine(Path.GetPathRoot(root) ?? @"C:\", @"Games\inFamous2Recomp");
            return "This folder will not work for the build or the game:\r\n\r\n    " + root + "\r\n\r\n- " + string.Join("\r\n- ", problems) +
                   "\r\n\r\nMove the whole folder somewhere short with plain letters, for example " + suggest +
                   " (spaces are fine), and start the launcher from there.";
        }

        void StartBuild(bool all)
        {
            if (building) return;
            string folder = FolderProblem(root);   // asked at startup (Program.Main); the build log says it again
            if (folder != null) Append("folder check: " + folder.Replace("\r\n", " "));
            try
            {
                var drive = new DriveInfo(Path.GetPathRoot(root));
                if (drive.AvailableFreeSpace < 12L << 30 &&
                    MessageBox.Show(this, "Only " + (drive.AvailableFreeSpace >> 30) + " GB free on " + drive.Name + ". The build needs about 5 GB, " +
                                    "and the game's first start installs about 2.5 GB more. Continue?", Text, MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes)
                    return;
            }
            catch { }
            // The first build clones the toolkit (step 1): say what is downloaded, from where, and ask first.
            // Cancel changes nothing.
            toolkitOk = false;
            if (Toolkit.NeedsDownload(root))
            {
                using (var ask = new ToolkitAskForm())
                    if (ask.ShowDialog(this) != DialogResult.OK)
                    {
                        Append("the toolkit download was declined: nothing was changed (Build and play asks again)");
                        return;
                    }
                toolkitOk = true;
                Append("toolkit download allowed: " + Toolkit.Url);
            }
            runFrom = all ? 0 : FirstStale();
            if (all) foreach (var s in Pipeline.Steps) state.V.Remove("done." + s.Id);
            state.Set("failed", null);
            if (runFrom >= Pipeline.Steps.Length) { LaunchGame(); return; }
            Directory.CreateDirectory(Path.Combine(root, "logs"));
            logPath = Path.Combine(root, "logs", "launcher-" + DateTime.Now.ToString("yyyyMMdd-HHmmss") + ".log");
            logFile = new StreamWriter(logPath, false, new UTF8Encoding(false));
            building = true; cancelling = false;
            runStart = DateTime.Now;
            txtLog.Clear();
            Append("log file: " + logPath);
            if (runFrom > 0) Append("steps 1-" + runFrom + " are up to date: starting at step " + (runFrom + 1) + " (" + Pipeline.Steps[runFrom].Id + ")");
            RefreshUi();
            RunStep(runFrom);
        }

        bool toolkitOk;               // the player allowed this build's toolkit download (StartBuild)
        bool stepOnlyMoved;           // the running step runs again only because the folder moved (issue #5)
        readonly List<string> stepLines = new List<string>();   // the running step's own output (the failure dialog)

        void RunStep(int i)
        {
            stepIndex = i; stepFrac = 0; stepStart = DateTime.Now;
            var s = Pipeline.Steps[i];
            stepOnlyMoved = OnlyMoved(i);
            lock (qLock) stepLines.Clear();
            if (stepOnlyMoved && pipe.SkipMovedBuild(i, Moved(i), Append))
            {
                state.V["root." + s.Id] = Path.GetFullPath(root).TrimEnd('\\');
                state.Save();
                int after = FirstStale();
                if (after < Pipeline.Steps.Length && after != i) RunStep(after);
                else Finish(true, null);
                return;
            }
            if (stepOnlyMoved)
                Append("the folder moved (step " + s.Id + " ran in " + Moved(i) + "): running it again here; the steps after it keep their results");
            state.Set("done." + s.Id, null);
            if (!stepOnlyMoved)
                for (int k = i + 1; k < Pipeline.Steps.Length; k++) state.V.Remove("done." + Pipeline.Steps[k].Id);
            state.Save();
            lblStep.Text = "Step " + (i + 1) + " of " + Pipeline.Steps.Length + ": " + s.Title;
            Append("");
            Append("===== step " + (i + 1) + "/" + Pipeline.Steps.Length + ": " + s.Id + " -- " + s.Title + " =====");
            var psi = new ProcessStartInfo(Path.Combine(Environment.SystemDirectory, @"WindowsPowerShell\v1.0\powershell.exe"),
                "-NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"" + Path.Combine(root, @"tools\build.ps1") + "\" -Step " + s.Id)
            {
                UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = root,
                RedirectStandardOutput = true, RedirectStandardError = true, RedirectStandardInput = true,
                StandardOutputEncoding = Encoding.GetEncoding(CultureInfo.CurrentCulture.TextInfo.OEMCodePage),
                StandardErrorEncoding = Encoding.GetEncoding(CultureInfo.CurrentCulture.TextInfo.OEMCodePage),
            };
            psi.EnvironmentVariables["PATH"] = Prereqs.ChildPath(prereqs);
            psi.EnvironmentVariables["PYTHONUNBUFFERED"] = "1";
            psi.EnvironmentVariables["NINJA_STATUS"] = "[%f/%t] ";
            if (toolkitOk && s.Id == "setup") psi.EnvironmentVariables["IF2_TOOLKIT_DOWNLOAD"] = "yes";
            try
            {
                proc = new Process { StartInfo = psi, EnableRaisingEvents = true };
                proc.OutputDataReceived += (o, e) => { if (e.Data != null) { lock (qLock) stepLines.Add(e.Data); Enqueue(e.Data); } };
                proc.ErrorDataReceived += (o, e) => { if (e.Data != null) { lock (qLock) stepLines.Add(e.Data); Enqueue(e.Data); } };
                proc.Exited += (o, e) => BeginInvoke(new Action(() => StepExited(i)));
                proc.Start();
                proc.StandardInput.Close();
                proc.BeginOutputReadLine();
                proc.BeginErrorReadLine();
            }
            catch (Exception ex)
            {
                Append("could not start powershell: " + ex.Message);
                Finish(false, "could not start powershell.exe");
            }
        }

        void StepExited(int i)
        {
            if (proc == null) return;
            proc.WaitForExit();            // drains the output readers
            int code = proc.ExitCode;
            proc.Dispose(); proc = null;
            Flush();
            var s = Pipeline.Steps[i];
            Append("step " + s.Id + " ended with exit code " + code + " after " + Util.Elapsed(DateTime.Now - stepStart));
            if (cancelling) { Finish(false, null); return; }
            if (code != 0)
            {
                string first = FirstError();
                List<string> diag;
                lock (qLock) diag = ErrorLinesOf(stepLines, 20);
                if (diag.Count > 1)
                {
                    Append("step " + s.Id + " errors (" + diag.Count + (diag.Count == 20 ? "+" : "") + "):");
                    foreach (var d in diag) Append("  " + d);
                }
                if (first != null) Append("step " + s.Id + " failed; its first error: " + first);
                string hint = LibsreHint(s.Id);
                if (hint != null) Append(hint);
                Finish(false, "Step " + (i + 1) + " of " + Pipeline.Steps.Length + " (" + s.Id + ": " + s.Title + ") failed" +
                              (first != null ? ": " + first : ".") + (hint != null ? "\r\n\r\n" + hint : ""));
                return;
            }
            fps = pipe.Fingerprints();     // a step may create inputs of later ones (e.g. setup the toolkit)
            state.V["root." + s.Id] = Path.GetFullPath(root).TrimEnd('\\');
            state.Set("done." + s.Id, fps[i]);
            int next = stepOnlyMoved ? FirstStale() : i + 1;   // after a moved-only step: the next one that is stale
            if (next < Pipeline.Steps.Length) RunStep(next);
            else Finish(true, null);
        }

        // Issue #4: the failure names the failing step's own first error line -- never a note from the file check (an
        // [ok] / [i] / [!] line, which never stops a build) or a line of an earlier step.
        static readonly Regex ErrLine = new Regex(@"(^\[x\]|\berror\b|\bERROR\b|FAILED|FATAL|\w+Error:|Traceback|Exception|^fatal|: No such file|not found -- )",
                                                  RegexOptions.IgnoreCase);
        static readonly Regex NotErr = new Regex(@"^\s*(\[ok\]|\[i\]|\[!\]|warning\b|-- |\[\d+/\d+\] .*\.(c|cpp|obj)$)|errors?: 0\b|0 errors|error_|_error|Werror|-Wno-error",
                                                 RegexOptions.IgnoreCase);
        // Issue #4: a player's build failed with libsre.prx from firmware 4.80 and worked with 4.93's. When a step that
        // uses libsre fails and that libsre's SPURS modules are not the ones the port uses, the failure says so.
        string LibsreHint(string stepId)
        {
            if (stepId != "libsre" && stepId != "jobs" && stepId != "build") return null;
            try
            {
                string p = new GameFiles(root).LibsrePath;
                if (!File.Exists(p) || Ps3Files.KnownLibsre.ContainsKey(Util.Sha1File(p)) || Ps3Files.LibsreModulesMatch(p)) return null;
            }
            catch { return null; }
            return "Your " + Ps3Files.OldLibsreHelp + " Then press Build again.";
        }

        string FirstError()
        {
            List<string> lines;
            lock (qLock) lines = new List<string>(stepLines);
            return FirstErrorOf(lines);
        }

        // Issue #8: a compiler's own diagnostic ("file(l,c): error: ...", "error LNK2019", ninja's "FAILED: x.obj")
        // wins over any other line. The source lines clang quotes under a warning ("  572 |  printf(... failed ...")
        // are never an error, whatever words they hold.
        static readonly Regex DiagErr = new Regex(@"(\(\d+(,\d+)?\)\s*:\s*(fatal )?error\b|:\d+(:\d+)?: (fatal )?error:|\b(fatal )?error (LNK|C|MSB|RC|CS)\d+|^LINK : fatal|^clang(-cl)?: error|^lld-link: error|^FAILED: (?!Step))",
                                                  RegexOptions.IgnoreCase);
        static readonly Regex SourceQuote = new Regex(@"^(\d+\s*)?\|");
        public static List<string> ErrorLinesOf(IEnumerable<string> lines, int max)
        {
            var r = new List<string>();
            foreach (var l in lines)
            {
                string t = l.Trim();
                if (t.Length == 0 || SourceQuote.IsMatch(t) || !DiagErr.IsMatch(t)) continue;
                r.Add(t.Length > 400 ? t.Substring(0, 400) + "..." : t);
                if (r.Count >= max) break;
            }
            return r;
        }

        public static string FirstErrorOf(List<string> lines)
        {
            var diag = ErrorLinesOf(lines, 1);
            if (diag.Count > 0) return diag[0].Length > 300 ? diag[0].Substring(0, 300) + "..." : diag[0];
            foreach (var l in lines)
            {
                string t = l.Trim();
                if (t.Length == 0 || NotErr.IsMatch(t) || SourceQuote.IsMatch(t)) continue;
                if (t.StartsWith("Traceback")) continue;            // the exception line at its end says what happened
                if (ErrLine.IsMatch(t)) return t.Length > 300 ? t.Substring(0, 300) + "..." : t;
            }
            // nothing that looks like an error: the step's last line
            for (int k = lines.Count - 1; k >= 0; k--) if (lines[k].Trim().Length > 0) return lines[k].Trim();
            return null;
        }

        void Finish(bool ok, string error)
        {
            building = false;
            Append(ok ? "build finished in " + Util.Elapsed(DateTime.Now - runStart)
                      : cancelling ? "cancelled after " + Util.Elapsed(DateTime.Now - runStart) : "FAILED: " + error);
            if (!ok && !cancelling) state.Set("failed", Pipeline.Steps[Math.Max(0, stepIndex)].Id);
            if (logFile != null) { Flush(); logFile.Dispose(); logFile = null; }
            stepIndex = -1;
            RefreshUi();
            if (ok)
            {
                lblStep.Text = "Built. Starting the game...";
                LaunchGame();                 // "press play"
            }
            else if (!cancelling)
            {
                lblStep.Text = error;
                string t;
                lock (qLock) t = string.Join("\r\n", tail.Skip(Math.Max(0, tail.Count - 25)));
                MessageBox.Show(this, error + "\r\n\r\nThe last lines of the log:\r\n\r\n" + t + "\r\n\r\nFull log: " + logPath +
                                "\r\n\r\nFix the cause and press Retry: the steps that already finished are not run again. " +
                                "Asking for help? Press Save report and attach the file it saves.",
                                "Build failed", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
            else lblStep.Text = "Cancelled. Press \"Continue build\" to go on from the step that was interrupted.";
        }

        void CancelRun()
        {
            if (!building || proc == null) return;
            if (MessageBox.Show(this, "Stop the build? Finished steps are kept; the current one starts over next time.", "Cancel",
                                MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes) return;
            cancelling = true;
            Append("cancelling: ending the step's processes");
            try { Util.KillTree(proc.Id); } catch { }
        }

        void OnClosing(object sender, FormClosingEventArgs e)
        {
            if (building)
            {
                if (MessageBox.Show(this, "A build is running. Stop it and close?", Text, MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes)
                { e.Cancel = true; return; }
                cancelling = true;
                try { if (proc != null) Util.KillTree(proc.Id); } catch { }
            }
        }

        // ---------------------------------------------------------- the log pane
        void Enqueue(string line)
        {
            lock (qLock)
            {
                queue.Add(line);
                tail.Add(line);
                if (tail.Count > 400) tail.RemoveRange(0, 200);
            }
        }
        void Append(string line) { Enqueue(line); }

        void Flush()
        {
            List<string> lines;
            lock (qLock) { if (queue.Count == 0) return; lines = new List<string>(queue); queue.Clear(); }
            var sb = new StringBuilder();
            foreach (var l in lines)
            {
                sb.Append(l).Append("\r\n");
                if (logFile != null) logFile.WriteLine(l);
                // ninja's "[n/total]" lines: the build step's own progress
                if (stepIndex >= 0 && l.Length > 3 && l[0] == '[')
                {
                    int sl = l.IndexOf('/'), rb = l.IndexOf(']');
                    int n, t;
                    if (sl > 1 && rb > sl && int.TryParse(l.Substring(1, sl - 1), out n) && int.TryParse(l.Substring(sl + 1, rb - sl - 1), out t) && t > 0)
                        stepFrac = Math.Min(1.0, (double)n / t);
                }
            }
            if (logFile != null) logFile.Flush();
            if (txtLog.TextLength > 400000) { string keep = txtLog.Text.Substring(txtLog.TextLength - 200000); txtLog.Text = keep; }
            txtLog.AppendText(sb.ToString());
        }

        void OnTick()
        {
            Flush();
            if (building)
            {
                lblElapsed.Text = "elapsed " + Util.Elapsed(DateTime.Now - runStart) + "   (this step " + Util.Elapsed(DateTime.Now - stepStart) + ")";
                if (stepIndex >= 0)
                {
                    double w = DoneWeight(stepIndex) + Pipeline.Steps[stepIndex].Weight * stepFrac;
                    bar.Value = Math.Max(0, Math.Min(1000, (int)(1000 * w / TotalWeight())));
                }
            }
            else if (game != null)
            {
                lblElapsed.Text = "playing " + Util.Elapsed(DateTime.Now - gameStart);
                if (game.HasExited) GameExited();
            }
        }

        // ---------------------------------------------------------- the game
        // Started the way tools\run.ps1 starts it -- run.ps1 itself: the project folder as the working directory,
        // the lifted EBOOT as the argument, the runtime switches it sets, and its log in logs\run-<time>.log.
        // On top, for players: PS3_VERBOSE=0 (quieter log, as when the exe is double-clicked) and the play-session
        // issues file logs\play-<time>-issues.txt (tools\play_triage.py). Either one set beforehand wins.
        void LaunchGame()
        {
            if (game != null) return;
            if (!File.Exists(ExePath)) { MessageBox.Show(this, "There is no build\\infamous2.exe yet.", Text); return; }
            var running = Process.GetProcessesByName("infamous2").Where(p => SameExe(p)).ToList();
            if (running.Count > 0) { MessageBox.Show(this, "The game is already running (process " + running[0].Id + ").", Text); return; }
            if (!EncryptedDataOk()) return;
            string stamp = DateTime.Now.ToString("yyyyMMdd-HHmmss");
            var psi = new ProcessStartInfo(Path.Combine(Environment.SystemDirectory, @"WindowsPowerShell\v1.0\powershell.exe"),
                "-NoProfile -ExecutionPolicy Bypass -File \"" + Path.Combine(root, @"tools\run.ps1") + "\"")
            { UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = root };
            if (Environment.GetEnvironmentVariable("PS3_VERBOSE") == null) psi.EnvironmentVariables["PS3_VERBOSE"] = "0";
            if (Environment.GetEnvironmentVariable("PLAY_ISSUES") == null)
                psi.EnvironmentVariables["PLAY_ISSUES"] = Path.Combine(root, @"logs\play-" + stamp + "-issues.txt");
            Directory.CreateDirectory(Path.Combine(root, "logs"));
            try
            {
                game = Process.Start(psi);
                gameStart = DateTime.Now;
                state.Set("last.play", stamp);
                Append("");
                Append("starting the game: tools\\run.ps1 (log: logs\\run-" + stamp + ".log or the next second's)");
                lblStep.Text = "Playing. The launcher waits here until the game closes.";
                WindowState = FormWindowState.Minimized;
            }
            catch (Exception ex) { game = null; MessageBox.Show(this, "Could not start the game: " + ex.Message, Text); }
            RefreshUi();
        }

        // Issue #9: never start the game on encrypted disc data (a black screen after the warning). An install the
        // game made from such data earlier is encrypted too: moved aside (never deleted), the game rebuilds it from the
        // disc files on its next start.
        bool EncryptedDataOk()
        {
            var gf = new GameFiles(root);
            if (Ps3Files.DiscDataEncrypted(gf.DiscGame))
            {
                Append("game files: the disc data is encrypted -- not starting the game (see the message)");
                MessageBox.Show(this, Ps3Files.EncryptedDiscHelp, Text, MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return false;
            }
            string inst = Ps3Files.InstallDir(gf.Game, gf.TitleId);
            if (!Ps3Files.InstallEncrypted(inst)) return true;
            string aside = inst + ".encrypted-" + DateTime.Now.ToString("yyyyMMdd-HHmmss");
            var r = MessageBox.Show(this,
                "The game's installed data (game\\hdd0\\game\\" + Path.GetFileName(inst) + ") was made from encrypted disc files, " +
                "so the game can't read it and would show a black screen.\r\n\r\nMove it aside to " + Path.GetFileName(aside) +
                " and start? The game rebuilds it from your disc files on this start (it takes a few minutes the first time). " +
                "You can delete the old folder afterwards.", Text, MessageBoxButtons.YesNo, MessageBoxIcon.Warning);
            if (r != DialogResult.Yes) return false;
            try { Directory.Move(inst, aside); Append("moved the encrypted install aside: " + aside); return true; }
            catch (Exception ex) { MessageBox.Show(this, "Could not move " + inst + ": " + ex.Message, Text); return false; }
        }

        bool SameExe(Process p)
        {
            try { return string.Equals(p.MainModule.FileName, ExePath, StringComparison.OrdinalIgnoreCase); }
            catch { return false; }
        }

        void GameExited()
        {
            int code = -1;
            try { code = game.ExitCode; } catch { }
            game.Dispose(); game = null;
            var newest = new DirectoryInfo(Path.Combine(root, "logs")).GetFiles("run-*.log").OrderByDescending(f => f.LastWriteTimeUtc).FirstOrDefault();
            Append("the game closed (exit code " + code + ")" + (newest != null ? "; its log: logs\\" + newest.Name : ""));
            if (WindowState == FormWindowState.Minimized) WindowState = FormWindowState.Normal;
            RefreshUi();
            lblStep.Text = code == 0 ? "The game closed." : "The game ended with exit code " + code + ". Its log is in logs\\ (Open logs).";
        }

        static void OpenFolder(string dir, string select)
        {
            Directory.CreateDirectory(dir);
            Process.Start("explorer.exe", select != null ? "/select,\"" + select + "\"" : "\"" + dir + "\"");
        }

        // ---------------------------------------------------------- the support report (Save report, --report-out)
        // The report as .txt plus a .zip of it with the newest logs: GitHub refuses .log attachments but takes a .zip.
        void SaveReport()
        {
            try
            {
                string name = (Ps3Files.IsBeta(files.TitleId) ? "eu-report-" : "report-") + DateTime.Now.ToString("yyyyMMdd-HHmmss");
                string logs = Path.Combine(root, "logs");
                Directory.CreateDirectory(logs);
                string report = SupportReport(root);
                File.WriteAllText(Path.Combine(logs, name + ".txt"), report, new UTF8Encoding(false));
                string zip = Path.Combine(logs, name + ".zip");
                var inside = WriteReportZip(root, zip, name + ".txt", report);
                Append("report saved: logs\\" + name + ".zip (" + string.Join(", ", inside) + "; personal paths replaced by %USERPROFILE%)");
                OpenFolder(logs, zip);
                var r = MessageBox.Show(this,
                    "Saved logs\\" + name + ".zip with this report and your newest logs:\r\n    " + string.Join("\r\n    ", inside) +
                    "\r\n\r\nYour user folder, user name and computer name are replaced in them.\r\n\r\n" +
                    "GitHub takes .zip files (not .log): drag this .zip into a new issue or a comment.\r\n\r\nOpen the project's new-issue page now?",
                    "Report saved", MessageBoxButtons.YesNo, MessageBoxIcon.Information);
                if (r == DialogResult.Yes) OpenUrl("https://github.com/" + AppInfo.Repo + "/issues/new");
            }
            catch (Exception ex) { MessageBox.Show(this, "Could not save the report: " + ex.Message, Text, MessageBoxButtons.OK, MessageBoxIcon.Error); }
        }

        // report + the newest launcher log, game log (run-* / play-*) and play-session issues file, each scrubbed;
        // a log over 16 MB keeps its last 16 MB. Returns the names inside.
        public static List<string> WriteReportZip(string root, string zipPath, string reportName, string report)
        {
            var names = new List<string>();
            var dir = new DirectoryInfo(Path.Combine(root, "logs"));
            var add = new List<FileInfo>();
            if (dir.Exists)
            {
                Func<IEnumerable<FileInfo>, FileInfo> newest = fs => fs.OrderByDescending(f => f.LastWriteTimeUtc).FirstOrDefault();
                add.Add(newest(dir.GetFiles("launcher-*.log")));
                add.Add(newest(dir.GetFiles("run-*.log").Concat(dir.GetFiles("play-*.log"))));
                add.Add(newest(dir.GetFiles("play-*-issues.txt")));
            }
            if (File.Exists(zipPath)) File.Delete(zipPath);
            using (var fs = new FileStream(zipPath, FileMode.CreateNew))
            using (var z = new ZipArchive(fs, ZipArchiveMode.Create))
            {
                Action<string, string> put = (n, text) =>
                {
                    using (var w = new StreamWriter(z.CreateEntry(n, CompressionLevel.Optimal).Open(), new UTF8Encoding(false))) w.Write(text);
                    names.Add(n);
                };
                put(reportName, report);
                foreach (var f in add)
                {
                    if (f == null) continue;
                    try { put(f.Name, Scrub(ReadLogText(f.FullName, 16 << 20))); }
                    catch (Exception ex) { put(f.Name + ".unreadable.txt", "could not read " + f.Name + ": " + ex.Message); }
                }
            }
            return names;
        }

        // A log as text (UTF-16 game logs, UTF-8 the rest), at most its last maxBytes; readable while the game writes it.
        static string ReadLogText(string path, int maxBytes)
        {
            byte[] b;
            using (var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
            {
                long start = Math.Max(0, fs.Length - maxBytes);
                bool utf16 = false;
                if (fs.Length >= 4)
                {
                    var head = new byte[4];
                    fs.Read(head, 0, 4);
                    utf16 = (head[0] == 0xFF && head[1] == 0xFE) || (head[1] == 0 && head[3] == 0);
                }
                if (utf16 && (start & 1) != 0) start++;
                if (start == 0 && utf16 && fs.Length >= 2) { fs.Position = 0; var h = new byte[2]; fs.Read(h, 0, 2); start = (h[0] == 0xFF && h[1] == 0xFE) ? 2 : 0; }
                fs.Position = start;
                b = new byte[fs.Length - start];
                int got = 0, n;
                while (got < b.Length && (n = fs.Read(b, got, b.Length - got)) > 0) got += n;
                string t = utf16 ? Encoding.Unicode.GetString(b, 0, got & ~1) : Encoding.UTF8.GetString(b, 0, got);
                return (start > 2 ? "[... the first " + start + " bytes left out ...]\r\n" : "") + t;
            }
        }

        // The user's folder and name never leave the machine in a report: %USERPROFILE% / %USERNAME% instead.
        public static string Scrub(string t)
        {
            if (t == null) return "";
            string prof = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
            if (!string.IsNullOrEmpty(prof))
            {
                t = Regex.Replace(t, Regex.Escape(prof), "%USERPROFILE%", RegexOptions.IgnoreCase);
                t = Regex.Replace(t, Regex.Escape(prof.Replace('\\', '/')), "%USERPROFILE%", RegexOptions.IgnoreCase);
                t = Regex.Replace(t, Regex.Escape(prof.Replace('\\', '/').Replace("C:", "/c")), "%USERPROFILE%", RegexOptions.IgnoreCase);
                t = Regex.Replace(t, Regex.Escape(prof.Replace(@"\", @"\\")), "%USERPROFILE%", RegexOptions.IgnoreCase);   // JSON-escaped
            }
            string user = Environment.UserName;
            if (!string.IsNullOrEmpty(user) && user.Length >= 3)
                t = Regex.Replace(t, @"(?<=[\\/]Users[\\/]+)" + Regex.Escape(user) + @"(?=[\\/])", "%USERNAME%", RegexOptions.IgnoreCase);
            string machine = Environment.MachineName;
            if (!string.IsNullOrEmpty(machine) && machine.Length >= 3)
                t = Regex.Replace(t, @"\b" + Regex.Escape(machine) + @"\b", "%COMPUTERNAME%", RegexOptions.IgnoreCase);
            return t;
        }

        static string[] TailOf(string path, int n)
        {
            byte[] b = File.ReadAllBytes(path);
            string t;
            if (b.Length >= 2 && b[0] == 0xFF && b[1] == 0xFE) t = Encoding.Unicode.GetString(b, 2, b.Length - 2);
            else if (b.Length >= 4 && b[1] == 0 && b[3] == 0) t = Encoding.Unicode.GetString(b);   // UTF-16 without BOM
            else t = Encoding.UTF8.GetString(b);
            var lines = t.Replace("\r\n", "\n").Split('\n');
            return lines.Skip(Math.Max(0, lines.Length - n)).ToArray();
        }

        public static string SupportReport(string root)
        {
            var sb = new StringBuilder();
            Action<string> L = x => sb.Append(x).Append("\r\n");
            L("inFamous 2 recomp -- support report (launcher v" + AppInfo.Version + ", " + DateTime.Now.ToString("yyyy-MM-dd HH:mm") + ")");
            L("Send this file with your issue / beta report. Your user folder, user name and computer name are replaced by");
            L("%USERPROFILE%, %USERNAME% and %COMPUTERNAME%; nothing else personal is in it (read it first if you like).");
            L("");
            L("folder: " + root + "  (" + root.Length + " characters)");
            string folder = FolderProblem(root);
            L("folder check: " + (folder == null ? "ok" : folder.Replace("\r\n", " ")));
            L("Windows: " + Environment.OSVersion.VersionString + (Environment.Is64BitOperatingSystem ? ", 64-bit" : "") +
              "; language " + CultureInfo.CurrentUICulture.Name);
            L("");
            L("== your files (analysis\\inputs.json, written by step 2)");
            var gf = new GameFiles(root);
            try
            {
                var sfo = Ps3Files.ParseSfo(Path.Combine(gf.DiscGame, "PARAM.SFO"));
                string v;
                L("disc PARAM.SFO: TITLE_ID " + (sfo.TryGetValue("TITLE_ID", out v) ? v : "?") + ", VERSION " +
                  (sfo.TryGetValue("VERSION", out v) ? v : "?") + ", APP_VER " + (sfo.TryGetValue("APP_VER", out v) ? v : "?"));
            }
            catch (Exception e) { L("disc PARAM.SFO: " + e.Message); }
            {
                var core = Ps3Files.IsPsar(Path.Combine(gf.DiscGame, @"USRDIR\cache\psarc\core.psarc"));
                string inst = Ps3Files.InstallDir(gf.Game, gf.TitleId);
                L("disc data (core.psarc): " + (core == null ? "not found" : core == true ? "decrypted (PSAR)" : "ENCRYPTED -- extract a decrypted copy (issue #9)"));
                L("game install (" + Path.GetFileName(inst) + "): " + (!Directory.Exists(inst) ? "not made yet" :
                  Ps3Files.InstallEncrypted(inst) ? "ENCRYPTED -- made from encrypted disc files" : "ok"));
            }
            try
            {
                var u = Ps3Files.ParseSfo(Path.Combine(gf.UpdateDir, "PARAM.SFO"));
                string v;
                L("update PARAM.SFO: APP_VER " + (u.TryGetValue("APP_VER", out v) ? v : "?"));
            }
            catch { L("update PARAM.SFO: none in game\\hdd0\\game\\" + gf.TitleId); }
            try
            {
                string e = gf.TargetEboot;
                if (e != null)
                {
                    string h = Util.Sha1File(e), n;
                    L("EBOOT: " + e.Substring(Math.Min(e.Length, root.Length)).TrimStart('\\') + ", " + new FileInfo(e).Length.ToString("N0", CultureInfo.InvariantCulture) +
                      " bytes, SHA-1 " + h + (Ps3Files.KnownEboots.TryGetValue(h, out n) ? " (" + n + ")" : " (not known by SHA-1)"));
                }
                else L("EBOOT: none found");
                if (File.Exists(gf.LibsrePath))
                {
                    string h = Util.Sha1File(gf.LibsrePath);
                    L("libsre.prx: SHA-1 " + h + (Ps3Files.KnownLibsre.ContainsKey(h) ? " (the tested one)" : Ps3Files.LibsreModulesMatch(gf.LibsrePath) ? " (another firmware build; its SPURS modules match: fine)" : " (OLDER FIRMWARE: its SPURS modules differ -- use 4.92 or newer)"));
                }
            }
            catch (Exception e) { L("files: " + e.Message); }
            string inputs = Path.Combine(root, @"analysis\inputs.json");
            if (File.Exists(inputs))
                foreach (var l in File.ReadAllLines(inputs))
                    if (!l.Contains("\"LICENSE\"")) L("  " + l);
            L("");
            L("== what the build found in your EBOOT (analysis\\resolve_report.txt, written by step 4)");
            string rep = Path.Combine(root, @"analysis\resolve_report.txt");
            if (File.Exists(rep)) foreach (var l in File.ReadAllLines(rep)) L(l);
            else L("(not made yet: step 4, lift, writes it)");
            L("");
            L("== build state");
            if (gf.Present)
            {
                var pl = new Pipeline(root, gf);
                var st = new State(root);
                var fp = pl.Fingerprints();
                for (int i = 0; i < Pipeline.Steps.Length; i++)
                    L("step " + Pipeline.Steps[i].Id + ": " + (st.Get("done." + Pipeline.Steps[i].Id) == fp[i] && pl.MarkerOk(i) ? "up to date" : "to run") +
                      (pl.MovedFrom(i, st.Get("root." + Pipeline.Steps[i].Id)) != null ? " (the folder moved since)" : ""));
                if (st.Get("failed") != null) L("last build failed at: " + st.Get("failed"));
            }
            else L("game files not set up");
            L("infamous2.exe: " + (File.Exists(Path.Combine(root, @"build\infamous2.exe")) ?
              "built " + File.GetLastWriteTime(Path.Combine(root, @"build\infamous2.exe")).ToString("yyyy-MM-dd HH:mm") : "not built"));
            var logs = new DirectoryInfo(Path.Combine(root, "logs"));
            if (logs.Exists)
            {
                var la = logs.GetFiles("launcher-*.log").OrderByDescending(f => f.LastWriteTimeUtc).FirstOrDefault();
                if (la != null)
                {
                    L("");
                    var errs = ErrorLinesOf(TailOf(la.FullName, int.MaxValue), 40);
                    L("== error lines in the newest launcher log (logs\\" + la.Name + ")" + (errs.Count == 0 ? ": none" : ""));
                    foreach (var l in errs) L(l);
                    L("");
                    L("== the newest launcher log (logs\\" + la.Name + "), last 80 lines");
                    foreach (var l in TailOf(la.FullName, 80)) L(l);
                }
                var runs = logs.GetFiles("run-*.log").Concat(logs.GetFiles("play-*.log")).OrderByDescending(f => f.LastWriteTimeUtc).ToList();
                if (runs.Count > 0)
                {
                    var first = runs.Last();
                    L("");
                    L("== the first game log (logs\\" + first.Name + "): its first 60 and last 120 lines");
                    var all = TailOf(first.FullName, int.MaxValue);
                    foreach (var l in all.Take(60)) L(l);
                    if (all.Length > 180) L("[...]");
                    foreach (var l in all.Skip(Math.Max(60, all.Length - 120))) L(l);
                    if (runs.Count > 1)
                    {
                        L("");
                        L("== the newest game log (logs\\" + runs[0].Name + "), last 120 lines");
                        foreach (var l in TailOf(runs[0].FullName, 120)) L(l);
                    }
                }
                else { L(""); L("(no game log yet: the game has not been started)"); }
            }
            return Scrub(sb.ToString());
        }

        // ---------------------------------------------------------- --detect-out
        public static string Report(string root, Options o)
        {
            var sb = new StringBuilder();
            sb.AppendLine("root: " + root);
            string folder = FolderProblem(root);
            sb.AppendLine("folder check: " + (folder == null ? "ok" : folder.Replace("\r\n", " ")));
            if (o.Disc != null || o.Update != null || o.Eboot != null)
            {
                // the game-files dialog's own check on the files given with --disc / --update / --eboot / --libsre
                string dg, ep, lp;
                foreach (var x in GameFiles.Check(o.Disc, o.Update, o.Eboot, o.Libsre, out dg, out ep, out lp))
                    sb.AppendLine("files " + (x.Level == 0 ? "[ok] " : x.Level == 1 ? "[!]  " : "[x]  ") + x.Text);
            }
            foreach (var p in Prereqs.Detect(o))
                sb.AppendLine("prereq " + p.Id + ": " + (p.Ok ? "ok " + p.Found : "MISSING" + (p.Note != null ? " (" + p.Note + ")" : "")) + "  [winget: " + Prereqs.WingetCommand(p) + "]");
            sb.AppendLine("winget: " + (Prereqs.WingetExe() ?? "not found"));
            var gf = new GameFiles(root);
            sb.AppendLine("game files present: " + gf.Present + "  target EBOOT: " + (gf.TargetEboot ?? "none"));
            if (gf.Present)
                sb.AppendLine("edition: " + Ps3Files.EditionOf(gf.TitleId) + (gf.TitleId == "BCUS98125" ? "" : Ps3Files.IsBeta(gf.TitleId) ? " -- beta" : " -- not supported"));
            if (gf.Present)
            {
                var pl = new Pipeline(root, gf);
                var st = new State(root);
                var fp = pl.Fingerprints();
                for (int i = 0; i < Pipeline.Steps.Length; i++)
                {
                    string mv = pl.MovedFrom(i, st.Get("root." + Pipeline.Steps[i].Id));
                    sb.AppendLine("step " + Pipeline.Steps[i].Id + ": " + (st.Get("done." + Pipeline.Steps[i].Id) == fp[i] && pl.MarkerOk(i) && mv == null ? "up to date" : "to run") +
                                  (mv != null ? " (its output names another folder: " + mv + ")" : ""));
                }
            }
            return sb.ToString();
        }
    }

    // ------------------------------------------------------------------ the toolkit download (build step 1)
    static class Toolkit
    {
        public const string DefaultUrl = "https://github.com/sp00nznet/ps3recomp.git";
        // IF2_TOOLKIT_URL: another clone source (tests: a local checkout); tools\setup.ps1 reads the same variable
        public static string Url
        {
            get { string e = Environment.GetEnvironmentVariable("IF2_TOOLKIT_URL"); return string.IsNullOrEmpty(e) ? DefaultUrl : e; }
        }
        // step 1 clones it when external\ps3recomp is not a git checkout yet (later builds only fetch a new pin)
        public static bool NeedsDownload(string root)
        {
            string g = Path.Combine(root, @"external\ps3recomp\.git");
            return !Directory.Exists(g) && !File.Exists(g);
        }
        public static string What
        {
            get
            {
                return "the ps3recomp toolkit -- the open-source static recompiler and PS3 runtime this port is built with " +
                       "(MIT licence; no game code) -- about 15 MB, cloned with git from " + Url + " into external\\ps3recomp " +
                       "(about 35 MB on disk). The build runs it on your own files; nothing else is downloaded by this step " +
                       "besides its Python packages (pip, from its requirements.txt)";
            }
        }
    }

    class ToolkitAskForm : Form
    {
        public ToolkitAskForm()
        {
            Text = "inFamous 2 -- download the toolkit?";
            Font = new Font("Segoe UI", 9f);
            ClientSize = new Size(620, 250);
            FormBorderStyle = FormBorderStyle.FixedDialog; MaximizeBox = false; MinimizeBox = false;
            StartPosition = FormStartPosition.CenterParent;
            var l = new Label
            {
                Left = 16, Top = 14, Width = 588, Height = 180,
                Text = "The first build downloads " + Toolkit.What + ".\r\n\r\n" +
                       "Why: the toolkit is what turns your game's PS3 code into a Windows program. It is a separate " +
                       "open-source project, fetched at the version this port was tested with (ps3recomp.pin) rather " +
                       "than bundled.\r\n\r\nContinue downloads it and builds; Cancel changes nothing."
            };
            var ok = new Button { Text = "Continue", Left = 404, Top = 206, Width = 96, Height = 30, DialogResult = DialogResult.OK };
            var no = new Button { Text = "Cancel", Left = 508, Top = 206, Width = 96, Height = 30, DialogResult = DialogResult.Cancel };
            Controls.AddRange(new Control[] { l, ok, no });
            AcceptButton = ok; CancelButton = no;
        }
    }

    // ------------------------------------------------------------------ headless: --prepare, --build, --play
    static class Headless
    {
        public const int Ok = 0, EUsage = 1, EFiles = 2, EImport = 3, ETools = 4, EConsent = 5, EStep = 6, EPlay = 7, EFolder = 8;

        [DllImport("kernel32.dll")] static extern bool AttachConsole(int pid);
        [DllImport("kernel32.dll")] static extern IntPtr GetStdHandle(int n);
        [DllImport("kernel32.dll")] static extern uint GetFileType(IntPtr h);

        static readonly object outLock = new object();
        static StreamWriter log;
        static Process proc;
        static volatile bool cancelled;

        // A Windows program has no console: a pipe or file it was given is used as it is; started from a console
        // without one, it writes to that console (which does not wait for it: start /wait).
        public static void InitConsole()
        {
            IntPtr h = GetStdHandle(-11);
            if (h == IntPtr.Zero || h == new IntPtr(-1) || GetFileType(h) == 0) AttachConsole(-1);
            try { Console.SetOut(new StreamWriter(Console.OpenStandardOutput(), new UTF8Encoding(false)) { AutoFlush = true }); } catch { }
        }

        public static void Say(string line)
        {
            lock (outLock)
            {
                try { Console.Out.WriteLine(line); } catch { }
                if (log != null) try { log.WriteLine(line); log.Flush(); } catch { }
            }
        }
        static int Fail(int code, string msg) { Say("ERROR " + msg); Say("EXIT " + code); return code; }
        static void Progress(string step, int n, int total, double frac, string text)
        {
            Say("PROGRESS " + step + " " + n + "/" + total);
            Say("progress " + Math.Max(0, Math.Min(1, frac)).ToString("0.###", CultureInfo.InvariantCulture) + " " + text);
        }

        public const string Usage =
            "inFamous2Launcher.exe [--prepare] [--build] [--yes] [--play] [--game <folder>] [--disc <folder>] [--update <folder>]\r\n" +
            "                      [--eboot <EBOOT.elf>] [--libsre <libsre.prx | RPCS3 folder>]\r\n" +
            "  --prepare  check your game files and set up game\\ (as the window's \"Use these files\")\r\n" +
            "  --build    run the build steps that are not up to date (--yes allows the first build's toolkit download)\r\n" +
            "  --play     start the game once the build is up to date, and wait for it\r\n" +
            "Without any of them the launcher opens its window. Exit codes: 0 done, 1 bad command line, 2 game files missing or\r\n" +
            "wrong, 3 setting up game\\ failed, 4 build tools missing, 5 the toolkit download needs --yes, 6 a build step failed,\r\n" +
            "7 cannot play, 8 the folder cannot be used; once --play started the game, tools\\run.ps1's exit code.";

        public static int Run(Options o, string root)
        {
            InitConsole();
            Console.CancelKeyPress += (s, e) => { e.Cancel = true; cancelled = true; try { if (proc != null) Util.KillTree(proc.Id); } catch { } };
            Say("inFamous2Launcher v" + AppInfo.Version + " (no window): " + root);
            if (o.Unknown.Count > 0) { Say(Usage); return Fail(EUsage, "unknown argument(s): " + string.Join(" ", o.Unknown)); }
            string folder = MainForm.FolderProblem(root);
            if (folder != null) return Fail(EFolder, folder.Replace("\r\n", " "));
            int rc;
            if (o.Prepare && (rc = Prepare(o, root)) != Ok) return rc;
            if (o.Build && (rc = Build(o, root)) != Ok) return rc;
            if (o.Play) return Play(root);
            Say("EXIT 0");
            return Ok;
        }

        static string ExePath(string root) { return Path.Combine(root, @"build\infamous2.exe"); }

        static bool GameRunning(string root)
        {
            string exe = ExePath(root);
            foreach (var p in Process.GetProcessesByName("infamous2"))
                try { if (string.Equals(p.MainModule.FileName, exe, StringComparison.OrdinalIgnoreCase)) return true; } catch { }
            return false;
        }

        // ---------------------------------------------------------- --prepare
        static bool IsDir(string d) { try { return !string.IsNullOrEmpty(d) && Directory.Exists(d); } catch { return false; } }
        static string FileIn(string dir, string rel) { string f = Path.Combine(dir, rel); return File.Exists(f) ? f : null; }
        static IEnumerable<string> SubDirs(string d)
        {
            try { return Directory.GetDirectories(d); } catch { return new string[0]; }
        }

        // --game <folder>: the disc (a folder holding PS3_GAME, or PS3_GAME itself), the update (named after the disc's
        // title id, beside it or in an RPCS3-style dev_hdd0\game), the EBOOT.elf (only when the update's USRDIR has none)
        // and libsre (libsre.prx, firmware\libsre.prx, or an RPCS3 dev_flash with the decrypted one). Parts given on their
        // own (--disc, ...) win.
        static void Search(string g, ref string disc, ref string upd, ref string eboot, ref string libsre)
        {
            if (string.IsNullOrEmpty(disc))
            {
                foreach (var c in new[] { g, Path.Combine(g, "disc") }.Concat(SubDirs(g)))
                    if (FileIn(c, @"PS3_GAME\PARAM.SFO") != null) { disc = c; break; }
                if (disc == null && string.Equals(Path.GetFileName(g.TrimEnd('\\')), "PS3_GAME", StringComparison.OrdinalIgnoreCase) &&
                    FileIn(g, "PARAM.SFO") != null)
                    disc = g;
            }
            string tid = "BCUS98125";
            if (disc != null)
            {
                string dg = IsDir(Path.Combine(disc, "PS3_GAME")) ? Path.Combine(disc, "PS3_GAME") : disc;
                try { string v; if (Ps3Files.ParseSfo(Path.Combine(dg, "PARAM.SFO")).TryGetValue("TITLE_ID", out v) && !string.IsNullOrEmpty(v)) tid = v; } catch { }
            }
            if (string.IsNullOrEmpty(upd))
            {
                foreach (var c in new[] { Path.Combine(g, tid), Path.Combine(g, "update"), Path.Combine(g, @"update\" + tid),
                                          Path.Combine(g, @"hdd0\game\" + tid), Path.Combine(g, @"dev_hdd0\game\" + tid),
                                          Path.Combine(g, @"game\" + tid) })
                    if (FileIn(c, "PARAM.SFO") != null && IsDir(Path.Combine(c, "USRDIR"))) { upd = c; break; }
                if (upd == null && string.Equals(Path.GetFileName(g.TrimEnd('\\')), tid, StringComparison.OrdinalIgnoreCase) && FileIn(g, "PARAM.SFO") != null)
                    upd = g;
            }
            if (string.IsNullOrEmpty(eboot) && (upd == null || Ps3Files.FindEboot(Path.Combine(upd, "USRDIR")) == null))
                eboot = FileIn(g, "EBOOT.elf");
            if (string.IsNullOrEmpty(libsre))
                libsre = FileIn(g, "libsre.prx") ?? FileIn(g, @"firmware\libsre.prx") ?? FileIn(g, @"dev_flash\sys\external\libsre.prx");
        }

        // An update installed in RPCS3 sits in <RPCS3>\dev_hdd0\game\<title id>; RPCS3's Decrypt PS3 Binaries leaves the
        // decrypted libsre.prx beside the firmware's libsre.sprx in that RPCS3's dev_flash.
        static string LibsreNear(string upd)
        {
            try
            {
                string d = IsDir(upd) ? Path.GetFullPath(upd) : null;
                for (int up = 0; up < 4 && d != null; up++)
                {
                    string f = FileIn(d, @"dev_flash\sys\external\libsre.prx");
                    if (f != null) return f;
                    d = Path.GetDirectoryName(d);
                }
            }
            catch { }
            return null;
        }

        public const string LibsreManual =
            "Decrypted libsre.prx: not found. It comes from your own PS3 firmware (game\\README.md: RPCS3 > Utilities > Decrypt " +
            "PS3 Binaries on dev_flash\\sys\\external\\libsre.sprx). Other launchers cannot ask for firmware files, so put it at " +
            "game\\firmware\\libsre.prx in this folder (or give --libsre <file | RPCS3 folder>) and run the setup again.";

        static int Prepare(Options o, string root)
        {
            var files = new GameFiles(root);
            if (GameRunning(root)) return Fail(EPlay, "the game is running from this folder: close it first");
            Progress("prepare", 1, 3, 0.05, "Finding your game files");
            string disc = o.Disc, upd = o.Update, eboot = o.Eboot, libsre = o.Libsre;
            if (!string.IsNullOrEmpty(o.Game))
            {
                if (!IsDir(o.Game)) return Fail(EUsage, "--game: there is no folder " + o.Game);
                Search(o.Game, ref disc, ref upd, ref eboot, ref libsre);
            }
            if (string.IsNullOrEmpty(eboot)) eboot = null;
            string libsreFrom = string.IsNullOrEmpty(libsre) ? null : "given";
            if (libsreFrom == null && (libsre = LibsreNear(upd)) != null) libsreFrom = "found beside the update (RPCS3 dev_flash)";
            bool keepLifted = false;
            if (libsreFrom == null && File.Exists(files.LibsrePath)) { libsre = files.LibsrePath; libsreFrom = "the one already in game\\firmware"; }
            if (libsreFrom == null && File.Exists(Path.Combine(root, @"generated\prx\libsre.linked.bin"))) { libsre = null; keepLifted = true; }
            Say("INFO disc: " + (disc ?? "(not found)"));
            Say("INFO update: " + (upd ?? "(not found)"));
            Say("INFO EBOOT.elf: " + (eboot ?? "(the update's USRDIR\\EBOOT.elf)"));
            Say("INFO libsre.prx: " + (keepLifted ? "none given: keeping the libsre lifted earlier (generated\\prx)" :
                                       libsreFrom == null ? "(not found)" : libsre + " (" + libsreFrom + ")"));

            Progress("prepare", 2, 3, 0.15, "Checking your game files");
            string dg, ep, lp;
            var f = GameFiles.Check(disc ?? "", upd ?? "", eboot, libsre, out dg, out ep, out lp);
            if (libsre == null && !keepLifted) f.Add(new GameFiles.Finding(2, LibsreManual));
            foreach (var x in f) Say((x.Level == 0 ? "INFO [ok] " : x.Level == 1 ? "WARN " : "ERROR ") + x.Text);
            if (f.Any(x => x.Level == 2)) { Say("EXIT " + EFiles); return EFiles; }

            Progress("prepare", 3, 3, 0.3, "Setting up game\\: linking the disc, copying the update (about 230 MB)");
            try { GameSetup.Import(files, new State(root), dg, upd, ep, keepLifted ? null : lp); }
            catch (Exception e) { return Fail(EImport, "could not set up game\\: " + e.Message); }
            Say("progress 1 Your game files are set up");
            Say("OK prepare: game files set up in game\\ (disc linked from " + dg + ")");
            return Ok;
        }

        // ---------------------------------------------------------- --build
        static int FirstStale(Pipeline pipe, State st, out string[] fps)
        {
            fps = pipe.Fingerprints();
            for (int i = 0; i < Pipeline.Steps.Length; i++)
                if (st.Get("done." + Pipeline.Steps[i].Id) != fps[i] || !pipe.MarkerOk(i) ||
                    pipe.MovedFrom(i, st.Get("root." + Pipeline.Steps[i].Id)) != null) return i;
            return Pipeline.Steps.Length;
        }
        static double Weight(int upTo) { double w = 0; for (int i = 0; i < upTo; i++) w += Pipeline.Steps[i].Weight; return w; }
        static double TotalWeight() { return Weight(Pipeline.Steps.Length); }

        static int Build(Options o, string root)
        {
            var files = new GameFiles(root);
            int total = Pipeline.Steps.Length;
            if (!files.Present) return Fail(EFiles, "your game files are not set up in game\\ yet: run --prepare first (with --game, or --disc, --update, --eboot and --libsre)");
            if (GameRunning(root)) return Fail(EPlay, "the game is running from this folder: close it first");
            var pipe = new Pipeline(root, files);
            var state = new State(root);
            string[] fps;
            int i = FirstStale(pipe, state, out fps);
            if (i >= total && File.Exists(ExePath(root)))
            {
                Progress("build", total, total, 1, "The build is up to date");
                Say("OK build: up to date");
                return Ok;
            }
            var prereqs = Prereqs.Detect(o);
            foreach (var p in prereqs) Say("INFO prerequisite " + p.Id + ": " + (p.Ok ? "ok (" + p.Found + ")" : "MISSING"));
            var miss = prereqs.Where(p => !p.Ok).ToList();
            if (miss.Count > 0)
                return Fail(ETools, "build tools missing: " + string.Join("; ", miss.Select(p => p.Name + (p.WingetId != null ? " (" + Prereqs.WingetCommand(p) + ")" : ""))) +
                                    ". Install them (the launcher's window offers each with winget) and run again.");
            if (Toolkit.NeedsDownload(root) && !o.Yes)
                return Fail(EConsent, "the first build downloads " + Toolkit.What + ". Run again with --yes to allow it. Nothing was changed.");

            Directory.CreateDirectory(Path.Combine(root, "logs"));
            string logPath = Path.Combine(root, "logs", "launcher-" + DateTime.Now.ToString("yyyyMMdd-HHmmss") + ".log");
            log = new StreamWriter(logPath, false, new UTF8Encoding(false));
            Say("INFO log file: " + logPath);
            if (Toolkit.NeedsDownload(root)) Say("INFO toolkit download allowed (--yes): " + Toolkit.Url);
            state.Set("failed", null);
            if (i > 0) Say("INFO steps 1-" + i + " are up to date: starting at step " + (i + 1) + " (" + Pipeline.Steps[i].Id + ")");
            DateTime start = DateTime.Now;
            try
            {
                while (i < total)
                {
                    var s = Pipeline.Steps[i];
                    string moved = pipe.MovedFrom(i, state.Get("root." + s.Id));
                    bool onlyMoved = state.Get("done." + s.Id) == fps[i] && pipe.MarkerOk(i) && moved != null;
                    if (onlyMoved && pipe.SkipMovedBuild(i, moved, x => Say("INFO " + x)))
                    {
                        state.V["root." + s.Id] = Path.GetFullPath(root).TrimEnd('\\');
                        state.Save();
                        int nx = FirstStale(pipe, state, out fps);
                        if (nx == i) return Fail(EStep, "the build step stays out of date after a folder move: press Rebuild");
                        i = nx;
                        continue;
                    }
                    if (onlyMoved) Say("INFO the folder moved (step " + s.Id + " ran in " + moved + "): running it again here; the steps after it keep their results");
                    state.Set("done." + s.Id, null);
                    if (!onlyMoved) for (int k = i + 1; k < total; k++) state.V.Remove("done." + Pipeline.Steps[k].Id);
                    state.Save();
                    Progress(s.Id, i + 1, total, Weight(i) / TotalWeight(), "Step " + (i + 1) + " of " + total + ": " + s.Title);
                    Say("===== step " + (i + 1) + "/" + total + ": " + s.Id + " -- " + s.Title + " =====");
                    DateTime t0 = DateTime.Now;
                    List<string> lines;
                    int code = RunStep(root, s, i, prereqs, o.Yes, out lines);
                    Say("INFO step " + s.Id + " ended with exit code " + code + " after " + Util.Elapsed(DateTime.Now - t0));
                    if (cancelled) return Fail(EStep, "stopped (Ctrl+C) in step " + (i + 1) + " (" + s.Id + "); finished steps are kept");
                    if (code != 0)
                    {
                        state.Set("failed", s.Id);
                        string first = MainForm.FirstErrorOf(lines);
                        return Fail(EStep, "step " + (i + 1) + " of " + total + " (" + s.Id + ": " + s.Title + ") failed" +
                                           (first != null ? ": " + first : "") + "  (full log: " + logPath + ")");
                    }
                    fps = pipe.Fingerprints();
                    state.V["root." + s.Id] = Path.GetFullPath(root).TrimEnd('\\');
                    state.Set("done." + s.Id, fps[i]);
                    i = onlyMoved ? FirstStale(pipe, state, out fps) : i + 1;
                }
                Progress("build", total, total, 1, "Built");
                Say("OK build: finished in " + Util.Elapsed(DateTime.Now - start));
                return Ok;
            }
            finally
            {
                lock (outLock) { if (log != null) { log.Dispose(); log = null; } }
            }
        }

        static int RunStep(string root, StepDef s, int index, List<Prereq> prereqs, bool yes, out List<string> lines)
        {
            var got = new List<string>();
            lines = got;
            var psi = new ProcessStartInfo(Path.Combine(Environment.SystemDirectory, @"WindowsPowerShell\v1.0\powershell.exe"),
                "-NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"" + Path.Combine(root, @"tools\build.ps1") + "\" -Step " + s.Id)
            {
                UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = root,
                RedirectStandardOutput = true, RedirectStandardError = true, RedirectStandardInput = true,
                StandardOutputEncoding = Encoding.GetEncoding(CultureInfo.CurrentCulture.TextInfo.OEMCodePage),
                StandardErrorEncoding = Encoding.GetEncoding(CultureInfo.CurrentCulture.TextInfo.OEMCodePage),
            };
            psi.EnvironmentVariables["PATH"] = Prereqs.ChildPath(prereqs);
            psi.EnvironmentVariables["PYTHONUNBUFFERED"] = "1";
            psi.EnvironmentVariables["NINJA_STATUS"] = "[%f/%t] ";
            if (yes && s.Id == "setup") psi.EnvironmentVariables["IF2_TOOLKIT_DOWNLOAD"] = "yes";
            double w0 = Weight(index) / TotalWeight(), w1 = Weight(index + 1) / TotalWeight();
            int lastPct = -1;
            DataReceivedEventHandler on = (sender, e) =>
            {
                if (e.Data == null) return;
                lock (got) got.Add(e.Data);
                Say(e.Data);
                // ninja's "[n/total]": the build step's own progress
                var m = Regex.Match(e.Data, @"^\[(\d+)/(\d+)\]");
                int n, t;
                if (m.Success && int.TryParse(m.Groups[1].Value, out n) && int.TryParse(m.Groups[2].Value, out t) && t > 0)
                {
                    int pct = (int)(100L * n / t);
                    if (pct != lastPct)
                    {
                        lastPct = pct;
                        Say("progress " + (w0 + (w1 - w0) * n / t).ToString("0.###", CultureInfo.InvariantCulture) + " Step " + (index + 1) +
                            " of " + Pipeline.Steps.Length + ": " + s.Title + " (" + n + "/" + t + ")");
                    }
                }
            };
            try
            {
                proc = new Process { StartInfo = psi };
                proc.OutputDataReceived += on;
                proc.ErrorDataReceived += on;
                proc.Start();
                proc.StandardInput.Close();
                proc.BeginOutputReadLine();
                proc.BeginErrorReadLine();
                proc.WaitForExit();
                int code = proc.ExitCode;
                proc.Dispose(); proc = null;
                return code;
            }
            catch (Exception ex)
            {
                Say("ERROR could not start powershell: " + ex.Message);
                proc = null;
                return -1;
            }
        }

        // ---------------------------------------------------------- --play
        static int Play(string root)
        {
            var files = new GameFiles(root);
            if (!File.Exists(ExePath(root))) return Fail(EPlay, "there is no build\\infamous2.exe yet: run --build first");
            if (files.Present)
            {
                string[] fps;
                int stale = FirstStale(new Pipeline(root, files), new State(root), out fps);
                if (stale < Pipeline.Steps.Length)
                    return Fail(EPlay, "the build is not up to date (next: step " + (stale + 1) + ", " + Pipeline.Steps[stale].Id + "): run --build first");
            }
            if (GameRunning(root)) return Fail(EPlay, "the game is already running");
            if (Ps3Files.DiscDataEncrypted(files.DiscGame)) return Fail(EPlay, Ps3Files.EncryptedDiscHelp.Replace("\r\n\r\n", " "));
            if (Ps3Files.InstallEncrypted(Ps3Files.InstallDir(files.Game, files.TitleId)))
                return Fail(EPlay, "game\\hdd0\\game\\" + files.TitleId + "_INSTALL was made from encrypted disc files: press Play in the " +
                                   "launcher's window once (it offers to move the folder aside), or move it aside yourself");
            // as the window's Play: tools\run.ps1, quieter log, the play-session issues file (either set beforehand wins)
            string stamp = DateTime.Now.ToString("yyyyMMdd-HHmmss");
            var psi = new ProcessStartInfo(Path.Combine(Environment.SystemDirectory, @"WindowsPowerShell\v1.0\powershell.exe"),
                "-NoProfile -ExecutionPolicy Bypass -File \"" + Path.Combine(root, @"tools\run.ps1") + "\"")
            { UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = root };
            if (Environment.GetEnvironmentVariable("PS3_VERBOSE") == null) psi.EnvironmentVariables["PS3_VERBOSE"] = "0";
            if (Environment.GetEnvironmentVariable("PLAY_ISSUES") == null)
                psi.EnvironmentVariables["PLAY_ISSUES"] = Path.Combine(root, @"logs\play-" + stamp + "-issues.txt");
            Directory.CreateDirectory(Path.Combine(root, "logs"));
            try
            {
                using (proc = Process.Start(psi))
                {
                    Say("OK play: the game is starting (log: logs\\run-" + stamp + ".log or the next second's); waiting for it to close");
                    proc.WaitForExit();
                    int code = proc.ExitCode;
                    proc = null;
                    Say("INFO the game closed (exit code " + code + ")");
                    Say("EXIT " + code);
                    return code;
                }
            }
            catch (Exception ex) { proc = null; return Fail(EPlay, "could not start the game: " + ex.Message); }
        }
    }

    static class Program
    {
        static string FindRoot()
        {
            string d = Path.GetDirectoryName(Application.ExecutablePath);
            for (int up = 0; up < 3 && d != null; up++)
            {
                if (File.Exists(Path.Combine(d, @"tools\build.ps1")) && File.Exists(Path.Combine(d, "ps3recomp.pin"))) return d;
                d = Path.GetDirectoryName(d);
            }
            return null;
        }

        [STAThread]
        static int Main(string[] args)
        {
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            var o = Options.Parse(args);
            if (o.ApplyUpdate != null)
            {
                Application.Run(new ApplyForm(o.ApplyUpdate));
                return 0;
            }
            if (o.UpdateCheckOut != null)
            {
                File.WriteAllText(o.UpdateCheckOut, Updates.Report(o));
                return 0;
            }
            string root = FindRoot();
            if (o.Help)
            {
                Headless.InitConsole();
                Headless.Say(Headless.Usage);
                return 0;
            }
            if (o.IsHeadless)
            {
                if (root != null) return Headless.Run(o, root);
                Headless.InitConsole();
                Headless.Say("ERROR put inFamous2Launcher.exe in the inFamous 2 recomp folder (the one with tools\\build.ps1 and ps3recomp.pin)");
                Headless.Say("EXIT 8");
                return 8;
            }
            if (root == null)
            {
                MessageBox.Show("Put inFamous2Launcher.exe in the inFamous 2 recomp folder (the one with tools\\build.ps1 and ps3recomp.pin).",
                                "inFamous 2 launcher", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return 1;
            }
            // Before anything builds a path below the folder: a folder the build or the game cannot use is said up
            // front, with a suggestion, instead of failing late (issue #2). Too deep a folder can also make the
            // launcher's own path handling fail (PathTooLongException), so nothing below runs before this.
            string folder = MainForm.FolderProblem(root);
            if (o.MarkBuilt)
            {
                var gf = new GameFiles(root);
                if (!gf.Present) return 1;
                var pl = new Pipeline(root, gf);
                var st = new State(root);
                var fp = pl.Fingerprints();
                for (int i = 0; i < Pipeline.Steps.Length; i++)
                    if (pl.MarkerOk(i))
                    {
                        st.V["done." + Pipeline.Steps[i].Id] = fp[i];
                        st.V["root." + Pipeline.Steps[i].Id] = Path.GetFullPath(root).TrimEnd('\\');
                    }
                st.Save();
                return 0;
            }
            if (o.ReportOut != null)
            {
                string rep = MainForm.SupportReport(root);
                if (o.ReportOut.EndsWith(".zip", StringComparison.OrdinalIgnoreCase))
                    MainForm.WriteReportZip(root, o.ReportOut, Path.GetFileNameWithoutExtension(o.ReportOut) + ".txt", rep);
                else File.WriteAllText(o.ReportOut, rep, new UTF8Encoding(false));
                return 0;
            }
            if (o.DetectOut != null)
            {
                string report;
                try { report = MainForm.Report(root, o); }
                catch (Exception ex) { report = "root: " + root + "\r\nfolder check: " + (folder ?? "ok").Replace("\r\n", " ") + "\r\nreport failed: " + ex.Message + "\r\n"; }
                File.WriteAllText(o.DetectOut, report);
                return 0;
            }
            if (folder != null &&
                MessageBox.Show(folder + "\r\n\r\nContinue in this folder anyway?", "inFamous 2 launcher", MessageBoxButtons.YesNo,
                                MessageBoxIcon.Warning) != DialogResult.Yes)
                return 1;
            Application.Run(new MainForm(root, o));
            return 0;
        }
    }
}
