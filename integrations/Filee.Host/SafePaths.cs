namespace Pulse.FileConversion;

internal static class SafePaths
{
    public static string Absolute(string path)
    {
        if (string.IsNullOrWhiteSpace(path) || path.Length > 32767 || path.Any(c => c < ' ' || c is '"' or '<' or '>' or '|' or '*'))
            throw new InvalidDataException("Invalid path.");
        var plain = path;
        if (plain.StartsWith(@"\\?\UNC\", StringComparison.OrdinalIgnoreCase)) plain = @"\\" + plain[8..];
        else if (plain.StartsWith(@"\\?\", StringComparison.Ordinal)) plain = plain[4..];
        var drive = plain.Length >= 3 && char.IsAsciiLetter(plain[0]) && plain[1] == ':' && plain[2] is '\\' or '/';
        var unc = plain.StartsWith(@"\\", StringComparison.Ordinal) && !plain.StartsWith(@"\\.\", StringComparison.Ordinal);
        if ((!drive && !unc) || plain.Contains('?') || plain.IndexOf(':', drive ? 2 : 0) >= 0 || !Path.IsPathFullyQualified(plain))
            throw new InvalidDataException("Only ordinary absolute file paths are accepted.");
        if (unc)
        {
            var separator = plain.IndexOf('\\', 2);
            if (separator <= 2 || separator + 1 >= plain.Length || plain[separator + 1] == '\\')
                throw new InvalidDataException("A network path must include its server and share.");
        }
        foreach (var segment in plain[(drive ? 3 : 2)..].Split(['\\', '/'], StringSplitOptions.RemoveEmptyEntries))
        {
            if (segment is "." or "..") continue;
            var stem = segment.Split('.')[0].ToUpperInvariant();
            if (segment.EndsWith(' ') || segment.EndsWith('.') || stem is "CON" or "PRN" or "AUX" or "NUL" or "CONIN$" or "CONOUT$" ||
                (stem.Length == 4 && (stem.StartsWith("COM") || stem.StartsWith("LPT")) && (stem[3] is >= '0' and <= '9' or '¹' or '²' or '³')))
                throw new InvalidDataException("Windows device names and ambiguous path segments are not accepted.");
        }
        return Path.GetFullPath(path);
    }

    public static string Input(string path)
    {
        var full = Absolute(path);
        if (!File.Exists(full)) throw new FileNotFoundException("Input file is unavailable.");
        RejectLinks(full);
        if (new FileInfo(full).Length > 2L * 1024 * 1024 * 1024) throw new InvalidDataException("Input exceeds the 2 GiB limit.");
        return full;
    }

    public static void RejectLinks(string path)
    {
        for (var current = Path.GetFullPath(path); !string.IsNullOrEmpty(current); current = Path.GetDirectoryName(current))
            if ((File.Exists(current) || Directory.Exists(current)) &&
                (File.GetAttributes(current) & FileAttributes.ReparsePoint) != 0)
                throw new IOException("Reparse points are not allowed in input, output or runtime paths.");
    }

    public static string DirectoryPath(string path)
    {
        var full = Absolute(path);
        RejectLinks(full);
        Directory.CreateDirectory(full);
        RejectLinks(full);
        return full;
    }

    public static bool Within(string path, string root) => Path.GetFullPath(path).StartsWith(
        Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase);

    public static string Publish(string source, string destination, CancellationToken cancellation = default)
    {
        RejectLinks(destination);
        RejectLinks(source);
        var name = Path.GetFileName(source);
        if (string.IsNullOrEmpty(name)) throw new IOException("Invalid output name.");
        for (var n = 0; n < 10000; n++)
        {
            cancellation.ThrowIfCancellationRequested();
            var candidate = Path.Combine(destination, n == 0 ? name : Path.GetFileNameWithoutExtension(name) +
                " (" + (n + 1) + ")" + Path.GetExtension(name));
            if (!Within(candidate, destination)) throw new IOException("Output escaped its target folder.");
            if (File.Exists(candidate) || Directory.Exists(candidate)) continue;
            try
            {
                if (File.Exists(source))
                {
                    var temporary = Path.Combine(destination, ".pulse-" + Guid.NewGuid().ToString("N") + ".tmp");
                    try
                    {
                        CopyFile(source, temporary, cancellation);
                        cancellation.ThrowIfCancellationRequested();
                        RejectLinks(destination);
                        File.Move(temporary, candidate, false);
                    }
                    finally { if (File.Exists(temporary)) File.Delete(temporary); }
                }
                else
                {
                    // Validate the entire tree before a same-volume move.
                    foreach (var entry in TreeEntries(source)) cancellation.ThrowIfCancellationRequested();
                    RejectLinks(destination);
                    if (string.Equals(Path.GetPathRoot(source), Path.GetPathRoot(destination), StringComparison.OrdinalIgnoreCase))
                        Directory.Move(source, candidate);
                    else CopyNewDirectory(source, candidate, cancellation);
                }
                return candidate;
            }
            catch (IOException) when (File.Exists(candidate) || Directory.Exists(candidate))
            {
                // A different process took this name. Never replace its data.
                if (!File.Exists(source) && !Directory.Exists(source)) throw;
            }
        }
        throw new IOException("No unused output name could be reserved.");
    }

    private static void CopyNewDirectory(string source, string target, CancellationToken cancellation)
    {
        // CreateDirectory cannot reserve an absent directory exclusively. Reserve
        // a unique sibling, fill it, then publish it with the non-overwriting move.
        var staged = target + ".pulse-" + Guid.NewGuid().ToString("N") + ".tmp";
        Directory.CreateDirectory(staged);
        try
        {
            foreach (var entry in TreeEntries(source))
            {
                cancellation.ThrowIfCancellationRequested();
                var copy = Path.Combine(staged, Path.GetRelativePath(source, entry.FullName));
                if (!Within(copy, staged)) throw new IOException("Invalid extracted directory.");
                if (entry is DirectoryInfo) Directory.CreateDirectory(copy);
                else
                {
                    Directory.CreateDirectory(Path.GetDirectoryName(copy)!);
                    CopyFile(entry.FullName, copy, cancellation);
                }
            }
            cancellation.ThrowIfCancellationRequested();
            RejectLinks(Path.GetDirectoryName(target)!);
            Directory.Move(staged, target);
        }
        finally { if (Directory.Exists(staged)) Directory.Delete(staged, true); }
    }

    private static void CopyFile(string source, string target, CancellationToken cancellation)
    {
        RejectLinks(source);
        using var input = new FileStream(source, FileMode.Open, FileAccess.Read, FileShare.Read, 65536);
        using var output = new FileStream(target, FileMode.CreateNew, FileAccess.Write, FileShare.None, 65536);
        var buffer = new byte[65536];
        int count;
        while ((count = input.Read(buffer)) > 0)
        {
            cancellation.ThrowIfCancellationRequested();
            output.Write(buffer, 0, count);
        }
    }

    public static IEnumerable<FileSystemInfo> TreeEntries(string root)
    {
        RejectLinks(root);
        var pending = new Stack<DirectoryInfo>();
        pending.Push(new DirectoryInfo(root));
        while (pending.TryPop(out var directory))
        {
            FileSystemInfo[] entries;
            try { entries = directory.GetFileSystemInfos(); }
            catch (DirectoryNotFoundException) { continue; }
            foreach (var entry in entries)
            {
                if (!entry.Exists) continue;
                if ((entry.Attributes & FileAttributes.ReparsePoint) != 0)
                    throw new IOException("Output trees cannot contain reparse points.");
                yield return entry;
                if (entry is DirectoryInfo child) pending.Push(child);
            }
        }
    }
}
