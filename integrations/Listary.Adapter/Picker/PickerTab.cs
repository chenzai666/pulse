using System.Threading.Tasks;

namespace Listary.FileAppPlugin.PulseAdapter;

public sealed class PickerTab : IFileTab, IGetFolder, IOpenFolder, IOpenFile, IOpenFolderAndSelectFile
{
    private readonly AdapterWindow _window;
    private readonly uint _generation;
    internal PickerTab(AdapterWindow window, uint generation) { _window = window; _generation = generation; }
    public Task<string> GetCurrentFolder() => _window.Folder(_generation);
    public Task<bool> OpenFolder(string path) => _window.Send(path, _generation, false);
    public Task<bool> OpenFile(string path) => _window.Send(path, _generation, true);
    public Task<bool> OpenFolderAndSelectFile(string path) => _window.Send(path, _generation, true);
}
