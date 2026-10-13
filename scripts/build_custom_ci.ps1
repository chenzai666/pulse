#requires -Version 7.2
param([string]$BuildDir = 'build-custom')
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path -Parent $PSScriptRoot
Set-Location $taskRepo
$taskBuild = [IO.Path]::GetFullPath((Join-Path $taskRepo $BuildDir))
if (-not $taskBuild.StartsWith($taskRepo + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw '构建目录必须位于当前仓库内'
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsRoot = & $vswhere -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw '需要 Visual Studio 2022 x64 C++ 工具' }
$vcvars = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
$sdkRoot = & "$PSScriptRoot/verify_lumatext_sdk.ps1"
New-Item -ItemType Directory -Path $taskBuild -Force | Out-Null
& "$PSScriptRoot/build_quicklook_host.ps1" -BuildDir $taskBuild
& "$PSScriptRoot/build_listary_plugin.ps1" -OutputPath (Join-Path $taskBuild 'ListaryPlugin')
& "$taskRepo/integrations/Listary.Pulse/tests/run_protocol_tests.ps1"
& "$PSScriptRoot/bundle_preview_packs.ps1" -BuildDir $taskBuild
& "$PSScriptRoot/bundle_preview_packs.ps1" -BuildDir $taskBuild -VerifyOnly
& "$PSScriptRoot/build_filee_host.ps1" -BuildDir $taskBuild
& dotnet run --project "$taskRepo/integrations/Filee.Host/tests/Pulse.Filee.Host.Tests.csproj" -c Release -- --runtime (Join-Path $taskBuild 'conversion/Pulse.Filee.Host.exe') |
    Tee-Object -FilePath (Join-Path $taskBuild 'filee-conversion-smoke.log')
if ($LASTEXITCODE) { throw 'Filee 与 Pandoc 定向转换验证失败' }
$buildBatch = Join-Path $taskBuild 'compile-custom.bat'
@"
@echo off
call "$vcvars"
if errorlevel 1 exit /b 1
chcp 65001 >nul
cmake -S "$taskRepo" -B "$taskBuild" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DPULSE_CUSTOM_EDITION=ON -DPULSE_WITH_SELFTEST=ON -DPULSE_WITH_LUMATEXT=ON -DCMAKE_PREFIX_PATH="$sdkRoot" -DPULSE_UPDATE_MANIFEST_URL= -DPULSE_UPDATE_PUBLIC_KEY_HEX=
if errorlevel 1 exit /b 1
cmake --build "$taskBuild" --parallel 4 --target pulse pulse_session_window_test pulse_pane_drop_test pulse_bundled_preview_packs_test pulse_drop_navigation_test pulse_file_conversion_protocol_test
exit /b %errorlevel%
"@ | Set-Content -LiteralPath $buildBatch -Encoding utf8
& $buildBatch
if ($LASTEXITCODE) { throw 'Pulse 定制版原生编译失败' }
foreach ($testName in @('pulse_session_window_test', 'pulse_pane_drop_test', 'pulse_bundled_preview_packs_test', 'pulse_drop_navigation_test', 'pulse_file_conversion_protocol_test')) {
    & (Join-Path $taskBuild "$testName.exe") | Tee-Object -FilePath (Join-Path $taskBuild "$testName.log")
    if ($LASTEXITCODE) { throw "定向回归失败：$testName" }
}
$env:PULSE_SELFTEST_NO_SCREENSHOTS = '1'
foreach ($testCase in @('tab-transfer', 'tab-pin-boundary', 'file-conversion-ui')) {
    $env:PULSE_SELFTEST_CASE = $testCase
    $testProcess = Start-Process -FilePath (Join-Path $taskBuild 'pulse.exe') -ArgumentList '--selftest' -WindowStyle Hidden -PassThru
    if (-not $testProcess.WaitForExit(60000)) {
        $testProcess.Kill()
        throw "标签定向回归超时：$testCase"
    }
    $testProcess.Refresh()
    Copy-Item -LiteralPath 'bench_data/selftest_1b2_last.log' -Destination (Join-Path $taskBuild "selftest-$testCase.log") -ErrorAction SilentlyContinue
    if ($testProcess.ExitCode) { throw "标签定向回归失败：$testCase" }
}
Remove-Item Env:PULSE_SELFTEST_CASE -ErrorAction SilentlyContinue
$productionBatch = Join-Path $taskBuild 'compile-production.bat'
@"
@echo off
call "$vcvars"
if errorlevel 1 exit /b 1
cmake -S "$taskRepo" -B "$taskBuild" -G Ninja -DPULSE_WITH_SELFTEST=OFF
if errorlevel 1 exit /b 1
cmake --build "$taskBuild" --parallel 4 --target pulse
exit /b %errorlevel%
"@ | Set-Content -LiteralPath $productionBatch -Encoding utf8
& $productionBatch
if ($LASTEXITCODE) { throw '移除测试入口后的正式原生编译失败' }
& "$PSScriptRoot/check_release_payload.ps1" -BuildDir $taskBuild
& "$PSScriptRoot/package_custom.ps1" -BuildDir $taskBuild
