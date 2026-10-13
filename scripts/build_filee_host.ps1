[CmdletBinding()]
param(
    [string]$BuildDir = 'build-custom',
    [string]$SourceArchivePath,
    [string]$FfmpegRuntimePath,
    [switch]$PrepareOnly
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
Set-StrictMode -Version Latest
$repo = [IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent))
$integration = Join-Path $repo 'integrations/Filee.Host'
$upstream = Get-Content -LiteralPath (Join-Path $integration 'upstream.json') -Encoding UTF8 -Raw | ConvertFrom-Json
$utf8 = [Text.UTF8Encoding]::new($false)

function Assert-NoReparse([string]$Path) {
    for ($current = [IO.Path]::GetFullPath($Path); $current; $current = [IO.Path]::GetDirectoryName($current)) {
        if ((Test-Path -LiteralPath $current) -and ((Get-Item -LiteralPath $current -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw ('路径链包含重解析点：' + $current)
        }
    }
}

function Assert-Within([string]$Path, [string]$Root) {
    $full = [IO.Path]::GetFullPath($Path)
    $prefix = [IO.Path]::GetFullPath($Root).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if (!$full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw ('路径越界：' + $full) }
    Assert-NoReparse $full
}

$buildPath = if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $repo $BuildDir }
$buildRoot = [IO.Path]::GetFullPath($buildPath)
Assert-Within $buildRoot $repo
$runtime = [IO.Path]::GetFullPath((Join-Path $buildRoot 'conversion'))
Assert-Within $runtime $buildRoot
if (Test-Path -LiteralPath $runtime) {
    if (!(Test-Path -LiteralPath $runtime -PathType Container) -or (Get-ChildItem -LiteralPath $runtime -Force | Select-Object -First 1)) {
        throw 'conversion 输出必须是空目录；请为本次构建选择新的 BuildDir。'
    }
}
$cache = Join-Path $repo 'out/filee-dependencies'
$work = Join-Path $repo ('out/filee-host-work/' + [Guid]::NewGuid().ToString('N'))
Assert-Within $cache $repo
Assert-Within $work $repo
New-Item -ItemType Directory -Force -Path $cache, $work | Out-Null

function Get-VerifiedFile([string]$Path, [string]$Name, [string]$Url, [string]$Sha256, [int64]$Bytes = 0) {
    if (!$Path) { $Path = Join-Path $cache $Name }
    $Path = [IO.Path]::GetFullPath($Path)
    Assert-NoReparse $Path
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) {
        if (![IO.Path]::GetDirectoryName($Path).Equals($cache, [StringComparison]::OrdinalIgnoreCase)) { throw '显式指定的源码归档不存在。' }
        $temporary = $Path + '.' + [Guid]::NewGuid().ToString('N') + '.download'
        try {
            Invoke-WebRequest -Uri $Url -OutFile $temporary -UseBasicParsing
            if (($Bytes -gt 0 -and (Get-Item -LiteralPath $temporary).Length -ne $Bytes) -or
                !(Get-FileHash -LiteralPath $temporary -Algorithm SHA256).Hash.Equals($Sha256, [StringComparison]::OrdinalIgnoreCase)) {
                throw ('固定下载 SHA256 或大小校验失败：' + $Name)
            }
            [IO.File]::Move($temporary, $Path)
        } finally { if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force } }
    }
    if (($Bytes -gt 0 -and (Get-Item -LiteralPath $Path).Length -ne $Bytes) -or
        !(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.Equals($Sha256, [StringComparison]::OrdinalIgnoreCase)) {
        throw ('固定归档 SHA256 或大小校验失败：' + $Name)
    }
    return $Path
}

function Expand-SafeZip([string]$Archive, [string]$Destination) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    Assert-Within $Destination $work
    New-Item -ItemType Directory -Path $Destination | Out-Null
    $zip = [IO.Compression.ZipFile]::OpenRead($Archive)
    try {
        if ($zip.Entries.Count -gt 50000) { throw '归档包含过多条目。' }
        $totalBytes = [int64]0
        $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        foreach ($entry in $zip.Entries) {
            $relative = $entry.FullName.Replace('/', [IO.Path]::DirectorySeparatorChar)
            if (!$relative) { continue }
            if ([IO.Path]::IsPathRooted($relative) -or $relative.Contains(':') -or
                ($relative -split '[\\/]' | Where-Object { $_ -eq '..' -or ($_ -and $_ -notin @('.') -and ($_ -match '[<>"|?*\x00-\x1f]' -or $_.EndsWith(' ') -or $_.EndsWith('.'))) })) {
                throw '归档条目含无效路径。'
            }
            $target = [IO.Path]::GetFullPath((Join-Path $Destination $relative))
            Assert-Within $target $Destination
            if (!$seen.Add($target.TrimEnd('\', '/'))) { throw '归档含重复路径。' }
            if ((($entry.ExternalAttributes -shr 16) -band 0xF000) -eq 0xA000) { throw '归档不能包含符号链接。' }
            $totalBytes += $entry.Length
            if ($totalBytes -gt 2GB) { throw '归档展开大小超出 2 GiB 限制。' }
            if ($entry.FullName.EndsWith('/') -or $entry.FullName.EndsWith('\')) {
                [IO.Directory]::CreateDirectory($target) | Out-Null
            } else {
                [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target)) | Out-Null
                [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target, $false)
            }
        }
    } finally { $zip.Dispose() }
}

function Copy-Contents([string]$From, [string]$To) {
    Assert-NoReparse $From
    if (Get-ChildItem -LiteralPath $From -Recurse -Force -Attributes ReparsePoint | Select-Object -First 1) { throw '复制源包含重解析点。' }
    Assert-Within $To $runtime
    New-Item -ItemType Directory -Force -Path $To | Out-Null
    Get-ChildItem -LiteralPath $From -Force | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $To -Recurse }
}

$SourceArchivePath = Get-VerifiedFile $SourceArchivePath 'Filee-1.10.2-source.zip' $upstream.source_url $upstream.source_sha256
$sourceContainer = Join-Path $work 'source'
Expand-SafeZip $SourceArchivePath $sourceContainer
$source = Join-Path $sourceContainer ('Filee-' + $upstream.source_commit)
if (!(Test-Path -LiteralPath (Join-Path $source 'src/Filee.Engines/Filee.Engines.csproj')) -or
    !(Test-Path -LiteralPath (Join-Path $source 'LICENSE'))) { throw 'Filee 固定源码目录不完整。' }

function Update-Source([string]$RelativePath, [scriptblock]$Transform) {
    $path = Join-Path $source $RelativePath
    Assert-Within $path $source
    $original = [IO.File]::ReadAllText($path)
    $updated = & $Transform $original
    if ($updated -ceq $original) { throw ('Filee 安全适配点未匹配：' + $RelativePath) }
    [IO.File]::WriteAllText($path, ($updated -replace '\r?\n', "`r`n"), $utf8)
}

Update-Source 'src/Filee.Core/Conversion/JobQueue.cs' {
    param($text)
    $text = $text.Replace('MaxDegreeOfParallelism = Environment.ProcessorCount', 'MaxDegreeOfParallelism = Math.Min(Environment.ProcessorCount, 2)')
    $text.Replace('var max = converter.MaxParallelism <= 0 ? Environment.ProcessorCount : converter.MaxParallelism;',
        'var max = Math.Min(converter.MaxParallelism <= 0 ? Environment.ProcessorCount : converter.MaxParallelism, 2);')
}
Update-Source 'src/Filee.Engines/Archives/ArchiveSafety.cs' {
    param($text)
    $text = $text.Replace('MaxEntries = 1_000_000', 'MaxEntries = 50_000')
    [regex]::Replace($text, '(public static void EnsureRoom\(long bytes, string directory, string archiveName\)\s*\{)', @'
$1
        if (bytes < 0 || bytes > 2L * 1024 * 1024 * 1024)
            throw new InvalidDataException($"{archiveName} exceeds the Pulse 2 GiB expanded archive limit.");
'@)
}
Update-Source 'src/Filee.Engines/Infrastructure/ProcessRunner.cs' {
    param($text)
    $text = $text.Replace('stdout.AppendLine(e.Data);', 'if (stdout.Length < 2 * 1024 * 1024) stdout.AppendLine(e.Data[..Math.Min(e.Data.Length, 65536)]);')
    $text.Replace('stderr.AppendLine(e.Data);', 'if (stderr.Length < 2 * 1024 * 1024) stderr.AppendLine(e.Data[..Math.Min(e.Data.Length, 65536)]);')
}
Update-Source 'src/Filee.Engines/Infrastructure/EngineEnvironment.cs' {
    param($text)
    $start = $text.IndexOf('    public static string? FindBundled(string name)')
    $end = $text.IndexOf('    /// <summary>', $start)
    if ($start -lt 0 -or $end -le $start) { throw '找不到引擎定位边界。' }
    $text.Substring(0, $start) + @'
    public static string? FindBundled(string name)
    {
        var local = Path.Combine(AppContext.BaseDirectory, "engines", name);
        return Directory.Exists(local) ? local : null;
    }

'@ + $text.Substring($end)
}
Update-Source 'src/Filee.Engines/Office/PandocConverter.cs' {
    param($text)
    $text.Replace('List<string> args = [step.InputPath, "-f",', 'List<string> args = ["--sandbox", step.InputPath, "-f",')
}
Update-Source 'src/Filee.Engines/Hwp/Hwpx/DocumentReaders.cs' {
    param($text)
    $text.Replace('[path, "-f", pandocReader, "-t", "json",', '["--sandbox", path, "-f", pandocReader, "-t", "json",')
}
Update-Source 'src/Filee.Engines/Media/MediaEncoding.cs' {
    param($text)
    foreach ($container in @('mp4', 'mkv', 'mov')) {
        $old = '["' + $container + '"] = new("' + $(if ($container -eq 'mkv') { 'matroska' } else { $container }) + '", "libx264", "aac")'
        if (!$text.Contains($old)) { throw ('找不到视频默认编码适配点：' + $container) }
        $text = $text.Replace($old, $old.Replace('libx264', 'mpeg4'))
    }
    # Filee's existing MPEG-4 branch already uses -q:v and no CRF/preset.
    # XVID is a valid AVI tag, but MP4/MOV need the muxer's default mp4v tag.
    $text.Replace('"-c:v", "mpeg4", "-tag:v", "XVID", "-q:v",',
        '"-c:v", "mpeg4", .. (target == "avi" ? new[] { "-tag:v", "XVID" } : Array.Empty<string>()), "-q:v",')
}

$hostSource = Join-Path $source 'Pulse.Filee.Host'
New-Item -ItemType Directory -Path $hostSource | Out-Null
Get-ChildItem -LiteralPath $integration -File -Recurse | Where-Object { $_.FullName -notmatch '[\\/](bin|obj)[\\/]' } | ForEach-Object {
    $relative = $_.FullName.Substring($integration.Length + 1)
    $destination = Join-Path $hostSource $relative
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination)) | Out-Null
    Copy-Item -LiteralPath $_.FullName -Destination $destination
}
Copy-Item -LiteralPath $PSCommandPath -Destination (Join-Path $hostSource 'build_filee_host.ps1')
$vendorEngines = Get-Content -LiteralPath (Join-Path $source 'src/Filee.Engines/Infrastructure/engines.json') -Encoding UTF8 -Raw | ConvertFrom-Json
foreach ($engine in $upstream.engines) {
    $vendor = $vendorEngines.components.PSObject.Properties[$engine.id].Value
    if ($engine.kind -eq 'source-build') {
        if ($vendor.version -cne $engine.version) { throw '固定 FFmpeg 源版本与 Filee 不一致。' }
        continue
    }
    if ($vendor.url -cne $engine.url -or !$vendor.sha256.Equals($engine.sha256, [StringComparison]::OrdinalIgnoreCase) -or $vendor.version -cne $engine.version) {
        throw ('固定运行时清单与源码版本不一致：' + $engine.id)
    }
}
if ($PrepareOnly) {
    Write-Output ('Filee 固定源码已校验并完成安全适配；尚未下载引擎或构建宿主。源码：' + $source)
    Write-Output ('计划输出：' + $runtime)
    return
}
if ($env:OS -ne 'Windows_NT') { throw '完整宿主构建要求 Windows x64 runner。' }
if (!(Get-Command dotnet -ErrorAction SilentlyContinue)) { throw '请在已配置 .NET 10 SDK 的 GitHub Actions Windows runner 上构建。' }
New-Item -ItemType Directory -Force -Path $runtime | Out-Null
$hostProject = Join-Path $hostSource 'Pulse.Filee.Host.csproj'
& dotnet restore $hostProject -r win-x64 '-p:SelfContained=true' '-p:RestorePackagesWithLockFile=true' --nologo
if ($LASTEXITCODE -ne 0) { throw 'Filee 宿主依赖恢复失败。' }
& dotnet publish $hostProject -c Release -r win-x64 --self-contained true --no-restore -o $runtime '-p:PublishSingleFile=false' '-p:DebugSymbols=false' '-p:DebugType=None' --nologo
if ($LASTEXITCODE -ne 0) { throw 'Filee 宿主发布失败。' }

foreach ($engine in $upstream.engines) {
    $target = Join-Path $runtime ('engines/' + $engine.id)
    Assert-Within $target $runtime
    if ($engine.kind -eq 'source-build' -and $engine.id -eq 'ffmpeg') {
        $prepared = if (!$FfmpegRuntimePath) { Join-Path $buildRoot $engine.build_directory }
            elseif ([IO.Path]::IsPathRooted($FfmpegRuntimePath)) { $FfmpegRuntimePath }
            else { Join-Path $repo $FfmpegRuntimePath }
        $prepared = [IO.Path]::GetFullPath($prepared)
        Assert-Within $prepared $repo
        if (!(Test-Path -LiteralPath $prepared -PathType Container)) { throw '请先运行云端 build_conversion_ffmpeg.sh，生成可重建 LGPL FFmpeg 运行时。' }
        Copy-Contents $prepared $target
    } elseif ($engine.kind -eq 'zip') {
        $name = [Uri]::UnescapeDataString(([Uri]$engine.url).AbsolutePath.Split('/')[-1])
        $archive = Get-VerifiedFile '' $name $engine.url $engine.sha256 $engine.bytes
        $expanded = Join-Path $work ('engine-' + $engine.id)
        Expand-SafeZip $archive $expanded
        $items = @(Get-ChildItem -LiteralPath $expanded -Force)
        $engineRoot = if ($items.Count -eq 1 -and $items[0].PSIsContainer) { $items[0].FullName } else { $expanded }
        Copy-Contents $engineRoot $target
    } elseif ($engine.id -eq '7zip' -and $engine.kind -eq 'msi') {
        $name = [Uri]::UnescapeDataString(([Uri]$engine.url).AbsolutePath.Split('/')[-1])
        $archive = Get-VerifiedFile '' $name $engine.url $engine.sha256 $engine.bytes
        $admin = Join-Path $work '7zip-admin'
        Assert-Within $admin $work
        New-Item -ItemType Directory -Path $admin | Out-Null
        $msiexec = Join-Path $env:SystemRoot 'System32/msiexec.exe'
        $process = Start-Process -FilePath $msiexec -ArgumentList @('/a', "`"$archive`"", '/qn', "TARGETDIR=`"$admin`"") -WindowStyle Hidden -Wait -PassThru
        if ($process.ExitCode -ne 0) { throw ('7-Zip 文件解包失败：' + $process.ExitCode) }
        $executables = @(Get-ChildItem -LiteralPath $admin -Recurse -File -Filter '7z.exe')
        if ($executables.Count -ne 1) { throw '7-Zip MSI 缺少唯一的 console 程序。' }
        New-Item -ItemType Directory -Path $target | Out-Null
        foreach ($filename in $engine.required) {
            $origin = Join-Path $executables[0].DirectoryName $filename
            Assert-Within $origin $admin
            Copy-Item -LiteralPath $origin -Destination $target
        }
    } else { throw ('不支持的引擎包类型：' + $engine.id) }
    foreach ($required in $engine.required) {
        $path = Join-Path $target $required
        Assert-Within $path $runtime
        if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw ('引擎包缺少必要文件：' + $engine.id + '/' + $required) }
    }
}

$licenses = Join-Path $runtime 'licenses'
$sources = Join-Path $runtime 'sources'
$locks = Join-Path $runtime 'dependency-locks'
New-Item -ItemType Directory -Path $licenses, $sources, $locks | Out-Null
Copy-Item -LiteralPath (Join-Path $repo 'LICENSE') -Destination (Join-Path $licenses 'Pulse-APACHE-2.0.txt')
Copy-Item -LiteralPath (Join-Path $source 'LICENSE') -Destination (Join-Path $licenses 'Filee-MIT.txt')
Copy-Item -LiteralPath (Join-Path $integration 'licenses/rhwp-MIT.txt') -Destination $licenses
Copy-Item -LiteralPath (Join-Path $source 'THIRD-PARTY-NOTICES.md') -Destination (Join-Path $licenses 'Filee-原始第三方许可清单.md')
Copy-Item -LiteralPath (Join-Path $integration 'THIRD-PARTY-NOTICES.md') -Destination (Join-Path $runtime 'THIRD-PARTY-NOTICES.md')
Copy-Item -LiteralPath (Join-Path $integration 'README.md') -Destination (Join-Path $runtime '格式转换说明.md')
Copy-Item -LiteralPath (Join-Path $integration 'PROTOCOL.md') -Destination (Join-Path $runtime '格式转换协议.md')
Copy-Item -LiteralPath (Join-Path $integration 'upstream.json') -Destination (Join-Path $runtime 'upstream.json')
Copy-Item -LiteralPath $SourceArchivePath -Destination (Join-Path $sources 'Filee-1.10.2-upstream-source.zip')
foreach ($item in $upstream.sources | Where-Object bundle) {
    $compiledSource = Join-Path $runtime ('engines/ffmpeg/sources/' + $item.filename)
    $archive = if ($item.id -in @('ffmpeg', 'lame') -and (Test-Path -LiteralPath $compiledSource -PathType Leaf)) {
        Get-VerifiedFile $compiledSource $item.filename $item.url $item.sha256
    } else { Get-VerifiedFile '' $item.filename $item.url $item.sha256 }
    Copy-Item -LiteralPath $archive -Destination (Join-Path $sources $item.filename)
}
foreach ($project in @('src/Filee.Core', 'src/Filee.Engines', 'Pulse.Filee.Host')) {
    $lock = Join-Path $source ($project + '/packages.lock.json')
    if (!(Test-Path -LiteralPath $lock -PathType Leaf)) { throw ('缺少依赖锁文件：' + $project) }
    Copy-Item -LiteralPath $lock -Destination (Join-Path $locks (($project -replace '[\\/]', '-') + '.packages.lock.json'))
}
# NuGet package metadata retains exact versions, license expressions/URLs and
# bundled license/notice texts. The original vendor list is kept separately.
$assets = Get-Content -LiteralPath (Join-Path $hostSource 'obj/project.assets.json') -Encoding UTF8 -Raw | ConvertFrom-Json
$packageRoots = @($assets.packageFolders.PSObject.Properties.Name)
foreach ($library in $assets.libraries.PSObject.Properties | Where-Object { $_.Value.type -eq 'package' }) {
    $packagePath = $null
    foreach ($packageRoot in $packageRoots) {
        $candidate = Join-Path $packageRoot $library.Value.path
        if (Test-Path -LiteralPath $candidate -PathType Container) { $packagePath = $candidate; break }
    }
    if (!$packagePath) { throw ('找不到依赖许可元数据：' + $library.Name) }
    Assert-NoReparse $packagePath
    $packageLicense = Join-Path $licenses ('NuGet/' + ($library.Name.Replace('/', '-')))
    New-Item -ItemType Directory -Force -Path $packageLicense | Out-Null
    $materials = @(Get-ChildItem -LiteralPath $packagePath -File -Recurse | Where-Object {
        $_.Name -like '*.nuspec' -or $_.Name -match '^(LICENSE|LICENCE|COPYING|NOTICE|OFL|THIRD[-_. ]?PARTY)([._ -]|$)'
    })
    foreach ($material in $materials) {
        Assert-Within $material.FullName $packagePath
        if ($material.Length -gt 4MB) { throw ('依赖许可文件大小异常：' + $library.Name) }
        $relative = $material.FullName.Substring($packagePath.Length + 1)
        $destination = Join-Path $packageLicense $relative
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination)) | Out-Null
        Copy-Item -LiteralPath $material.FullName -Destination $destination
    }
}
# Source archives contain the resolved lockfiles, patches and build instructions;
# remove generated bin/obj folders only inside this checked temporary source tree.
foreach ($project in @('src/Filee.Core', 'src/Filee.Engines', 'Pulse.Filee.Host')) {
    foreach ($generated in @('bin', 'obj')) {
        $generatedPath = Join-Path $source ($project + '/' + $generated)
        Assert-Within $generatedPath $source
        if (Test-Path -LiteralPath $generatedPath) { Remove-Item -LiteralPath $generatedPath -Recurse -Force }
    }
}
$context = Join-Path $source 'Pulse-打包上下文'
New-Item -ItemType Directory -Path (Join-Path $context 'scripts'), (Join-Path $context 'integrations/Filee.Host') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $repo 'LICENSE') -Destination $context
Copy-Item -LiteralPath $PSCommandPath -Destination (Join-Path $context 'scripts/build_filee_host.ps1')
Copy-Item -LiteralPath (Join-Path $repo 'scripts/build_conversion_ffmpeg.sh') -Destination (Join-Path $context 'scripts/build_conversion_ffmpeg.sh')
Get-ChildItem -LiteralPath $hostSource -Force | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $context 'integrations/Filee.Host') -Recurse
}
Compress-Archive -LiteralPath $source -DestinationPath (Join-Path $sources 'Filee-1.10.2-Pulse-host-source.zip') -CompressionLevel Optimal
$ffmpeg = Join-Path $runtime 'engines/ffmpeg/bin/ffmpeg.exe'
function Save-EngineInfo([string]$Argument, [string]$Filename) {
    $start = [Diagnostics.ProcessStartInfo]::new($ffmpeg, ('-hide_banner ' + $Argument))
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    try {
        if (!$process.Start()) { throw '预置 FFmpeg 无法启动。' }
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (!$process.WaitForExit(30000)) { $process.Kill(); throw 'FFmpeg 元数据读取超时。' }
        if ($process.ExitCode -ne 0) { throw ('预置 FFmpeg 检查失败：' + $Argument) }
        [IO.File]::WriteAllText((Join-Path $licenses $Filename), ($stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult()), $utf8)
    } finally { $process.Dispose() }
}
Save-EngineInfo '-version' 'FFmpeg-版本与编解码库.txt'
Save-EngineInfo '-buildconf' 'FFmpeg-构建配置.txt'
if (Get-ChildItem -LiteralPath $runtime -Recurse -Force -Attributes ReparsePoint | Select-Object -First 1) { throw '运行时包含重解析点。' }
$files = @(Get-ChildItem -LiteralPath $runtime -Recurse -File -Force | Sort-Object FullName | ForEach-Object {
    [ordered]@{
        path = $_.FullName.Substring($runtime.Length + 1).Replace('\', '/')
        bytes = $_.Length
        sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
})
$manifest = [ordered]@{
    schema = 1; component = 'Pulse.Filee.Host'; protocol = 1; version = $upstream.version
    source_commit = $upstream.source_commit; source_archive_sha256 = $upstream.source_sha256
    runtime_id = 'win-x64'; host_filename = 'Pulse.Filee.Host.exe'
    engines = $upstream.engines; sources = $upstream.sources; files = $files
}
[IO.File]::WriteAllText((Join-Path $runtime 'runtime-manifest.json'), ($manifest | ConvertTo-Json -Depth 8), $utf8)
Write-Output ('Pulse Filee 转换宿主已生成：' + $runtime)
Write-Output ('已预置 rhwp、7-Zip、FFmpeg、Pandoc；逐文件 SHA256、NuGet SHA512 锁、许可和源码材料已保存。文件数：' + $files.Count)
