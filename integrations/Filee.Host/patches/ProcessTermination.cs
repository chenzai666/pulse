using System.ComponentModel;
using System.Diagnostics;

namespace Filee.Engines.Infrastructure;

internal static class ProcessTermination
{
    internal static async Task KillAndWaitAsync(Process process, string engine)
    {
        var pid = process.Id;
        long created = 0;
        try { created = process.StartTime.ToUniversalTime().ToFileTimeUtc(); }
        catch (InvalidOperationException) { }
        catch (Win32Exception) { }
        // Only a fixed engine basename and process identity are logged. Never
        // write its command line, working directory or source document path.
        Console.Error.WriteLine($"PULSE_ENGINE_STOP engine={engine} pid={pid} created={created} phase=request");
        try
        {
            if (!process.HasExited) process.Kill(entireProcessTree: true);
        }
        catch (InvalidOperationException) { }
        catch (Win32Exception ex)
        {
            Console.Error.WriteLine($"PULSE_ENGINE_STOP engine={engine} pid={pid} phase=tree_kill_failed win32={ex.NativeErrorCode}");
            try { if (!process.HasExited) process.Kill(); }
            catch (InvalidOperationException) { }
            catch (Win32Exception fallback)
            { Console.Error.WriteLine($"PULSE_ENGINE_STOP engine={engine} pid={pid} phase=kill_failed win32={fallback.NativeErrorCode}"); }
        }
        // Killing is asynchronous. Wait on the original Process handle without
        // the already-cancelled job token, before reporting cancellation.
        // A finite WaitForExit confirms the kernel process exit without waiting
        // indefinitely for stdout EOF held open by a surviving console helper.
        // The host's Windows Job drain then confirms the whole remaining tree.
        if (!await Task.Run(() => process.WaitForExit(2000)))
        {
            Console.Error.WriteLine($"PULSE_ENGINE_STOP engine={engine} pid={pid} created={created} phase=exit_timeout");
            throw new IOException("An engine did not exit after cancellation.");
        }
        Console.Error.WriteLine($"PULSE_ENGINE_STOP engine={engine} pid={pid} created={created} phase=exited");
    }
}
