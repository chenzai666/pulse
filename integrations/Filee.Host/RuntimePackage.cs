using System.Security.Cryptography;
using System.Text.Json;

namespace Pulse.FileConversion;

internal static class RuntimePackage
{
    public static void Verify(string? directory = null)
    {
        var root = Path.GetFullPath(directory ?? AppContext.BaseDirectory);
        SafePaths.RejectLinks(root);
        var file = Path.Combine(root, "runtime-manifest.json");
        SafePaths.RejectLinks(file);
        if (!File.Exists(file) || new FileInfo(file).Length > 8 * 1024 * 1024)
            throw new InvalidDataException("Conversion runtime manifest is unavailable.");
        using var manifest = JsonDocument.Parse(File.ReadAllText(file));
        if (manifest.RootElement.GetProperty("component").GetString() != "Pulse.Filee.Host" ||
            manifest.RootElement.GetProperty("schema").GetInt32() != 1)
            throw new InvalidDataException("Invalid conversion package.");
        var listed = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        if (manifest.RootElement.GetProperty("files").GetArrayLength() is < 1 or > 50000)
            throw new InvalidDataException("Invalid conversion file list.");
        foreach (var entry in manifest.RootElement.GetProperty("files").EnumerateArray())
        {
            var relative = entry.GetProperty("path").GetString() ?? "";
            if (Path.IsPathRooted(relative) || relative.Contains(':') || relative.Split('/', '\\').Any(p => p == ".."))
                throw new InvalidDataException("Invalid runtime entry.");
            var full = Path.GetFullPath(Path.Combine(root, relative));
            if (!SafePaths.Within(full, root)) throw new InvalidDataException("Runtime entry escaped its directory.");
            if (!listed.Add(full)) throw new InvalidDataException("Duplicate runtime entry.");
            SafePaths.RejectLinks(full);
            if (!File.Exists(full) || new FileInfo(full).Length != entry.GetProperty("bytes").GetInt64())
                throw new InvalidDataException("Conversion package is incomplete.");
            using var input = File.OpenRead(full);
            var expected = entry.GetProperty("sha256").GetString();
            if (!string.Equals(Convert.ToHexString(SHA256.HashData(input)), expected, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("Conversion package integrity check failed.");
        }
        foreach (var required in new[] { "Pulse.Filee.Host.exe", "Pulse.Filee.Host.dll", "Pulse.Filee.Host.deps.json",
            "Pulse.Filee.Host.runtimeconfig.json", "Filee.Core.dll", "Filee.Engines.dll", "engines/rhwp/rhwp.exe",
            "engines/pandoc/pandoc.exe", "engines/7zip/7z.exe", "engines/7zip/7z.dll", "engines/ffmpeg/bin/ffmpeg.exe",
            "engines/ffmpeg/bin/ffprobe.exe" })
            if (!listed.Contains(Path.GetFullPath(Path.Combine(root, required))))
                throw new InvalidDataException("A required runtime file is missing from its manifest.");
        foreach (var actual in SafePaths.TreeEntries(root).OfType<FileInfo>())
            if (!string.Equals(actual.FullName, file, StringComparison.OrdinalIgnoreCase) && !listed.Contains(actual.FullName))
                throw new InvalidDataException("The conversion package contains an unlisted file.");
    }
}
