using System.Threading.Tasks;

namespace Listary.FileAppPlugin.PulseAdapter;

// The public SDK loader discovers capabilities from exported tab types.
public sealed class ManagerTab : IFileTab, IGetFolder, IOpenFolder
{
    private readonly AdapterWindow _window;
    private readonly uint _generation;
    internal ManagerTab(AdapterWindow window, uint generation) { _window = window; _generation = generation; }
    public Task<string> GetCurrentFolder() => _window.Folder(_generation);
    public Task<bool> OpenFolder(string path) => _window.Send(path, _generation, false);
}
