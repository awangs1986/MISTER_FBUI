using System.Collections.Concurrent;
using System.Globalization;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace GamelistEditor;

/// <summary>
/// Resolves Libretro thumbnail lookup names for a local ROM.
/// Priority: override JSON → DAT CRC/game name → filename rules (strip setname) → setname.
/// </summary>
internal static class ThumbMatch
{
    private const long MaxHashBytes = 8 * 1024 * 1024; // skip hashing huge images/disks

    private static readonly ConcurrentDictionary<string, DatIndex?> DatCache = new(StringComparer.OrdinalIgnoreCase);
    private static Dictionary<string, Dictionary<string, string>>? _overrides; // system → romBase → thumbName
    private static string? _dataRoot;

    internal sealed record SysInfo(
        string Playlist,
        string? DatRelativePath,
        bool TryHeaderlessCrc);

    // MiSTer folder → Libretro playlist + optional No-Intro/Redump DAT
    private static readonly Dictionary<string, SysInfo> Systems = new(StringComparer.OrdinalIgnoreCase)
    {
        ["NES"] = new("Nintendo - Nintendo Entertainment System",
            "no-intro/Nintendo - Nintendo Entertainment System.dat", true),
        ["SNES"] = new("Nintendo - Super Nintendo Entertainment System",
            "no-intro/Nintendo - Super Nintendo Entertainment System.dat", false),
        ["N64"] = new("Nintendo - Nintendo 64",
            "no-intro/Nintendo - Nintendo 64.dat", false),
        ["GAMEBOY"] = new("Nintendo - Game Boy",
            "no-intro/Nintendo - Game Boy.dat", false),
        ["GBC"] = new("Nintendo - Game Boy Color",
            "no-intro/Nintendo - Game Boy Color.dat", false),
        ["GBA"] = new("Nintendo - Game Boy Advance",
            "no-intro/Nintendo - Game Boy Advance.dat", false),
        ["MegaDrive"] = new("Sega - Mega Drive - Genesis",
            "no-intro/Sega - Mega Drive - Genesis.dat", false),
        ["Genesis"] = new("Sega - Mega Drive - Genesis",
            "no-intro/Sega - Mega Drive - Genesis.dat", false),
        ["SMS"] = new("Sega - Master System - Mark III",
            "no-intro/Sega - Master System - Mark III.dat", false),
        ["GG"] = new("Sega - Game Gear",
            "no-intro/Sega - Game Gear.dat", false),
        ["32X"] = new("Sega - 32X",
            "no-intro/Sega - 32X.dat", false),
        ["Saturn"] = new("Sega - Saturn",
            "redump/Sega - Saturn.dat", false),
        ["MegaCD"] = new("Sega - Mega-CD - Sega CD",
            "redump/Sega - Mega-CD - Sega CD.dat", false),
        ["PSX"] = new("Sony - PlayStation",
            "redump/Sony - PlayStation.dat", false),
        ["TGFX16"] = new("NEC - PC Engine - TurboGrafx 16",
            "no-intro/NEC - PC Engine - TurboGrafx 16.dat", false),
        ["TGFX16-CD"] = new("NEC - PC Engine CD - TurboGrafx-CD",
            "redump/NEC - PC Engine CD - TurboGrafx-CD.dat", false),
        ["Atari2600"] = new("Atari - 2600",
            "no-intro/Atari - 2600.dat", false),
        ["Atari7800"] = new("Atari - 7800",
            "no-intro/Atari - 7800.dat", false),
        ["AtariLynx"] = new("Atari - Lynx",
            "no-intro/Atari - Lynx.dat", false),
        ["WonderSwan"] = new("Bandai - WonderSwan",
            "no-intro/Bandai - WonderSwan.dat", false),
        ["NeoGeo"] = new("SNK - Neo Geo", null, false),
        ["AES"] = new("SNK - Neo Geo", null, false),
        ["NeoGeoCD"] = new("SNK - Neo Geo CD",
            "redump/SNK - Neo Geo CD.dat", false),
        ["CPS1"] = new("FBNeo - Arcade Games", null, false),
        ["CPS2"] = new("FBNeo - Arcade Games", null, false),
        ["Arcade"] = new("FBNeo - Arcade Games", null, false),
    };

    public static bool TryGetSys(string misterSystem, out SysInfo info) =>
        Systems.TryGetValue(misterSystem, out info!);

    public static bool UsesOutsideParenthesesOnly(string misterSystem) =>
        misterSystem.Equals("NeoGeo", StringComparison.OrdinalIgnoreCase) ||
        misterSystem.Equals("AES", StringComparison.OrdinalIgnoreCase) ||
        misterSystem.Equals("CPS1", StringComparison.OrdinalIgnoreCase) ||
        misterSystem.Equals("CPS2", StringComparison.OrdinalIgnoreCase) ||
        misterSystem.Equals("Arcade", StringComparison.OrdinalIgnoreCase);

    public static string? FindDataRoot()
    {
        if (_dataRoot != null) return _dataRoot;
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        for (var i = 0; i < 10 && dir != null; i++, dir = dir.Parent)
        {
            var a = Path.Combine(dir.FullName, "data", "libretro-database", "metadat");
            if (Directory.Exists(a))
            {
                _dataRoot = Path.Combine(dir.FullName, "data");
                return _dataRoot;
            }
            var b = Path.Combine(dir.FullName, "tools", "data", "libretro-database", "metadat");
            if (Directory.Exists(b))
            {
                _dataRoot = Path.Combine(dir.FullName, "tools", "data");
                return _dataRoot;
            }
        }
        return null;
    }

    public static IReadOnlyList<string> ResolveNames(
        string misterSystem,
        string romFileName,
        byte[]? romBytes)
    {
        var baseName = Path.GetFileNameWithoutExtension(romFileName);
        var ordered = new List<string>();
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        void Add(string? s)
        {
            if (string.IsNullOrWhiteSpace(s)) return;
            s = s.Trim();
            if (seen.Add(s)) ordered.Add(s);
        }

        // Arcade/NeoGeo ROM packs append set, revision and short set names in
        // parentheses. Libretro screenshots are keyed by the human title, so
        // only the text outside every parenthesized segment participates.
        if (UsesOutsideParenthesesOnly(misterSystem))
        {
            var outside = Regex.Replace(baseName, @"\s*\([^)]*\)", " ");
            outside = Regex.Replace(outside, @"\s+", " ").Trim();
            Add(outside);
            return ordered;
        }

        // 1) local override
        foreach (var o in OverrideNames(misterSystem, romFileName, baseName))
            Add(o);

        // 2) DAT CRC → official game / rom name
        if (romBytes != null && TryGetSys(misterSystem, out var sys) && sys.DatRelativePath != null)
        {
            var dat = GetDat(sys.DatRelativePath);
            if (dat != null)
            {
                foreach (var crc in CandidateCrcs(romBytes, sys.TryHeaderlessCrc))
                {
                    if (!dat.ByCrc.TryGetValue(crc, out var hit)) continue;
                    Add(hit.GameName);
                    Add(Path.GetFileNameWithoutExtension(hit.RomName));
                    break;
                }
            }
        }

        // 3) filename rules: full → strip trailing (… ) repeatedly
        Add(baseName);
        var cur = baseName;
        while (true)
        {
            var m = Regex.Match(cur, @"^(.*)\s*\([^)]*\)\s*$");
            if (!m.Success) break;
            cur = m.Groups[1].Value.Trim();
            Add(cur);
        }

        // 4) trailing setname only (mslug / kof98) for FBNeo/MAME-style thumbs
        var lastParen = Regex.Match(baseName, @"\(([^)]+)\)\s*$");
        if (lastParen.Success)
        {
            var inner = lastParen.Groups[1].Value.Trim();
            if (inner.Length is > 0 and <= 16 && !inner.Contains(' '))
                Add(inner);
        }

        return ordered;
    }

    public static string SanitizeLibretroName(string name)
    {
        var chars = name.ToCharArray();
        for (var i = 0; i < chars.Length; i++)
        {
            switch (chars[i])
            {
                case '&':
                case '*':
                case '/':
                case ':':
                case '`':
                case '<':
                case '>':
                case '?':
                case '\\':
                case '|':
                case '"':
                    chars[i] = '_';
                    break;
            }
        }
        return new string(chars);
    }

    public static string BuildThumbUrl(string playlist, string thumbType, string gameBaseName)
    {
        var safe = SanitizeLibretroName(gameBaseName);
        return $"https://thumbnails.libretro.com/{Uri.EscapeDataString(playlist)}/{thumbType}/{Uri.EscapeDataString(safe + ".png")}";
    }

    public static bool ShouldHash(long fileSizeHint) =>
        fileSizeHint > 0 && fileSizeHint <= MaxHashBytes;

    private static IEnumerable<string> OverrideNames(string system, string romFile, string baseName)
    {
        EnsureOverrides();
        if (_overrides == null) yield break;
        if (!_overrides.TryGetValue(system, out var map)) yield break;
        if (map.TryGetValue(romFile, out var a)) yield return a;
        if (map.TryGetValue(baseName, out var b)) yield return b;
    }

    private static void EnsureOverrides()
    {
        if (_overrides != null) return;
        _overrides = new(StringComparer.OrdinalIgnoreCase);
        var root = FindDataRoot();
        if (root == null) return;
        var path = Path.Combine(root, "thumb_overrides.json");
        if (!File.Exists(path)) return;
        try
        {
            using var fs = File.OpenRead(path);
            var doc = JsonSerializer.Deserialize<Dictionary<string, Dictionary<string, string>>>(fs,
                new JsonSerializerOptions { PropertyNameCaseInsensitive = true });
            if (doc != null) _overrides = new(doc, StringComparer.OrdinalIgnoreCase);
        }
        catch
        {
            // ignore malformed override file
        }
    }

    private static DatIndex? GetDat(string relativePath)
    {
        return DatCache.GetOrAdd(relativePath, rel =>
        {
            var root = FindDataRoot();
            if (root == null) return null;
            var path = Path.Combine(root, "libretro-database", "metadat", rel);
            if (!File.Exists(path)) return null;
            return DatIndex.Load(path);
        });
    }

    private static IEnumerable<uint> CandidateCrcs(byte[] data, bool headerless)
    {
        yield return Crc32(data);
        if (headerless && data.Length > 16)
            yield return Crc32(data.AsSpan(16));
    }

    private static uint Crc32(ReadOnlySpan<byte> data)
    {
        uint crc = 0xFFFFFFFFu;
        foreach (var b in data)
        {
            crc ^= b;
            for (var i = 0; i < 8; i++)
                crc = (crc & 1) != 0 ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
        }
        return crc ^ 0xFFFFFFFFu;
    }

    private sealed class DatIndex
    {
        public Dictionary<uint, (string GameName, string RomName)> ByCrc { get; } = new();

        public static DatIndex Load(string path)
        {
            var idx = new DatIndex();
            var text = File.ReadAllText(path);
            // Walk game( name "…" ) and subsequent rom( name "…" crc XXX ) in document order.
            var re = new Regex(
                @"game\s*\(\s*name\s+""((?:\\.|[^""\\])*)""|rom\s*\(\s*name\s+""((?:\\.|[^""\\])*)""(?:[^)]*?)\bcrc\s+([0-9A-Fa-f]+)",
                RegexOptions.IgnoreCase | RegexOptions.Singleline);
            string? gameName = null;
            foreach (Match m in re.Matches(text))
            {
                if (m.Groups[1].Success)
                {
                    gameName = Unescape(m.Groups[1].Value);
                    continue;
                }
                if (gameName == null || !m.Groups[3].Success) continue;
                var rom = Unescape(m.Groups[2].Value);
                if (!uint.TryParse(m.Groups[3].Value, NumberStyles.HexNumber, null, out var crc))
                    continue;
                idx.ByCrc.TryAdd(crc, (gameName, rom));
            }
            return idx;
        }

        private static string Unescape(string s) =>
            s.Replace("\\\"", "\"").Replace("\\\\", "\\");
    }
}
