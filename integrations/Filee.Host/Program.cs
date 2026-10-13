using Microsoft.Win32.SafeHandles;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Text;
using System.Text.Json;

namespace Pulse.FileConversion;

internal static class Program
{
    [DllImport("advapi32.dll")] private static extern bool OpenProcessToken(IntPtr process, uint access, out SafeAccessTokenHandle token);

    public static async Task<int> Main(string[] args)
    {
        if (!OperatingSystem.IsWindows()) return 64;
        Console.SetOut(new StreamWriter(Console.OpenStandardOutput(), new UTF8Encoding(false)) { AutoFlush = true });
        using var lifetime = new CancellationTokenSource();
        SessionTemp? temporary = null;
        ConversionHost? host = null;
        try
        {
            if (args.Length != 2 || args[0] != "--owner-pid" || !int.TryParse(args[1], out var pid) || pid <= 0)
                throw new ArgumentException("A valid owner PID is required.");
            using var owner = Process.GetProcessById(pid);
            using var self = Process.GetCurrentProcess();
            if (owner.SessionId != self.SessionId || !OpenProcessToken(owner.Handle, 8, out var token))
                throw new InvalidOperationException("Owner session is unavailable.");
            using (token)
            using (var identity = new WindowsIdentity(token.DangerousGetHandle()))
            using (var current = WindowsIdentity.GetCurrent())
                if (identity.User != current.User) throw new InvalidOperationException("Owner user does not match.");
            using var job = new WindowsJob();
            var local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
            if (string.IsNullOrEmpty(local)) throw new IOException("Local profile is unavailable.");
            var profile = SafePaths.DirectoryPath(Path.Combine(local, "PulseCustom", "FileConversion"));
            temporary = SessionTemp.Create(profile);
            var session = temporary.Root;
            var cache = SafePaths.DirectoryPath(Path.Combine(session, "Cache"));
            Environment.SetEnvironmentVariable("TEMP", session);
            Environment.SetEnvironmentVariable("TMP", session);
            Environment.SetEnvironmentVariable("MAGICK_TMPDIR", cache);
            Environment.SetEnvironmentVariable("MAGICK_TEMPORARY_PATH", cache);
            _ = Task.Run(async () => { try { await owner.WaitForExitAsync(); lifetime.Cancel(); if (host is not null) await host.StopAsync(); temporary.Dispose(); Environment.Exit(0); } catch (Exception) { lifetime.Cancel(); } });
            RuntimePackage.Verify();
            var engines = await EngineSet.CreateAsync(profile, lifetime.Token);
            host = new ConversionHost(engines, session, lifetime.Token, job);
            Protocol.Send(new { id = "", type = "ready", protocol = 1, version = "1.10.2" });
            using var input = new StreamReader(Console.OpenStandardInput(), new UTF8Encoding(false, true), false, 4096);
            string? line;
            while (!lifetime.IsCancellationRequested && (line = Protocol.ReadLine(input)) is not null)
            {
                Request? request = null;
                try
                {
                    request = JsonSerializer.Deserialize<Request>(line, Protocol.Json) ?? throw new InvalidDataException("Empty request.");
                    if (!Protocol.ValidId(request.Id)) { Protocol.Error("", "INVALID_ID"); continue; }
                    if (request.Action == "shutdown") { Protocol.Send(new { id = request.Id, type = "stopping" }); break; }
                    host.Handle(request);
                }
                catch (NotSupportedException ex) { Protocol.Error(request?.Id ?? "", "UNSUPPORTED", ex.Message); }
                catch (Exception ex) when (ex is JsonException or IOException or InvalidDataException or UnauthorizedAccessException or ArgumentException)
                { Protocol.Error(request?.Id ?? "", "INVALID_REQUEST", ex.Message); }
            }
            lifetime.Cancel();
            await host.StopAsync();
            return 0;
        }
        catch (Exception ex)
        {
            Protocol.Error("", "HOST_UNAVAILABLE", ex.GetType().Name);
            return 1;
        }
        finally
        {
            temporary?.Dispose();
        }
    }
}
