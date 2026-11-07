using System;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Threading;
using System.Threading.Tasks;

namespace Listary.FileAppPlugin.PulseAdapter;

internal sealed class AdapterCore : IDisposable
{
    private readonly CancellationTokenSource _lifetime = new CancellationTokenSource();
    private readonly IWindows _windows;
    private readonly Func<AdapterWindow, uint, IFileTab> _createTab;
    private IFileAppPluginHost? _host;
    private string? _executable;
    private int _disposed;
    private readonly bool _picker;
    public bool IsOpenedFolderProvider => !_picker;
    public bool IsQuickSwitchTarget => _picker;
    public bool IsSharedAcrossApplications => false;
    public SearchBarType SearchBarType => _picker ? SearchBarType.Fixed : SearchBarType.Floating;

    internal AdapterCore(bool picker, Func<AdapterWindow, uint, IFileTab> createTab)
        : this(picker, new NativeWindows(), null, createTab) { }
    internal AdapterCore(bool picker, IWindows windows, string? executable,
        Func<AdapterWindow, uint, IFileTab> createTab)
    {
        _picker = picker; _windows = windows; _executable = executable;
        _createTab = createTab;
    }

    public Task<bool> Initialize(IFileAppPluginHost host)
    {
        Diagnostics.Write(_picker, "initialize_enter");
        if (host == null || Volatile.Read(ref _disposed) != 0) return Task.FromResult(false);
        _host = host;
        if (_executable == null) _executable = ReadExecutable();
        Diagnostics.Write(_picker, _executable == null ? "initialize_no_trusted_executable" : "initialize_ready");
        return Task.FromResult(_executable != null);
    }

    private static string? ReadExecutable()
    {
        try
        {
            var folder = Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location);
            if (folder == null) return null;
            var config = Path.Combine(folder, "PulseExecutable.path");
            using var stream = new FileStream(config, FileMode.Open, FileAccess.Read, FileShare.Read);
            if (stream.Length > 65536) return null;
            using var reader = new StreamReader(stream);
            var path = reader.ReadToEnd().Trim();
            if (!PathProtocol.IsValid(path) || path.StartsWith(@"\\", StringComparison.Ordinal)) return null;
            var name = Path.GetFileName(path);
            if ((!string.Equals(name, "pulse.exe", StringComparison.OrdinalIgnoreCase) &&
                 !string.Equals(name, "PulseCustom.exe", StringComparison.OrdinalIgnoreCase)) || !File.Exists(path)) return null;
            return Path.GetFullPath(path);
        }
        catch (IOException) { return null; }
        catch (UnauthorizedAccessException) { return null; }
        catch (ArgumentException) { return null; }
        catch (System.Security.SecurityException) { return null; }
    }

    public IFileWindow BindFileWindow(IntPtr handle)
    {
        var executable = _executable;
        var host = _host;
        if (executable == null || host == null || Volatile.Read(ref _disposed) != 0) return null!;
        var identity = _windows.Capture(handle, _picker, executable);
        if (identity == null) return null!;
        Diagnostics.Write(_picker, "bind_accepted");
        return new AdapterWindow(identity, _windows, host, _lifetime.Token, _createTab);
    }

    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
        _lifetime.Cancel();
        Diagnostics.Write(_picker, "dispose");
        // Bound windows retain the token; don't dispose its source while they can link it.
    }
}

public sealed class AdapterWindow : IFileWindow, IDisposable
{
    private readonly WindowIdentity _identity;
    private readonly IWindows _windows;
    private readonly IFileAppPluginHost _host;
    private readonly CancellationTokenSource _lifetime;
    private readonly CancellationToken _token;
    private readonly Func<AdapterWindow, uint, IFileTab> _createTab;
    private int _disposed;
    public IntPtr Handle => _identity.Handle;

    internal AdapterWindow(WindowIdentity identity, IWindows windows, IFileAppPluginHost host, CancellationToken lifetime,
        Func<AdapterWindow, uint, IFileTab> createTab)
    {
        _identity = identity; _windows = windows; _host = host;
        _lifetime = CancellationTokenSource.CreateLinkedTokenSource(lifetime);
        _token = _lifetime.Token;
        _createTab = createTab;
    }

    public Task<IFileTab> GetCurrentTab()
    {
        if (!Valid()) return Task.FromResult<IFileTab>(null!);
        if (_identity.Picker) return Task.FromResult(_createTab(this, 0));
        var generation = _windows.Generation(_identity);
        var folder = _windows.ReadFolder(_identity);
        if (generation == 0 || folder == null || _windows.Generation(_identity) != generation || !Valid())
            return Task.FromResult<IFileTab>(null!);
        return Task.FromResult(_createTab(this, generation));
    }

    internal bool Valid() => !_token.IsCancellationRequested &&
        _identity.Same(_windows.Capture(Handle, _identity.Picker, _identity.Executable));

    internal bool ValidTab(uint generation) => Valid() && (_identity.Picker || _windows.Generation(_identity) == generation);

    internal Task<string> Folder(uint generation)
    {
        if (!ValidTab(generation)) return Task.FromResult<string>(null!);
        var path = _windows.ReadFolder(_identity);
        return Task.FromResult(PathProtocol.IsValid(path) && ValidTab(generation) ? path! : null!);
    }

    internal Task<bool> Send(string path, uint generation, bool file)
    {
        var data = PathProtocol.Encode(path, _identity.Picker, generation);
        if (data == null || (!_identity.Picker && file) || !ValidTab(generation)) return Task.FromResult(false);
        var queued = Stopwatch.GetTimestamp();
        return Task.Run(() =>
        {
            // A queued callback must not navigate a window long after the user left it.
            if (Stopwatch.GetTimestamp() - queued >= Stopwatch.Frequency || !ValidTab(generation)) return false;
            var sender = _identity.Picker ? new IntPtr(file ? 2 : 1) : _windows.Sender();
            if (sender == IntPtr.Zero || !ValidTab(generation)) return false;
            try
            {
                var result = _host.SendCopyData(Handle, sender,
                    new IntPtr(_identity.Picker ? PathProtocol.PickerTag : PathProtocol.ManagerTag),
                    data, checked((uint)data.Length), 750);
                return result != IntPtr.Zero && Valid();
            }
            catch (System.ComponentModel.Win32Exception) { return false; }
            catch (InvalidOperationException) { return false; }
            catch (System.Runtime.InteropServices.ExternalException) { return false; }
        });
    }

    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
        _lifetime.Cancel();
        _lifetime.Dispose();
    }
}
