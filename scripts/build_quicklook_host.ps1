[CmdletBinding()]
param(
    [string]$BuildDir = 'build-custom',
    [string]$ArchivePath,
    [string]$SourceArchivePath,
    [switch]$PrepareOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = [IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent))
$integration = Join-Path $repo 'integrations/QuickLook.FullHost'
$upstreamInfo = Get-Content -LiteralPath (Join-Path $integration 'upstream.json') -Raw | ConvertFrom-Json
$repoPrefix = $repo.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
$buildPath = if ([IO.Path]::IsPathRooted($BuildDir)) {
    $BuildDir
} else {
    Join-Path $repo $BuildDir
}
$buildRoot = [IO.Path]::GetFullPath($buildPath)
if (!$buildRoot.StartsWith($repoPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'BuildDir 必须位于当前仓库内的独立构建目录。'
}
$runtime = [IO.Path]::GetFullPath((Join-Path $buildRoot 'quicklook'))
if (![IO.Path]::GetDirectoryName($runtime).Equals($buildRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'QuickLook 输出目录越界。'
}
if (Test-Path -LiteralPath $runtime) {
    if (Get-ChildItem -LiteralPath $runtime -Force | Select-Object -First 1) {
        throw 'QuickLook 输出目录已有内容，请使用新的空构建目录。'
    }
    if ((Get-Item -LiteralPath $runtime).Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw 'QuickLook 输出目录不能是重解析点。'
    }
}
$cache = Join-Path $repo 'out/quicklook-dependencies'
$work = Join-Path $repo ('out/quicklook-host-work/' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $cache, $work, $runtime | Out-Null

function Get-VerifiedArchive([string]$Path, [string]$DefaultName, [string]$Url, [string]$Sha256) {
    if (!$Path) { $Path = Join-Path $cache $DefaultName }
    $Path = [IO.Path]::GetFullPath($Path)
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) {
        if (![IO.Path]::GetDirectoryName($Path).Equals($cache, [StringComparison]::OrdinalIgnoreCase)) {
            throw '显式指定的依赖归档不存在。'
        }
        Invoke-WebRequest -Uri $Url -OutFile $Path
    }
    if ((Get-Item -LiteralPath $Path).Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw '依赖归档不能是重解析点。'
    }
    if (!(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.Equals($Sha256, [StringComparison]::OrdinalIgnoreCase)) {
        throw ('QuickLook 固定归档 SHA256 校验失败：' + [IO.Path]::GetFileName($Path))
    }
    return $Path
}

function Expand-SafeZip([string]$Archive, [string]$Destination) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $root = [IO.Path]::GetFullPath($Destination).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $zip = [IO.Compression.ZipFile]::OpenRead($Archive)
    try {
        $totalBytes = [int64]0
        if ($zip.Entries.Count -gt 20000) { throw '依赖归档包含过多条目。' }
        foreach ($entry in $zip.Entries) {
            $relative = $entry.FullName.Replace('/', [IO.Path]::DirectorySeparatorChar)
            if (!$relative) { continue }
            if ([IO.Path]::IsPathRooted($relative) -or $relative.Contains(':')) { throw '归档条目含无效路径。' }
            $target = [IO.Path]::GetFullPath((Join-Path $Destination $relative))
            if (!$target.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) { throw '归档条目路径越界。' }
            if ((($entry.ExternalAttributes -shr 16) -band 0xF000) -eq 0xA000) { throw '归档不能包含符号链接。' }
            $totalBytes += $entry.Length
            if ($totalBytes -gt 4GB) { throw '依赖归档展开大小超出限制。' }
            if ($entry.FullName.EndsWith('/') -or $entry.FullName.EndsWith('\')) {
                [IO.Directory]::CreateDirectory($target) | Out-Null
            } else {
                [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target)) | Out-Null
                [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target, $false)
            }
        }
    } finally { $zip.Dispose() }
    if (Get-ChildItem -LiteralPath $Destination -Recurse -Force -Attributes ReparsePoint | Select-Object -First 1) {
        throw '展开的依赖目录包含重解析点。'
    }
}

$ArchivePath = Get-VerifiedArchive $ArchivePath 'QuickLook-4.5.0.zip' $upstreamInfo.runtime_url $upstreamInfo.runtime_sha256
$SourceArchivePath = Get-VerifiedArchive $SourceArchivePath 'QuickLook-4.5.0-source.zip' $upstreamInfo.source_url $upstreamInfo.source_sha256
$sourceContainer = Join-Path $work 'source'
New-Item -ItemType Directory -Path $sourceContainer | Out-Null
Expand-SafeZip $SourceArchivePath $sourceContainer
Expand-SafeZip $ArchivePath $runtime
$source = Join-Path $sourceContainer ('QuickLook-' + $upstreamInfo.source_commit)
if (!(Test-Path -LiteralPath (Join-Path $source 'QuickLook/QuickLook.csproj')) -or
    !(Test-Path -LiteralPath (Join-Path $source 'LICENSE-GPL.txt'))) {
    throw 'QuickLook 固定源码目录不完整。'
}
$pluginRoot = Join-Path $runtime 'QuickLook.Plugin'
if (!(Test-Path -LiteralPath $pluginRoot -PathType Container)) { throw 'QuickLook 发行包缺少完整插件目录。' }
$originalPlugins = @(Get-ChildItem -LiteralPath $pluginRoot -File -Recurse | Sort-Object FullName | ForEach-Object {
    [pscustomobject]@{
        path = $_.FullName.Substring($pluginRoot.Length + 1).Replace('\', '/')
        sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        bytes = $_.Length
    }
})
$pluginNames = @(Get-ChildItem -LiteralPath $pluginRoot -Directory | Select-Object -ExpandProperty Name | Sort-Object)
foreach ($required in @('ArchiveViewer','CsvViewer','FontViewer','HtmlViewer','ImageViewer','MarkdownViewer','MediaInfoViewer','PDFViewer','TextViewer','VideoViewer')) {
    if ($pluginNames -notcontains ('QuickLook.Plugin.' + $required)) { throw ('QuickLook 发行包缺少预期插件：' + $required) }
}

$utf8 = [Text.UTF8Encoding]::new($false)
function Update-Source([string]$RelativePath, [scriptblock]$Transform) {
    $target = Join-Path $source $RelativePath
    $original = [IO.File]::ReadAllText($target)
    $updated = & $Transform $original
    if ($updated -ceq $original) { throw ('QuickLook 适配点未匹配：' + $RelativePath) }
    [IO.File]::WriteAllText($target, ($updated -replace '\r?\n', "`r`n"), $utf8)
}

Copy-Item -LiteralPath (Join-Path $integration 'Directory.Build.props'), (Join-Path $integration 'Directory.Packages.props') -Destination $source
$version = [Reflection.AssemblyName]::GetAssemblyName((Join-Path $runtime 'QuickLook.Common.dll')).Version
[IO.File]::WriteAllText((Join-Path $source 'GitVersion.cs'), ('[assembly: System.Reflection.AssemblyVersion("{0}")]' -f $version), $utf8)

Update-Source 'QuickLook/App.xaml.cs' {
    param($text)
    $start = $text.IndexOf('    protected override void OnStartup(')
    $end = $text.IndexOf('    protected override void OnExit(', $start)
    if ($start -lt 0 -or $end -le $start) { throw '找不到 QuickLook 启动边界。' }
    $text.Substring(0, $start) + @'
    protected override void OnStartup(StartupEventArgs e)
    {
        _cleanExit = false;
        base.OnStartup(e);
        PulseBridge.Start(this, e.Args);
    }

'@ + $text.Substring($end)
}
Update-Source 'QuickLook/ViewWindowManager.cs' {
    param($text)
    foreach ($point in @('NativeMethods.QuickLook.GetCurrentSelection()', 'FocusMonitor.GetInstance().Start();',
        'FocusMonitor.GetInstance().Stop();', '_viewerWindow = new ViewerWindow();',
        'var matchedPlugin = PluginManager.GetInstance().FindMatch(path);')) {
        if (!$text.Contains($point)) { throw ('找不到窗口适配点：' + $point) }
    }
    $text = $text.Replace('NativeMethods.QuickLook.GetCurrentSelection()', 'PulseBridge.SelectedPath').
        Replace('FocusMonitor.GetInstance().Start();', '// Pulse supplies its own selection.').
        Replace('FocusMonitor.GetInstance().Stop();', '// No external focus monitor in the Pulse host.').
        Replace('_viewerWindow = new ViewerWindow();', "_viewerWindow = new ViewerWindow();`r`n        PulseBridge.Attach(_viewerWindow);").
        Replace('var matchedPlugin = PluginManager.GetInstance().FindMatch(path);', @'
var matchedPlugin = PluginManager.GetInstance().FindMatch(path);
        if (!PulseBridge.CanPreview(matchedPlugin, path))
        {
            PulseBridge.Unsupported(path);
            return;
        }
'@)
    $start = $text.IndexOf('    private void CurrentPluginFailed(')
    $end = $text.IndexOf('    private void InitNewViewerWindow(', $start)
    if ($start -lt 0 -or $end -le $start) { throw '找不到预览失败适配点。' }
    $text.Substring(0, $start) + @'
    private void CurrentPluginFailed(string path, ExceptionDispatchInfo e)
    {
        PulseBridge.PreviewFailed(path);
    }

'@ + $text.Substring($end)
}
Update-Source 'QuickLook/PipeServerManager.cs' {
    param($text)
    $start = $text.IndexOf('    public static void SendMessage(')
    $end = $text.IndexOf('    private bool MessageReceived(', $start)
    if ($start -lt 0 -or $end -le $start) { throw '找不到原版消息边界。' }
    $text.Substring(0, $start) + @'
    public static void SendMessage(string pipeMessage, string path = null, string[] options = null)
        => PulseBridge.Dispatch(pipeMessage, path, options);

'@ + $text.Substring($end)
}
Update-Source 'QuickLook/PluginManager.cs' {
    param($text)
    $text = $text.Replace('LoadPlugins(App.UserPluginPath);', '// Only the verified, packaged plugins are loaded.');
    $start = $text.IndexOf('        // If any plugins failed to load, show a message box with the details')
    $end = $text.IndexOf('    private void InitLoadedPlugins()', $start)
    if ($start -lt 0 -or $end -le $start) { throw '找不到插件失败边界。' }
    $text.Substring(0, $start) + @'
        if (failedPlugins.Any()) Console.Error.WriteLine("PLUGIN_LOAD_FAILED");
    }

'@ + $text.Substring($end)
}
Update-Source 'QuickLook.Common/Helpers/SettingHelper.cs' {
    param($text)
    $start = $text.IndexOf('    public static readonly string LocalDataPath =')
    $end = $text.IndexOf('    private static readonly Dictionary', $start)
    if ($start -lt 0 -or $end -le $start) { throw '找不到配置目录边界。' }
    $text = $text.Substring(0, $start) + @'
    public static readonly string LocalDataPath = GetPulseDataPath();

    private static string GetPulseDataPath()
    {
        var local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        if (string.IsNullOrEmpty(local) || !Path.IsPathRooted(local))
            throw new IOException("User profile is unavailable.");
        var profile = Path.GetFullPath(Path.Combine(local, "PulseCustom"));
        var data = Path.Combine(profile, "QuickLook");
        foreach (var directory in new[] { profile, data })
            if (Directory.Exists(directory) && (File.GetAttributes(directory) & FileAttributes.ReparsePoint) != 0)
                throw new IOException("Preview profile cannot be a reparse point.");
        Directory.CreateDirectory(data);
        return data;
    }

    // Atomic replacement prevents different Pulse windows from reading half
    // written XML. The mutex also serializes first-run creation and saves.
    private static void SavePulseConfig(XmlDocument doc, string file)
    {
        file = Path.GetFullPath(file);
        if (!file.StartsWith(LocalDataPath.TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar,
            StringComparison.OrdinalIgnoreCase)) throw new IOException("Invalid configuration path.");
        using (var mutex = new System.Threading.Mutex(false, @"Local\PulseCustom.QuickLook.Settings"))
        {
            var locked = false;
            try
            {
                try { locked = mutex.WaitOne(TimeSpan.FromSeconds(3)); }
                catch (System.Threading.AbandonedMutexException) { locked = true; }
                if (!locked) throw new IOException("Preview settings are busy.");
                var temporary = file + "." + Guid.NewGuid().ToString("N") + ".tmp";
                try
                {
                    doc.Save(temporary);
                    if (File.Exists(file)) File.Replace(temporary, file, null);
                    else File.Move(temporary, file);
                }
                finally { if (File.Exists(temporary)) File.Delete(temporary); }
            }
            finally { if (locked) mutex.ReleaseMutex(); }
        }
    }

'@ + $text.Substring($end)
    $text = $text.Replace('doc.Save(new Uri(doc.BaseURI).LocalPath);', 'SavePulseConfig(doc, new Uri(doc.BaseURI).LocalPath);')
    $text = [regex]::Replace($text, '(?s)    private static void CreateNewConfig\(string file\)\s*\{.*?\r?\n    \}', @'
    private static void CreateNewConfig(string file)
    {
        var doc = new XmlDocument { XmlResolver = null };
        doc.LoadXml("<Settings />");
        SavePulseConfig(doc, file);
    }
'@)
    $text
}
Copy-Item -LiteralPath (Join-Path $integration 'PulseBridge.cs') -Destination (Join-Path $source 'QuickLook/PulseBridge.cs')
Update-Source 'QuickLook/QuickLook.csproj' {
    param($text)
    $text = [regex]::Replace($text, '(?s)<Reference Include="(?:windows|Windows.Foundation.FoundationContract|Windows.Foundation.UniversalApiContract)">.*?</Reference>', '')
    $text = $text.Replace('<TargetFramework>net462</TargetFramework>', '<TargetFramework>net48</TargetFramework>')
    [regex]::Replace($text, '</Project>\s*$', @'
    <PropertyGroup>
        <GenerateResourceUsePreserializedResources>true</GenerateResourceUsePreserializedResources>
        <CodeAnalysisRuleSet></CodeAnalysisRuleSet>
    </PropertyGroup>
    <ItemGroup>
        <PackageReference Include="System.Resources.Extensions" Version="8.0.0" />
        <PackageReference Include="Microsoft.NETFramework.ReferenceAssemblies" Version="1.0.3" PrivateAssets="all" />
        <PackageReference Include="Microsoft.Windows.SDK.Contracts" Version="10.0.26100.1" />
    </ItemGroup>
</Project>
'@)
}
Update-Source 'QuickLook.Common/QuickLook.Common.csproj' {
    param($text)
    $text = $text.Replace('<TargetFramework>net462</TargetFramework>', '<TargetFramework>net48</TargetFramework>')
    [regex]::Replace($text, '</Project>\s*$', @'
    <ItemGroup>
        <PackageReference Include="Microsoft.NETFramework.ReferenceAssemblies" Version="1.0.3" PrivateAssets="all" />
    </ItemGroup>
</Project>
'@)
}

if ($PrepareOnly) {
    Write-Output ('QuickLook 固定源码和完整插件已校验并完成适配；尚未构建宿主。源码：' + $source)
    Write-Output ('原版插件目录数：' + $pluginNames.Count)
    return
}
if (!(Get-Command dotnet -ErrorAction SilentlyContinue)) { throw '构建宿主需要 .NET SDK；建议在 GitHub Actions Windows runner 上运行。' }
# Debug avoids Costura's Release-only dependency repacking. Optimization is
# enabled and debug symbols disabled, preserving the proven full-host layout.
& dotnet build (Join-Path $source 'QuickLook/QuickLook.csproj') -c Debug '-p:Platform=x64' '-p:PreBuildEvent=' '-p:Optimize=true' '-p:DebugSymbols=false' '-p:DebugType=None' -v:minimal --nologo
if ($LASTEXITCODE -ne 0) { throw 'Pulse 完整 QuickLook 宿主构建失败。' }
$built = Join-Path $source 'Build/Debug'
Get-ChildItem -LiteralPath $built -Force | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $runtime -Recurse -Force
}
Copy-Item -LiteralPath (Join-Path $runtime 'QuickLook.exe') -Destination (Join-Path $runtime 'Pulse.QuickLook.Host.exe') -Force
Copy-Item -LiteralPath (Join-Path $runtime 'QuickLook.exe.config') -Destination (Join-Path $runtime 'Pulse.QuickLook.Host.exe.config') -Force
foreach ($name in @('QuickLook.exe', 'QuickLook.exe.config', 'portable.lock')) {
    $target = [IO.Path]::GetFullPath((Join-Path $runtime $name))
    if (![IO.Path]::GetDirectoryName($target).Equals($runtime, [StringComparison]::OrdinalIgnoreCase)) {
        throw '生成目录清理路径越界。'
    }
    if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Force }
}

foreach ($plugin in $originalPlugins) {
    $file = Join-Path $pluginRoot $plugin.path
    if (!(Test-Path -LiteralPath $file -PathType Leaf) -or
        !(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.Equals($plugin.sha256, [StringComparison]::OrdinalIgnoreCase)) {
        throw ('完整插件文件校验失败：' + $plugin.path)
    }
}
$licenses = Join-Path $runtime 'licenses'
$sources = Join-Path $runtime 'sources'
New-Item -ItemType Directory -Force -Path $licenses, $sources | Out-Null
Copy-Item -LiteralPath (Join-Path $source 'LICENSE-GPL.txt') -Destination $licenses
Copy-Item -LiteralPath (Join-Path $integration 'README.md') -Destination (Join-Path $runtime 'QuickLook集成说明.md')
Copy-Item -LiteralPath $SourceArchivePath -Destination (Join-Path $sources 'QuickLook-4.5.0-upstream-source.zip')
$buildInstructions = Join-Path $source 'Pulse-integration'
New-Item -ItemType Directory -Path $buildInstructions | Out-Null
Get-ChildItem -LiteralPath $integration -File | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $buildInstructions
}
Copy-Item -LiteralPath $PSCommandPath -Destination $buildInstructions
Compress-Archive -LiteralPath $source -DestinationPath (Join-Path $sources 'QuickLook-4.5.0-Pulse-host-source.zip') -CompressionLevel Optimal

$files = @(Get-ChildItem -LiteralPath $runtime -File -Recurse | Sort-Object FullName | ForEach-Object {
    [pscustomobject]@{
        path = $_.FullName.Substring($runtime.Length + 1).Replace('\', '/')
        sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        bytes = $_.Length
    }
})
$manifest = [ordered]@{
    schema = 1
    component = 'Pulse.QuickLook.FullHost'
    version = $upstreamInfo.version
    source_commit = $upstreamInfo.source_commit
    runtime_archive_sha256 = $upstreamInfo.runtime_sha256
    source_archive_sha256 = $upstreamInfo.source_sha256
    host_filename = 'Pulse.QuickLook.Host.exe'
    plugins = $pluginNames
    plugin_file_count = $originalPlugins.Count
    original_plugins = $originalPlugins
    files = $files
}
[IO.File]::WriteAllText((Join-Path $runtime 'runtime-manifest.json'), ($manifest | ConvertTo-Json -Depth 6), $utf8)
Write-Output ('Pulse 完整 QuickLook 宿主已生成：' + $runtime)
Write-Output ('已保留并校验 {0} 个插件目录、{1} 个原版插件文件；GPL、原始源码和改后宿主源码已随包保存。' -f $pluginNames.Count, $originalPlugins.Count)
