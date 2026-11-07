using System;

namespace Listary.FileAppPlugin.PulseAdapter;

internal sealed class WindowIdentity
{
    internal readonly IntPtr Handle;
    internal readonly uint Pid;
    internal readonly ulong Created;
    internal readonly string Executable;
    internal readonly bool Picker;

    internal WindowIdentity(IntPtr handle, uint pid, ulong created, string executable, bool picker)
    {
        Handle = handle; Pid = pid; Created = created; Executable = executable; Picker = picker;
    }

    internal bool Same(WindowIdentity? other) => other != null && Handle == other.Handle && Pid == other.Pid &&
        Created == other.Created && Picker == other.Picker &&
        string.Equals(Executable, other.Executable, StringComparison.OrdinalIgnoreCase);
}

internal interface IWindows
{
    WindowIdentity? Capture(IntPtr handle, bool picker, string executable);
    uint Generation(WindowIdentity window);
    string? ReadFolder(WindowIdentity window);
    IntPtr Sender();
}
