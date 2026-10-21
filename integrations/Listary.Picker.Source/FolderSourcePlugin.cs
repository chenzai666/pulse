using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;

namespace Listary.FileAppPlugin.PulseFolderSource;

public sealed class FolderSourcePlugin : IFileAppPlugin
{
    public bool IsOpenedFolderProvider => true;
    public bool IsQuickSwitchTarget => false;
    public bool IsSharedAcrossApplications => false;
    public SearchBarType SearchBarType => SearchBarType.Floating;
    public Task<bool> Initialize(IFileAppPluginHost host) => Task.FromResult(true);
    public IFileWindow BindFileWindow(IntPtr window) =>
        FolderSourceWindow.Matches(window) ? new FolderSourceWindow(window) : null!;
}

public sealed class FolderSourceWindow : IFileWindow
{
    private readonly uint _processId;
    public IntPtr Handle { get; }
    internal FolderSourceWindow(IntPtr window)
    {
        Handle = window;
        GetWindowThreadProcessId(window, out _processId);
    }
    internal static bool Matches(IntPtr window)
    {
        if (!IsWindow(window) || !HasClass(window, "PulseCustomMainWindow") ||
            GetPropW(window, "Pulse.FolderSource.v1") != new IntPtr(1)) return false;
        GetWindowThreadProcessId(window, out var pid);
        try
        {
            using var process = Process.GetProcessById((int)pid);
            return string.Equals(Path.GetFileName(process.MainModule?.FileName), "pulse.exe", StringComparison.OrdinalIgnoreCase);
        }
        catch (InvalidOperationException) { return false; }
        catch (System.ComponentModel.Win32Exception) { return false; }
        catch (ArgumentException) { return false; }
    }
    private bool Live()
    {
        GetWindowThreadProcessId(Handle, out var pid);
        return pid == _processId && Matches(Handle);
    }
    public Task<IFileTab> GetCurrentTab() => Task.FromResult(Live() ? (IFileTab)new FolderSourceTab(this) : null!);
    internal Task<string> GetCurrentFolder()
    {
        if (!Live()) return Task.FromResult<string>(null!);
        var edit = GetDlgItem(Handle, 0x6F51);
        GetWindowThreadProcessId(edit, out var pid);
        if (edit == IntPtr.Zero || GetParent(edit) != Handle || pid != _processId || !HasClass(edit, "Edit"))
            return Task.FromResult<string>(null!);
        var text = new StringBuilder(32768);
        // WM_GETTEXT is marshalled by Windows; no external pointer or filesystem I/O is needed.
        if (SendMessageTimeoutW(edit, 0x000D, new IntPtr(text.Capacity), text, 2, 1000, out var length) == IntPtr.Zero ||
            length.ToInt64() <= 0 || length.ToInt64() >= text.Capacity - 1 || !Live())
            return Task.FromResult<string>(null!);
        var path = text.ToString();
        return Task.FromResult(IsFolderPath(path) ? path : null!);
    }
    private static bool IsFolderPath(string path)
    {
        if (string.IsNullOrWhiteSpace(path) || path.IndexOfAny(new[] {'\0', '\r', '\n', '"', '<', '>', '|', '*', '?', '/'}) >= 0) return false;
        if (path.Length >= 3 && ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
            path[1] == ':' && path[2] == '\\') return path.IndexOf(':', 2) < 0;
        if (!path.StartsWith(@"\\", StringComparison.Ordinal) || path.StartsWith(@"\\?\", StringComparison.Ordinal) ||
            path.StartsWith(@"\\.\", StringComparison.Ordinal)) return false;
        var separator = path.IndexOf('\\', 2);
        return separator > 2 && separator + 1 < path.Length && path[separator + 1] != '\\' && path.IndexOf(':') < 0;
    }
    private static bool HasClass(IntPtr window, string expected)
    {
        var text = new StringBuilder(128);
        return GetClassNameW(window, text, text.Capacity) != 0 &&
            string.Equals(text.ToString(), expected, StringComparison.OrdinalIgnoreCase);
    }
    [DllImport("user32.dll")] private static extern bool IsWindow(IntPtr window);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetClassNameW(IntPtr window, StringBuilder text, int capacity);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr GetPropW(IntPtr window, string name);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
    [DllImport("user32.dll")] private static extern IntPtr GetDlgItem(IntPtr window, int id);
    [DllImport("user32.dll")] private static extern IntPtr GetParent(IntPtr window);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr SendMessageTimeoutW(
        IntPtr window, uint message, IntPtr wparam, StringBuilder text, uint flags, uint timeout, out IntPtr result);
}

// The official loader discovers window and tab implementations in separate branches.
public sealed class FolderSourceTab : IFileTab, IGetFolder
{
    private readonly FolderSourceWindow _window;
    internal FolderSourceTab(FolderSourceWindow window) { _window = window; }
    public Task<string> GetCurrentFolder() => _window.GetCurrentFolder();
}
