using System.Diagnostics;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace Pulse.FileConversion;

internal sealed class WindowsJob : IDisposable
{
    private IntPtr handle;
    private bool assigned;
    [StructLayout(LayoutKind.Sequential)] private struct BasicLimits
    {
        public long ProcessTime, JobTime;
        public uint Flags;
        public UIntPtr MinWorkingSet, MaxWorkingSet;
        public uint ActiveProcesses;
        public UIntPtr Affinity;
        public uint Priority, Scheduling;
    }
    [StructLayout(LayoutKind.Sequential)] private struct IoCounters { public ulong A, B, C, D, E, F; }
    [StructLayout(LayoutKind.Sequential)] private struct Limits
    {
        public BasicLimits Basic;
        public IoCounters Io;
        public UIntPtr ProcessMemory, JobMemory, PeakProcessMemory, PeakJobMemory;
    }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] private static extern IntPtr CreateJobObject(IntPtr attributes, string? name);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool SetInformationJobObject(IntPtr job, int kind, ref Limits data, uint size);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);
    [DllImport("kernel32.dll")] private static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool QueryInformationJobObject(IntPtr job, int kind, IntPtr data, uint length, out uint returned);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern SafeProcessHandle OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool IsProcessInJob(SafeProcessHandle process, IntPtr job, out bool member);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool TerminateProcess(SafeProcessHandle process, uint code);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern uint WaitForSingleObject(SafeProcessHandle process, uint milliseconds);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool GetProcessTimes(SafeProcessHandle process, out long created, out long exited, out long kernel, out long user);

    public WindowsJob()
    {
        handle = CreateJobObject(IntPtr.Zero, null);
        var limits = new Limits
        {
            Basic = new BasicLimits { Flags = 0x2000 | 0x200 | 0x8, ActiveProcesses = 64 },
            JobMemory = (UIntPtr)(2UL * 1024 * 1024 * 1024)
        };
        using var self = Process.GetCurrentProcess();
        if (handle == IntPtr.Zero || !SetInformationJobObject(handle, 9, ref limits, (uint)Marshal.SizeOf<Limits>()) ||
            !AssignProcessToJobObject(handle, self.Handle))
        {
            Dispose();
            throw new InvalidOperationException("Cannot isolate the conversion process tree.");
        }
        assigned = true;
    }

    public async Task<bool> DrainChildrenAsync()
    {
        var deadline = Stopwatch.StartNew();
        try
        {
            while (deadline.Elapsed < TimeSpan.FromSeconds(2))
            {
                var alive = 0;
                foreach (var pid in Members())
                {
                    if (pid == Environment.ProcessId) continue;
                    using var process = OpenProcess(0x1000 | 0x100000 | 0x1, false, pid);
                    if (process.IsInvalid)
                    {
                        var error = Marshal.GetLastWin32Error();
                        if (error == 87) continue; // the process exited before OpenProcess
                        Console.Error.WriteLine($"PULSE_JOB_STOP pid={pid} phase=open_failed win32={error}");
                        return false;
                    }
                    // A PID might have been reused between querying the job and
                    // opening the process. Never terminate a nonmember.
                    if (!IsProcessInJob(process, handle, out var member))
                    {
                        Console.Error.WriteLine($"PULSE_JOB_STOP pid={pid} phase=membership_failed win32={Marshal.GetLastWin32Error()}");
                        return false;
                    }
                    if (!member || WaitForSingleObject(process, 0) == 0) continue;
                    alive++;
                    GetProcessTimes(process, out var created, out _, out _, out _);
                    Console.Error.WriteLine($"PULSE_JOB_STOP pid={pid} created={created} phase=terminate_member");
                    if (!TerminateProcess(process, 3))
                    {
                        var error = Marshal.GetLastWin32Error();
                        if (WaitForSingleObject(process, 0) != 0)
                            Console.Error.WriteLine($"PULSE_JOB_STOP pid={pid} phase=terminate_failed win32={error}");
                    }
                }
                if (alive == 0) return true;
                await Task.Delay(25);
            }
        }
        catch (IOException) { Console.Error.WriteLine("PULSE_JOB_STOP phase=query_failed"); }
        Console.Error.WriteLine("PULSE_JOB_STOP phase=drain_timeout");
        return false;
    }

    private int[] Members()
    {
        var bytes = 8 + 64 * IntPtr.Size;
        var buffer = Marshal.AllocHGlobal(bytes);
        try
        {
            if (!QueryInformationJobObject(handle, 3, buffer, (uint)bytes, out _))
                throw new IOException("Cannot inspect the conversion job process list.");
            var count = Marshal.ReadInt32(buffer, 4);
            if (count is < 0 or > 64) throw new IOException("Invalid conversion job process count.");
            var result = new int[count];
            for (var i = 0; i < count; i++) result[i] = checked((int)Marshal.ReadIntPtr(buffer, 8 + i * IntPtr.Size).ToInt64());
            return result;
        }
        finally { Marshal.FreeHGlobal(buffer); }
    }

    // The OS closes the job handle on host termination, killing its engine children.
    public void Dispose()
    {
        // Closing a KILL_ON_CLOSE job containing ourselves would terminate the
        // host before its normal exit code is returned. Keep this one handle
        // until OS process teardown; unsuccessful setup has no assigned member.
        if (!assigned && handle != IntPtr.Zero) { CloseHandle(handle); handle = IntPtr.Zero; }
    }
}
