using System;
using System.Runtime.InteropServices;
using System.Text;
using Listary.FileAppPlugin;
using Listary.FileAppPlugin.PulsePicker;

internal sealed class ProtocolHost : IFileAppPluginHost
{
    public Microsoft.Extensions.Logging.ILogger Logger => Microsoft.Extensions.Logging.Abstractions.NullLogger.Instance;
    public string ExpectedPath = "";
    public int ExpectedOperation;
    public int Requests;
    public bool PostMessage(IntPtr window, uint message, IntPtr wparam, IntPtr lparam) => false;
    public IntPtr SendMessage(IntPtr window, uint message, IntPtr wparam, IntPtr lparam) => IntPtr.Zero;
    public IntPtr SendMessage(IntPtr window, uint message, IntPtr wparam, byte[] data) => IntPtr.Zero;
    public IntPtr SendCopyData(IntPtr window, IntPtr sourceWindow, IntPtr dwData, byte[] data, uint dataSize, uint timeout)
    {
        var expected = Encoding.Unicode.GetBytes(ExpectedPath + "\0");
        if (sourceWindow.ToInt64() != ExpectedOperation || dwData.ToInt64() != 0x504C5354 ||
            dataSize != data.Length || dataSize != expected.Length || timeout != 1500 ||
            Convert.ToBase64String(data) != Convert.ToBase64String(expected))
            throw new InvalidOperationException("Invalid Listary path request: operation, bytes or byte count");
        Requests++;
        return new IntPtr(1);
    }
}

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
    [DllImport("user32.dll")] private static extern bool DestroyWindow(IntPtr window);

    private static int Main()
    {
        var spec = new WindowClass { Procedure = Procedure, Name = "PulseFolderPickerWindow" };
        if (RegisterClassW(ref spec) == 0) throw new InvalidOperationException("RegisterClass failed");
        var window = CreateWindowExW(0, spec.Name, "Isolated protocol fixture", 0, 0, 0, 100, 100,
            IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
        if (window == IntPtr.Zero) throw new InvalidOperationException("CreateWindow failed");
        try
        {
            if (!SetPropW(window, "Pulse.Picker.Listary.v1", new IntPtr(1))) throw new InvalidOperationException("SetProp failed");
            const string folder = @"E:\isolated fixture\中文目录";
            if (CreateWindowExW(0, "EDIT", folder, 0x40000000, 0, 0, 10, 10, window,
                new IntPtr(101), IntPtr.Zero, IntPtr.Zero) == IntPtr.Zero) throw new InvalidOperationException("Edit failed");
            var host = new ProtocolHost();
            var plugin = new PickerPlugin();
            Require(plugin.Initialize(host).GetAwaiter().GetResult(), "plugin initializes");
            var bound = plugin.BindFileWindow(window);
            Require(bound != null, "marked picker binds");
            var tab = bound!.GetCurrentTab().GetAwaiter().GetResult();
            Require(((IGetFolder)tab).GetCurrentFolder().GetAwaiter().GetResult() == folder, "current folder reads hidden address field");
            host.ExpectedPath = folder;
            host.ExpectedOperation = 1;
            Require(((IOpenFolder)tab).OpenFolder(folder).GetAwaiter().GetResult(), "folder request includes full UTF-16 payload length");
            host.ExpectedPath = folder + @"\保存.txt";
            host.ExpectedOperation = 2;
            Require(((IOpenFile)tab).OpenFile(host.ExpectedPath).GetAwaiter().GetResult(), "file request includes full UTF-16 payload length");
            Require(!((IOpenFolder)tab).OpenFolder("relative").GetAwaiter().GetResult() && host.Requests == 2, "relative paths rejected before host call");
            return 0;
        }
        finally { DestroyWindow(window); }
    }
    private static void Require(bool condition, string name)
    {
        if (!condition) throw new InvalidOperationException(name);
        Console.WriteLine("[PASS] " + name);
    }
}
