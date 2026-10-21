#requires -Version 7.2
param([switch]$NativeHandoff, [switch]$ReuseNativeVerification, [switch]$ExplorerReveal)
$ErrorActionPreference = 'Stop'
if ($ReuseNativeVerification -and -not $NativeHandoff) { throw '复用证据必须指定原生接管验证范围' }
$env:PYTHONUTF8 = '1'
$env:PYTHONIOENCODING = 'utf-8'
$taskRepo = Split-Path -Parent $PSScriptRoot
Set-Location $taskRepo
$baseCommit = 'f93f9ba7fe7b927e2a3f30a160e409d192e98e39'
$baseArtifact = '12248306000'
$baseDigest = 'f78faaa087894c2e3a6b58361d0817d82aaf90a473da521c0d9be7a841998025'
$taskBuild = Join-Path $taskRepo 'build-custom'
New-Item -ItemType Directory -Path $taskBuild -Force | Out-Null

# 仅替换主程序；任何已有组件源码变化都必须走完整构建。
$allowed = @('.github/workflows/custom-build.yml', '.github/workflows/picker.yml', '.github/workflows/picker-plugin.yml', 'scripts/build_picker_ci.ps1', 'scripts/verify_native_handoff_evidence.ps1', 'scripts/package_custom.ps1',
    'src/app/native_dialog_picker_host.cpp', 'src/app/native_dialog_history.h',
    'src/ui/folder_picker_dialog.cpp', 'src/ui/folder_picker_dialog.h', 'src/ui/ui_renderer.cpp', 'src/ui/ui_renderer.h',
    'src/bench/file_picker_dialog_test.cpp')
if ($NativeHandoff -or $ExplorerReveal) {
    $allowed += @('src/app/native_dialog_takeover.cpp', 'src/app/native_file_dialog_adapter.cpp',
        'src/app/native_file_dialog_adapter.h', 'src/app/native_dialog_takeover_test.cpp',
        'src/bench/native_dialog_fixture.cpp', 'src/bench/native_file_dialog_adapter_test.cpp')
}
if ($ExplorerReveal) {
    $allowed += @('CMakeLists.txt', 'src/app/explorer_window_takeover.cpp', 'src/app/explorer_takeover_guard.h',
        'src/bench/explorer_takeover_guard_test.cpp')
}
$changed = @(& git diff --name-only $baseCommit HEAD)
if ($LASTEXITCODE) {
    & git fetch --no-tags --depth=1 origin $baseCommit
    if ($LASTEXITCODE) { throw '无法读取固定组件源码版本' }
    $changed = @(& git diff --name-only $baseCommit HEAD)
    if ($LASTEXITCODE) { throw '无法比较组件源码版本' }
}
foreach ($file in $changed) {
    if ($file -notin $allowed -and -not $file.StartsWith('integrations/Listary.Picker/')) { throw "文件窗口定向构建不支持此改动，请执行完整构建：$file" }
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
    Tee-Object -FilePath (Join-Path $taskBuild 'status-center-base-verification.log')
if ($LASTEXITCODE) { throw '完整组件逐文件验证失败' }
$payload = Join-Path $baseRoot ('Pulse-Custom-' + $baseCommit.Substring(0,9))

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsRoot = & $vswhere -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw '需要 Visual Studio 2022 C++ 工具' }
$vcvars = Join-Path $vsRoot 'VC/Auxiliary/Build/vcvars64.bat'
$sdkRoot = & "$PSScriptRoot/verify_lumatext_sdk.ps1"
$buildBatch = Join-Path $taskBuild 'compile-picker.bat'
$selftestMode = if ($NativeHandoff -and -not $ReuseNativeVerification) { 'ON' } else { 'OFF' }
$targets = if ($NativeHandoff -and -not $ReuseNativeVerification) { 'pulse pulse_native_dialog_fixture pulse_native_file_dialog_adapter_test' }
    elseif ($ExplorerReveal) { 'pulse pulse_explorer_takeover_guard_test' } else { 'pulse' }
@"
@echo off
call "$vcvars"
if errorlevel 1 exit /b 1
chcp 65001 >nul
cmake -S "$taskRepo" -B "$taskBuild" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DPULSE_CUSTOM_EDITION=ON -DPULSE_WITH_SELFTEST=$selftestMode -DPULSE_WITH_LUMATEXT=ON -DCMAKE_PREFIX_PATH="$sdkRoot" -DPULSE_UPDATE_MANIFEST_URL= -DPULSE_UPDATE_PUBLIC_KEY_HEX=
if errorlevel 1 exit /b 1
cmake --build "$taskBuild" --parallel 4 --target $targets
exit /b %errorlevel%
"@ | Set-Content -LiteralPath $buildBatch -Encoding utf8
$galleryOutput = Join-Path $taskBuild 'picker-verification'
New-Item -ItemType Directory -Path $galleryOutput -Force | Out-Null
if ($ExplorerReveal) {
    $guardBatch = Join-Path $taskBuild 'compile-explorer-guard-first.bat'
    $guardBuild = [IO.File]::ReadAllText($buildBatch).Replace("--target $targets", '--target pulse_explorer_takeover_guard_test')
    [IO.File]::WriteAllText($guardBatch, $guardBuild, [Text.UTF8Encoding]::new($false))
    & $guardBatch
    if ($LASTEXITCODE) { throw '资源管理器接管保护构建失败' }
    & (Join-Path $taskBuild 'pulse_explorer_takeover_guard_test.exe') | Tee-Object -FilePath (Join-Path $galleryOutput 'explorer-guard.log')
    if ($LASTEXITCODE) { throw '资源管理器接管保护回归失败' }
}
if ($ReuseNativeVerification) { & "$PSScriptRoot/verify_native_handoff_evidence.ps1" -OutputDirectory $galleryOutput }
if ($NativeHandoff -and -not $ReuseNativeVerification) {
    $adapterBatch = Join-Path $taskBuild 'compile-picker-adapter-first.bat'
    $adapterBuild = [IO.File]::ReadAllText($buildBatch).Replace("--target $targets", '--target pulse_native_file_dialog_adapter_test')
    [IO.File]::WriteAllText($adapterBatch, $adapterBuild, [Text.UTF8Encoding]::new($false))
    & $adapterBatch
    if ($LASTEXITCODE) { throw '原生适配器构建失败' }
    $adapter = Start-Process -FilePath (Join-Path $taskBuild 'pulse_native_file_dialog_adapter_test.exe') -ArgumentList '--interaction' -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $galleryOutput 'adapter.log') -RedirectStandardError (Join-Path $galleryOutput 'adapter-error.log')
    if (-not $adapter.WaitForExit(180000)) { $adapter.Kill(); throw '原生窗口提交验证超时' }
    $adapter.Refresh()
    if ($adapter.ExitCode) { throw '原生窗口单次提交验证失败' }
}
& $buildBatch
if ($LASTEXITCODE) { throw '文件窗口定向构建失败' }
if ($NativeHandoff -and -not $ReuseNativeVerification) {
    $env:PULSE_SELFTEST_CASE = 'native-dialog-takeover'
    $env:PULSE_SELFTEST_NO_SCREENSHOTS = '1'
    $env:PULSE_TEST_DATA_DIR = Join-Path $taskBuild 'isolated-native-picker-data'
    $env:PULSE_TEST_NATIVE_HANDOFF_DELAY = '1'
    New-Item -ItemType Directory -Path 'bench_data' -Force | Out-Null
    $handoff = Start-Process -FilePath (Join-Path $taskBuild 'pulse.exe') -ArgumentList '--selftest' -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $galleryOutput 'handoff-output.log') -RedirectStandardError (Join-Path $galleryOutput 'handoff-error.log')
    if (-not $handoff.WaitForExit(300000)) { $handoff.Kill(); throw '原生窗口接管链路验证超时' }
    $handoff.Refresh()
    Copy-Item 'bench_data/native-dialogs-takeover.log' (Join-Path $galleryOutput 'handoff.log')
    Remove-Item Env:PULSE_SELFTEST_CASE, Env:PULSE_SELFTEST_NO_SCREENSHOTS, Env:PULSE_TEST_DATA_DIR, Env:PULSE_TEST_NATIVE_HANDOFF_DELAY -ErrorAction SilentlyContinue
    if ($handoff.ExitCode) { throw '原生窗口接管链路验证失败' }
    $releaseBatch = Join-Path $taskBuild 'compile-picker-release.bat'
    $releaseBuild = [IO.File]::ReadAllText($buildBatch).Replace('-DPULSE_WITH_SELFTEST=ON', '-DPULSE_WITH_SELFTEST=OFF').Replace("--target $targets", '--target pulse')
    [IO.File]::WriteAllText($releaseBatch, $releaseBuild, [Text.UTF8Encoding]::new($false))
    & $releaseBatch
    if ($LASTEXITCODE) { throw '关闭内置测试后构建正式程序失败' }
}
foreach ($case in @('history', 'save')) {
    $env:PULSE_PICKER_TEST_SNAPSHOT = Join-Path $galleryOutput ($case + '.png')
    $test = Start-Process -FilePath (Join-Path $taskBuild 'pulse.exe') -ArgumentList '--picker-dialog-test',("--"+$case) -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $galleryOutput ($case+'.log')) -RedirectStandardError (Join-Path $galleryOutput ($case+'-error.log'))
    if (-not $test.WaitForExit(60000)) { $test.Kill(); throw "文件窗口验证超时：$case" }
    $test.Refresh()
    Get-Content -LiteralPath (Join-Path $galleryOutput ($case+'.log'))
    if ($test.ExitCode) { throw "文件窗口验证失败：$case" }
}
Remove-Item Env:PULSE_PICKER_TEST_SNAPSHOT -ErrorAction SilentlyContinue
$pluginProject = 'integrations/Listary.Picker/Listary.FileAppPlugin.PulsePicker.csproj'
dotnet restore $pluginProject --locked-mode --configfile 'integrations/Listary.Picker/NuGet.config' --nologo
if ($LASTEXITCODE) { throw 'Listary 对话框适配依赖校验失败' }
$pluginOutput = Join-Path $payload 'listary-picker'
dotnet build $pluginProject -c Release --no-restore --nologo -o $pluginOutput
if ($LASTEXITCODE) { throw 'Listary 对话框适配编译失败' }
Copy-Item 'integrations/Listary.Picker/README.md' -Destination $pluginOutput
Copy-Item -LiteralPath (Join-Path $taskBuild 'pulse.exe') -Destination (Join-Path $payload 'pulse.exe')
Copy-Item -LiteralPath (Join-Path $taskBuild 'lumatext.dll') -Destination (Join-Path $payload 'lumatext.dll')
& "$PSScriptRoot/package_custom.ps1" -BuildDir $payload

# 更新包仅包含逐文件比较后变化的载荷，安装前还需核对基线清单。
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
