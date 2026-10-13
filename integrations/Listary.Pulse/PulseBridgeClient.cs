using System;
using System.IO;
using System.IO.Pipes;
using System.Security.Principal;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace Listary.FileAppPlugin.Pulse;

internal enum PulseOperation : uint
{
    CurrentTab = 1, CurrentFolder = 2, OpenFolder = 3, SelectItem = 4,
    NewTabFolder = 5, NewTabSelectItem = 6
}

internal enum PulseStatus : uint
{
    Ok = 0, NoFolder = 1, StaleTab = 2, Invalid = 3, Closing = 4, Busy = 5
}

internal sealed class PulseResponse
{
    internal PulseStatus Status { get; }
    internal ulong Tab { get; }
    internal string Folder { get; }
    internal bool Accepted => Status == PulseStatus.Ok;
    internal PulseResponse(PulseStatus status, ulong tab, string folder)
    {
        Status = status;
        Tab = tab;
        Folder = folder;
    }
}

internal static class PulseBridgeClient
{
    internal const uint Magic = 0x50554c50;
    internal const uint Version = 1;
    internal const int TimeoutMs = 1800;
    internal const int MaxPathChars = 32767;
    private static readonly Encoding Utf16 = new UnicodeEncoding(false, false, true);

    // Validate shape before opening a pipe; the native side independently
    // canonicalizes and admits the path. Do not probe network file existence.
    internal static bool IsAbsoluteFileSystemPath(string? path)
    {
        if (string.IsNullOrEmpty(path) || path!.Length > MaxPathChars) return false;
        path = path.Replace('/', '\\');
        if (path.StartsWith(@"\\?\UNC\", StringComparison.OrdinalIgnoreCase)) path = @"\\" + path.Substring(8);
        else if (path.StartsWith(@"\\?\", StringComparison.Ordinal)) path = path.Substring(4);
        var drive = path.Length >= 3 && ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
            path[1] == ':' && path[2] == '\\';
        var unc = path.StartsWith(@"\\", StringComparison.Ordinal) && path.Length > 4 && path[2] != '.' && path[2] != '?';
        if (!drive && !unc) return false;
        for (var i = 0; i < path.Length; i++)
        {
            var ch = path[i];
            if (ch < 32 || ch == '"' || ch == '<' || ch == '>' || ch == '|' || ch == '*' || ch == '?' ||
                (ch == ':' && !(drive && i == 1))) return false;
            if (char.IsHighSurrogate(ch))
            {
                if (++i >= path.Length || !char.IsLowSurrogate(path[i])) return false;
            }
            else if (char.IsLowSurrogate(ch)) return false;
        }
        if (unc)
        {
            var serverEnd = path.IndexOf('\\', 2);
            if (serverEnd <= 2 || serverEnd + 1 >= path.Length) return false;
            var shareEnd = path.IndexOf('\\', serverEnd + 1);
            var share = shareEnd < 0 ? path.Substring(serverEnd + 1) : path.Substring(serverEnd + 1, shareEnd - serverEnd - 1);
            if (share.Length == 0 || share == "." || share == "..") return false;
        }
        return true;
    }

    internal static async Task<PulseResponse?> Send(PulseWindowIdentity window, PulseOperation operation,
        ulong tab, string? path, CancellationToken lifetime)
    {
        if (!window.StillValid() || lifetime.IsCancellationRequested) return null;
        var query = operation == PulseOperation.CurrentTab || operation == PulseOperation.CurrentFolder;
        if (!query && !IsAbsoluteFileSystemPath(path)) return null;
        path ??= string.Empty;
        if (query && path.Length != 0) return null;
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(lifetime);
        timeout.CancelAfter(TimeoutMs);
        using var pipe = new NamedPipeClientStream(".", window.PipeName, PipeDirection.InOut,
            PipeOptions.Asynchronous, TokenImpersonationLevel.Identification);
        // Closing the handle also cancels .NET Framework pipe operations when
        // a host unloads the plugin or the absolute deadline expires.
        using var registration = timeout.Token.Register(pipe.Dispose);
        try
        {
            var deadline = NativeMethods.GetTickCount64() + TimeoutMs;
            await pipe.ConnectAsync(TimeoutMs, timeout.Token).ConfigureAwait(false);
            if (!window.StillValid() || !NativeMethods.GetNamedPipeServerProcessId(pipe.SafePipeHandle, out var serverPid) ||
                serverPid != window.ProcessId) return null;
            if (!query) NativeMethods.AllowSetForegroundWindow(window.ProcessId);
            byte[] request;
            using (var buffer = new MemoryStream(48 + path.Length * 2))
            using (var writer = new BinaryWriter(buffer, Utf16, true))
            {
                writer.Write(Magic);
                writer.Write(Version);
                writer.Write((uint)operation);
                writer.Write((uint)path.Length);
                writer.Write(unchecked((ulong)window.Handle.ToInt64()));
                writer.Write(window.Created);
                writer.Write(tab);
                writer.Write(deadline);
                writer.Write(Utf16.GetBytes(path));
                request = buffer.ToArray();
            }
            await pipe.WriteAsync(request, 0, request.Length, timeout.Token).ConfigureAwait(false);
            var responseHeader = new byte[24];
            await ReadExactly(pipe, responseHeader, timeout.Token).ConfigureAwait(false);
            PulseStatus status;
            uint chars;
            ulong responseTab;
            using (var buffer = new MemoryStream(responseHeader, false))
            using (var reader = new BinaryReader(buffer, Utf16, true))
            {
                if (reader.ReadUInt32() != Magic || reader.ReadUInt32() != Version) return null;
                status = (PulseStatus)reader.ReadUInt32();
                chars = reader.ReadUInt32();
                responseTab = reader.ReadUInt64();
                if (status > PulseStatus.Busy || chars > MaxPathChars) return null;
            }
            var folderBytes = new byte[checked((int)chars * 2)];
            await ReadExactly(pipe, folderBytes, timeout.Token).ConfigureAwait(false);
            var folder = Utf16.GetString(folderBytes);
            if (chars != 0 && !IsAbsoluteFileSystemPath(folder)) return null;
            await pipe.WriteAsync(new byte[] { 1 }, 0, 1, timeout.Token).ConfigureAwait(false);
            return new PulseResponse(status, responseTab, folder);
        }
        catch (IOException) { return null; }
        catch (TimeoutException) { return null; }
        catch (OperationCanceledException) { return null; }
        catch (ObjectDisposedException) { return null; }
        catch (DecoderFallbackException) { return null; }
        catch (EncoderFallbackException) { return null; }
        catch (UnauthorizedAccessException) { return null; }
    }

    private static async Task ReadExactly(Stream pipe, byte[] buffer, CancellationToken token)
    {
        var offset = 0;
        while (offset < buffer.Length)
        {
            var count = await pipe.ReadAsync(buffer, offset, buffer.Length - offset, token).ConfigureAwait(false);
            if (count == 0) throw new EndOfStreamException();
            offset += count;
        }
    }
}
