using System;
using System.Linq;
using System.Reflection;
using System.Text;
using System.Threading.Tasks;
using Listary.FileAppPlugin;
using Listary.FileAppPlugin.PulseAdapter;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;

internal static class Program
{
    private static int _checks;

    private static void Check(bool condition, string name)
    {
        if (!condition) throw new InvalidOperationException(name);
        ++_checks;
        Console.WriteLine("[PASS] " + name);
    }

    private static int Main(string[] args)
    {
        try
        {
            if (args.Length == 3 && args[0] == "--loader-contract")
            {
                LoaderContract(args[1], false);
                LoaderContract(args[2], true);
            }
            else if (args.Length == 0) Run().GetAwaiter().GetResult();
            else throw new ArgumentException("Usage: no arguments, or --loader-contract <ManagerDll> <PickerDll>");
            Console.WriteLine("[PASS] " + _checks + " adapter checks");
            return 0;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine("[FAIL] " + error);
            return 1;
        }
    }

    private static void LoaderContract(string path, bool picker)
    {
        var assembly = Assembly.LoadFrom(System.IO.Path.GetFullPath(path));
        var expected = picker ? "Listary.FileAppPlugin.PulsePickerAdapter" : "Listary.FileAppPlugin.PulseManager";
        Check(assembly.GetName().Name == expected && assembly != typeof(Program).Assembly,
            expected + " checks independent production assembly");
        Check(assembly.GetName().Name != "Listary.FileAppPlugin.PulsePicker" &&
            assembly.GetName().Name != "Listary.FileAppPlugin.PulseFolderSource",
            expected + " has no legacy plugin assembly identity collision");
        var exported = assembly.GetExportedTypes();
        var plugins = exported.Where(type => typeof(IFileAppPlugin).IsAssignableFrom(type)).ToArray();
        var windows = exported.Where(type => typeof(IFileWindow).IsAssignableFrom(type)).ToArray();
        var tabs = exported.Where(type => typeof(IFileTab).IsAssignableFrom(type)).ToArray();
        Check(plugins.Length == 1 && !plugins[0].IsAbstract && plugins[0].GetConstructor(Type.EmptyTypes) != null,
            expected + " exports one constructible SDK plugin");
        Check(windows.Length == 1 && !windows[0].IsAbstract, expected + " exports one SDK window");
        Check(tabs.Length == 1 && !tabs[0].IsAbstract, expected + " exports one SDK tab");
        Check(typeof(IGetFolder).IsAssignableFrom(tabs[0]) && typeof(IOpenFolder).IsAssignableFrom(tabs[0]) &&
            typeof(IOpenFile).IsAssignableFrom(tabs[0]) == picker &&
            typeof(IOpenFolderAndSelectFile).IsAssignableFrom(tabs[0]) == picker,
            expected + " exposes only its intended tab capabilities");
        Check(windows[0].GetConstructors().Length == 0 && tabs[0].GetConstructors().Length == 0,
            expected + " keeps window and tab construction internal");
    }

    private static AdapterCore Core(bool picker, FakeWindows windows) => new AdapterCore(picker, windows,
        FakeWindows.Executable, (window, generation) => picker ?
            (IFileTab)new PickerTab(window, generation) : new ManagerTab(window, generation));

    private static async Task Run()
    {
        var good = new[] { @"C:\", @"D:\中文\folder", @"\\server\share", @"\\server\share\", "C:\\emoji-\ud83d\ude00" };
        foreach (var path in good) Check(PathProtocol.IsValid(path), "valid absolute filesystem path");
        var bad = new[] { "", "relative", @"C:relative", @"C:/bad", @"\\?\C:\x", @"\\.\pipe\x", @"\\server",
            @"C:\..\x", @"C:\x.\y", @"C:\x \y", @"C:\CON.txt", @"C:\COM¹", @"C:\LPT9", @"C:\x:y",
            @"C:\x\" + "\0", "C:\\bad\nname", "C:\\bad\ud800", "C:\\bad\udc00", @"C:\a\\b" };
        foreach (var path in bad) Check(!PathProtocol.IsValid(path), "reject unsafe or malformed path");
        Check(!PathProtocol.IsValid(null), "reject null path");
        var limit = @"C:\" + new string('a', PathProtocol.MaxChars - 3);
        Check(PathProtocol.Encode(limit, true, 0)!.Length == PathProtocol.MaxBytes, "picker max UTF16 payload exact");
        Check(PathProtocol.Encode(limit + "a", true, 0) == null, "picker oversized payload rejected");
        Check(PathProtocol.Encode(limit, false, 1) == null, "manager accounts for 16-byte header");
        Check(PathProtocol.Encode(@"C:\x", false, 0) == null, "manager rejects zero tab generation");
        var packet = PathProtocol.Encode(@"C:\测试", false, 0x12345678)!;
        Check(BitConverter.ToUInt32(packet, 0) == 1 && BitConverter.ToUInt32(packet, 4) == 6 &&
            BitConverter.ToUInt64(packet, 8) == 0x12345678 && Encoding.Unicode.GetString(packet, 16, packet.Length - 16) == "C:\\测试\0",
            "manager PLSM header and strict UTF16 contract");

        using var manager = new PulseManagerPlugin();
        using var picker = new PulsePickerPlugin();
        Check(manager.IsOpenedFolderProvider && !manager.IsQuickSwitchTarget && manager.SearchBarType == SearchBarType.Floating,
            "manager Floating/source/target contract");
        Check(!picker.IsOpenedFolderProvider && picker.IsQuickSwitchTarget && picker.SearchBarType == SearchBarType.Fixed,
            "picker Fixed/source/target contract");
        Check(typeof(PulseManagerPlugin).Assembly.GetTypes().Count(t => typeof(IFileAppPlugin).IsAssignableFrom(t)) == 2,
            "only concrete plugin classes implement SDK plugin interface");

        var windows = new FakeWindows();
        var host = new FakeHost();
        using var core = Core(false, windows);
        Check(await core.Initialize(host), "initialize with trusted executable");
        Check(core.BindFileWindow(new IntPtr(9)) == null, "reject unrelated HWND");
        var window = core.BindFileWindow(new IntPtr(10));
        Check(window != null, "bind manager exact identity");
        var tab = await window!.GetCurrentTab();
        Check(tab is IGetFolder && tab is IOpenFolder && !(tab is IOpenFile), "manager exposes folder capabilities only");
        Check(await ((IGetFolder)tab).GetCurrentFolder() == @"C:\current", "read current manager directory");
        Check(await ((IOpenFolder)tab).OpenFolder(@"D:\target") && host.Calls == 1 && host.Tag == PathProtocol.ManagerTag &&
            host.WParam == new IntPtr(30) && host.Timeout == 750 && host.DataSize == host.Data!.Length &&
            host.DataSize == 16 + Encoding.Unicode.GetByteCount("D:\\target\0"),
            "manager sends bounded SDK CopyData with exact dataSize");
        windows.GenerationValue = 8;
        Check(!await ((IOpenFolder)tab).OpenFolder(@"D:\target") && host.Calls == 1, "reject stale tab navigation after switch");
        Check(await ((IGetFolder)tab).GetCurrentFolder() == null, "reject stale tab folder read");
        tab = await window.GetCurrentTab();
        windows.Created = 22;
        Check(!await ((IOpenFolder)tab).OpenFolder(@"D:\target") && host.Calls == 1, "reject reused HWND or PID creation time");
        windows.Created = 11;
        windows.IdentityPid = 999;
        Check(await window.GetCurrentTab() == null, "reject changed owning PID");
        windows.IdentityPid = 20;
        windows.ExecutablePath = @"C:\other\pulse.exe";
        Check(await window.GetCurrentTab() == null, "reject same-name different executable");
        windows.ExecutablePath = FakeWindows.Executable;
        windows.Present = false;
        Check(!await ((IOpenFolder)tab).OpenFolder(@"D:\target"), "reject closed or unmarked window");
        windows.Present = true;
        windows.ChangeGenerationOnRead = true;
        Check(await window.GetCurrentTab() == null, "reject tab switch during folder capture");
        windows.ChangeGenerationOnRead = false;
        tab = await window.GetCurrentTab();
        windows.SenderValue = IntPtr.Zero;
        Check(!await ((IOpenFolder)tab).OpenFolder(@"D:\target") && host.Calls == 1, "reject missing authenticated sender HWND");
        windows.SenderValue = new IntPtr(30);
        host.Result = IntPtr.Zero;
        Check(!await ((IOpenFolder)tab).OpenFolder(@"D:\target") && host.Calls == 2, "host timeout or rejection is failure");
        core.Dispose();
        Check(!await ((IOpenFolder)tab).OpenFolder(@"D:\target") && host.Calls == 2 && core.BindFileWindow(new IntPtr(10)) == null,
            "unload cancellation revokes existing and new bindings");

        windows = new FakeWindows { Picker = true };
        host = new FakeHost();
        using var pickerCore = Core(true, windows);
        await pickerCore.Initialize(host);
        window = pickerCore.BindFileWindow(new IntPtr(10));
        tab = await window.GetCurrentTab();
        Check(tab is IOpenFile && tab is IOpenFolderAndSelectFile, "picker exposes file preparation capabilities");
        Check(await ((IOpenFolder)tab).OpenFolder(@"D:\target") && host.WParam == new IntPtr(1) &&
            host.Tag == PathProtocol.PickerTag && host.DataSize == host.Data!.Length &&
            host.DataSize == Encoding.Unicode.GetByteCount("D:\\target\0"),
            "picker folder uses original PLST opcode 1 and exact dataSize");
        Check(await ((IOpenFile)tab).OpenFile(@"D:\target\file.txt") && host.WParam == new IntPtr(2) &&
            Encoding.Unicode.GetString(host.Data!) == "D:\\target\\file.txt\0" && host.DataSize == host.Data!.Length &&
            host.DataSize == Encoding.Unicode.GetByteCount("D:\\target\\file.txt\0"),
            "picker file uses original PLST opcode 2 and exact dataSize");
        Check(await ((IOpenFolderAndSelectFile)tab).OpenFolderAndSelectFile(@"D:\target\file.txt") && host.Calls == 3,
            "picker select result prepares file without confirm messages");
        Check(host.NonCopyDataCalls == 0, "never sends enter/click or confirmation message");
        ((IDisposable)window).Dispose();
        Check(!await ((IOpenFile)tab).OpenFile(@"D:\target\file.txt") && host.Calls == 3,
            "window disposal revokes picker callbacks");
        using var wrongKind = Core(false, windows);
        await wrongKind.Initialize(host);
        Check(wrongKind.BindFileWindow(new IntPtr(10)) == null, "manager cannot bind picker class/marker");
    }

    private sealed class FakeWindows : IWindows
    {
        internal const string Executable = @"C:\Pulse\pulse.exe";
        internal bool Picker;
        internal bool Present = true;
        internal ulong Created = 11;
        internal uint IdentityPid = 20;
        internal string ExecutablePath = Executable;
        internal uint GenerationValue = 7;
        internal bool ChangeGenerationOnRead;
        internal IntPtr SenderValue = new IntPtr(30);
        public WindowIdentity? Capture(IntPtr handle, bool picker, string executable) =>
            Present && handle == new IntPtr(10) && picker == Picker && executable == ExecutablePath ?
                new WindowIdentity(handle, IdentityPid, Created, ExecutablePath, Picker) : null;
        public uint Generation(WindowIdentity window) => GenerationValue;
        public string ReadFolder(WindowIdentity window)
        {
            if (ChangeGenerationOnRead) ++GenerationValue;
            return @"C:\current";
        }
        public IntPtr Sender() => SenderValue;
    }

    private sealed class FakeHost : IFileAppPluginHost
    {
        public ILogger Logger => NullLogger.Instance;
        internal int Calls;
        internal int NonCopyDataCalls;
        internal uint Tag;
        internal IntPtr WParam;
        internal byte[]? Data;
        internal uint DataSize;
        internal uint Timeout;
        internal IntPtr Result = new IntPtr(1);
        public IntPtr SendCopyData(IntPtr window, IntPtr wparam, IntPtr tag, byte[] data, uint dataSize, uint timeout)
        {
            if (dataSize != data.Length) throw new InvalidOperationException("SDK dataSize truncates the COPYDATA payload");
            ++Calls; WParam = wparam; Tag = unchecked((uint)tag.ToInt64()); Data = data; DataSize = dataSize; Timeout = timeout;
            return Result;
        }
        public IntPtr SendMessage(IntPtr window, uint msg, IntPtr wp, IntPtr lp) { ++NonCopyDataCalls; return IntPtr.Zero; }
        public IntPtr SendMessage(IntPtr window, uint msg, IntPtr wp, byte[] lp) { ++NonCopyDataCalls; return IntPtr.Zero; }
        public bool PostMessage(IntPtr window, uint msg, IntPtr wp, IntPtr lp) { ++NonCopyDataCalls; return false; }
    }
}
