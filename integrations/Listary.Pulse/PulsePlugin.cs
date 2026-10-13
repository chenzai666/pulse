using System;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Threading;
using System.Threading.Tasks;

namespace Listary.FileAppPlugin.Pulse;

public sealed class PulsePlugin : IFileAppPlugin, IOpenFolder, IOpenFolderAndSelectFile, IDisposable
{
    private readonly CancellationTokenSource _lifetime = new CancellationTokenSource();
    private PulseWindowIdentity? _lastWindow;
    private string? _configuredExecutable;
    private int _disposed;

    public bool IsOpenedFolderProvider => true;
    public bool IsQuickSwitchTarget => false;
    public bool IsSharedAcrossApplications => false;
    public SearchBarType SearchBarType => SearchBarType.Floating;

    public Task<bool> Initialize(IFileAppPluginHost host)
    {
        // Deployment writes this optional, local-only path beside the plugin.
        // No Listary user settings or registry keys are read or changed.
        try
        {
            var folder = Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location);
            if (folder != null)
            {
                var config = Path.Combine(folder, "PulseExecutable.path");
                if (File.Exists(config) && new FileInfo(config).Length <= 65536)
                {
                    var executable = File.ReadAllText(config).Trim();
                    if (ValidExecutablePath(executable)) _configuredExecutable = executable;
                }
            }
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
        return Task.FromResult(Volatile.Read(ref _disposed) == 0);
    }

    public IFileWindow BindFileWindow(IntPtr hWnd)
    {
        if (Volatile.Read(ref _disposed) != 0) return null!;
        var window = PulseWindowIdentity.Capture(hWnd);
        if (window == null) return null!;
        Interlocked.Exchange(ref _lastWindow, window);
        return new PulseWindow(window, _lifetime.Token);
    }

    // For a result selected from Listary's global launcher, reuse a running
    // custom window and create a tab. A bound file tab uses its own scope.
    public Task<bool> OpenFolder(string path) => OpenGlobal(path, false);
    public Task<bool> OpenFolderAndSelectFile(string path) => OpenGlobal(path, true);

    private async Task<bool> OpenGlobal(string path, bool select)
    {
        if (Volatile.Read(ref _disposed) != 0 || !PulseBridgeClient.IsAbsoluteFileSystemPath(path)) return false;
        var window = Volatile.Read(ref _lastWindow);
        if (window == null || !window.StillValid())
        {
            PulseWindowIdentity? visible = null;
            PulseWindowIdentity? hidden = null;
            NativeMethods.EnumWindows((handle, _) =>
            {
                var candidate = PulseWindowIdentity.Capture(handle);
                if (candidate == null) return true;
                if (NativeMethods.IsWindowVisible(handle)) { visible = candidate; return false; }
                hidden ??= candidate;
                return true;
            }, IntPtr.Zero);
            window = visible ?? hidden;
        }
        if (window != null)
        {
            Interlocked.Exchange(ref _lastWindow, window);
            var response = await PulseBridgeClient.Send(window,
                select ? PulseOperation.NewTabSelectItem : PulseOperation.NewTabFolder,
                0, path, _lifetime.Token).ConfigureAwait(false);
            return response?.Accepted == true;
        }
        var executable = _configuredExecutable;
        if (executable == null || _lifetime.IsCancellationRequested) return false;
        // ProcessStartInfo bypasses cmd/PowerShell and uses one safely quoted
        // absolute path argument. Starting a process acknowledges launch only.
        return await Task.Run(() =>
        {
            if (_lifetime.IsCancellationRequested) return false;
            try
            {
                using var process = Process.Start(new ProcessStartInfo
                {
                    FileName = executable,
                    Arguments = QuoteArgument(path),
                    UseShellExecute = false,
                    WorkingDirectory = Path.GetDirectoryName(executable)!
                });
                return process != null;
            }
            catch (System.ComponentModel.Win32Exception) { return false; }
            catch (IOException) { return false; }
            catch (UnauthorizedAccessException) { return false; }
        }).ConfigureAwait(false);
    }

    private static bool ValidExecutablePath(string path)
    {
        if (!PulseBridgeClient.IsAbsoluteFileSystemPath(path)) return false;
        var ordinary = path.StartsWith(@"\\?\", StringComparison.Ordinal) ? path.Substring(4) : path;
        if (ordinary.StartsWith(@"\\", StringComparison.Ordinal)) return false;
        var name = Path.GetFileName(path);
        return (string.Equals(name, "pulse.exe", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(name, "PulseCustom.exe", StringComparison.OrdinalIgnoreCase)) && File.Exists(path);
    }

    private static string QuoteArgument(string path)
    {
        var trailingSlashes = 0;
        for (var index = path.Length - 1; index >= 0 && path[index] == '\\'; index--) trailingSlashes++;
        return "\"" + path + new string('\\', trailingSlashes) + "\"";
    }

    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
        _lifetime.Cancel();
        // Existing windows/tabs may still hold the token while unloading;
        // disposing the source here would race CreateLinkedTokenSource.
        Interlocked.Exchange(ref _lastWindow, null);
    }
}

public sealed class PulseWindow : IFileWindow, IOpenFolder, IOpenFolderAndSelectFile
{
    private readonly PulseWindowIdentity _window;
    private readonly CancellationToken _lifetime;
    public IntPtr Handle => _window.Handle;

    internal PulseWindow(PulseWindowIdentity window, CancellationToken lifetime)
    {
        _window = window;
        _lifetime = lifetime;
    }

    public async Task<IFileTab> GetCurrentTab()
    {
        var response = await PulseBridgeClient.Send(_window, PulseOperation.CurrentTab, 0, null, _lifetime).ConfigureAwait(false);
        if (response == null || response.Tab == 0 ||
            (response.Status != PulseStatus.Ok && response.Status != PulseStatus.NoFolder)) return null!;
        return new PulseTab(_window, response.Tab, _lifetime);
    }

    public async Task<bool> OpenFolder(string path)
    {
        var response = await PulseBridgeClient.Send(_window, PulseOperation.NewTabFolder, 0, path, _lifetime).ConfigureAwait(false);
        return response?.Accepted == true;
    }

    public async Task<bool> OpenFolderAndSelectFile(string path)
    {
        var response = await PulseBridgeClient.Send(_window, PulseOperation.NewTabSelectItem, 0, path, _lifetime).ConfigureAwait(false);
        return response?.Accepted == true;
    }
}

public sealed class PulseTab : IFileTab, IGetFolder, IOpenFolder, IOpenFolderAndSelectFile
{
    private readonly PulseWindowIdentity _window;
    private long _tab;
    private readonly CancellationToken _lifetime;

    internal PulseTab(PulseWindowIdentity window, ulong tab, CancellationToken lifetime)
    {
        _window = window;
        _tab = unchecked((long)tab);
        _lifetime = lifetime;
    }

    public async Task<string> GetCurrentFolder()
    {
        var response = await PulseBridgeClient.Send(_window, PulseOperation.CurrentFolder,
            unchecked((ulong)Interlocked.Read(ref _tab)), null, _lifetime).ConfigureAwait(false);
        return response?.Accepted == true && response.Folder.Length != 0 ? response.Folder : null!;
    }

    public async Task<bool> OpenFolder(string path)
    {
        var response = await PulseBridgeClient.Send(_window, PulseOperation.OpenFolder,
            unchecked((ulong)Interlocked.Read(ref _tab)), path, _lifetime).ConfigureAwait(false);
        if (response?.Accepted == true) Interlocked.Exchange(ref _tab, unchecked((long)response.Tab));
        return response?.Accepted == true;
    }

    public async Task<bool> OpenFolderAndSelectFile(string path)
    {
        var response = await PulseBridgeClient.Send(_window, PulseOperation.SelectItem,
            unchecked((ulong)Interlocked.Read(ref _tab)), path, _lifetime).ConfigureAwait(false);
        if (response?.Accepted == true) Interlocked.Exchange(ref _tab, unchecked((long)response.Tab));
        return response?.Accepted == true;
    }
}
