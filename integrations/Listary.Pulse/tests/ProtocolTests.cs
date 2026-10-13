using System;
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Security.AccessControl;
using System.Security.Principal;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Listary.FileAppPlugin;
using Listary.FileAppPlugin.Pulse;

internal static class ProtocolTests
{
    private static readonly Encoding Utf16 = new UnicodeEncoding(false, false, true);

    private static int Main(string[] args)
    {
        try
        {
            if (args.Length == 4 && args[0] == "--native-window" && args[2] == "--fixture")
            {
                NativeSmoke(new IntPtr(long.Parse(args[1])), args[3]).GetAwaiter().GetResult();
                return 0;
            }
            using var window = new FixtureWindow(NativeMethods.WindowClass);
            var identity = PulseWindowIdentity.Capture(window.Handle);
            Check(identity != null, "synthetic custom window identity");
            using var upstream = new FixtureWindow("PulseMainWindow");
            Check(PulseWindowIdentity.Capture(upstream.Handle) == null, "upstream edition rejected");
            ManagedProtocol(identity!).GetAwaiter().GetResult();
            return 0;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine("[FAIL] " + error.GetType().Name + ": " + error.Message);
            return 1;
        }
    }

    private static async Task ManagedProtocol(PulseWindowIdentity identity)
    {
        var client = new PulseWindow(identity, CancellationToken.None);
        var tab = await Exchange(identity, client.GetCurrentTab, request =>
        {
            Check(request.Operation == PulseOperation.CurrentTab && request.Tab == 0 && request.Path.Length == 0,
                "current-tab frame has zero scope and no path");
            return Reply(PulseStatus.Ok, 41, @"C:\测试目录");
        }, true);
        Check(tab is PulseTab, "official SDK produces a file tab");
        var fileTab = (PulseTab)tab;
        var folder = await Exchange(identity, fileTab.GetCurrentFolder, request =>
        {
            Check(request.Operation == PulseOperation.CurrentFolder && request.Tab == 41, "tab scope is retained");
            return Reply(PulseStatus.Ok, 41, @"C:\测试目录");
        });
        Check(folder == @"C:\测试目录", "fragmented UTF-16 response decoded");
        var selected = await Exchange(identity, () => fileTab.OpenFolderAndSelectFile(@"C:\测试目录\a & b.txt"), request =>
        {
            Check(request.Operation == PulseOperation.SelectItem && request.Tab == 41 &&
                request.Path == @"C:\测试目录\a & b.txt", "selection is a data path, not a command");
            return Reply(PulseStatus.Ok, 42, string.Empty);
        });
        Check(selected, "current-tab file selection accepted");
        await Exchange(identity, fileTab.GetCurrentFolder, request =>
        {
            Check(request.Tab == 42, "successful navigation refreshes the tab token");
            return Reply(PulseStatus.NoFolder, 42, string.Empty);
        });
        var newTab = await Exchange(identity, () => client.OpenFolderAndSelectFile(@"C:\测试目录\a.txt"), request =>
        {
            Check(request.Operation == PulseOperation.NewTabSelectItem && request.Tab == 0, "window selection opens a new tab");
            return Reply(PulseStatus.Ok, 43, string.Empty);
        });
        Check(newTab, "window-level file selection accepted");
        var stale = await Exchange(identity, fileTab.GetCurrentFolder, _ => Reply(PulseStatus.StaleTab, 0, string.Empty));
        Check(stale == null, "stale tab is not treated as success");

        foreach (var input in new[] { "relative.txt", @"C:relative.txt", @"\\.\PhysicalDrive0", @"\\?\GLOBALROOT\Device\Harddisk0",
            @"\\server", @"C:\item:stream", "C:\\bad\0name", "C:\\bad\uD800name", "pulse:search:query" })
            Check(!PulseBridgeClient.IsAbsoluteFileSystemPath(input), "unsafe path rejected before connecting");
        Check(PulseBridgeClient.IsAbsoluteFileSystemPath(@"\\server\share\中文\文件.txt"), "UNC file path admitted");
        Check(PulseBridgeClient.IsAbsoluteFileSystemPath(@"\\?\C:\long\file.txt"), "extended DOS path admitted");
        Check(PulseBridgeClient.IsAbsoluteFileSystemPath(@"\\?\UNC\server\share\file.txt"), "extended UNC path admitted");

        var malformed = await Exchange(identity, () => PulseBridgeClient.Send(identity, PulseOperation.CurrentTab, 0, null,
            CancellationToken.None), _ =>
        {
            var response = Reply(PulseStatus.Ok, 1, string.Empty);
            Array.Copy(BitConverter.GetBytes((uint)PulseBridgeClient.MaxPathChars + 1), 0, response, 12, 4);
            return response;
        }, expectReceipt: false);
        Check(malformed == null, "oversized response rejected");

        using (var server = Server(identity.PipeName))
        {
            var connected = server.WaitForConnectionAsync();
            var elapsed = Stopwatch.StartNew();
            var query = PulseBridgeClient.Send(identity, PulseOperation.CurrentTab, 0, null, CancellationToken.None);
            await connected;
            await ReadRequest(server);
            Check(await query == null && elapsed.ElapsedMilliseconds < 3000, "hung peer expires within the request deadline");
        }
        using (var server = Server(identity.PipeName))
        using (var lifetime = new CancellationTokenSource())
        {
            var connected = server.WaitForConnectionAsync();
            var query = PulseBridgeClient.Send(identity, PulseOperation.CurrentTab, 0, null, lifetime.Token);
            await connected;
            await ReadRequest(server);
            var elapsed = Stopwatch.StartNew();
            lifetime.Cancel();
            Check(await query == null && elapsed.ElapsedMilliseconds < 500, "plugin cancellation closes a pending pipe");
        }
    }

    private static async Task NativeSmoke(IntPtr handle, string fixture)
    {
        Check(PulseBridgeClient.IsAbsoluteFileSystemPath(fixture), "explicit native fixture path");
        using var plugin = new PulsePlugin();
        await plugin.Initialize(null!);
        var window = plugin.BindFileWindow(handle);
        Check(window is PulseWindow, "native custom window binds to official SDK");
        var tab = (PulseTab)await window.GetCurrentTab();
        Check(tab != null, "native bridge returns the active tab");
        Check(await tab!.OpenFolder(fixture), "native current-tab directory navigation accepted");
        Check(string.Equals((await tab.GetCurrentFolder())?.TrimEnd('\\'), fixture.TrimEnd('\\'),
            StringComparison.OrdinalIgnoreCase), "native current directory follows navigation");
        Check(await tab.OpenFolderAndSelectFile(Path.Combine(fixture, "listary-selected.txt")), "native selection request accepted");
        Check(await ((IOpenFolderAndSelectFile)window).OpenFolderAndSelectFile(Path.Combine(fixture, "listary-selected.txt")),
            "native new-tab selection request accepted");
        Check(await tab.GetCurrentFolder() == null, "native previous tab becomes stale after new-tab activation");
        Console.WriteLine("[INFO] 最终选中行、滚动位置和真实 Listary 搜索交接仍需界面验证。");
    }

    private static async Task<T> Exchange<T>(PulseWindowIdentity identity, Func<Task<T>> client,
        Func<WireRequest, byte[]> reply, bool fragment = false, bool expectReceipt = true)
    {
        using var server = Server(identity.PipeName);
        var peer = Respond(server, identity, reply, fragment, expectReceipt);
        var response = await client();
        await peer;
        return response;
    }

    private static async Task Respond(NamedPipeServerStream server, PulseWindowIdentity identity,
        Func<WireRequest, byte[]> reply, bool fragment, bool expectReceipt)
    {
        await server.WaitForConnectionAsync();
        var request = await ReadRequest(server);
        Check(request.Window == unchecked((ulong)identity.Handle.ToInt64()) && request.Created == identity.Created,
            "request binds the HWND and process creation time");
        Check(request.Deadline > NativeMethods.GetTickCount64() &&
            request.Deadline - NativeMethods.GetTickCount64() <= 2000, "request carries a bounded absolute deadline");
        var response = reply(request);
        for (var offset = 0; offset < response.Length;)
        {
            var count = fragment ? Math.Min(5, response.Length - offset) : response.Length - offset;
            await server.WriteAsync(response, offset, count);
            offset += count;
            if (fragment) await Task.Yield();
        }
        if (expectReceipt)
        {
            var receipt = new byte[1];
            await ReadExactly(server, receipt);
            Check(receipt[0] == 1, "response delivery acknowledged before disconnect");
        }
    }

    private sealed class WireRequest
    {
        internal PulseOperation Operation;
        internal ulong Window, Created, Tab, Deadline;
        internal string Path = string.Empty;
    }

    private static async Task<WireRequest> ReadRequest(Stream pipe)
    {
        var header = new byte[48];
        await ReadExactly(pipe, header);
        var request = new WireRequest();
        uint chars;
        using (var reader = new BinaryReader(new MemoryStream(header, false), Utf16))
        {
            Check(reader.ReadUInt32() == PulseBridgeClient.Magic && reader.ReadUInt32() == 1, "request protocol header");
            request.Operation = (PulseOperation)reader.ReadUInt32();
            chars = reader.ReadUInt32();
            request.Window = reader.ReadUInt64();
            request.Created = reader.ReadUInt64();
            request.Tab = reader.ReadUInt64();
            request.Deadline = reader.ReadUInt64();
        }
        var path = new byte[checked((int)chars * 2)];
        await ReadExactly(pipe, path);
        request.Path = Utf16.GetString(path);
        return request;
    }

    private static byte[] Reply(PulseStatus status, ulong tab, string folder)
    {
        using var buffer = new MemoryStream();
        using var writer = new BinaryWriter(buffer, Utf16, true);
        writer.Write(PulseBridgeClient.Magic);
        writer.Write(1u);
        writer.Write((uint)status);
        writer.Write((uint)folder.Length);
        writer.Write(tab);
        writer.Write(Utf16.GetBytes(folder));
        return buffer.ToArray();
    }

    private static NamedPipeServerStream Server(string name)
    {
        using var identity = WindowsIdentity.GetCurrent();
        var security = new PipeSecurity();
        security.SetAccessRuleProtection(true, false);
        security.AddAccessRule(new PipeAccessRule(identity.User!, PipeAccessRights.FullControl, AccessControlType.Allow));
        return new NamedPipeServerStream(name, PipeDirection.InOut, 1, PipeTransmissionMode.Byte,
            PipeOptions.Asynchronous, 65536, 65536, security);
    }

    private static async Task ReadExactly(Stream stream, byte[] bytes)
    {
        var offset = 0;
        while (offset < bytes.Length)
        {
            var count = await stream.ReadAsync(bytes, offset, bytes.Length - offset);
            if (count == 0) throw new EndOfStreamException();
            offset += count;
        }
    }

    private static void Check(bool condition, string label)
    {
        if (!condition) throw new InvalidOperationException(label);
        Console.WriteLine("[PASS] " + label);
    }

    private sealed class FixtureWindow : IDisposable
    {
        private delegate IntPtr WindowProc(IntPtr handle, uint message, IntPtr wparam, IntPtr lparam);
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct WindowClass
        {
            public uint Size, Style;
            public IntPtr Procedure;
            public int ClassExtra, WindowExtra;
            public IntPtr Instance, Icon, Cursor, Background;
            public string? MenuName;
            public string ClassName;
            public IntPtr SmallIcon;
        }
        private readonly WindowProc _procedure = DefWindowProcW;
        private readonly string _className;
        private readonly IntPtr _instance;
        internal IntPtr Handle { get; }

        internal FixtureWindow(string className)
        {
            _className = className;
            _instance = GetModuleHandleW(null);
            var definition = new WindowClass { Size = (uint)Marshal.SizeOf<WindowClass>(),
                Procedure = Marshal.GetFunctionPointerForDelegate(_procedure), Instance = _instance, ClassName = className };
            if (RegisterClassExW(ref definition) == 0) throw new System.ComponentModel.Win32Exception();
            Handle = CreateWindowExW(0, className, "Listary protocol fixture", 0, 0, 0, 1, 1,
                IntPtr.Zero, IntPtr.Zero, _instance, IntPtr.Zero);
            if (Handle == IntPtr.Zero || !SetPropW(Handle, NativeMethods.BridgeProperty, new IntPtr(1)))
                throw new System.ComponentModel.Win32Exception();
        }

        public void Dispose()
        {
            DestroyWindow(Handle);
            UnregisterClassW(_className, _instance);
            GC.KeepAlive(_procedure);
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
        private static extern IntPtr GetModuleHandleW(string? name);
        [DllImport("user32.dll", CharSet = CharSet.Unicode, ExactSpelling = true, SetLastError = true)]
        private static extern ushort RegisterClassExW(ref WindowClass definition);
        [DllImport("user32.dll", CharSet = CharSet.Unicode, ExactSpelling = true, SetLastError = true)]
        private static extern IntPtr CreateWindowExW(uint extendedStyle, string className, string name, uint style,
            int x, int y, int width, int height, IntPtr parent, IntPtr menu, IntPtr instance, IntPtr parameter);
        [DllImport("user32.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
        private static extern IntPtr DefWindowProcW(IntPtr handle, uint message, IntPtr wparam, IntPtr lparam);
        [DllImport("user32.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool SetPropW(IntPtr handle, string name, IntPtr value);
        [DllImport("user32.dll", ExactSpelling = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool DestroyWindow(IntPtr handle);
        [DllImport("user32.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool UnregisterClassW(string name, IntPtr instance);
    }
}
