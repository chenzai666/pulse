#requires -Version 7.2
$ErrorActionPreference = 'Stop'
$env:PYTHONUTF8 = '1'
$env:PYTHONIOENCODING = 'utf-8'
$taskRepo = Split-Path -Parent $PSScriptRoot
Set-Location $taskRepo
$taskBuild = Join-Path $taskRepo 'build-custom'
New-Item -ItemType Directory -Path $taskBuild -Force | Out-Null
$baseCommit = '3b3e871a1c61252a1bc8f8c14f435c26c276d01d'
# 此固定包已包含当前预览、转换、Listary 和原生选择窗口；仅重建搜索依赖。
$allowed = @('CMakeLists.txt', '.github/workflows/custom-build.yml',
    'installer/PulseCustomSetup.iss', 'integrations/Installer/README.md',
    'integrations/Listary.Pulse/README.md', 'docs/custom-edition.md',
    'scripts/build_search_ci.ps1', 'scripts/build_custom_installer_ci.ps1', 'scripts/build_custom_ci.ps1', 'scripts/package_custom.ps1',
    'scripts/package_custom_installer.ps1', 'scripts/test_custom_installer.ps1',
    'scripts/publish_custom_release.ps1', 'src/common/settings_pages.h', 'src/common/crash_reporter.cpp',
    'src/app/address_search.cpp', 'src/app/app_commands.cpp', 'src/app/app_main.cpp',
    'src/app/app_navigation.cpp', 'src/app/app_runtime.cpp', 'src/app/context_menu.cpp',
    'src/app/global_search_controller.cpp', 'src/app/global_search_controller.h', 'src/app/global_search_window.cpp', 'src/app/selftest_1b2.cpp',
    'src/app/search_options.cpp', 'src/app/search_routing_test.cpp',
    'src/app/settings_controller.cpp', 'src/app/settings_flow_test.cpp',
    'src/ui/ui_hit_test.cpp', 'src/ui/ui_settings_view.cpp', 'src/ui/ui_renderer_internal.h')
& git fetch --no-tags --depth=1 origin $baseCommit
if ($LASTEXITCODE) { throw '无法取得固定运行时来源' }
$changed = @(& git diff --name-only $baseCommit HEAD)
if ($LASTEXITCODE) { throw '无法比较固定运行时来源' }
foreach ($file in $changed) {
    if ($file -in $allowed -or $file.StartsWith('src/index/') -or
        $file -eq 'src/common/custom_index_storage.h' -or $file -eq 'src/bench/custom_index_storage_test.cpp') { continue }
    throw "搜索构建范围之外的改动，停止复用旧组件：$file"
}
$env:PULSE_SEARCH_BUILD = $taskBuild
@'
import hashlib, os, urllib.error, urllib.request
from pathlib import Path
token=os.environ['GH_TOKEN']
request=urllib.request.Request('https://api.github.com/repos/chenzai666/pulse/actions/artifacts/11954525144/zip',headers={'Authorization':'Bearer '+token})
class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self,*args): return None
try: reply=urllib.request.build_opener(NoRedirect).open(request,timeout=30)
except urllib.error.HTTPError as error: reply=error
if reply.code!=302: raise RuntimeError('固定组件包下载失败: '+str(reply.code))
location=reply.headers['Location'];reply.close()
digest=hashlib.sha256()
with urllib.request.urlopen(location,timeout=120) as reader, (Path(os.environ['PULSE_SEARCH_BUILD'])/'base.zip').open('xb') as writer:
    while block:=reader.read(1024*1024): digest.update(block);writer.write(block)
if digest.hexdigest()!='792e88ee988b0433a927c78bde9a788869a317d2fa28a1953ab7d67b30de0836': raise RuntimeError('固定包摘要不匹配')
'@ | python -
if ($LASTEXITCODE) { throw '固定运行时下载校验失败' }
& python scripts/verify_custom_package.py (Join-Path $taskBuild 'base.zip') $baseCommit (Join-Path $taskBuild 'verified-base') |
    Tee-Object -FilePath (Join-Path $taskBuild 'search-base-verification.log')
if ($LASTEXITCODE) { throw '固定运行时逐文件校验失败' }
$payload = Join-Path $taskBuild ('verified-base/Pulse-Custom-' + $baseCommit.Substring(0,9))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsRoot = & $vswhere -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw '需要 Visual Studio 2022 C++ 工具' }
$vcvars = Join-Path $vsRoot 'VC/Auxiliary/Build/vcvars64.bat'
$sdkRoot = & "$PSScriptRoot/verify_lumatext_sdk.ps1"
$buildBatch = Join-Path $taskBuild 'compile-search.bat'
@"
@echo off
call "$vcvars"
if errorlevel 1 exit /b 1
chcp 65001 >nul
cmake -S "$taskRepo" -B "$taskBuild" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DPULSE_CUSTOM_EDITION=ON -DPULSE_WITH_SELFTEST=ON -DPULSE_WITH_LUMATEXT=ON -DCMAKE_PREFIX_PATH="$sdkRoot" -DPULSE_UPDATE_MANIFEST_URL= -DPULSE_UPDATE_PUBLIC_KEY_HEX=
if errorlevel 1 exit /b 1
cmake --build "$taskBuild" --parallel 4 --target pulse pulse_index pulse_document pulse_preview pulse_shell pulse_integration pulse_elevated pulse_custom_index_storage_test
exit /b %errorlevel%
"@ | Set-Content -LiteralPath $buildBatch -Encoding utf8
& $buildBatch
if ($LASTEXITCODE) { throw '内置搜索构建失败' }
& (Join-Path $taskBuild 'pulse_custom_index_storage_test.exe') |
    Tee-Object -FilePath (Join-Path $taskBuild 'custom-index-storage.log')
if ($LASTEXITCODE) { throw '安装目录索引与隔离验证失败' }
$env:PULSE_SELFTEST_CASE = 'search-routing'
$env:PULSE_SELFTEST_NO_SCREENSHOTS = '1'
$routing = Start-Process -FilePath (Join-Path $taskBuild 'pulse.exe') -ArgumentList '--selftest' -WindowStyle Hidden -PassThru
if (-not $routing.WaitForExit(60000)) { $routing.Kill(); throw '搜索结果路由验证超时' }
$routing.Refresh()
if ($routing.ExitCode) { throw '搜索结果路由验证失败' }
Remove-Item Env:PULSE_SELFTEST_CASE
$settingsOutput = Join-Path $taskRepo 'bench_data/search-settings'
New-Item -ItemType Directory -Path $settingsOutput -Force | Out-Null
$env:PULSE_TEST_SETTINGS_FLOW = Join-Path $settingsOutput 'results.log'
$env:PULSE_TEST_SETTINGS_NAVIGATION = '1'
$env:PULSE_TEST_DATA_DIR = Join-Path $taskRepo 'bench_data/search-settings/data'
try {
    $settings = Start-Process -FilePath (Join-Path $taskBuild 'pulse.exe') -ArgumentList '--test-instance' -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $settingsOutput 'stdout.log') -RedirectStandardError (Join-Path $settingsOutput 'stderr.log')
    if (-not $settings.WaitForExit(60000)) { $settings.Kill(); throw '搜索设置页验证超时' }
    $settings.Refresh()
    if ($settings.ExitCode) { throw '搜索设置页验证失败' }
} finally {
    Remove-Item Env:PULSE_TEST_SETTINGS_FLOW,Env:PULSE_TEST_SETTINGS_NAVIGATION,Env:PULSE_TEST_DATA_DIR -ErrorAction SilentlyContinue
}
$productionBatch = Join-Path $taskBuild 'compile-search-production.bat'
@"
@echo off
call "$vcvars"
if errorlevel 1 exit /b 1
cmake -S "$taskRepo" -B "$taskBuild" -G Ninja -DPULSE_WITH_SELFTEST=OFF
if errorlevel 1 exit /b 1
cmake --build "$taskBuild" --parallel 4 --target pulse pulse_index pulse_document pulse_preview pulse_shell pulse_integration pulse_elevated
exit /b %errorlevel%
"@ | Set-Content -LiteralPath $productionBatch -Encoding utf8
& $productionBatch
if ($LASTEXITCODE) { throw '搜索正式载荷构建失败' }
. "$PSScriptRoot/release_payload.ps1"
foreach ($file in $PulseReleaseExecutables) {
    Copy-Item -LiteralPath (Join-Path $taskBuild $file) -Destination (Join-Path $payload $file)
}
& "$PSScriptRoot/package_custom.ps1" -BuildDir $payload
