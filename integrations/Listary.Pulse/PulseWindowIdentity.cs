using System;
using System.IO;
using System.Security.Principal;
using System.Security;
using System.Text;

namespace Listary.FileAppPlugin.Pulse;

internal sealed class PulseWindowIdentity
{
    internal IntPtr Handle { get; }
    internal uint ProcessId { get; }
    internal ulong Created { get; }
    internal string ExecutablePath { get; }
    internal string PipeName { get; }

    private PulseWindowIdentity(IntPtr handle, uint processId, uint session, ulong created,
        string executablePath, string sid)
    {
        Handle = handle;
        ProcessId = processId;
        Created = created;
        ExecutablePath = executablePath;
        PipeName = "PulseCustom.Listary.v1." + sid + "." + session + "." + processId + "." +
            unchecked((ulong)handle.ToInt64()).ToString("X16") + "." + created.ToString("X16");
    }

    internal bool StillValid()
    {
        NativeMethods.GetWindowThreadProcessId(Handle, out var processId);
        return processId == ProcessId && NativeMethods.IsWindow(Handle) &&
            NativeMethods.GetPropW(Handle, NativeMethods.BridgeProperty) == new IntPtr(1);
    }

    internal static PulseWindowIdentity? Capture(IntPtr handle)
    {
        try { return CaptureCore(handle); }
        catch (SecurityException) { return null; }
        catch (ArgumentException) { return null; }
        catch (UnauthorizedAccessException) { return null; }
    }

    private static PulseWindowIdentity? CaptureCore(IntPtr handle)
    {
        if (!NativeMethods.IsWindow(handle) ||
            NativeMethods.GetPropW(handle, NativeMethods.BridgeProperty) != new IntPtr(1)) return null;
        var windowClass = new StringBuilder(128);
        if (NativeMethods.GetClassNameW(handle, windowClass, windowClass.Capacity) == 0 ||
            windowClass.ToString() != NativeMethods.WindowClass) return null;
        NativeMethods.GetWindowThreadProcessId(handle, out var processId);
        if (processId == 0 || !NativeMethods.ProcessIdToSessionId(processId, out var session) ||
            !NativeMethods.ProcessIdToSessionId(NativeMethods.GetCurrentProcessId(), out var ownSession) ||
            session != ownSession) return null;
        using var process = NativeMethods.OpenProcess(NativeMethods.ProcessQueryLimitedInformation, false, processId);
        if (process.IsInvalid || !NativeMethods.GetProcessTimes(process, out var created, out _, out _, out _)) return null;
        var path = new StringBuilder(32768);
        var chars = (uint)path.Capacity;
        if (!NativeMethods.QueryFullProcessImageNameW(process, 0, path, ref chars)) return null;
        var executablePath = path.ToString();
        var executableName = Path.GetFileName(executablePath);
        if (!string.Equals(executableName, "pulse.exe", StringComparison.OrdinalIgnoreCase) &&
            !string.Equals(executableName, "PulseCustom.exe", StringComparison.OrdinalIgnoreCase)) return null;
        if (!NativeMethods.OpenProcessToken(process, NativeMethods.TokenQuery, out var token)) return null;
        using (token)
        using (var peer = new WindowsIdentity(token.DangerousGetHandle()))
        using (var own = WindowsIdentity.GetCurrent())
        {
            var sid = own.User?.Value;
            if (string.IsNullOrEmpty(sid) || !string.Equals(peer.User?.Value, sid, StringComparison.Ordinal)) return null;
            return new PulseWindowIdentity(handle, processId, session, created.Value, executablePath, sid!);
        }
    }
}
