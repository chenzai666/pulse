using System.Diagnostics;
using System.Runtime.InteropServices;

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
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr CreateJobObject(IntPtr attributes, string? name);
    [DllImport("kernel32.dll")] private static extern bool SetInformationJobObject(IntPtr job, int kind, ref Limits data, uint size);
    [DllImport("kernel32.dll")] private static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);
    [DllImport("kernel32.dll")] private static extern bool CloseHandle(IntPtr handle);

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

    // The OS closes the job handle on host termination, killing its engine children.
    public void Dispose()
    {
        // Closing a KILL_ON_CLOSE job containing ourselves would terminate the
        // host before its normal exit code is returned. Keep this one handle
        // until OS process teardown; unsuccessful setup has no assigned member.
        if (!assigned && handle != IntPtr.Zero) { CloseHandle(handle); handle = IntPtr.Zero; }
    }
}
