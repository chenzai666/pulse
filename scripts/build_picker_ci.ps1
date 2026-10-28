#requires -Version 7.2
param([switch]$ReuseRemovalVerification, [switch]$BuildOnly)
$ErrorActionPreference = 'Stop'
$env:PYTHONUTF8 = '1'
$env:PYTHONIOENCODING = 'utf-8'
$taskRepo = Split-Path -Parent $PSScriptRoot
Set-Location $taskRepo
$baseCommit = 'f93f9ba7fe7b927e2a3f30a160e409d192e98e39'
$baseArtifact = '12248306000'
$baseDigest = 'f78faaa087894c2e3a6b58361d0817d82aaf90a473da521c0d9be7a841998025'
$taskBuild = Join-Path $taskRepo 'build-custom'
New-Item -ItemType Directory -Path $taskBuild -Force | Out-Null

# 仅允许主程序与撤回功能的源码变化，完整组件使用固定清单逐文件核验。
$allowed = @('.github/workflows/custom-build.yml', '.github/workflows/picker.yml', '.github/workflows/picker-plugin.yml',
    'scripts/build_picker_ci.ps1', 'scripts/build_tab_drag_ci.ps1', 'scripts/verify_native_handoff_evidence.ps1',
    'scripts/package_custom.ps1', 'scripts/check_release_payload.ps1', 'scripts/verify_dialog_removal_evidence.py', 'scripts/publish_picker_release.ps1', 'docs/custom-edition.md',
    'CMakeLists.txt', 'integrations/NativeDialogs/README.md',
    'src/app/app_commands.cpp', 'src/app/app_core.h', 'src/app/app_main.cpp', 'src/app/app_runtime.cpp',
    'src/app/app_prefs.cpp', 'src/app/app_prefs.h', 'src/app/settings_controller.cpp', 'src/app/settings_controller.h',
    'src/app/settings_flow_test.cpp', 'src/app/selftest_1b2.cpp', 'src/app/shell_window_sync.cpp', 'src/app/shell_window_sync.h',
    'src/app/native_dialog_picker_host.cpp', 'src/app/native_dialog_history.h', 'src/app/native_dialog_takeover.cpp',
    'src/app/native_dialog_takeover.h', 'src/app/native_dialog_takeover_test.cpp', 'src/app/native_dialog_wire.h',
    'src/app/native_dialog_wire_test.cpp', 'src/app/native_file_dialog_adapter.cpp', 'src/app/native_file_dialog_adapter.h',
    'src/app/folder_source.h', 'src/app/native_folder_follow.h', 'src/bench/native_folder_follow_test.cpp', 'src/bench/folder_source_test.cpp',
    'src/app/native_dialog_folder_link.h', 'src/app/native_dialog_folder_link.cpp', 'src/app/native_dialog_folder_link_policy.h',
    'src/bench/native_dialog_folder_link_policy_test.cpp', 'src/bench/native_dialog_folder_link_driver.cpp', 'src/bench/native_dialog_folder_link_sequence.cpp', 'src/bench/native_dialog_folder_link_lifecycle_test.cpp',
    'src/bench/native_dialog_folder_link_fixture.cs',
    'src/bench/native_dialog_fixture.cpp', 'src/bench/native_file_dialog_adapter_test.cpp',
    'src/app/explorer_window_takeover.cpp', 'src/app/explorer_takeover_guard.h',
    'src/bench/explorer_takeover_guard_test.cpp', 'src/bench/explorer_takeover_connection_test.cpp',
    'src/ui/folder_picker_dialog.cpp', 'src/ui/folder_picker_dialog.h', 'src/bench/file_picker_dialog_test.cpp',
    'src/ui/ui_renderer.cpp', 'src/ui/ui_renderer.h', 'src/ui/ui_renderer_internal.h', 'src/ui/ui_hit_test.cpp',
    'src/ui/ui_settings_core.cpp', 'src/ui/settings_layout_sections.h')
$changed = @(& git diff --name-only $baseCommit HEAD)
if ($LASTEXITCODE) {
    & git fetch --no-tags --depth=1 origin $baseCommit
    if ($LASTEXITCODE) { throw '无法读取固定组件源码版本' }
    $changed = @(& git diff --name-only $baseCommit HEAD)
    if ($LASTEXITCODE) { throw '无法比较固定组件源码版本' }
}
foreach ($file in $changed) {
    if ($file -notin $allowed -and -not $file.StartsWith('integrations/Listary.Picker/') -and
        -not $file.StartsWith('integrations/Listary.Picker.Source/')) { throw "定向构建不支持此改动：$file" }
}
$env:PULSE_BASE_ARTIFACT = $baseArtifact
$env:PULSE_BASE_DIGEST = $baseDigest
$env:PULSE_BASE_OUTPUT = Join-Path $taskBuild 'base-artifact.zip'
@'
import hashlib, os, urllib.error, urllib.request
class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args): return None
url = 'https://api.github.com/repos/chenzai666/pulse/actions/artifacts/' + os.environ['PULSE_BASE_ARTIFACT'] + '/zip'
request = urllib.request.Request(url, headers={'Authorization': 'Bearer ' + os.environ['GH_TOKEN']})
try:
    reply = urllib.request.build_opener(NoRedirect).open(request, timeout=30)
except urllib.error.HTTPError as error:
    reply = error
if reply.code != 302: raise RuntimeError('固定包读取失败: HTTP ' + str(reply.code))
location = reply.headers['Location']
reply.close()
digest = hashlib.sha256()
with urllib.request.urlopen(location, timeout=120) as reader, open(os.environ['PULSE_BASE_OUTPUT'], 'xb') as writer:
    while block := reader.read(1024 * 1024):
        digest.update(block); writer.write(block)
if digest.hexdigest() != os.environ['PULSE_BASE_DIGEST']: raise RuntimeError('固定包摘要不匹配')
print('固定完整包摘要通过')
'@ | python -
if ($LASTEXITCODE) { throw '固定组件下载失败' }
$baseRoot = Join-Path $taskBuild 'verified-base'
python scripts/verify_custom_package.py $env:PULSE_BASE_OUTPUT $baseCommit $baseRoot
if ($LASTEXITCODE) { throw '完整组件逐文件验证失败' }
$payload = Join-Path $baseRoot ('Pulse-Custom-' + $baseCommit.Substring(0, 9))
$galleryOutput = Join-Path $taskBuild 'picker-verification'
New-Item -ItemType Directory -Path $galleryOutput -Force | Out-Null
if ($ReuseRemovalVerification) {
    python scripts/verify_dialog_removal_evidence.py $galleryOutput
    if ($LASTEXITCODE) { throw '已通过的设置回归证据核对失败' }
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsRoot = & $vswhere -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw '需要 Visual Studio 2022 C++ 工具' }
$vcvars = Join-Path $vsRoot 'VC/Auxiliary/Build/vcvars64.bat'
$sdkRoot = & "$PSScriptRoot/verify_lumatext_sdk.ps1"
$buildBatch = Join-Path $taskBuild 'compile-picker.bat'
$selftestMode = if ($ReuseRemovalVerification -or $BuildOnly) { 'OFF' } else { 'ON' }
$skipRuntime = if ($BuildOnly) { 'goto :compile_release' } else { 'rem Run regression checks' }
@"
@echo off
call "$vcvars"
if errorlevel 1 exit /b 1
chcp 65001 >nul
rem Pure transition regression has no windows, input or user configuration.
cl /nologo /std:c++20 /EHsc /W4 /WX "$taskRepo\src\bench\native_dialog_folder_link_policy_test.cpp" /Fe:"$taskBuild\native-dialog-folder-link-policy-test.exe" /Fo:"$taskBuild\native-dialog-folder-link-policy-test.obj"
if errorlevel 1 exit /b 1
"$taskBuild\native-dialog-folder-link-policy-test.exe" > "$galleryOutput\native-dialog-folder-link-policy.log" 2>&1
if errorlevel 1 exit /b 1
$skipRuntime
cl /nologo /std:c++20 /EHsc /W4 /WX "$taskRepo\src\bench\folder_source_test.cpp" /Fe:"$taskBuild\folder-source-test.exe" /Fo:"$taskBuild\folder-source-test.obj" user32.lib
if errorlevel 1 exit /b 1
"$taskBuild\folder-source-test.exe"
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /WX /DPULSE_WITH_SELFTEST=1 "$taskRepo\src\bench\native_dialog_folder_link_driver.cpp" "$taskRepo\src\bench\native_dialog_folder_link_sequence.cpp" /Fe:"$taskBuild\native-dialog-folder-link-driver.exe" /Fo:"$taskBuild\\" /link user32.lib advapi32.lib ole32.lib oleaut32.lib uiautomationcore.lib uuid.lib
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /WX /DPULSE_WITH_SELFTEST=1 "$taskRepo\src\bench\native_dialog_folder_link_lifecycle_test.cpp" "$taskRepo\src\app\native_dialog_folder_link.cpp" /Fe:"$taskBuild\native-dialog-folder-link-lifecycle-test.exe" /Fo:"$taskBuild\\" /link user32.lib advapi32.lib ole32.lib oleaut32.lib uiautomationcore.lib uuid.lib
if errorlevel 1 exit /b 1
"$taskBuild\native-dialog-folder-link-lifecycle-test.exe" > "$galleryOutput\native-dialog-folder-link-lifecycle.log" 2>&1
if errorlevel 1 exit /b 1
"%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe" /nologo /target:exe /out:"$taskBuild\native-dialog-folder-link-fixture.exe" "$taskRepo\src\bench\native_dialog_folder_link_fixture.cs"
if errorlevel 1 exit /b 1
"$taskBuild\native-dialog-folder-link-fixture.exe" > "$galleryOutput\native-dialog-folder-link.log" 2>&1
if errorlevel 1 exit /b 1
:compile_release
cmake -S "$taskRepo" -B "$taskBuild" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DPULSE_CUSTOM_EDITION=ON -DPULSE_WITH_SELFTEST=$selftestMode -DPULSE_WITH_LUMATEXT=ON -DCMAKE_PREFIX_PATH="$sdkRoot" -DPULSE_UPDATE_MANIFEST_URL= -DPULSE_UPDATE_PUBLIC_KEY_HEX=
if errorlevel 1 exit /b 1
cmake --build "$taskBuild" --parallel 4 --target pulse pulse_native_folder_follow_test
exit /b %errorlevel%
"@ | Set-Content -LiteralPath $buildBatch -Encoding utf8
& $buildBatch
if ($LASTEXITCODE) { throw '系统集成设置验证版构建失败' }
if (-not $ReuseRemovalVerification -and -not $BuildOnly) {
$env:PULSE_TEST_SETTINGS_FLOW = Join-Path $galleryOutput 'settings-integration.log'
$env:PULSE_TEST_SETTINGS_INTEGRATION = '1'
$env:PULSE_TEST_DATA_DIR = Join-Path $taskBuild 'isolated-native-dialog-removal'
try {
    $test = Start-Process -FilePath (Join-Path $taskBuild 'pulse.exe') -ArgumentList '--test-instance' -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $galleryOutput 'settings-output.log') -RedirectStandardError (Join-Path $galleryOutput 'settings-error.log')
    if (-not $test.WaitForExit(60000)) { $test.Kill(); $test.WaitForExit(); throw '系统集成设置回归超时' }
    if ($test.ExitCode) { throw '系统集成设置回归失败' }
    Get-Content -LiteralPath $env:PULSE_TEST_SETTINGS_FLOW
} finally {
    Remove-Item Env:PULSE_TEST_SETTINGS_FLOW, Env:PULSE_TEST_SETTINGS_INTEGRATION, Env:PULSE_TEST_DATA_DIR -ErrorAction SilentlyContinue
}
}
if (-not $BuildOnly) {
foreach ($case in @('save', 'columns')) {
    $env:PULSE_PICKER_TEST_SNAPSHOT = Join-Path $galleryOutput ($case + '.png')
    $test = Start-Process -FilePath (Join-Path $taskBuild 'pulse.exe') -ArgumentList '--picker-dialog-test', ('--' + $case) -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $galleryOutput ($case + '.log')) -RedirectStandardError (Join-Path $galleryOutput ($case + '-error.log'))
    if (-not $test.WaitForExit(60000)) { $test.Kill(); $test.WaitForExit(); throw "内部选择器回归超时：$case" }
    Get-Content -LiteralPath (Join-Path $galleryOutput ($case + '.log'))
    if ($test.ExitCode) { throw "内部选择器回归失败：$case" }
}
Remove-Item Env:PULSE_PICKER_TEST_SNAPSHOT -ErrorAction SilentlyContinue
}
if (-not $ReuseRemovalVerification -and -not $BuildOnly) {
$releaseBatch = Join-Path $taskBuild 'compile-picker-release.bat'
[IO.File]::WriteAllText($releaseBatch, [IO.File]::ReadAllText($buildBatch).Replace('-DPULSE_WITH_SELFTEST=ON', '-DPULSE_WITH_SELFTEST=OFF'), [Text.UTF8Encoding]::new($false))
& $releaseBatch
if ($LASTEXITCODE) { throw '关闭自检后生成正式程序失败' }
}
& "$PSScriptRoot/check_release_payload.ps1" -BuildDir $taskBuild
Copy-Item -LiteralPath (Join-Path $taskBuild 'pulse.exe') -Destination (Join-Path $payload 'pulse.exe')
Copy-Item -LiteralPath (Join-Path $taskBuild 'lumatext.dll') -Destination (Join-Path $payload 'lumatext.dll')
& "$PSScriptRoot/package_custom.ps1" -BuildDir $payload
$buildInfo = Get-Content -LiteralPath 'dist/custom-build.json' -Raw | ConvertFrom-Json
$stage = Join-Path $taskRepo ('dist/' + $buildInfo.package)
$baseManifest = Get-Content -LiteralPath (Join-Path $payload 'package-manifest.json') -Raw | ConvertFrom-Json
$nextManifest = Get-Content -LiteralPath (Join-Path $stage 'package-manifest.json') -Raw | ConvertFrom-Json
$baseFiles = @{}
foreach ($entry in $baseManifest.files) { $baseFiles[$entry.path] = $entry.sha256 }
$update = Join-Path $taskBuild 'picker-update'
New-Item -ItemType Directory -Path $update | Out-Null
foreach ($entry in $nextManifest.files) {
    if ($baseFiles[$entry.path] -eq $entry.sha256) { continue }
    $target = Join-Path $update $entry.path
    New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $stage $entry.path) -Destination $target
}
Copy-Item -LiteralPath (Join-Path $stage 'package-manifest.json') -Destination $update
[ordered]@{base_commit=$baseCommit;commit=$nextManifest.commit} | ConvertTo-Json |
    Set-Content -LiteralPath (Join-Path $update 'update-base.json') -Encoding utf8
