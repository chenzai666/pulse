[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PackagePath,
    [Parameter(Mandatory)][string]$PulseExecutablePath,
    [Parameter(Mandatory)][string]$BackupDirectory,
    [Parameter(Mandatory)][string]$ResultPath
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-OrdinaryPath([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    for ($item = $full; $item; $item = [IO.Path]::GetDirectoryName($item)) {
        if ((Test-Path -LiteralPath $item) -and
            ((Get-Item -LiteralPath $item -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw '安装路径不能经过重解析点。'
        }
    }
    return $full
}

$package = Assert-OrdinaryPath $PackagePath
$pulse = Assert-OrdinaryPath $PulseExecutablePath
$backup = Assert-OrdinaryPath $BackupDirectory
$result = Assert-OrdinaryPath $ResultPath
$resultParent = [IO.Path]::GetDirectoryName($result)
if (-not $resultParent.Equals([IO.Path]::GetDirectoryName($backup), [StringComparison]::OrdinalIgnoreCase)) {
    throw '结果文件和备份目录必须位于同一个专用目录下。'
}
if ((Test-Path -LiteralPath $backup) -or (Test-Path -LiteralPath $result)) { throw '拒绝覆盖已有备份或结果文件。' }
if (-not (Test-Path -LiteralPath $pulse -PathType Leaf) -or [IO.Path]::GetFileName($pulse) -ine 'pulse.exe') {
    throw '必须指定已部署的定制版 pulse.exe。'
}
$manifestFile = Join-Path $package 'package-manifest.json'
$packageManifest = Get-Content -LiteralPath $manifestFile -Raw | ConvertFrom-Json
if ($packageManifest.edition -ne 'PulseCustom' -or $packageManifest.commit -notmatch '^[0-9a-f]{40}$') {
    throw '完整包清单不属于 Pulse 定制版。'
}
$pulseEntry = @($packageManifest.files | Where-Object { $_.path -eq 'pulse.exe' })
if ($pulseEntry.Count -ne 1 -or (Get-FileHash -LiteralPath $pulse -Algorithm SHA256).Hash -ine $pulseEntry[0].sha256) {
    throw '已部署的主程序与完整包不一致。'
}
$files = @('Listary.FileAppPlugin.Pulse.dll', 'ListaryPluginManifest.json', 'Icon.png', 'README.md')
$source = Join-Path $package 'ListaryPlugin'
foreach ($name in $files) {
    $file = Assert-OrdinaryPath (Join-Path $source $name)
    $entry = @($packageManifest.files | Where-Object { $_.path -eq ('ListaryPlugin/' + $name) })
    if ($entry.Count -ne 1 -or -not (Test-Path -LiteralPath $file -PathType Leaf) -or
        (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ine $entry[0].sha256) {
        throw "插件文件未通过完整包校验：$name"
    }
}
$plugin = Get-Content -LiteralPath (Join-Path $source 'ListaryPluginManifest.json') -Raw | ConvertFrom-Json
if ($plugin.Id -ne 'PulseCustom' -or $plugin.Entry -ne $files[0] -or $plugin.PluginType -ne 'FileAppPlugin') {
    throw '插件清单身份不正确。'
}

$listaryRoot = Assert-OrdinaryPath (Join-Path $env:ProgramFiles 'Listary')
if (-not (Test-Path -LiteralPath (Join-Path $listaryRoot 'Listary.exe') -PathType Leaf)) { throw '未找到现有 Listary 安装。' }
$targetRoot = Assert-OrdinaryPath (Join-Path $listaryRoot 'FileAppPlugins')
$target = Assert-OrdinaryPath (Join-Path $targetRoot 'PulseCustom')
if (-not [IO.Path]::GetDirectoryName($target).Equals($targetRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw '插件目标越界。'
}
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw '现有 Listary 位于 Program Files，安装插件需要 Windows 管理员确认。'
}

New-Item -ItemType Directory -Path $resultParent -Force | Out-Null
New-Item -ItemType Directory -Path $backup | Out-Null
$hadTarget = Test-Path -LiteralPath $target
if ($hadTarget) { Copy-Item -LiteralPath $target -Destination (Join-Path $backup 'PulseCustom') -Recurse }
try {
    New-Item -ItemType Directory -Path $target -Force | Out-Null
    foreach ($name in $files) { Copy-Item -LiteralPath (Join-Path $source $name) -Destination (Join-Path $target $name) -Force }
    [IO.File]::WriteAllText((Join-Path $target 'PulseExecutable.path'), $pulse, [Text.UTF8Encoding]::new($false))
    foreach ($name in $files) {
        if ((Get-FileHash -LiteralPath (Join-Path $source $name)).Hash -ne
            (Get-FileHash -LiteralPath (Join-Path $target $name)).Hash) { throw "部署后校验失败：$name" }
    }
    [ordered]@{status='installed'; commit=$packageManifest.commit; target=$target; pulse=$pulse; backup=$backup} |
        ConvertTo-Json | Set-Content -LiteralPath $result -Encoding utf8
} catch {
    foreach ($name in ($files + 'PulseExecutable.path')) {
        $old = Join-Path (Join-Path $backup 'PulseCustom') $name
        $destination = Join-Path $target $name
        if (Test-Path -LiteralPath $old -PathType Leaf) { Copy-Item -LiteralPath $old -Destination $destination -Force }
        elseif (Test-Path -LiteralPath $destination -PathType Leaf) { Remove-Item -LiteralPath $destination }
    }
    [ordered]@{status='failed'; error=$_.Exception.Message; backup=$backup} |
        ConvertTo-Json | Set-Content -LiteralPath $result -Encoding utf8
    throw
}
Write-Output '插件已安装。请在普通用户会话中重启 Listary，避免以管理员身份运行搜索程序。'
