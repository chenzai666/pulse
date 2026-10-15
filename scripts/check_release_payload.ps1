param(
    [Parameter(Mandatory = $true)]
    [string]$BuildDir
)

$ErrorActionPreference = 'Stop'
$path = Join-Path $BuildDir 'pulse.exe'
# Inspect the binary as well as build settings: /skipbuild can reuse an old EXE.
$bytes = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $path).Path)
$text = [Text.Encoding]::ASCII.GetString($bytes)
$marker = [Text.Encoding]::ASCII.GetString(
    [Text.Encoding]::Unicode.GetBytes('PULSE_SELFTEST_CASE'))
if ($text.Contains($marker)) {
    throw 'Release payload contains the embedded selftest suite. Rebuild with PULSE_WITH_SELFTEST=OFF before packaging.'
}
$customMarker = [Text.Encoding]::ASCII.GetString([Text.Encoding]::Unicode.GetBytes('PulseCustomMainWindow'))
if ($text.Contains($customMarker)) {
    foreach ($projectLink in @('https://github.com/chenzai666/pulse', 'https://github.com/chenzai666/pulse/releases')) {
        $linkMarker = [Text.Encoding]::ASCII.GetString([Text.Encoding]::Unicode.GetBytes($projectLink))
        if (-not $text.Contains($linkMarker)) { throw "定制版项目入口未指向当前仓库：$projectLink" }
    }
}
Write-Output 'Release payload check passed: no embedded selftest dispatcher.'
