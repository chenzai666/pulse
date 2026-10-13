// SPDX-License-Identifier: GPL-3.0-or-later
using QuickLook.Common.Helpers;
using QuickLook.Common.Plugin;
using QuickLook.Helpers;
using System;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Threading;
using Wpf.Ui.Appearance;
using Wpf.Ui.Violeta.Appearance;

namespace QuickLook;

// Pulse supplies selection and lifetime. Original QuickLook viewers and their
// pin/fullscreen/open/copy actions stay in this isolated, on-demand process.
internal static class PulseBridge
{
    internal static string SelectedPath { get; private set; }
    private static readonly object outputLock = new();
    private static IntPtr ownerHandle;
    private static int ownerPid;
    private static ulong generation;
    private static bool stopping;
    private static int stateQueued;
    private static string lastState;
    private static StreamReader input;
    private static DispatcherTimer ownerTimer;
    private static DispatcherTimer closeTimer;
    private static bool hasWebView2;

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsWindow(IntPtr hwnd);
    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);

    internal static void Start(App app, string[] args)
    {
        try
        {
            if (args.Length != 4 || args[3] != "--popup" ||
                !ulong.TryParse(args[0], NumberStyles.None, CultureInfo.InvariantCulture, out var hwnd) ||
                hwnd == 0 || hwnd > long.MaxValue ||
                !int.TryParse(args[1], NumberStyles.None, CultureInfo.InvariantCulture, out ownerPid) ||
                ownerPid <= 0 || !ulong.TryParse(args[2], NumberStyles.None,
                    CultureInfo.InvariantCulture, out generation) || generation == 0)
                throw new ArgumentException("Invalid preview owner.");
            ownerHandle = (IntPtr)(long)hwnd;
            if (!OwnerAlive()) throw new ArgumentException("Preview owner does not match.");
            using (var owner = Process.GetProcessById(ownerPid))
            using (var self = Process.GetCurrentProcess())
                if (owner.SessionId != self.SessionId) throw new ArgumentException("Invalid owner session.");

            ValidateRuntime();
            input = new StreamReader(Console.OpenStandardInput(), new UTF8Encoding(false, true), false, 4096);
            Console.SetOut(new StreamWriter(Console.OpenStandardOutput(), new UTF8Encoding(false)) { AutoFlush = true });
            SelectedPath = ReadPath(ReadLine());

            app.DispatcherUnhandledException += (_, e) =>
            {
                e.Handled = true;
                Send("ERROR\t" + Id() + "\tVIEWER_FAILED");
                Stop();
            };
            TaskScheduler.UnobservedTaskException += (_, e) => e.SetObserved();
            app.ShutdownMode = ShutdownMode.OnExplicitShutdown;
            ThemeManager.Apply(OSThemeHelper.AppsUseDarkTheme() ? ApplicationTheme.Dark : ApplicationTheme.Light);
            PluginManager.GetInstance();
            hasWebView2 = HasWebView2();
            ownerTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
            ownerTimer.Tick += (_, _) => { if (!OwnerAlive()) Stop(); };
            ownerTimer.Start();

            ViewWindowManager.GetInstance().InvokePreview(SelectedPath);
            Send("READY");
            QueueState();
            _ = ReadCommands();
            _ = Task.Run(() =>
            {
                try { using (var owner = Process.GetProcessById(ownerPid)) owner.WaitForExit(); }
                catch (ArgumentException) { }
                catch (InvalidOperationException) { }
                Application.Current.Dispatcher.BeginInvoke(new Action(Stop));
            });
        }
        catch (FileNotFoundException)
        {
            Send("ERROR\t" + Id() + "\tTARGET_UNAVAILABLE");
            app.Shutdown(1);
        }
        catch (Exception)
        {
            Send("ERROR\t" + Id() + "\tHOST_UNAVAILABLE");
            app.Shutdown(1);
        }
    }

    private static bool OwnerAlive()
    {
        return IsWindow(ownerHandle) && GetWindowThreadProcessId(ownerHandle, out var pid) != 0 && pid == ownerPid;
    }

    private static string Id() => generation.ToString(CultureInfo.InvariantCulture);
    private static void Send(string message)
    {
        lock (outputLock)
        {
            try { Console.Out.WriteLine(message); Console.Out.Flush(); }
            catch (IOException) { }
        }
    }

    private static string ReadLine()
    {
        var line = new StringBuilder();
        while (true)
        {
            var ch = input.Read();
            if (ch < 0) return line.Length == 0 ? null : line.ToString();
            if (ch == '\n')
            {
                if (line.Length > 0 && line[line.Length - 1] == '\r') line.Length--;
                return line.ToString();
            }
            if (line.Length >= 131136) throw new InvalidDataException("IPC line is too long.");
            line.Append((char)ch);
        }
    }

    private static Task ReadCommands() => Task.Run(() =>
    {
        try
        {
            string line;
            while ((line = ReadLine()) != null)
            {
                var parts = line.Split(new[] { '\t' }, 3);
                if (parts.Length < 2 || !ulong.TryParse(parts[1], NumberStyles.None,
                    CultureInfo.InvariantCulture, out var id) || id == 0)
                    throw new InvalidDataException("Invalid IPC generation.");
                var command = parts[0];
                if ((command == "CLOSE" && parts.Length != 2) ||
                    ((command == "TOGGLE" || command == "SWITCH") && parts.Length != 3) ||
                    (command != "TOGGLE" && command != "SWITCH" && command != "CLOSE"))
                    throw new InvalidDataException("Invalid preview command.");
                string path = null;
                try { if (parts.Length == 3) path = ReadPath(parts[2]); }
                catch (FileNotFoundException)
                {
                    Application.Current.Dispatcher.Invoke(() =>
                    {
                        if (id <= generation) return;
                        generation = id;
                        Send("ERROR\t" + Id() + "\tTARGET_UNAVAILABLE");
                        ViewWindowManager.GetInstance().ClosePreview();
                        QueueState();
                    });
                    continue;
                }
                Application.Current.Dispatcher.Invoke(() =>
                {
                    if (stopping || id <= generation) return;
                    generation = id;
                    if (path != null) SelectedPath = path;
                    closeTimer?.Stop();
                    var manager = ViewWindowManager.GetInstance();
                    switch (command)
                    {
                        case "TOGGLE": manager.TogglePreview(path); break;
                        case "SWITCH": manager.SwitchPreview(path); break;
                        case "CLOSE": manager.ClosePreview(); break;
                    }
                    QueueState();
                }, DispatcherPriority.Input);
            }
        }
        catch (Exception)
        {
            Send("ERROR\t" + Id() + "\tIPC_FAILED");
        }
        finally { Application.Current.Dispatcher.BeginInvoke(new Action(Stop)); }
    });

    private static string ReadPath(string path)
    {
        if (string.IsNullOrWhiteSpace(path) || path.Length > 32767 ||
            path.Any(ch => ch < ' ' || ch == '"' || ch == '<' || ch == '>' || ch == '|' || ch == '*'))
            throw new FileNotFoundException("Invalid preview target.");
        var raw = path;
        var extendedUnc = raw.StartsWith(@"\\?\UNC\", StringComparison.OrdinalIgnoreCase);
        if (extendedUnc) raw = @"\\" + raw.Substring(8);
        else if (raw.StartsWith(@"\\?\", StringComparison.Ordinal)) raw = raw.Substring(4);
        var drive = raw.Length >= 3 && char.IsLetter(raw[0]) && raw[1] == ':' && (raw[2] == '\\' || raw[2] == '/');
        var unc = raw.StartsWith(@"\\", StringComparison.Ordinal) && !raw.StartsWith(@"\\.\", StringComparison.Ordinal);
        if ((!drive && !unc) || raw.Contains('?') || raw.IndexOf(':', drive ? 2 : 0) >= 0)
            throw new FileNotFoundException("Invalid preview target.");
        if (unc)
        {
            var serverEnd = raw.IndexOf('\\', 2);
            if (serverEnd <= 2 || serverEnd + 1 >= raw.Length || raw[serverEnd + 1] == '\\')
                throw new FileNotFoundException("Invalid network target.");
        }
        try
        {
            var full = Path.GetFullPath(path);
            if (!File.Exists(full) && !Directory.Exists(full)) throw new FileNotFoundException();
            return full;
        }
        catch (ArgumentException) { throw new FileNotFoundException("Invalid preview target."); }
        catch (NotSupportedException) { throw new FileNotFoundException("Invalid preview target."); }
        catch (PathTooLongException) { throw new FileNotFoundException("Invalid preview target."); }
    }

    private static void ValidateRuntime()
    {
        var executable = Assembly.GetExecutingAssembly().Location;
        if (!string.Equals(Path.GetFileName(executable), "Pulse.QuickLook.Host.exe", StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("Invalid host filename.");
        var root = Path.GetDirectoryName(Path.GetFullPath(executable));
        RejectReparse(root);
        RejectReparse(executable);
        var plugins = Path.Combine(root, "QuickLook.Plugin");
        if (!Directory.Exists(plugins)) throw new DirectoryNotFoundException();
        RejectReparse(plugins);
        foreach (var directory in Directory.EnumerateDirectories(plugins, "*", SearchOption.AllDirectories))
            RejectReparse(directory);
        foreach (var file in Directory.EnumerateFiles(plugins, "*", SearchOption.AllDirectories))
            RejectReparse(file);
        var fixedBrowser = Path.Combine(root, "WebView2Runtime");
        if (Directory.Exists(fixedBrowser))
        {
            RejectReparse(fixedBrowser);
            if (!File.Exists(Path.Combine(fixedBrowser, "msedgewebview2.exe")))
                throw new InvalidDataException("Invalid packaged browser runtime.");
            Environment.SetEnvironmentVariable("WEBVIEW2_BROWSER_EXECUTABLE_FOLDER", fixedBrowser);
        }
    }

    private static void RejectReparse(string path)
    {
        if ((File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0)
            throw new IOException("Runtime directories cannot be reparse points.");
    }

    private static bool HasWebView2()
    {
        try
        {
            var library = Path.Combine(App.AppPath, "QuickLook.Plugin", "QuickLook.Plugin.HtmlViewer", "Microsoft.Web.WebView2.Core.dll");
            var type = Assembly.LoadFrom(library).GetType("Microsoft.Web.WebView2.Core.CoreWebView2Environment", true);
            var method = type.GetMethod("GetAvailableBrowserVersionString", new[] { typeof(string) });
            return !string.IsNullOrEmpty(method?.Invoke(null, new object[] { null }) as string);
        }
        catch (Exception) { return false; }
    }

    internal static bool CanPreview(IViewer plugin, string path)
    {
        if (plugin == null || plugin.GetType() == PluginManager.GetInstance().DefaultPlugin.GetType())
            return false;
        var name = plugin.GetType().Assembly.GetName().Name;
        var extension = Path.GetExtension(path).ToLowerInvariant();
        var requiresBrowser = name == "QuickLook.Plugin.HtmlViewer" || name == "QuickLook.Plugin.MarkdownViewer" ||
            name == "QuickLook.Plugin.FontViewer" || name == "QuickLook.Plugin.ChmViewer" ||
            (name == "QuickLook.Plugin.ImageViewer" &&
                new[] { ".svg", ".svgz", ".json", ".lottie", ".tgs" }.Contains(extension));
        return !requiresBrowser || hasWebView2;
    }

    internal static void Unsupported(string path)
    {
        Send("ERROR\t" + Id() + "\tUNSUPPORTED");
        ViewWindowManager.GetInstance().ClosePreview();
        QueueState();
    }

    internal static void PreviewFailed(string path)
    {
        Send("ERROR\t" + Id() + "\tVIEWER_FAILED");
        ViewWindowManager.GetInstance().ClosePreview();
        QueueState();
    }

    internal static void Attach(ViewerWindow window)
    {
        new WindowInteropHelper(window).Owner = ownerHandle;
        window.IsVisibleChanged += (_, _) => QueueState();
        window.PropertyChanged += (_, e) => { if (e.PropertyName == nameof(ViewerWindow.Pinned)) QueueState(); };
        window.ContextObject.PropertyChanged += (_, e) => { if (e.PropertyName == nameof(ContextObject.IsBusy)) QueueState(); };
        window.Closed += (_, _) => QueueState();
        window.AddHandler(Keyboard.KeyDownEvent, new KeyEventHandler((_, e) =>
        {
            if (Keyboard.Modifiers != ModifierKeys.None || e.IsRepeat || Keyboard.FocusedElement is TextBoxBase) return;
            if (e.Key == Key.Escape || e.Key == Key.Space)
            {
                e.Handled = true;
                window.Close();
            }
            else if (e.Key == Key.F11)
            {
                e.Handled = true;
                window.ToggleFullscreen();
            }
            else if (!e.Handled && !window.Pinned && (e.Key == Key.Left || e.Key == Key.Right || e.Key == Key.Up || e.Key == Key.Down))
            {
                e.Handled = true;
                Send("NAV\t" + Id() + "\t" + (e.Key == Key.Left || e.Key == Key.Up ? "-1" : "1"));
            }
        }), true);
    }

    internal static void Dispatch(string command, string path, string[] options)
    {
        Application.Current.Dispatcher.BeginInvoke(new Action(() =>
        {
            if (stopping) return;
            var manager = ViewWindowManager.GetInstance();
            switch (command)
            {
                case PipeMessages.Toggle: manager.TogglePreview(path, string.Join(",", options ?? Array.Empty<string>())); break;
                case PipeMessages.Switch: manager.SwitchPreview(path); break;
                case PipeMessages.Invoke: manager.InvokePreview(path); break;
                case PipeMessages.Close: manager.ClosePreview(); break;
                case PipeMessages.RunAndClose: manager.RunAndClosePreview(); break;
                case PipeMessages.Forget: manager.ForgetCurrentWindow(); break;
                case PipeMessages.Fullscreen: manager.ToggleFullscreen(); break;
                case PipeMessages.Quit: Stop(); break;
            }
            QueueState();
        }), DispatcherPriority.Input);
    }

    private static void QueueState()
    {
        if (stopping || Interlocked.Exchange(ref stateQueued, 1) != 0) return;
        Application.Current.Dispatcher.BeginInvoke(new Action(() =>
        {
            Interlocked.Exchange(ref stateQueued, 0);
            if (stopping) return;
            var windows = Application.Current.Windows.OfType<ViewerWindow>().Where(w => w.IsVisible).ToArray();
            var current = windows.FirstOrDefault(w => !w.Pinned);
            var state = "STATE\t" + Id() + "\t" + (current == null ? "0" : "1") + "\t" + windows.Length +
                "\t" + (current != null && current.ContextObject.IsBusy ? "1" : "0");
            if (state != lastState) { lastState = state; Send(state); }
            if (windows.Length == 0)
            {
                if (closeTimer == null)
                {
                    closeTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(250) };
                    closeTimer.Tick += (_, _) =>
                    {
                        closeTimer.Stop();
                        if (!Application.Current.Windows.OfType<ViewerWindow>().Any(w => w.IsVisible)) Stop();
                    };
                }
                closeTimer.Stop();
                closeTimer.Start();
            }
            else closeTimer?.Stop();
        }), DispatcherPriority.ApplicationIdle);
    }

    private static void Stop()
    {
        if (stopping) return;
        stopping = true;
        ownerTimer?.Stop();
        closeTimer?.Stop();
        Application.Current.Shutdown();
    }
}
