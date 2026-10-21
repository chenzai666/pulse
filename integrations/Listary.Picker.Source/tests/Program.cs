using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;
using Listary.FileAppPlugin;
using Listary.FileAppPlugin.PulseFolderSource;

internal static class Program
{
    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    private delegate IntPtr WindowProc(IntPtr window, uint message, IntPtr wparam, IntPtr lparam);
    private static readonly WindowProc Procedure = DefWindowProcW;
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct WindowClass
    {
        public uint Style;
        public WindowProc Procedure;
        public int ClassExtra, WindowExtra;
        public IntPtr Instance, Icon, Cursor, Background;
        public string? Menu;
        public string Name;
    }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern ushort RegisterClassW(ref WindowClass spec);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr CreateWindowExW(uint exStyle, string name,
        string title, uint style, int x, int y, int width, int height, IntPtr parent, IntPtr menu, IntPtr instance, IntPtr param);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr DefWindowProcW(IntPtr window, uint message, IntPtr wparam, IntPtr lparam);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern bool SetPropW(IntPtr window, string name, IntPtr value);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr RemovePropW(IntPtr window, string name);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern bool SetWindowTextW(IntPtr window, string text);
    [DllImport("user32.dll")] private static extern bool DestroyWindow(IntPtr window);

    private static int Main()
    {
        var spec = new WindowClass { Procedure = Procedure, Name = "PulseCustomMainWindow" };
        Require(RegisterClassW(ref spec) != 0, "isolated source window class registers");
        var window = CreateWindowExW(0, spec.Name, "Isolated folder source", 0, 0, 0, 100, 100,
            IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
        Require(window != IntPtr.Zero, "isolated source window creates");
        try
        {
            var plugin = new FolderSourcePlugin();
            Require(plugin.Initialize(null!).GetAwaiter().GetResult(), "source initializes without a search host");
            Require(plugin.IsOpenedFolderProvider && !plugin.IsQuickSwitchTarget && !plugin.IsSharedAcrossApplications,
                "source capability stays independent from picker target");
            Require(plugin.BindFileWindow(window) == null, "unmarked main window rejected");
            Require(SetPropW(window, "Pulse.FolderSource.v1", new IntPtr(1)), "source marker publishes");
            var bound = plugin.BindFileWindow(window);
            Require(bound != null, "marked main window binds");
            var tab = bound!.GetCurrentTab().GetAwaiter().GetResult();
            Require(tab is IGetFolder && !(tab is IOpenFolder) && !(tab is IOpenFile) &&
                !(bound is IOpenFolder) && !((object)plugin is IOpenFolder), "source exposes no navigation or file opening");
            var implementations = 0;
            foreach (var type in typeof(FolderSourcePlugin).Assembly.GetExportedTypes())
            {
                if (typeof(IFileAppPlugin).IsAssignableFrom(type)) implementations |= 1;
                else if (typeof(IFileWindow).IsAssignableFrom(type)) implementations |= 2;
                else if (typeof(IFileTab).IsAssignableFrom(type)) implementations |= 4;
                Require(!typeof(IOpenFolder).IsAssignableFrom(type) && !typeof(IOpenFile).IsAssignableFrom(type) &&
                    !typeof(IOpenFolderAndSelectFile).IsAssignableFrom(type), "exported source type has no navigation capability");
            }
            Require(implementations == 7, "official loader discovers plugin, window and tab separately");
            var reader = (IGetFolder)tab;
            Require(reader.GetCurrentFolder().GetAwaiter().GetResult() == null, "missing edit returns no folder");
            var wrong = Child(window, "STATIC", @"E:\wrong");
            Require(reader.GetCurrentFolder().GetAwaiter().GetResult() == null, "wrong control class rejected");
            Require(DestroyWindow(wrong), "wrong control released");
            const string folder = @"E:\isolated fixture\中文目录";
            var edit = Child(window, "EDIT", folder);
            Require(reader.GetCurrentFolder().GetAwaiter().GetResult() == folder, "active Unicode directory reads");
            Require(SetWindowTextW(edit, @"\\server\share\next"), "next directory publishes");
            Require(reader.GetCurrentFolder().GetAwaiter().GetResult() == @"\\server\share\next", "same tab reads latest UNC directory");
            foreach (var path in new[] { "", "relative", @"E:relative", @"\root-relative", @"\\server", @"\\?\E:\device", "E:\\bad\rline",
                @"中:\fake", @"E:\bad?name", @"E:\name:stream", @"\\server\share:stream" })
            {
                Require(SetWindowTextW(edit, path), "invalid fixture publishes");
                Require(reader.GetCurrentFolder().GetAwaiter().GetResult() == null, "non-folder source value rejected");
            }
            Require(SetWindowTextW(edit, folder), "valid directory republishes");
            RemovePropW(window, "Pulse.FolderSource.v1");
            Require(bound.GetCurrentTab().GetAwaiter().GetResult() == null && reader.GetCurrentFolder().GetAwaiter().GetResult() == null,
                "revoked marker invalidates old tab");
            Require(SetPropW(window, "Pulse.FolderSource.v1", new IntPtr(1)), "source marker republishes");
            Require(DestroyWindow(window), "source window releases");
            Require(reader.GetCurrentFolder().GetAwaiter().GetResult() == null && plugin.BindFileWindow(window) == null,
                "destroyed source returns no folder");
            window = IntPtr.Zero;
            var picker = new WindowClass { Procedure = Procedure, Name = "PulseFolderPickerWindow" };
            Require(RegisterClassW(ref picker) != 0, "isolated picker class registers");
            var pickerWindow = CreateWindowExW(0, picker.Name, "Isolated picker", 0, 0, 0, 100, 100,
                IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
            Require(pickerWindow != IntPtr.Zero, "isolated picker creates");
            try
            {
                Require(SetPropW(pickerWindow, "Pulse.FolderSource.v1", new IntPtr(1)), "picker fixture marker publishes");
                Require(plugin.BindFileWindow(pickerWindow) == null, "picker cannot bind as an opened-folder source");
            }
            finally { DestroyWindow(pickerWindow); }
            CheckReadTimeout(plugin);
            return 0;
        }
        finally { if (window != IntPtr.Zero) DestroyWindow(window); }
    }
    private static IntPtr Child(IntPtr parent, string name, string text)
    {
        var child = CreateWindowExW(0, name, text, 0x40000000, 0, 0, 10, 10, parent,
            new IntPtr(0x6F51), IntPtr.Zero, IntPtr.Zero);
        if (child == IntPtr.Zero) throw new InvalidOperationException("Fixture child creation failed");
        return child;
    }
    private static void CheckReadTimeout(FolderSourcePlugin plugin)
    {
        using var ready = new ManualResetEventSlim();
        using var release = new ManualResetEventSlim();
        var window = IntPtr.Zero;
        Exception? failure = null;
        var thread = new Thread(() =>
        {
            try
            {
                window = CreateWindowExW(0, "PulseCustomMainWindow", "Blocked isolated source", 0, 0, 0, 100, 100,
                    IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
                if (window == IntPtr.Zero || !SetPropW(window, "Pulse.FolderSource.v1", new IntPtr(1)))
                    throw new InvalidOperationException("Timeout fixture window creation failed");
                Child(window, "EDIT", @"E:\blocked fixture");
                ready.Set();
                // An unresponsive owner thread must not block Listary's UI indefinitely.
                release.Wait();
            }
            catch (Exception error) { failure = error; ready.Set(); }
            finally { if (window != IntPtr.Zero) DestroyWindow(window); }
        });
        thread.IsBackground = true;
        thread.Start();
        try
        {
            Require(ready.Wait(3000), "timeout fixture initializes within bound");
            if (failure != null) throw failure;
            var bound = plugin.BindFileWindow(window);
            Require(bound != null, "blocked source still has valid identity");
            var reader = (IGetFolder)bound!.GetCurrentTab().GetAwaiter().GetResult();
            var watch = Stopwatch.StartNew();
            Require(reader.GetCurrentFolder().GetAwaiter().GetResult() == null && watch.ElapsedMilliseconds < 2500,
                "unresponsive source read times out without a folder");
        }
        finally
        {
            release.Set();
            Require(thread.Join(3000), "timeout fixture thread and windows release");
        }
    }
    private static void Require(bool condition, string name)
    {
        if (!condition) throw new InvalidOperationException(name);
        Console.WriteLine("[PASS] " + name);
    }
}
