using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;

namespace Listary.FileAppPlugin.PulsePicker;

// A dialog adapter: Listary owns the fixed search bar and Quick Switch UI.
public sealed class PickerPlugin : IFileAppPlugin
{
    private IFileAppPluginHost? _host;
    public bool IsOpenedFolderProvider => false;
    public bool IsQuickSwitchTarget => true;
    public bool IsSharedAcrossApplications => false;
    public SearchBarType SearchBarType => SearchBarType.Fixed;
    public Task<bool> Initialize(IFileAppPluginHost host) { _host = host; return Task.FromResult(true); }
    public IFileWindow BindFileWindow(IntPtr window)
    {
        if (_host == null || !PickerWindow.Matches(window)) return null!;
        return new PickerWindow(_host, window);
    }
}

public sealed class PickerWindow : IFileWindow, IFileTab, IGetFolder, IOpenFolder, IOpenFile
{
    private readonly IFileAppPluginHost _host;
    private readonly uint _processId;
    public IntPtr Handle { get; }
    internal PickerWindow(IFileAppPluginHost host, IntPtr window)
    {
        _host = host; Handle = window;
        GetWindowThreadProcessId(window, out _processId);
    }
    internal static bool Matches(IntPtr window)
    {
        var name = new StringBuilder(128);
        if (!IsWindow(window) || GetClassNameW(window, name, name.Capacity) == 0 ||
            name.ToString() != "PulseFolderPickerWindow" ||
            GetPropW(window, "Pulse.Picker.Listary.v1") != new IntPtr(1)) return false;
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
    public Task<IFileTab> GetCurrentTab() => Task.FromResult(Live() ? (IFileTab)this : null!);
    public Task<string> GetCurrentFolder()
    {
        if (!Live()) return Task.FromResult<string>(null!);
        var edit = GetDlgItem(Handle, 101);
        var text = new StringBuilder(32768);
        var ok = edit != IntPtr.Zero && SendMessageTimeoutW(edit, 0x000D, new IntPtr(text.Capacity), text,
            2, 1000, out _) != IntPtr.Zero;
        return Task.FromResult(ok ? text.ToString() : null!);
    }
    public Task<bool> OpenFolder(string path) => SendPath(path, 1);
    public Task<bool> OpenFile(string path) => SendPath(path, 2);
    private Task<bool> SendPath(string path, int operation)
    {
        if (!Live() || string.IsNullOrWhiteSpace(path) || path.Length > 32767 ||
            path.IndexOfAny(new[] {'\0', '\r', '\n', '"'}) >= 0 || !Path.IsPathRooted(path))
            return Task.FromResult(false);
        var bytes = Encoding.Unicode.GetBytes(path + "\0");
        return Task.FromResult(_host.SendCopyData(Handle, new IntPtr(operation), new IntPtr(0x504C5354),
            bytes, 2, 1500) != IntPtr.Zero);
    }
    [DllImport("user32.dll")] private static extern bool IsWindow(IntPtr window);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetClassNameW(IntPtr window, StringBuilder text, int capacity);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr GetPropW(IntPtr window, string name);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
    [DllImport("user32.dll")] private static extern IntPtr GetDlgItem(IntPtr window, int id);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr SendMessageTimeoutW(
        IntPtr window, uint message, IntPtr wparam, StringBuilder text, uint flags, uint timeout, out IntPtr result);
}
