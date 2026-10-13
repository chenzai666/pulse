using System.Text;

namespace Pulse.FileConversion;

internal sealed class SessionTemp : IDisposable
{
    private const string Marker = "Pulse.Filee.Host temporary session v1";
    private FileStream? lease;
    private readonly string temporaryRoot;
    public string Root { get; }

    private SessionTemp(string temporaryRoot, string root, FileStream lease)
    { this.temporaryRoot = temporaryRoot; Root = root; this.lease = lease; }

    public static SessionTemp Create(string profile)
    {
        var temporaryRoot = SafePaths.DirectoryPath(Path.Combine(profile, "Temp"));
        CleanupStale(temporaryRoot);
        var root = SafePaths.DirectoryPath(Path.Combine(temporaryRoot, Guid.NewGuid().ToString("N")));
        var lease = new FileStream(Path.Combine(root, "host.lock"), FileMode.CreateNew, FileAccess.ReadWrite, FileShare.None);
        lease.Write(Encoding.ASCII.GetBytes(Marker));
        lease.Flush();
        return new SessionTemp(temporaryRoot, root, lease);
    }

    internal static void CleanupStale(string temporaryRoot)
    {
        SafePaths.RejectLinks(temporaryRoot);
        var removed = 0;
        foreach (var directory in new DirectoryInfo(temporaryRoot).EnumerateDirectories().Take(128))
        {
            if (removed >= 8) break;
            if (!Guid.TryParseExact(directory.Name, "N", out _)) continue;
            try
            {
                SafePaths.RejectLinks(directory.FullName);
                var marker = Path.Combine(directory.FullName, "host.lock");
                SafePaths.RejectLinks(marker);
                if (!File.Exists(marker) || DateTime.UtcNow - File.GetLastWriteTimeUtc(marker) < TimeSpan.FromMinutes(5)) continue;
                using (var probe = new FileStream(marker, FileMode.Open, FileAccess.Read, FileShare.None))
                {
                    if (probe.Length != Marker.Length) continue;
                    var data = new byte[Marker.Length];
                    probe.ReadExactly(data);
                    if (Encoding.ASCII.GetString(data) != Marker) continue;
                }
                Remove(directory.FullName, temporaryRoot);
                removed++;
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { }
        }
    }

    private static void Remove(string root, string temporaryRoot)
    {
        // Only a marked, direct GUID child of this component's private Temp
        // directory may be recursively removed. Never walk through links.
        if (!string.Equals(Path.GetDirectoryName(root), temporaryRoot, StringComparison.OrdinalIgnoreCase) ||
            !Guid.TryParseExact(Path.GetFileName(root), "N", out _)) throw new IOException("Invalid session cleanup directory.");
        SafePaths.RejectLinks(root);
        if (SafePaths.TreeEntries(root).Take(50002).Count() > 50001) return;
        Directory.Delete(root, true);
    }

    public void Dispose()
    {
        var previous = Interlocked.Exchange(ref lease, null);
        if (previous is null) return;
        previous.Dispose();
        try { Remove(Root, temporaryRoot); }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { }
    }
}
