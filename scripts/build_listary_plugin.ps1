[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [string]$OutputPath,
    [string]$PulseExecutablePath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$pulseRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$pluginRoot = Join-Path $pulseRoot 'integrations\Listary.Pulse'
$pluginProject = Join-Path $pluginRoot 'Listary.FileAppPlugin.Pulse.csproj'
$buildOutput = Join-Path $pulseRoot "out\listary-plugin\build\$Configuration"
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path $pulseRoot 'out\listary-plugin\package'
}
$packageOutput = [System.IO.Path]::GetFullPath($OutputPath)
$dotnetPath = (Get-Command dotnet -ErrorAction Stop).Source

# 使用锁文件；插件只引用公开 SDK，不打包 Listary 的闭源程序或引擎。
& $dotnetPath restore $pluginProject --locked-mode --configfile (Join-Path $pluginRoot 'NuGet.config') --nologo
if ($LASTEXITCODE -ne 0) { throw 'Listary 插件依赖还原失败。' }
& $dotnetPath build $pluginProject -c $Configuration --no-restore --nologo -o $buildOutput
if ($LASTEXITCODE -ne 0) { throw 'Listary 插件编译失败。' }

$null = [System.IO.Directory]::CreateDirectory($packageOutput)
foreach ($fileName in @('Listary.FileAppPlugin.Pulse.dll', 'ListaryPluginManifest.json', 'Icon.png')) {
    $source = Join-Path $buildOutput $fileName
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "缺少插件产物：$fileName" }
    Copy-Item -LiteralPath $source -Destination (Join-Path $packageOutput $fileName) -Force
}
Copy-Item -LiteralPath (Join-Path $pluginRoot 'README.md') -Destination (Join-Path $packageOutput 'README.md') -Force

# 可选路径只供插件在 Pulse 未运行时启动已部署的定制版，不能指向上游版本。
$configuredPulse = ''
if (-not [string]::IsNullOrWhiteSpace($PulseExecutablePath)) {
    if (-not [System.IO.Path]::IsPathRooted($PulseExecutablePath) -or
        $PulseExecutablePath.StartsWith('\\') -or $PulseExecutablePath.IndexOfAny([char[]]@('"', "`r", "`n")) -ge 0) {
        throw 'PulseExecutablePath 必须是本机绝对路径。'
    }
    $configuredPulse = [System.IO.Path]::GetFullPath($PulseExecutablePath)
    $executableName = [System.IO.Path]::GetFileName($configuredPulse)
    if ($executableName -ine 'pulse.exe' -and $executableName -ine 'PulseCustom.exe') {
        throw 'PulseExecutablePath 必须指向 pulse.exe 或 PulseCustom.exe。'
    }
    if (-not (Test-Path -LiteralPath $configuredPulse -PathType Leaf)) { throw '指定的 Pulse 定制版不存在。' }
}
[System.IO.File]::WriteAllText((Join-Path $packageOutput 'PulseExecutable.path'), $configuredPulse,
    [System.Text.UTF8Encoding]::new($false))

$manifest = Get-Content -LiteralPath (Join-Path $packageOutput 'ListaryPluginManifest.json') -Raw | ConvertFrom-Json
$archivePath = Join-Path ([System.IO.Path]::GetDirectoryName($packageOutput)) "Listary.Pulse-$($manifest.PluginVersion).zip"
Compress-Archive -Path (Join-Path $packageOutput '*') -DestinationPath $archivePath -Force
$archiveHash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
[System.IO.File]::WriteAllText("$archivePath.sha256", "$archiveHash  $([System.IO.Path]::GetFileName($archivePath))`n",
    [System.Text.UTF8Encoding]::new($false))

Write-Output "插件目录：$packageOutput"
Write-Output "插件压缩包：$archivePath"
Write-Output "SHA256：$archiveHash"
