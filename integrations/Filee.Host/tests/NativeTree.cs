using Microsoft.Win32.SafeHandles;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

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
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] private static extern bool Process32FirstW(SafeFileHandle snapshot, ref Entry entry);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] private static extern bool Process32NextW(SafeFileHandle snapshot, ref Entry entry);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern SafeProcessHandle OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool GetProcessTimes(SafeProcessHandle process, out long created, out long exited, out long kernel, out long user);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] private static extern bool QueryFullProcessImageName(SafeProcessHandle process, uint flags, StringBuilder name, ref uint size);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern uint WaitForSingleObject(SafeProcessHandle process, uint milliseconds);

    internal sealed record Identity(int Pid, int Parent, string Name, long Created, SafeProcessHandle Handle) : IDisposable
    {
        public bool Exited => WaitForSingleObject(Handle, 0) switch
        {
            0 => true, 258 => false, _ => throw new IOException("无法等待原始进程句柄。")
        };
        public void Dispose() => Handle.Dispose();
    }

    private static Identity? Open(int pid, int parent)
    {
        var handle = OpenProcess(0x1000 | 0x100000, false, pid);
        if (handle.IsInvalid)
        {
            var error = Marshal.GetLastWin32Error();
            handle.Dispose();
            if (error == 87) return null; // exited between snapshot and open
            throw new IOException($"无法读取测试子进程身份，pid={pid}, win32={error}。");
        }
        try
        {
            if (!GetProcessTimes(handle, out var created, out _, out _, out _)) throw new IOException("无法读取测试进程创建时间。");
            var path = new StringBuilder(32768);
            uint size = (uint)path.Capacity;
            if (!QueryFullProcessImageName(handle, 0, path, ref size))
            {
                if (WaitForSingleObject(handle, 0) == 0) { handle.Dispose(); return null; }
                throw new IOException("无法读取测试进程映像身份。");
            }
            return new Identity(pid, parent, Path.GetFileName(path.ToString()), created, handle);
        }
        catch { handle.Dispose(); throw; }
    }

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

    public static async Task<Identity[]> WaitForEngine(int owner, string engine)
    {
        using var ownerIdentity = Open(owner, 0) ?? throw new IOException("测试宿主已经退出。");
        var deadline = Stopwatch.StartNew();
        while (deadline.Elapsed < TimeSpan.FromSeconds(30))
        {
            var processes = Snapshot();
            var ancestors = new Dictionary<int, long> { [owner] = ownerIdentity.Created };
            var tracked = new Dictionary<int, Identity>();
            var rejected = new HashSet<int>();
            var transferred = false;
            try
            {
                bool changed;
                do
                {
                    changed = false;
                    foreach (var item in processes)
                    {
                        if (item.Key == owner || tracked.ContainsKey(item.Key) || rejected.Contains(item.Key) || !ancestors.TryGetValue(item.Value.Parent, out var parentCreated)) continue;
                        var identity = Open(item.Key, item.Value.Parent);
                        if (identity is null) { rejected.Add(item.Key); continue; }
                        // ToolHelp retains the numeric parent PID after that
                        // parent exits. A child older than its alleged current
                        // parent cannot belong to this host's process tree.
                        if (identity.Created < parentCreated || !identity.Name.Equals(item.Value.Name, StringComparison.OrdinalIgnoreCase))
                        {
                            Console.Error.WriteLine($"PULSE_TEST_PROCESS phase=stale_snapshot pid={identity.Pid} ppid={identity.Parent} name={identity.Name} created={identity.Created} parent_created={parentCreated}");
                            identity.Dispose(); rejected.Add(item.Key); continue;
                        }
                        if (identity.Exited) { identity.Dispose(); rejected.Add(item.Key); continue; }
                        tracked.Add(item.Key, identity);
                        ancestors.Add(item.Key, identity.Created);
                        changed = true;
                    }
                } while (changed);
                if (tracked.Values.Any(item => item.Name.Equals(engine, StringComparison.OrdinalIgnoreCase) && !item.Exited))
                {
                    foreach (var item in tracked.Values)
                        Console.Error.WriteLine($"PULSE_TEST_PROCESS phase=captured pid={item.Pid} ppid={item.Parent} name={item.Name} created={item.Created}");
                    transferred = true;
                    return tracked.Values.ToArray();
                }
            }
            finally
            {
                if (!transferred)
                    foreach (var item in tracked.Values) item.Dispose();
            }
            await Task.Delay(20);
        }
        throw new TimeoutException("没有观察到实际转换引擎子进程：" + engine);
    }

    public static async Task WaitGone(Identity[] processes)
    {
        try
        {
            var deadline = Stopwatch.StartNew();
            while (deadline.Elapsed < TimeSpan.FromSeconds(5))
            {
                // The held handles refer to the original process objects even
                // if the same numeric PIDs are reused by the CI runner.
                if (processes.All(item => item.Exited))
                {
                    foreach (var item in processes)
                    {
                        try
                        {
                            using var current = Open(item.Pid, 0);
                            Console.Error.WriteLine($"PULSE_TEST_PROCESS phase={(current is not null && current.Created != item.Created ? "exited_pid_reused" : "exited")} pid={item.Pid} name={item.Name} created={item.Created} current_created={current?.Created ?? 0}");
                        }
                        catch (IOException)
                        {
                            // The original handle is already signalled. A new
                            // protected process using that PID is only diagnostic.
                            Console.Error.WriteLine($"PULSE_TEST_PROCESS phase=exited_current_identity_unavailable pid={item.Pid} name={item.Name} created={item.Created}");
                        }
                    }
                    return;
                }
                await Task.Delay(50);
            }
            foreach (var item in processes.Where(item => !item.Exited))
                Console.Error.WriteLine($"PULSE_TEST_PROCESS phase=still_alive pid={item.Pid} ppid={item.Parent} name={item.Name} created={item.Created}");
            throw new IOException("宿主留下了活动引擎子进程。");
        }
        finally { foreach (var item in processes) item.Dispose(); }
    }
}
