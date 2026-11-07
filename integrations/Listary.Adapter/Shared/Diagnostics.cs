using System;
using System.Diagnostics;
using System.IO;
using System.Text;

namespace Listary.FileAppPlugin.PulseAdapter;

internal static class Diagnostics
{
    private static readonly object Gate = new object();

    internal static void Write(bool picker, string code)
    {
#if ADAPTER_TESTS
        return;
#else
        // Callers pass fixed event names only; never log paths, queries, or payloads.
        try
        {
            lock (Gate)
            {
                var folder = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                    "PulseCustom", "Diagnostics", "ListaryAdapter");
                Directory.CreateDirectory(folder);
                var file = Path.Combine(folder, picker ? "picker.log" : "manager.log");
                if (File.Exists(file) && new FileInfo(file).Length >= 32768) return;
                using var process = Process.GetCurrentProcess();
                File.AppendAllText(file, DateTime.UtcNow.ToString("O") + " pid=" + process.Id + " " + code + Environment.NewLine,
                    new UTF8Encoding(false));
            }
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
        catch (System.Security.SecurityException) { }
#endif
    }
}
