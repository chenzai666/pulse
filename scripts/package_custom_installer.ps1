#requires -Version 7.2
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [Alias('PackageDirectory')]
    [string]$StageDir,
    [Alias('OutputDirectory')]
    [string]$OutputDir,
    [string]$IsccPath,
    [switch]$VerifyOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path -Parent $PSScriptRoot
. "$PSScriptRoot/release_payload.ps1"
$taskStage = (Resolve-Path -LiteralPath $StageDir).Path
if (-not (Test-Path -LiteralPath $taskStage -PathType Container)) { throw '输入不是完整包目录' }

function Assert-NoReparsePoint([IO.FileSystemInfo]$Item) {
    if (($Item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "不接受符号链接或重解析点：$($Item.FullName)"
    }
}

$taskAncestor = Get-Item -LiteralPath $taskStage -Force
while ($null -ne $taskAncestor) {
    Assert-NoReparsePoint $taskAncestor
    $taskAncestor = $taskAncestor.Parent
}
$taskItems = @(Get-ChildItem -LiteralPath $taskStage -Force -Recurse)
foreach ($taskItem in $taskItems) { Assert-NoReparsePoint $taskItem }
$taskManifestPath = Join-Path $taskStage 'package-manifest.json'
$taskManifest = Get-Content -LiteralPath $taskManifestPath -Raw -Encoding utf8 | ConvertFrom-Json
if ($taskManifest.schema -ne 1 -or $taskManifest.edition -cne 'PulseCustom' -or
    $taskManifest.commit -cnotmatch '^[0-9a-f]{40}$') {
    throw '仅接受 schema=1、edition=PulseCustom 且带完整提交号的完整包清单'
}
$taskManifestFiles = @($taskManifest.files)
if (-not $taskManifestFiles.Count) { throw '完整包清单没有文件' }
$taskFiles = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($taskEntry in $taskManifestFiles) {
    $taskRelative = ([string]$taskEntry.path).Replace('\', '/')
    $taskParts = $taskRelative.Split('/')
    if ([string]::IsNullOrWhiteSpace($taskRelative) -or $taskRelative.StartsWith('/') -or
        $taskRelative -match '[\x00-\x1f\x7f:*?"<>|;{}]' -or
        @($taskParts | Where-Object { $_ -in @('', '.', '..') -or $_ -match '[. ]$' -or
            $_ -match '^(?i:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)' }).Count -or
        $taskRelative -ieq 'package-manifest.json' -or $taskRelative -match '^(?i:\.installer)(/|$)') {
        throw "清单包含不安全或保留的相对路径：$taskRelative"
    }
    if (-not $taskFiles.TryAdd($taskRelative, $taskEntry)) { throw "清单路径重复：$taskRelative" }
    $taskFile = Get-Item -LiteralPath (Join-Path $taskStage $taskRelative) -Force
    if ($taskFile -isnot [IO.FileInfo]) { throw "清单条目不是文件：$taskRelative" }
    if ([string]$taskEntry.bytes -notmatch '^\d+$' -or $taskFile.Length -ne [long]$taskEntry.bytes -or
        [string]$taskEntry.sha256 -cnotmatch '^[0-9a-f]{64}$' -or
        (Get-FileHash -LiteralPath $taskFile.FullName -Algorithm SHA256).Hash -ine $taskEntry.sha256) {
        throw "完整包文件长度或 SHA256 校验失败：$taskRelative"
    }
}
foreach ($taskFile in @($taskItems | Where-Object { $_ -is [IO.FileInfo] })) {
    $taskRelative = [IO.Path]::GetRelativePath($taskStage, $taskFile.FullName).Replace('\', '/')
    if ($taskRelative -ine 'package-manifest.json' -and -not $taskFiles.ContainsKey($taskRelative)) {
        throw "完整包存在清单之外的文件：$taskRelative"
    }
}
foreach ($taskRequired in ($PulseReleasePayload + @('LICENSE-Pulse.txt', '使用说明.md',
    'sources/Pulse-Custom-source.zip',
    'licenses/ib-pinyin-cpp/LICENSE.txt', 'licenses/md4c/LICENSE.md'))) {
    if (-not $taskFiles.ContainsKey($taskRequired)) { throw "完整包缺少必要文件：$taskRequired" }
}
foreach ($taskDirectory in @('quicklook', 'packs', 'conversion', 'licenses')) {
    if (-not @($taskFiles.Keys | Where-Object { $_.StartsWith($taskDirectory + '/', [StringComparison]::OrdinalIgnoreCase) }).Count) {
        throw "完整包缺少组件内容：$taskDirectory"
    }
}
$taskExecutable = Join-Path $taskStage 'pulse.exe'
$taskBinary = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($taskExecutable))
$taskEditionMarker = [Text.Encoding]::ASCII.GetString([Text.Encoding]::Unicode.GetBytes('PulseCustomMainWindow'))
if (-not $taskBinary.Contains($taskEditionMarker)) { throw 'pulse.exe 不包含定制版窗口身份，拒绝打包原版' }
& "$PSScriptRoot/check_release_payload.ps1" -BuildDir $taskStage
Write-Output "完整包校验通过：$($taskManifest.commit)，$($taskFiles.Count) 个文件"
if ($VerifyOnly) { return }

if (-not $OutputDir) { $OutputDir = Join-Path $taskRepo 'dist/installer' }
$taskOutput = [IO.Path]::GetFullPath($OutputDir)
if ($taskOutput -ieq $taskStage -or $taskOutput.StartsWith($taskStage + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw '安装器输出目录不能位于完整包目录中'
}
New-Item -ItemType Directory -Path $taskOutput -Force | Out-Null
if (-not $IsccPath) {
    $taskCompiler = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if (-not $taskCompiler) { throw '找不到 Inno Setup 6.3+，请通过 -IsccPath 指定 ISCC.exe' }
    $IsccPath = $taskCompiler.Source
}
$taskIscc = (Resolve-Path -LiteralPath $IsccPath).Path
$taskVersionInfo = [Diagnostics.FileVersionInfo]::GetVersionInfo($taskExecutable)
$taskVersion = '{0}.{1}.{2}' -f $taskVersionInfo.FileMajorPart, $taskVersionInfo.FileMinorPart,
    $taskVersionInfo.FileBuildPart
if ($taskVersion -ne '1.0.53' -or $taskVersionInfo.FilePrivatePart -ne 0) {
    throw '当前按用户要求固定 v1.0.53，安装载荷版本不匹配'
}
$taskCommit = $taskManifest.commit.Substring(0, 9)
$taskName = "Pulse-Custom-Setup-v$taskVersion-x64"
$taskInstaller = Join-Path $taskOutput ($taskName + '.exe')
if ((Test-Path -LiteralPath $taskInstaller) -or (Test-Path -LiteralPath ($taskInstaller + '.sha256'))) {
    throw '安装器或摘要已存在，停止覆盖'
}
$taskTemp = Join-Path ([IO.Path]::GetTempPath()) ('PulseCustomInstaller-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $taskTemp | Out-Null
try {
    $taskIdentity = Join-Path $taskTemp 'pulse-custom-installation.id'
    'BF0350A3-5F01-44CD-AC64-C9AF030CC430' | Set-Content -LiteralPath $taskIdentity -Encoding ascii
    $taskInclude = Join-Path $taskTemp 'payload.iss'
    function ConvertTo-InnoString([string]$Value) {
        return $Value.Replace('{', '{{').Replace('"', '""')
    }
    $taskInstallFiles = @($taskFiles.Keys | Sort-Object) + @('package-manifest.json')
    $taskLines = foreach ($taskRelative in $taskInstallFiles) {
        $taskSource = ConvertTo-InnoString (Join-Path $taskStage $taskRelative)
        $taskParent = [IO.Path]::GetDirectoryName($taskRelative.Replace('/', '\'))
        $taskDestination = '{app}'
        if ($taskParent) { $taskDestination += '\' + (ConvertTo-InnoString $taskParent) }
        'Source: "{0}"; DestDir: "{1}"; Flags: ignoreversion' -f $taskSource, $taskDestination
    }
    $taskLines += 'Source: "{0}"; DestDir: "{{app}}\.installer"; Flags: ignoreversion' -f (ConvertTo-InnoString $taskIdentity)
    $taskLines | Set-Content -LiteralPath $taskInclude -Encoding utf8BOM
    & $taskIscc "/DStageDir=$taskStage" "/DPayloadList=$taskInclude" "/DOutputDir=$taskOutput" `
        "/DPackageVersion=$taskVersion" "/DPackageCommit=$taskCommit" "/DOutputName=$taskName" `
        (Join-Path $taskRepo 'installer/PulseCustomSetup.iss')
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $taskInstaller -PathType Leaf)) {
        throw 'Inno Setup 编译失败，未生成可交付安装器'
    }
    $taskHash = (Get-FileHash -LiteralPath $taskInstaller -Algorithm SHA256).Hash.ToLowerInvariant()
    "$taskHash  $([IO.Path]::GetFileName($taskInstaller))" | Set-Content -LiteralPath ($taskInstaller + '.sha256') -Encoding ascii
    Write-Output "安装器：$taskInstaller"
    Write-Output "SHA256：$taskHash"
} finally {
    $taskTempResolved = [IO.Path]::GetFullPath($taskTemp)
    $taskTempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if ($taskTempResolved.StartsWith($taskTempRoot, [StringComparison]::OrdinalIgnoreCase) -and
        [IO.Path]::GetFileName($taskTempResolved) -match '^PulseCustomInstaller-[0-9a-f]{32}$') {
        Remove-Item -LiteralPath $taskTempResolved -Recurse -Force
    }
}
