#requires -Version 7.2
$ErrorActionPreference = 'Stop'
$env:PYTHONUTF8 = '1'
$env:PYTHONIOENCODING = 'utf-8'
$taskRepo = Split-Path -Parent $PSScriptRoot
Set-Location $taskRepo
$baseCommit = '2316d7dc52669b17f644ed739859485f83d1b6d0'
$baseArtifact = '11963832718'
$baseDigest = '048b75115adc3b8c8c6c63ea9614ffdf71ca77ecc80273d528f5ccefcee970de'
$taskBuild = Join-Path $taskRepo 'build-custom'
New-Item -ItemType Directory -Path $taskBuild -Force | Out-Null

# 仅替换主程序；任何已有组件源码变化都必须走完整构建。
$allowed = @('.github/workflows/custom-build.yml', '.github/workflows/status-center.yml', 'scripts/build_status_center_ci.ps1',
    'src/ui/status_center_window.h', 'src/ui/status_center_window.cpp', 'src/bench/dialog_gallery.cpp',
    'src/common/string_ids.h', 'src/common/localization.h', 'src/common/localization.cpp',
    'src/common/strings.rcinc', 'src/common/strings_zh_tw.rcinc',
    'src/app/startup_location.cpp', 'src/app/app_prefs.cpp', 'src/app/app_main.cpp', 'src/app/settings_controller.cpp', 'src/app/selftest_1b2.cpp',
    'src/ui/ui_settings_core.cpp', 'src/ui/settings_layout_sections.h', 'src/ui/ui_renderer_internal.h', 'src/ui/ui_hit_test.cpp')
$changed = @(& git diff --name-only $baseCommit HEAD)
if ($LASTEXITCODE) {
    & git fetch --no-tags --depth=1 origin $baseCommit
    if ($LASTEXITCODE) { throw '无法读取固定组件源码版本' }
    $changed = @(& git diff --name-only $baseCommit HEAD)
    if ($LASTEXITCODE) { throw '无法比较组件源码版本' }
}
foreach ($file in $changed) {
    if ($file -notin $allowed) { throw "状态中心定向构建不支持此改动，请执行完整构建：$file" }
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
$buildBatch = Join-Path $taskBuild 'compile-status-center.bat'
@"
@echo off
call "$vcvars"
if errorlevel 1 exit /b 1
chcp 65001 >nul
cmake -S "$taskRepo" -B "$taskBuild" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DPULSE_CUSTOM_EDITION=ON -DPULSE_WITH_SELFTEST=OFF -DPULSE_WITH_LUMATEXT=ON -DCMAKE_PREFIX_PATH="$sdkRoot" -DPULSE_UPDATE_MANIFEST_URL= -DPULSE_UPDATE_PUBLIC_KEY_HEX=
if errorlevel 1 exit /b 1
cmake --build "$taskBuild" --parallel 4 --target pulse pulse_dialog_gallery
exit /b %errorlevel%
"@ | Set-Content -LiteralPath $buildBatch -Encoding utf8
& $buildBatch
if ($LASTEXITCODE) { throw '状态中心定向构建失败' }
$galleryOutput = Join-Path $taskBuild 'status-center-gallery'
$gallery = Start-Process -FilePath (Join-Path $taskBuild 'pulse_dialog_gallery.exe') -ArgumentList '--lang','zh-CN','--status-center',$galleryOutput -WindowStyle Hidden -PassThru
if (-not $gallery.WaitForExit(60000)) { $gallery.Kill(); throw '状态中心绘制验证超时' }
$gallery.Refresh()
if ($gallery.ExitCode) { throw '状态中心实际绘制验证失败' }
Get-Content -LiteralPath (Join-Path $galleryOutput 'status-center-gallery.txt')
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
$update = Join-Path $taskBuild 'status-center-update'
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
