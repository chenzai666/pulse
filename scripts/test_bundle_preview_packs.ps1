#requires -Version 7.2
param([string]$BuildDir = 'build', [string]$AssetDirectory = '')
$ErrorActionPreference = 'Stop'
$build = (Resolve-Path -LiteralPath $BuildDir).Path
$assets = if ($AssetDirectory) { (Resolve-Path -LiteralPath $AssetDirectory).Path } else { Join-Path $build 'preview-pack-assets' }
$fixture = Join-Path $build ("test-bundled-packs-" + [guid]::NewGuid().ToString('N'))
$bundle = Join-Path $PSScriptRoot 'bundle_preview_packs.ps1'
New-Item -ItemType Directory -Path $fixture | Out-Null
$fixtureAssets = Join-Path $fixture 'assets'
Copy-Item -LiteralPath $assets -Destination $fixtureAssets -Recurse

function Expect-Rejection([scriptblock]$Action, [string]$Reason) {
    $rejected = $false
    try { & $Action | Out-Null } catch { $rejected = $_.Exception.Message.Contains($Reason) }
    if (-not $rejected) { throw "Expected bundle verification rejection: $Reason" }
    Write-Output "[PASS] rejected $Reason"
}

try {
    & $bundle -BuildDir $fixture -AssetDirectory $fixtureAssets | Out-Null
    & $bundle -BuildDir $fixture -VerifyOnly | Out-Null
    foreach ($entry in @('ffmpeg/1.0.1/SOURCE.txt', 'ffmpeg/1.0.1/LICENSE-FFmpeg-LGPL-2.1.txt',
        'images/1.0.1/THIRD_PARTY_NOTICES.txt', 'raw/1.0.1/THIRD-PARTY-NOTICES.txt', 'archive/1.0.1/License.txt')) {
        if (-not (Test-Path -LiteralPath (Join-Path $fixture "packs/$entry"))) { throw "Missing bundled license or source notice: $entry" }
    }
    Write-Output '[PASS] four complete decoded packs, licenses and source notices verified offline'
    $compressed = Join-Path $fixtureAssets 'images-pulse-imgpack.exe.lzms'
    $original = [IO.File]::ReadAllBytes($compressed)
    $corrupted = [byte[]]$original.Clone()
    $corrupted[0] = $corrupted[0] -bxor 1
    [IO.File]::WriteAllBytes($compressed, $corrupted)
    $tool = Join-Path $fixture 'packs/images/1.0.1/pulse-imgpack.exe'
    $before = (Get-FileHash -LiteralPath $tool).Hash
    Expect-Rejection { & $bundle -BuildDir $fixture -AssetDirectory $fixtureAssets } 'SHA-256 verification failed'
    if ((Get-FileHash -LiteralPath $tool).Hash -ne $before) { throw 'Rejected download changed the previous bundle' }
    Write-Output '[PASS] rejected compressed payload preserves the existing complete bundle'
    [IO.File]::WriteAllBytes($compressed, $original)
    $plain = [IO.File]::ReadAllBytes($tool)
    $plain[0] = $plain[0] -bxor 1
    [IO.File]::WriteAllBytes($tool, $plain)
    Expect-Rejection { & $bundle -BuildDir $fixture -VerifyOnly } 'SHA-256 verification failed'
    [IO.File]::WriteAllText((Join-Path $fixture 'packs/images/1.0.1/unexpected.dll'), 'MZ')
    $plain[0] = $plain[0] -bxor 1
    [IO.File]::WriteAllBytes($tool, $plain)
    Expect-Rejection { & $bundle -BuildDir $fixture -VerifyOnly } 'Unexpected bundled preview pack entries'
} finally {
    $absolute = [IO.Path]::GetFullPath($fixture)
    if ((Split-Path -Parent $absolute) -ine $build -or (Split-Path -Leaf $absolute) -notmatch '^test-bundled-packs-[a-f0-9]{32}$') {
        throw 'Refusing to remove a fixture outside the build directory'
    }
    Remove-Item -LiteralPath $absolute -Recurse -Force
}
