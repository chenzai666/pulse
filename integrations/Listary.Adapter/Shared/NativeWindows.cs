using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace Listary.FileAppPlugin.PulseAdapter;

internal sealed class NativeWindows : IWindows
{
    internal const string MainClass = "PulseCustomMainWindow";
    internal const string PickerClass = "PulseFolderPickerWindow";
    private const string SourceMarker = "Pulse.FolderSource.v1";
    private const string AdapterMarker = "Pulse.Listary.Adapter.v1";
    private const string TabMarker = "Pulse.Listary.Tab.v1";
    private const string PickerMarker = "Pulse.Picker.Listary.v1";

    public WindowIdentity? Capture(IntPtr handle, bool picker, string executable)
    {
        try
        {
            if (!MatchesWindow(handle, picker)) return null;
            GetWindowThreadProcessId(handle, out var pid);
            if (pid == 0 || !ProcessIdToSessionId(pid, out var session) ||
                !ProcessIdToSessionId(GetCurrentProcessId(), out var ownSession) || session != ownSession) return null;
            using var process = OpenProcess(0x1000, false, pid);
            if (process.IsInvalid || !GetProcessTimes(process, out var created, out _, out _, out _)) return null;
            var path = new StringBuilder(32768);
            var chars = (uint)path.Capacity;
            if (!QueryFullProcessImageNameW(process, 0, path, ref chars) ||
                !string.Equals(path.ToString(), executable, StringComparison.OrdinalIgnoreCase)) return null;
            if (!OpenProcessToken(process, 8, out var token)) return null;
            using (token)
            using (var peer = new WindowsIdentity(token.DangerousGetHandle()))
            using (var own = WindowsIdentity.GetCurrent())
            {
                if (own.User == null || !own.User.Equals(peer.User)) return null;
            }
            GetWindowThreadProcessId(handle, out var finalPid);
            if (pid != finalPid || !MatchesWindow(handle, picker)) return null;
            return new WindowIdentity(handle, pid, created.Value, path.ToString(), picker);
        }
        catch (System.Security.SecurityException) { return null; }
        catch (UnauthorizedAccessException) { return null; }
        catch (ArgumentException) { return null; }
    }

    private static bool MatchesWindow(IntPtr handle, bool picker) => IsWindow(handle) &&
        ClassName(handle) == (picker ? PickerClass : MainClass) &&
        GetPropW(handle, picker ? PickerMarker : SourceMarker) == new IntPtr(1) &&
        (picker || GetPropW(handle, AdapterMarker) == new IntPtr(1));

    public uint Generation(WindowIdentity window)
    {
        var value = unchecked((ulong)GetPropW(window.Handle, TabMarker).ToInt64());
        return value > uint.MaxValue ? 0 : (uint)value;
    }

    public string? ReadFolder(WindowIdentity window)
    {
        var edit = GetDlgItem(window.Handle, window.Picker ? 101 : 0x6F51);
        if (edit == IntPtr.Zero || GetParent(edit) != window.Handle || ClassName(edit) != "Edit") return null;
        GetWindowThreadProcessId(edit, out var pid);
        if (pid != window.Pid) return null;
        var text = new StringBuilder(PathProtocol.MaxChars + 2);
        if (SendMessageTimeoutW(edit, 0x000D, new IntPtr(text.Capacity), text, 3, 200, out var count) == IntPtr.Zero ||
            count.ToUInt64() > PathProtocol.MaxChars || GetDlgItem(window.Handle, window.Picker ? 101 : 0x6F51) != edit) return null;
        var path = text.ToString();
        return PathProtocol.IsValid(path) ? path : null;
    }

    public IntPtr Sender()
    {
        var sender = IntPtr.Zero;
        var own = GetCurrentProcessId();
        EnumWindows((window, _) =>
        {
            GetWindowThreadProcessId(window, out var pid);
            if (pid != own) return true;
            sender = window;
            return false;
        }, IntPtr.Zero);
        return sender;
    }

    private static string ClassName(IntPtr window)
    {
        var value = new StringBuilder(128);
        return GetClassNameW(window, value, value.Capacity) == 0 ? "" : value.ToString();
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct FileTime
    {
        internal uint Low;
        internal uint High;
        internal ulong Value => ((ulong)High << 32) | Low;
    }
    private sealed class KernelHandle : SafeHandleZeroOrMinusOneIsInvalid
    {
        private KernelHandle() : base(true) { }
        protected override bool ReleaseHandle() => CloseHandle(handle);
    }
    private delegate bool EnumCallback(IntPtr window, IntPtr param);
    [DllImport("user32.dll", ExactSpelling = true)] [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsWindow(IntPtr window);
    [DllImport("user32.dll", ExactSpelling = true, CharSet = CharSet.Unicode)]
    private static extern int GetClassNameW(IntPtr window, StringBuilder value, int count);
    [DllImport("user32.dll", ExactSpelling = true, CharSet = CharSet.Unicode)]
    private static extern IntPtr GetPropW(IntPtr window, string name);
    [DllImport("user32.dll", ExactSpelling = true)]
    private static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
    [DllImport("user32.dll", ExactSpelling = true)]
    private static extern IntPtr GetDlgItem(IntPtr window, int id);
    [DllImport("user32.dll", ExactSpelling = true)]
    private static extern IntPtr GetParent(IntPtr window);
    [DllImport("user32.dll", ExactSpelling = true)] [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool EnumWindows(EnumCallback callback, IntPtr param);
    [DllImport("user32.dll", ExactSpelling = true, CharSet = CharSet.Unicode)]
    private static extern IntPtr SendMessageTimeoutW(IntPtr window, uint message, IntPtr wparam,
        StringBuilder text, uint flags, uint timeout, out UIntPtr result);
    [DllImport("kernel32.dll", ExactSpelling = true)]
    private static extern uint GetCurrentProcessId();
    [DllImport("kernel32.dll", ExactSpelling = true)] [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool ProcessIdToSessionId(uint pid, out uint session);
    [DllImport("kernel32.dll", ExactSpelling = true)]
    private static extern KernelHandle OpenProcess(uint access, [MarshalAs(UnmanagedType.Bool)] bool inherit, uint pid);
    [DllImport("kernel32.dll", ExactSpelling = true)] [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetProcessTimes(KernelHandle process, out FileTime created,
        out FileTime exited, out FileTime kernel, out FileTime user);
    [DllImport("kernel32.dll", ExactSpelling = true, CharSet = CharSet.Unicode)] [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool QueryFullProcessImageNameW(KernelHandle process, uint flags, StringBuilder name, ref uint count);
    [DllImport("advapi32.dll", ExactSpelling = true)] [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool OpenProcessToken(KernelHandle process, uint access, out KernelHandle token);
    [DllImport("kernel32.dll", ExactSpelling = true)] [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseHandle(IntPtr handle);
}
