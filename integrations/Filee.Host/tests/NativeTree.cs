using Microsoft.Win32.SafeHandles;
using System.Diagnostics;
using System.Runtime.InteropServices;

internal static class NativeTree
{
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] private struct Entry
    {
        public uint Size, Usage, Pid;
        public UIntPtr Heap;
        public uint Module, Threads, Parent;
        public int Priority;
        public uint Flags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)] public string Name;
    }
    [DllImport("kernel32.dll", SetLastError = true)] private static extern SafeFileHandle CreateToolhelp32Snapshot(uint flags, uint pid);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] private static extern bool Process32FirstW(SafeFileHandle snapshot, ref Entry entry);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] private static extern bool Process32NextW(SafeFileHandle snapshot, ref Entry entry);

    private static Dictionary<int, (int Parent, string Name)> Snapshot()
    {
        using var snapshot = CreateToolhelp32Snapshot(2, 0);
        if (snapshot.IsInvalid) throw new IOException("无法读取 Windows 进程树。");
        var entry = new Entry { Size = (uint)Marshal.SizeOf<Entry>() };
        var processes = new Dictionary<int, (int, string)>();
        if (Process32FirstW(snapshot, ref entry))
            do { processes[(int)entry.Pid] = ((int)entry.Parent, entry.Name); } while (Process32NextW(snapshot, ref entry));
        return processes;
    }

    public static async Task<int[]> WaitForEngine(int owner, string engine)
    {
        var deadline = Stopwatch.StartNew();
        while (deadline.Elapsed < TimeSpan.FromSeconds(30))
        {
            var processes = Snapshot();
            var descendants = new HashSet<int> { owner };
            bool changed;
            do
            {
                changed = false;
                foreach (var item in processes)
                    if (descendants.Contains(item.Value.Parent)) changed |= descendants.Add(item.Key);
            } while (changed);
            descendants.Remove(owner);
            if (descendants.Any(pid => processes.TryGetValue(pid, out var item) && item.Name.Equals(engine, StringComparison.OrdinalIgnoreCase))) return descendants.ToArray();
            await Task.Delay(20);
        }
        throw new TimeoutException("没有观察到实际转换引擎子进程：" + engine);
    }

    public static async Task WaitGone(int[] processes)
    {
        var deadline = Stopwatch.StartNew();
        while (deadline.Elapsed < TimeSpan.FromSeconds(5))
        {
            if (processes.All(pid => !Alive(pid))) return;
            await Task.Delay(50);
        }
        throw new IOException("宿主留下了活动引擎子进程。");
    }

    private static bool Alive(int pid)
    {
        try { using var process = Process.GetProcessById(pid); return !process.HasExited; }
        catch (ArgumentException) { return false; }
    }
}
