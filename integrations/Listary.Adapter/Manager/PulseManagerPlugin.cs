using System;
using System.Threading.Tasks;

namespace Listary.FileAppPlugin.PulseAdapter;

public sealed class PulseManagerPlugin : IFileAppPlugin, IDisposable
{
    private readonly AdapterCore _core = new AdapterCore(false, (window, generation) => new ManagerTab(window, generation));
    public bool IsOpenedFolderProvider => _core.IsOpenedFolderProvider;
    public bool IsQuickSwitchTarget => _core.IsQuickSwitchTarget;
    public bool IsSharedAcrossApplications => _core.IsSharedAcrossApplications;
    public SearchBarType SearchBarType => _core.SearchBarType;
    public Task<bool> Initialize(IFileAppPluginHost host) => _core.Initialize(host);
    public IFileWindow BindFileWindow(IntPtr handle) => _core.BindFileWindow(handle);
    public void Dispose() => _core.Dispose();
}
