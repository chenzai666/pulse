#requires -Version 7.2
param([string]$BuildDir = 'build-custom')
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path -Parent $PSScriptRoot
$taskBuild = (Resolve-Path -LiteralPath $BuildDir).Path
. "$PSScriptRoot/release_payload.ps1"
& "$PSScriptRoot/check_release_payload.ps1" -BuildDir $taskBuild
$commit = (& git -C $taskRepo rev-parse HEAD).Trim()
if ($LASTEXITCODE -or $commit -notmatch '^[0-9a-f]{40}$') { throw '无法读取对应构建提交' }
$packageName = 'Pulse-Custom-' + $commit.Substring(0, 9)
$taskDist = Join-Path $taskRepo 'dist'
$stage = Join-Path $taskDist $packageName
if (Test-Path -LiteralPath $stage) { throw '完整打包目录已存在，停止覆盖' }
New-Item -ItemType Directory -Path $stage -Force | Out-Null
foreach ($file in $PulseReleasePayload) {
    Copy-Item -LiteralPath (Join-Path $taskBuild $file) -Destination $stage
}
foreach ($directory in @('quicklook', 'packs', 'conversion')) {
    $source = Join-Path $taskBuild $directory
    if (-not (Test-Path -LiteralPath $source)) { throw "完整组件缺失：$directory" }
    Copy-Item -LiteralPath $source -Destination (Join-Path $stage $directory) -Recurse
}
if (Test-Path -LiteralPath (Join-Path $taskBuild 'listary-picker')) {
    Copy-Item -LiteralPath (Join-Path $taskBuild 'listary-picker') -Destination (Join-Path $stage 'listary-picker') -Recurse
}
$licenseRoot = Join-Path $stage 'licenses'
Copy-Item -LiteralPath (Join-Path $taskBuild 'licenses') -Destination $licenseRoot -Recurse
foreach ($entry in @(@('ib-pinyin-cpp', 'LICENSE.txt'), @('md4c', 'LICENSE.md'))) {
    $target = Join-Path $licenseRoot $entry[0]
    New-Item -ItemType Directory -Path $target -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $taskRepo ('third_party/' + $entry[0] + '/' + $entry[1])) -Destination $target
}
Copy-Item -LiteralPath (Join-Path $taskRepo 'LICENSE') -Destination (Join-Path $stage 'LICENSE-Pulse.txt')
Copy-Item -LiteralPath (Join-Path $taskRepo 'docs/custom-edition.md') -Destination (Join-Path $stage '使用说明.md')
New-Item -ItemType Directory -Path (Join-Path $stage 'sources') -Force | Out-Null
& git -C $taskRepo archive HEAD -o (Join-Path $stage 'sources/Pulse-Custom-source.zip')
if ($LASTEXITCODE) { throw '对应源码归档失败' }
$checksums = foreach ($file in Get-ChildItem -LiteralPath $stage -File -Recurse) {
    [ordered]@{
        path = [IO.Path]::GetRelativePath($stage, $file.FullName).Replace('\', '/')
        bytes = $file.Length
        sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}
$buildInfo = [ordered]@{schema = 1; edition = 'PulseCustom'; commit = $commit; files = @($checksums)}
$buildInfo | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $stage 'package-manifest.json') -Encoding utf8
[ordered]@{commit = $commit; package = $packageName; component_count = @($checksums).Count} |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $taskDist 'custom-build.json') -Encoding utf8
$archive = Join-Path $taskDist ($packageName + '.zip')
Compress-Archive -LiteralPath $stage -DestinationPath $archive
$hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
"$hash  $([IO.Path]::GetFileName($archive))" | Set-Content -LiteralPath ($archive + '.sha256') -Encoding ascii
Write-Output ('完整包：' + $archive)
Write-Output ('SHA256：' + $hash)
