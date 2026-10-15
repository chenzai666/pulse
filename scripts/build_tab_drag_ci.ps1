#requires -Version 7.2
param([switch]$DiagnosticsOnly, [switch]$VerifySettingsRegression, [switch]$FileUi, [switch]$HomeWidgetsOnly, [switch]$HomeMenuOnly, [switch]$HomePathDisplayOnly, [switch]$NativeDialogsOnly)
$ErrorActionPreference = 'Stop'
if ($HomeWidgetsOnly -and -not $FileUi) { throw '主页设置定向验证需要 FileUi 构建范围' }
if ($HomeMenuOnly -and -not $FileUi) { throw '最近访问菜单定向验证需要 FileUi 构建范围' }
if ($HomePathDisplayOnly -and -not $FileUi) { throw '主页路径显示定向验证需要 FileUi 构建范围' }
if ($NativeDialogsOnly -and -not $FileUi) { throw '第三方文件选择窗口验证需要 FileUi 构建范围' }
$env:PYTHONUTF8 = '1'
$env:PYTHONIOENCODING = 'utf-8'
$taskRepo = Split-Path -Parent $PSScriptRoot
Set-Location $taskRepo
$baseCommit = '4569ea9260d71d9bd4b7ddbb2a4d4e2ac957bfa8'
$baseArtifact = '11826279735'
$baseDigest = '67ffb9ad5eab90229cb495d511c9911e5b92f6dd2a3429ae4cd52f0bbd822828'
$taskBuild = Join-Path $taskRepo 'build-custom'
New-Item -ItemType Directory -Path $taskBuild -Force | Out-Null

# 重编译本次受影响的原生组件；完整预览与转换运行时保持固定且逐文件校验。
$allowed = @('.github/workflows/custom-build.yml', 'scripts/build_tab_drag_ci.ps1',
    'src/app/tab_transfer_test.cpp', 'src/app/selftest_1b2.cpp',
    'src/app/app_input.cpp', 'src/app/app_main.cpp', 'src/app/tab_detach.cpp', 'src/app/tab_detach.h')
if ($FileUi) {
    if ($NativeDialogsOnly) {
        $allowed += @('CMakeLists.txt', 'integrations/NativeDialogs/README.md',
            'src/app/about_info.h', 'docs/custom-edition.md', 'scripts/check_release_payload.ps1',
            'src/common/settings_pages.h', 'src/ui/ui_settings_view.cpp',
            'src/app/address_search.cpp', 'src/app/duplicate_scan_ui.cpp',
            'src/app/global_search_controller.cpp', 'src/app/global_search_controller.h', 'src/app/global_search_window.cpp',
            'src/app/search_options.cpp', 'src/app/search_routing_test.cpp',
            'installer/PulseCustomSetup.iss', 'integrations/Installer/README.md',
            'scripts/build_custom_installer_ci.ps1', 'scripts/package_custom_installer.ps1', 'scripts/test_custom_installer.ps1',
            'scripts/publish_custom_release.ps1',
            'src/app/native_file_dialog_adapter.h', 'src/app/native_file_dialog_adapter.cpp',
            'src/app/native_dialog_takeover.h', 'src/app/native_dialog_takeover.cpp',
            'src/app/native_dialog_wire.h', 'src/app/native_dialog_wire_test.cpp',
            'src/app/native_dialog_picker_host.cpp', 'src/app/native_dialog_takeover_test.cpp',
            'src/app/app_core.h', 'src/app/shell_window_sync.cpp',
            'src/app/settings_controller.h', 'src/app/settings_controller.cpp', 'src/app/settings_flow_test.cpp',
            'src/bench/file_picker_dialog_test.cpp', 'src/bench/file_picker_model_test.cpp',
            'src/bench/native_dialog_fixture.cpp', 'src/bench/native_file_dialog_adapter_test.cpp',
            'src/ui/folder_picker_dialog.h', 'src/ui/folder_picker_dialog.cpp',
            'src/ui/folder_picker_model.h', 'src/ui/folder_picker_model.cpp',
            'src/ui/folder_picker_loader.h', 'src/ui/folder_picker_loader.cpp',
            'src/ui/folder_picker_view.h', 'src/ui/folder_picker_view.cpp')
    }
    # 这些模块只链接进主程序；辅助进程和转换/预览组件仍复用校验后的完整包。
    $allowed += @('src/app/app_hover.cpp', 'src/app/app_model.cpp', 'src/app/app_model.h',
        'src/app/app_commands.cpp', 'src/app/context_menu.cpp', 'src/app/context_menu.h',
        'src/app/recycle_place_commands.cpp', 'src/app/recycle_place_commands.h',
        'src/app/recycle_place_menu_test.cpp', 'src/app/list_checkbox_test.cpp',
        'src/ui/ui_renderer.h', 'src/ui/ui_hit_test.cpp', 'src/ui/ui_pane_list.cpp',
        'src/ui/view_layout.cpp', 'src/ui/view_layout.h',
        'src/ui/fluent_components.cpp', 'src/ui/fluent_components.h',
        'src/ui/ui_toolbar.cpp', 'src/ui/recycle_toolbar.cpp', 'src/ui/recycle_toolbar.h')
    $allowed += @('src/app/app_prefs.cpp', 'src/app/app_prefs.h', 'src/app/app_runtime.cpp',
        'src/app/app_navigation.cpp', 'src/app/settings_controller.cpp', 'src/app/settings_ui.cpp',
        'src/app/sidebar_sections.cpp', 'src/ui/settings_layout_sections.h',
        'src/ui/ui_renderer_internal.h', 'src/ui/ui_settings_core.cpp',
        'src/app/home_widgets.cpp', 'src/app/home_widgets.h', 'src/app/home_widget_actions.cpp',
        'src/app/home_widgets_test.cpp', 'src/ui/home_widgets.cpp', 'src/ui/home_widgets.h')
    $allowed += @('src/app/app_state.h','src/app/app_runtime.h','src/app/startup_location.h',
        'src/app/resource.h','src/app/pulse.rc',
        'assets/fluent/home_document_24_color.svg','assets/fluent/home_picture_24_color.svg',
        'assets/fluent/home_music_24_color.svg','assets/fluent/home_video_24_color.svg',
        'assets/fluent/home_folder_24_color.svg','assets/fluent/home_drive_24_color.svg',
        'assets/fluent/home_download_24_color.svg')
}
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
$selftestMode = if ($DiagnosticsOnly) { 'OFF' } else { 'ON' }
$buildBatch = Join-Path $taskBuild 'compile-native-fixes.bat'
$nativeTargets = if ($NativeDialogsOnly) { 'pulse pulse_file_picker_model_test pulse_native_dialog_fixture pulse_native_file_dialog_adapter_test' } else { 'pulse' }
@"
@echo off
call "$vcvars"
if errorlevel 1 exit /b 1
chcp 65001 >nul
cmake -S "$taskRepo" -B "$taskBuild" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DPULSE_CUSTOM_EDITION=ON -DPULSE_WITH_SELFTEST=$selftestMode -DPULSE_WITH_LUMATEXT=ON -DCMAKE_PREFIX_PATH="$sdkRoot" -DPULSE_UPDATE_MANIFEST_URL= -DPULSE_UPDATE_PUBLIC_KEY_HEX=
if errorlevel 1 exit /b 1
cmake --build "$taskBuild" --parallel 4 --target $nativeTargets
exit /b %errorlevel%
"@ | Set-Content -LiteralPath $buildBatch -Encoding utf8
if ($VerifySettingsRegression) {
    if ($DiagnosticsOnly) { throw '旧实现复现需要启用定向测试' }
    $regressionBase = '3762af04f941d0b24ef328c626c842f5a2a7f9eb'
    & git fetch --no-tags --depth=1 origin $regressionBase
    if ($LASTEXITCODE) { throw '无法读取待复现的旧实现' }
    $detachFile = Join-Path $taskRepo 'src/app/tab_detach.cpp'
    $fixedSource = [IO.File]::ReadAllBytes($detachFile)
    try {
        $oldSource = & git show "${regressionBase}:src/app/tab_detach.cpp"
        if ($LASTEXITCODE) { throw '无法读取旧拖动实现' }
        [IO.File]::WriteAllText($detachFile, ($oldSource -join "`n") + "`n", [Text.UTF8Encoding]::new($false))
        & $buildBatch
        if ($LASTEXITCODE) { throw '旧实现复现构建失败' }
        $env:PULSE_SELFTEST_NO_SCREENSHOTS = '1'
        $env:PULSE_SELFTEST_CASE = 'tab-drag-input'
        $baseline = Start-Process -FilePath (Join-Path $taskBuild 'pulse.exe') -ArgumentList '--selftest' -WindowStyle Hidden -PassThru
        if (-not $baseline.WaitForExit(60000)) { $baseline.Kill(); $baseline.WaitForExit(); throw '旧实现复现超时' }
        $baseline.Refresh()
        $baselineLog = Join-Path $taskBuild 'settings-neighbor-before.log'
        Copy-Item -LiteralPath 'bench_data/tab-drag-input/results.log' -Destination $baselineLog
        $failures = @(Get-Content -LiteralPath $baselineLog | Where-Object { $_.StartsWith('[FAIL]') })
        $expected = @('[FAIL] content edge offers a split before release',
            '[FAIL] settings neighbor does not become the split background',
            '[FAIL] button release commits the actual same-window split')
        if ($baseline.ExitCode -eq 0 -or $expected[1] -notin $failures -or
            @($failures | Where-Object { $_ -notin $expected }).Count) {
            throw '旧实现没有按预期复现设置页阻断分栏，或出现无关失败'
        }
        Write-Host '已确认旧实现复现：相邻设置页阻断分栏，其余失败均属于同一场景。'
    } finally {
        [IO.File]::WriteAllBytes($detachFile, $fixedSource)
        Remove-Item Env:PULSE_SELFTEST_CASE -ErrorAction SilentlyContinue
    }
}
if ($NativeDialogsOnly -and -not $DiagnosticsOnly) {
    # 先验证独立适配器；失败时无需等待整个主程序编译，测试内容不变。
    $adapterBatch = Join-Path $taskBuild 'compile-native-adapter-first.bat'
    $adapterBuild = [IO.File]::ReadAllText($buildBatch).Replace(
        "--target $nativeTargets", '--target pulse_native_file_dialog_adapter_test')
    [IO.File]::WriteAllText($adapterBatch, $adapterBuild, [Text.UTF8Encoding]::new($false))
    & $adapterBatch
    if ($LASTEXITCODE) { throw '原生窗口适配器定向构建失败' }
    New-Item -ItemType Directory -Path 'bench_data' -Force | Out-Null
    $adapter = Start-Process -FilePath (Join-Path $taskBuild 'pulse_native_file_dialog_adapter_test.exe') -ArgumentList '--interaction' -WindowStyle Hidden -PassThru -RedirectStandardOutput 'bench_data/native-dialogs-adapter.log' -RedirectStandardError 'bench_data/native-dialogs-adapter-errors.log'
    if (-not $adapter.WaitForExit(180000)) {
        $adapter.Kill(); $adapter.WaitForExit()
        throw '原生窗口适配器验证超时，停止主程序构建'
    }
    $adapter.Refresh()
    if ($adapter.ExitCode) { throw '原生窗口适配器验证失败，停止主程序构建' }
}
& $buildBatch
if ($LASTEXITCODE) { throw '原生修复定向构建失败' }
if (-not $DiagnosticsOnly) {
$testFailures = [System.Collections.Generic.List[string]]::new()
$env:PULSE_SELFTEST_NO_SCREENSHOTS = '1'
$testCases = if ($NativeDialogsOnly) { @('native-dialog-wire', 'native-dialog-takeover', 'search-routing', 'listary-search-entry', 'filter-controls', 'home-menu') }
    elseif ($HomePathDisplayOnly) { @('home-path-display') }
    elseif ($HomeMenuOnly) { @('home-menu') }
    elseif ($HomeWidgetsOnly) { @('home-widgets') }
    elseif ($FileUi) { @('list-checkbox', 'recycle-place-menu', 'home-widgets') }
    else { @('tab-drag-input', 'tab-transfer', 'tab-pin-boundary') }
foreach ($testCase in $testCases) {
    $env:PULSE_SELFTEST_CASE = $testCase
    $process = Start-Process -FilePath (Join-Path $taskBuild 'pulse.exe') -ArgumentList '--selftest' -WindowStyle Hidden -PassThru
    $testTimeout = if ($testCase -eq 'native-dialog-takeover') { 300000 } else { 60000 }
    if (-not $process.WaitForExit($testTimeout)) { $process.Kill(); $process.WaitForExit(); $testFailures.Add("timeout: $testCase") }
    Copy-Item -LiteralPath 'bench_data/selftest_1b2_last.log' -Destination (Join-Path $taskBuild "selftest-$testCase.log") -ErrorAction SilentlyContinue
    $process.Refresh()
    if ($process.ExitCode) { $testFailures.Add($testCase) }
}
Remove-Item Env:PULSE_SELFTEST_CASE
if ($NativeDialogsOnly) {
    & (Join-Path $taskBuild 'pulse_file_picker_model_test.exe') --save 2>&1 | Tee-Object -FilePath 'bench_data/native-dialogs-save-model.log'
    if ($LASTEXITCODE) { $testFailures.Add('save-picker-model') }
    $save = Start-Process -FilePath (Join-Path $taskBuild 'pulse.exe') -ArgumentList '--picker-dialog-test --save' -WindowStyle Hidden -PassThru -RedirectStandardOutput 'bench_data/native-dialogs-save-picker.log' -RedirectStandardError 'bench_data/native-dialogs-save-picker-errors.log'
    if (-not $save.WaitForExit(60000)) { $save.Kill(); $save.WaitForExit(); $testFailures.Add('timeout: save-picker-dialog') }
    $save.Refresh()
    if ($save.ExitCode) { $testFailures.Add('save-picker-dialog') }
    foreach ($settingsCase in @('integration', 'navigation')) {
        $settingsOutput = Join-Path $taskRepo ('bench_data/native-dialogs-settings-' + $settingsCase)
        New-Item -ItemType Directory -Path $settingsOutput -Force | Out-Null
        $env:PULSE_TEST_SETTINGS_FLOW = Join-Path $settingsOutput 'results.log'
        $env:PULSE_TEST_SETTINGS_INTEGRATION = if ($settingsCase -eq 'integration') { '1' } else { $null }
        $env:PULSE_TEST_SETTINGS_NAVIGATION = if ($settingsCase -eq 'navigation') { '1' } else { $null }
        $env:PULSE_TEST_DATA_DIR = Join-Path $taskRepo ('bench_data/native-dialogs-settings-data-' + $settingsCase)
        try {
            $settings = Start-Process -FilePath (Join-Path $taskBuild 'pulse.exe') -ArgumentList '--test-instance' -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $settingsOutput 'stdout.log') -RedirectStandardError (Join-Path $settingsOutput 'stderr.log')
            if (-not $settings.WaitForExit(60000)) { $settings.Kill(); $settings.WaitForExit(); $testFailures.Add("timeout: native-dialogs-settings-$settingsCase") }
            $settings.Refresh()
            if ($settings.ExitCode) { $testFailures.Add("native-dialogs-settings-$settingsCase") }
        } finally {
            Remove-Item Env:PULSE_TEST_SETTINGS_FLOW,Env:PULSE_TEST_SETTINGS_INTEGRATION,Env:PULSE_TEST_SETTINGS_NAVIGATION,Env:PULSE_TEST_DATA_DIR -ErrorAction SilentlyContinue
        }
    }
}
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
}
Copy-Item -LiteralPath (Join-Path $taskBuild 'pulse.exe') -Destination (Join-Path $payload 'pulse.exe')
& "$PSScriptRoot/package_custom.ps1" -BuildDir $payload
# 小型更新包仅包含相对固定基线变化的文件；安装端仍须核对完整清单。
$buildInfo = Get-Content -LiteralPath 'dist/custom-build.json' -Raw | ConvertFrom-Json
$stage = Join-Path $taskRepo ('dist/' + $buildInfo.package)
$baseManifest = Get-Content -LiteralPath (Join-Path $payload 'package-manifest.json') -Raw | ConvertFrom-Json
$nextManifest = Get-Content -LiteralPath (Join-Path $stage 'package-manifest.json') -Raw | ConvertFrom-Json
$baseFiles = @{}
foreach ($entry in $baseManifest.files) { $baseFiles[$entry.path] = $entry.sha256 }
$update = Join-Path $taskBuild 'tab-update'
New-Item -ItemType Directory -Path $update | Out-Null
foreach ($entry in $nextManifest.files) {
    if ($baseFiles[$entry.path] -eq $entry.sha256) { continue }
    $target = Join-Path $update $entry.path
    New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $stage $entry.path) -Destination $target
}
Copy-Item -LiteralPath (Join-Path $stage 'package-manifest.json') -Destination $update
[ordered]@{base_commit=$baseCommit; commit=$nextManifest.commit} | ConvertTo-Json |
    Set-Content -LiteralPath (Join-Path $update 'update-base.json') -Encoding utf8
