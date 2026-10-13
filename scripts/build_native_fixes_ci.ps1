#requires -Version 7.2
$ErrorActionPreference = 'Stop'
$env:PYTHONUTF8 = '1'
$env:PYTHONIOENCODING = 'utf-8'
$taskRepo = Split-Path -Parent $PSScriptRoot
Set-Location $taskRepo
$baseCommit = '4c456fe91976cc784d65b2f92d7f4791d5416adf'
$baseArtifact = '11816006357'
$baseDigest = '8ce2ec0c31bd300e7559404b9033133cf613dcfe162ca709f837a247f9ee33ce'
$taskBuild = Join-Path $taskRepo 'build-custom'
New-Item -ItemType Directory -Path $taskBuild -Force | Out-Null

# 重编译本次受影响的原生组件；完整预览与转换运行时保持固定且逐文件校验。
$allowed = @('CMakeLists.txt', '.github/workflows/custom-build.yml', 'docs/custom-edition.md',
    'scripts/build_native_fixes_ci.ps1', 'scripts/build_custom_ci.ps1',
    'integrations/Listary.Pulse/README.md', 'integrations/QuickLook.FullHost/README.md',
    'src/app/app_commands.cpp', 'src/app/app_main.cpp', 'src/app/selftest_1b2.cpp',
    'src/app/tab_detach.cpp', 'src/app/tab_transfer_test.cpp',
    'src/app/global_search_controller.h', 'src/app/global_search_controller.cpp',
    'src/app/listary_search_entry.h', 'src/app/listary_search_entry.cpp', 'src/app/listary_search_entry_test.cpp',
    'src/app/shell_integration_registry.cpp', 'src/bench/integration_registry_test.cpp',
    'src/integration_host/main.cpp', 'src/ipc/preview_protocol.h',
    'src/preview_host/main.cpp', 'src/preview_host/preview_decoders.cpp',
    'src/ui/quick_preview_window.h', 'src/ui/quick_preview_window.cpp', 'src/ui/quick_preview_pages.cpp',
    'src/bench/preview_test_main.cpp', 'src/bench/quick_preview_audit_test.cpp')
$changed = @(& git diff --name-only $baseCommit HEAD)
if ($LASTEXITCODE) {
    & git fetch --no-tags --depth=1 origin $baseCommit
    if ($LASTEXITCODE) { throw '无法读取固定组件源码版本' }
    $changed = @(& git diff --name-only $baseCommit HEAD)
    if ($LASTEXITCODE) { throw '无法比较组件源码版本' }
}
foreach ($file in $changed) {
    if ($file -notin $allowed) { throw "原生修复定向构建不支持此改动，请执行完整构建：$file" }
}

$env:PULSE_BASE_ARTIFACT = $baseArtifact
$env:PULSE_BASE_DIGEST = $baseDigest
$env:PULSE_BASE_OUTPUT = Join-Path $taskBuild 'base-artifact.zip'
@'
import hashlib, os, urllib.error, urllib.request
token = os.environ['GH_TOKEN']
url = 'https://api.github.com/repos/chenzai666/pulse/actions/artifacts/' + os.environ['PULSE_BASE_ARTIFACT'] + '/zip'
# 不将 GitHub token 转发给 artifact CDN。
class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args): return None
try:
    reply = urllib.request.build_opener(NoRedirect).open(urllib.request.Request(url, headers={'Authorization': 'Bearer ' + token}), timeout=30)
except urllib.error.HTTPError as error:
    reply = error
if reply.code != 302: raise RuntimeError('无法读取固定完整包: HTTP ' + str(reply.code))
location = reply.headers['Location']
reply.close()
digest = hashlib.sha256()
with urllib.request.urlopen(location, timeout=120) as reader, open(os.environ['PULSE_BASE_OUTPUT'], 'xb') as writer:
    while block := reader.read(1024*1024):
        digest.update(block); writer.write(block)
if digest.hexdigest() != os.environ['PULSE_BASE_DIGEST']: raise RuntimeError('固定完整包摘要不匹配')
print('固定完整包下载摘要已验证')
'@ | python -
if ($LASTEXITCODE) { throw '完整组件下载或摘要验证失败' }
$baseRoot = Join-Path $taskBuild 'verified-base'
python scripts/verify_custom_package.py $env:PULSE_BASE_OUTPUT $baseCommit $baseRoot |
    Tee-Object -FilePath (Join-Path $taskBuild 'native-base-verification.log')
if ($LASTEXITCODE) { throw '完整组件逐文件验证失败' }
$payload = Join-Path $baseRoot ('Pulse-Custom-' + $baseCommit.Substring(0,9))

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsRoot = & $vswhere -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw '需要 Visual Studio 2022 C++ 工具' }
$vcvars = Join-Path $vsRoot 'VC/Auxiliary/Build/vcvars64.bat'
$sdkRoot = & "$PSScriptRoot/verify_lumatext_sdk.ps1"
$buildBatch = Join-Path $taskBuild 'compile-native-fixes.bat'
@"
@echo off
call "$vcvars"
if errorlevel 1 exit /b 1
chcp 65001 >nul
cmake -S "$taskRepo" -B "$taskBuild" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DPULSE_CUSTOM_EDITION=ON -DPULSE_WITH_SELFTEST=ON -DPULSE_WITH_LUMATEXT=ON -DCMAKE_PREFIX_PATH="$sdkRoot" -DPULSE_UPDATE_MANIFEST_URL= -DPULSE_UPDATE_PUBLIC_KEY_HEX=
if errorlevel 1 exit /b 1
cmake --build "$taskBuild" --parallel 4 --target pulse pulse_document pulse_preview_test pulse_integration_registry_test pulse_open_handoff_test pulse_pane_drop_test pulse_session_window_test
exit /b %errorlevel%
"@ | Set-Content -LiteralPath $buildBatch -Encoding utf8
& $buildBatch
if ($LASTEXITCODE) { throw '原生修复定向构建失败' }
$testFailures = [System.Collections.Generic.List[string]]::new()
foreach ($test in @('pulse_open_handoff_test', 'pulse_pane_drop_test', 'pulse_session_window_test')) {
    & (Join-Path $taskBuild "$test.exe") | Tee-Object -FilePath (Join-Path $taskBuild "$test.log")
    if ($LASTEXITCODE) { $testFailures.Add($test) }
}
& (Join-Path $taskBuild 'pulse_integration_registry_test.exe') --recycle-bin-only |
    Tee-Object -FilePath (Join-Path $taskBuild 'recycle-bin-registry.log')
if ($LASTEXITCODE) { $testFailures.Add('recycle-bin-registry') }
& (Join-Path $taskBuild 'pulse_preview_test.exe') --native-image-regression |
    Tee-Object -FilePath (Join-Path $taskBuild 'native-image-regression.log')
if ($LASTEXITCODE) { $testFailures.Add('native-image-regression') }
$env:PULSE_SELFTEST_NO_SCREENSHOTS = '1'
foreach ($testCase in @('tab-transfer', 'tab-pin-boundary', 'listary-search-entry')) {
    $env:PULSE_SELFTEST_CASE = $testCase
    $process = Start-Process -FilePath (Join-Path $taskBuild 'pulse.exe') -ArgumentList '--selftest' -WindowStyle Hidden -PassThru
    if (-not $process.WaitForExit(60000)) { $process.Kill(); $process.WaitForExit(); $testFailures.Add("timeout: $testCase") }
    Copy-Item -LiteralPath 'bench_data/selftest_1b2_last.log' -Destination (Join-Path $taskBuild "selftest-$testCase.log") -ErrorAction SilentlyContinue
    $process.Refresh()
    if ($process.ExitCode) { $testFailures.Add($testCase) }
}
Remove-Item Env:PULSE_SELFTEST_CASE
$env:PULSE_TEST_NATIVE_IMAGE_REGRESSION = '1'
$imageProcess = Start-Process -FilePath (Join-Path $taskBuild 'pulse.exe') -ArgumentList '--quick-preview-audit' -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $taskBuild 'native-image-ui.log') -RedirectStandardError (Join-Path $taskBuild 'native-image-ui-error.log')
if (-not $imageProcess.WaitForExit(60000)) { $imageProcess.Kill(); $imageProcess.WaitForExit(); $testFailures.Add('timeout: native-image-ui') }
$imageProcess.Refresh()
if ($imageProcess.ExitCode) { $testFailures.Add('native-image-ui') }
Remove-Item Env:PULSE_TEST_NATIVE_IMAGE_REGRESSION
if ($testFailures.Count) { throw ('定向验证未通过，停止打包：' + ($testFailures -join ', ')) }
# 测试入口移除后再生成用户使用的主程序。
$productionBatch = Join-Path $taskBuild 'compile-native-production.bat'
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
if ($LASTEXITCODE) { throw '移除测试入口后的正式构建失败' }
. "$PSScriptRoot/release_payload.ps1"
foreach ($file in $PulseReleasePayload) {
    Copy-Item -LiteralPath (Join-Path $taskBuild $file) -Destination (Join-Path $payload $file)
}
& "$PSScriptRoot/package_custom.ps1" -BuildDir $payload
