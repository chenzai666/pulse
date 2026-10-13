#requires -Version 7.2
param(
    [string]$BuildDir = 'build',
    [string]$AssetDirectory = '',
    [switch]$VerifyOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = Split-Path -Parent $PSScriptRoot
$build = (Resolve-Path -LiteralPath $BuildDir).Path
$target = Join-Path $build 'packs'
$repository = 'jimmgreen/pulse'
$tag = 'v1.0.53'
$version = '1.0.1'
$maximumFileBytes = 256MB
$tools = @{ ffmpeg = 'ffmpeg.exe'; images = 'pulse-imgpack.exe'; raw = 'pulse-rawpack.exe'; archive = '7z.exe' }
$metadata = [ordered]@{
    'preview-packs-assets.json' = @{ size = 2865; sha256 = '19c0e518f58c8be2099f865ae37fe1df7578f58675f637f9d4629eb6fa7075fb' }
    'pulse-pack-ffmpeg-1.0.1.json' = @{ size = 2076; sha256 = '1801517ed2b8f68ee2baf9f233206d8b0686d887f8ab2fe1ccf98a1bf43e07f5' }
    'pulse-pack-images-1.0.1.json' = @{ size = 1430; sha256 = '4870f99853834f8cb4bbc523446384857fe29b4a95b105b6f7ed6600e58b74b2' }
    'pulse-pack-raw-1.0.1.json' = @{ size = 1088; sha256 = '0bc63074a68d6d17d8a000f2298a776b897139af286fb523d9d29b9ca4e4e562' }
    'pulse-pack-archive-1.0.1.json' = @{ size = 1369; sha256 = 'f3597535e77100d3130362f4066e5cbe1d0f002e26c9dbf0c8aedfdeef94b1c2' }
}

function Assert-RegularPath([string]$Path, [bool]$Directory = $false) {
    $item = Get-Item -LiteralPath $Path -Force
    if ($item.PSIsContainer -ne $Directory -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Unexpected file type or reparse point: $Path"
    }
}

function Assert-File([string]$Path, [uint64]$Size, [string]$Hash) {
    Assert-RegularPath $Path
    if ((Get-Item -LiteralPath $Path).Length -ne $Size) { throw "Size verification failed: $Path" }
    if ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ine $Hash) {
        throw "SHA-256 verification failed: $Path"
    }
}

function Assert-DirectoryEntries([string]$Path, [string[]]$Expected) {
    $actual = @(Get-ChildItem -LiteralPath $Path -Force | ForEach-Object { $_.Name })
    if ($actual.Count -ne $Expected.Count -or @(Compare-Object $Expected $actual -CaseSensitive).Count) {
        throw "Unexpected bundled preview pack entries: $Path"
    }
}

function Read-Catalog {
    $source = Get-Content -LiteralPath (Join-Path $repo 'src/app/pack_catalog_generated.h') -Raw
    $arrays = @{}
    foreach ($match in [regex]::Matches($source, '(?s)inline constexpr PackFile (\w+)\[\] = \{(.*?)\r?\n\};')) {
        $arrays.Add($match.Groups[1].Value, $match.Groups[2].Value)
    }
    $releases = [regex]::Matches($source, 'inline constexpr PackRelease \w+\{L"([^"]+)", L"([^"]+)", L"([^"]+)", (\w+), std::size\(\4\)\};')
    if ($releases.Count -ne 4) { throw 'Expected four pinned preview pack releases' }
    $packs = [ordered]@{}
    $downloadNames = @{}
    foreach ($release in $releases) {
        $key = $release.Groups[1].Value
        if (-not $tools.ContainsKey($key) -or $packs.Contains($key) -or $release.Groups[2].Value -cne $version -or
            $release.Groups[3].Value -cne "https://github.com/$repository/releases/download/$tag/") {
            throw "Unexpected preview pack release: $key"
        }
        $body = $arrays[$release.Groups[4].Value]
        $rows = [regex]::Matches($body, '\{L"([A-Za-z0-9_.-]+)", (\d+), L"([a-f0-9]{64})", (\d+), L"([a-f0-9]{64})", L"([A-Za-z0-9_.-]+)"\},')
        if (-not $rows.Count -or $rows.Count -ne ([regex]::Matches($body, '\{')).Count) {
            throw "Unparsed catalog entries: $key"
        }
        $names = @{}
        $files = foreach ($row in $rows) {
            $name = $row.Groups[1].Value
            $downloadName = $row.Groups[6].Value
            $size = [uint64]$row.Groups[2].Value
            $packedSize = [uint64]$row.Groups[4].Value
            if ($name -in @('.', '..') -or $downloadName -cne "$key-$name.lzms" -or $names.ContainsKey($name) -or
                $downloadNames.ContainsKey($downloadName) -or -not $size -or $size -gt $maximumFileBytes -or
                -not $packedSize -or $packedSize -gt $maximumFileBytes) {
                throw "Unsafe or duplicate catalog entry: $key/$name"
            }
            $names.Add($name, $true)
            $downloadNames.Add($downloadName, $true)
            [ordered]@{
                name = $name; size = $size; sha256 = $row.Groups[3].Value
                packed_size = $packedSize; packed_sha256 = $row.Groups[5].Value; download_name = $downloadName
            }
        }
        if (-not $names.ContainsKey($tools[$key])) { throw "Catalog lacks the main preview tool: $key" }
        $packs.Add($key, @($files))
    }
    return $packs
}

function Assert-Bundle([string]$Root, $Packs) {
    Assert-RegularPath $Root $true
    $inventoryPath = Join-Path $Root 'bundled-packs.json'
    Assert-RegularPath $inventoryPath
    $inventory = Get-Content -LiteralPath $inventoryPath -Raw | ConvertFrom-Json
    if ($inventory.schema -ne 1 -or $inventory.repository -cne $repository -or $inventory.release_tag -cne $tag -or
        $inventory.version -cne $version) { throw 'Unexpected bundled preview pack inventory' }
    Assert-DirectoryEntries $Root (@($Packs.Keys) + @($metadata.Keys) + @('bundled-packs.json'))
    foreach ($key in $Packs.Keys) {
        $base = Join-Path $Root $key
        Assert-RegularPath $base $true
        Assert-DirectoryEntries $base @('installed.json', $version)
        $installedPath = Join-Path $base 'installed.json'
        Assert-RegularPath $installedPath
        $installed = Get-Content -LiteralPath $installedPath -Raw | ConvertFrom-Json
        if ($installed.version -cne $version -or $installed.dir -cne $version) {
            throw "Unexpected bundled preview pack marker: $key"
        }
        $directory = Join-Path $base $version
        Assert-RegularPath $directory $true
        foreach ($entry in $Packs[$key]) {
            Assert-File (Join-Path $directory $entry.name) $entry.size $entry.sha256
        }
        # Extra executables or DLLs can change dependency loading; ship only the pinned payload.
        Assert-DirectoryEntries $directory @($Packs[$key] | ForEach-Object { $_.name })
        $manifestName = "pulse-pack-$key-$version.json"
        Assert-File (Join-Path $Root $manifestName) $metadata[$manifestName].size $metadata[$manifestName].sha256
    }
    Assert-File (Join-Path $Root 'preview-packs-assets.json') $metadata['preview-packs-assets.json'].size $metadata['preview-packs-assets.json'].sha256
}

function Remove-Stage([string]$Path) {
    $absolute = [IO.Path]::GetFullPath($Path)
    if ((Split-Path -Parent $absolute) -ine $build -or (Split-Path -Leaf $absolute) -notmatch '^packs\.(stage|previous)-[a-f0-9]{32}$') {
        throw "Refusing cleanup outside the build staging directory: $Path"
    }
    if (Test-Path -LiteralPath $absolute) {
        Assert-RegularPath $absolute $true
        foreach ($entry in Get-ChildItem -LiteralPath $absolute -Recurse -Force) {
            if ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Refusing cleanup through reparse point: $($entry.FullName)" }
        }
        Remove-Item -LiteralPath $absolute -Recurse -Force
    }
}

Assert-RegularPath $build $true
$packs = Read-Catalog
if ($VerifyOnly) {
    Assert-Bundle $target $packs
    Write-Output "Verified four bundled preview packs from $repository@$tag ($version): $target"
    return
}
if (-not $IsWindows -or -not [Environment]::Is64BitProcess) { throw 'Bundling preview packs requires 64-bit Windows' }
# Only replace a complete bundle previously generated by this script.
if (Test-Path -LiteralPath $target) { Assert-Bundle $target $packs }
$cache = if ($AssetDirectory) { (Resolve-Path -LiteralPath $AssetDirectory).Path } else { Join-Path $build 'preview-pack-assets' }
New-Item -ItemType Directory -Path $cache -Force | Out-Null
Assert-RegularPath $cache $true
$expectedAssets = [ordered]@{}
foreach ($name in $metadata.Keys) { $expectedAssets.Add($name, $metadata[$name]) }
foreach ($key in $packs.Keys) {
    foreach ($entry in $packs[$key]) {
        $expectedAssets.Add($entry.download_name, @{ size = $entry.packed_size; sha256 = $entry.packed_sha256 })
    }
}
if (-not $AssetDirectory) {
    Get-Command gh -ErrorAction Stop | Out-Null
    $releaseText = & gh release view $tag --repo $repository --json tagName,isDraft,assets
    if ($LASTEXITCODE -ne 0) { throw 'Could not inspect the pinned preview pack release' }
    $release = $releaseText | ConvertFrom-Json
    if ($release.isDraft -or $release.tagName -cne $tag) { throw 'Preview pack source must be the pinned published release' }
    foreach ($name in $expectedAssets.Keys) {
        $asset = @($release.assets | Where-Object { $_.name -ceq $name })
        $expected = $expectedAssets[$name]
        if ($asset.Count -ne 1 -or $asset[0].size -ne $expected.size -or $asset[0].digest -cne "sha256:$($expected.sha256)") {
            throw "Pinned release asset metadata verification failed: $name"
        }
        $path = Join-Path $cache $name
        if (-not (Test-Path -LiteralPath $path)) {
            & gh release download $tag --repo $repository --pattern $name --dir $cache
            if ($LASTEXITCODE -ne 0) { throw "Could not download pinned release asset: $name" }
        }
    }
}
foreach ($name in $expectedAssets.Keys) {
    $entry = $expectedAssets[$name]
    Assert-File (Join-Path $cache $name) $entry.size $entry.sha256
}
# Both the metadata and the native catalog are pinned; compare them before unpacking.
$assetManifest = Get-Content -LiteralPath (Join-Path $cache 'preview-packs-assets.json') -Raw | ConvertFrom-Json
if ($assetManifest.release_tag -cne $tag -or $assetManifest.assets.Count -ne 17) { throw 'Unexpected release asset manifest' }
foreach ($key in $packs.Keys) {
    $manifest = Get-Content -LiteralPath (Join-Path $cache "pulse-pack-$key-$version.json") -Raw | ConvertFrom-Json
    if ($manifest.id -cne $key -or $manifest.version -cne $version -or $manifest.release_tag -cne $tag -or
        $manifest.files.Count -ne $packs[$key].Count) { throw "Unexpected preview pack manifest: $key" }
    foreach ($entry in $packs[$key]) {
        $remote = @($manifest.files | Where-Object { $_.name -ceq $entry.name })
        if ($remote.Count -ne 1) { throw "Manifest lacks the pinned preview file: $key/$($entry.name)" }
        foreach ($field in @('name', 'size', 'sha256', 'packed_size', 'packed_sha256', 'download_name')) {
            if ($remote[0].$field -cne $entry[$field]) { throw "Manifest differs from catalog: $key/$($entry.name)/$field" }
        }
        $asset = @($assetManifest.assets | Where-Object { $_.name -ceq $entry.download_name })
        if ($asset.Count -ne 1 -or $asset[0].size -ne $entry.packed_size -or $asset[0].sha256 -cne $entry.packed_sha256) {
            throw "Release manifest differs from catalog: $($entry.download_name)"
        }
    }
}

if (-not ('PulsePackBundle.Lzms' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;

namespace PulsePackBundle {
    public static class Lzms {
        [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
        [DllImport("cabinet.dll", SetLastError = true)]
        private static extern bool CreateDecompressor(uint algorithm, IntPtr allocation, out IntPtr handle);
        [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
        [DllImport("cabinet.dll", SetLastError = true)]
        private static extern bool Decompress(IntPtr handle, byte[] input, UIntPtr inputSize,
            byte[] output, UIntPtr outputSize, out UIntPtr written);
        [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
        [DllImport("cabinet.dll", SetLastError = true)]
        private static extern bool CloseDecompressor(IntPtr handle);

        public static void Unpack(string source, string destination, int size) {
            if (size <= 0 || size > 256 * 1024 * 1024) throw new ArgumentOutOfRangeException(nameof(size));
            byte[] input = File.ReadAllBytes(source);
            byte[] output = new byte[size];
            IntPtr handle;
            if (!CreateDecompressor(5, IntPtr.Zero, out handle)) throw new Win32Exception(Marshal.GetLastWin32Error());
            try {
                UIntPtr written;
                if (!Decompress(handle, input, (UIntPtr)input.Length, output, (UIntPtr)output.Length, out written)) {
                    throw new Win32Exception(Marshal.GetLastWin32Error());
                }
                if (written.ToUInt64() != (ulong)size) throw new InvalidDataException("Unexpected decompressed length");
                File.WriteAllBytes(destination, output);
            } finally {
                CloseDecompressor(handle);
            }
        }
    }
}
'@
}
$stage = Join-Path $build ("packs.stage-" + [guid]::NewGuid().ToString('N'))
$previous = Join-Path $build ("packs.previous-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage | Out-Null
try {
    foreach ($key in $packs.Keys) {
        $base = Join-Path $stage $key
        $directory = Join-Path $base $version
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
        foreach ($entry in $packs[$key]) {
            $packedPath = Join-Path $cache $entry.download_name
            # Recheck immediately before decoding, including when the download cache is reused.
            Assert-File $packedPath $entry.packed_size $entry.packed_sha256
            $path = Join-Path $directory $entry.name
            [PulsePackBundle.Lzms]::Unpack($packedPath, $path, [int]$entry.size)
            Assert-File $path $entry.size $entry.sha256
        }
        @{ version = $version; dir = $version } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $base 'installed.json') -Encoding utf8NoBOM
    }
    foreach ($name in $metadata.Keys) { Copy-Item -LiteralPath (Join-Path $cache $name) -Destination $stage }
    [ordered]@{ schema = 1; repository = $repository; release_tag = $tag; version = $version } |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $stage 'bundled-packs.json') -Encoding utf8NoBOM
    Assert-Bundle $stage $packs
    if (Test-Path -LiteralPath $target) { Move-Item -LiteralPath $target -Destination $previous }
    try {
        Move-Item -LiteralPath $stage -Destination $target
    } catch {
        if (Test-Path -LiteralPath $previous) { Move-Item -LiteralPath $previous -Destination $target }
        throw
    }
    Remove-Stage $previous
} finally {
    Remove-Stage $stage
}
Write-Output "Bundled four verified preview packs from $repository@$tag ($version): $target"
