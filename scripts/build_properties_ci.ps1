#requires -Version 7.2
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path -Parent $PSScriptRoot
Set-Location $taskRepo
$baseCommit = '9817d635b69e9e94b1c8a472c3644ffbfd9801c9'
$baseArtifact = '11808111877'
$baseDigest = 'fb0206db8714cd9fe8e35e9c020e4b1eb2c3c9e39af37c0f71be1bc4ba5bea55'
$taskBuild = Join-Path $taskRepo 'build-custom'
New-Item -ItemType Directory -Path $taskBuild -Force | Out-Null

# 仅替换主程序；任何已有组件源码变化都必须走完整构建。
$allowed = @('CMakeLists.txt', '.github/workflows/custom-build.yml', 'docs/custom-edition.md',
    'scripts/build_properties_ci.ps1', 'scripts/verify_custom_package.py', 'scripts/build_custom_ci.ps1',
    'src/app/app_properties.h', 'src/app/app_properties.cpp', 'src/app/app_core.h',
    'src/app/app_commands.cpp', 'src/app/app_preview_commands.cpp', 'src/app/breadcrumb_menu.cpp',
    'src/app/app_main.cpp', 'src/fs/item_properties.h', 'src/fs/item_properties.cpp',
    'src/ui/properties_window.h', 'src/ui/properties_window.cpp',
    'src/bench/item_properties_test.cpp', 'src/bench/properties_gallery.cpp')
$changed = @(& git diff --name-only $baseCommit HEAD)
if ($LASTEXITCODE) { throw '无法比较组件源码版本' }
foreach ($file in $changed) {
    if ($file -notin $allowed) { throw "属性定向构建不支持此改动，请执行完整构建：$file" }
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
    Tee-Object -FilePath (Join-Path $taskBuild 'properties-base-verification.log')
if ($LASTEXITCODE) { throw '完整组件逐文件验证失败' }
$payload = Join-Path $baseRoot ('Pulse-Custom-' + $baseCommit.Substring(0,9))

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsRoot = & $vswhere -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw '需要 Visual Studio 2022 C++ 工具' }
$vcvars = Join-Path $vsRoot 'VC/Auxiliary/Build/vcvars64.bat'
$sdkRoot = & "$PSScriptRoot/verify_lumatext_sdk.ps1"
$buildBatch = Join-Path $taskBuild 'compile-properties.bat'
@"
@echo off
call "$vcvars"
if errorlevel 1 exit /b 1
chcp 65001 >nul
cmake -S "$taskRepo" -B "$taskBuild" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DPULSE_CUSTOM_EDITION=ON -DPULSE_WITH_SELFTEST=OFF -DPULSE_WITH_LUMATEXT=ON -DCMAKE_PREFIX_PATH="$sdkRoot" -DPULSE_UPDATE_MANIFEST_URL= -DPULSE_UPDATE_PUBLIC_KEY_HEX=
if errorlevel 1 exit /b 1
cmake --build "$taskBuild" --parallel 4 --target pulse pulse_item_properties_test pulse_properties_gallery
exit /b %errorlevel%
"@ | Set-Content -LiteralPath $buildBatch -Encoding utf8
& $buildBatch
if ($LASTEXITCODE) { throw '属性定向构建失败' }
& (Join-Path $taskBuild 'pulse_item_properties_test.exe') |
    Tee-Object -FilePath (Join-Path $taskBuild 'item-properties-test.log')
if ($LASTEXITCODE) { throw '属性元数据定向验证失败' }
& (Join-Path $taskBuild 'pulse_properties_gallery.exe') (Join-Path $taskBuild 'properties-gallery')
if ($LASTEXITCODE) { throw '属性窗口实际绘制验证失败' }
Copy-Item -LiteralPath (Join-Path $taskBuild 'pulse.exe') -Destination (Join-Path $payload 'pulse.exe')
Copy-Item -LiteralPath (Join-Path $taskBuild 'lumatext.dll') -Destination (Join-Path $payload 'lumatext.dll')
& "$PSScriptRoot/package_custom.ps1" -BuildDir $payload
