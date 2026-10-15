#requires -Version 7.2
param(
    [Parameter(Mandatory)][string]$PackageDirectory,
    [Parameter(Mandatory)][string]$InstallerDirectory,
    [Parameter(Mandatory)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($env:GITHUB_ACTIONS -ne 'true') { throw '安装／卸载回归仅在隔离 GitHub Actions runner 执行' }
$package = (Resolve-Path -LiteralPath $PackageDirectory).Path
$output = (Resolve-Path -LiteralPath $OutputDirectory).Path
$installer = @(Get-ChildItem -LiteralPath $InstallerDirectory -File -Filter '*.exe')
if ($installer.Count -ne 1) { throw '需要唯一的定制版安装器' }
$log = Join-Path $output 'installer-tests.log'
function Check([bool]$Passed, [string]$Label) {
    if (-not $Passed) { Add-Content -LiteralPath $log -Value "[FAIL] $Label"; throw $Label }
    Add-Content -LiteralPath $log -Value "[PASS] $Label"
}
function SharedState {
    $snapshot = [ordered]@{}
    foreach ($key in @(
        'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{A3F47C2E-9D1B-4E58-8C6A-2B5D0F9E1734}_is1',
        'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{A3F47C2E-9D1B-4E58-8C6A-2B5D0F9E1734}_is1',
        'HKLM:\SYSTEM\CurrentControlSet\Services\PulseIndex',
        'HKCU:\Software\Classes\Directory\shell', 'HKCU:\Software\Classes\Drive\shell',
        'HKCU:\Software\Classes\Folder\shell')) {
        $snapshot[$key] = if (Test-Path -LiteralPath $key) {
            $registryEntries = @(Get-Item -LiteralPath $key)
            $registryEntries += @(Get-ChildItem -LiteralPath $key -Recurse -ErrorAction Stop)
            $registryEntries |
                ForEach-Object {
                    $registryEntry = $_
                    [ordered]@{path=$registryEntry.Name; values=@($registryEntry.GetValueNames() | Sort-Object | ForEach-Object {
                        [ordered]@{name=$_;value=$registryEntry.GetValue($_);kind=$registryEntry.GetValueKind($_).ToString()}
                    })}
                }
        } else { $null }
    }
    $snapshot | ConvertTo-Json -Depth 20 -Compress
}
# 安装器不得触碰原版卸载项、服务或全局打开关联。
$before = SharedState
$target = Join-Path $env:LOCALAPPDATA 'Programs\PulseCustom'
if (Test-Path -LiteralPath $target) { throw '隔离 runner 已有定制版安装，停止覆盖' }
$customUninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{BF0350A3-5F01-44CD-AC64-C9AF030CC430}_is1'
if (Test-Path -LiteralPath $customUninstallKey) { throw '隔离 runner 已有定制版卸载项，停止覆盖' }
$prefsRoot = Join-Path $env:LOCALAPPDATA 'PulseCustom'
New-Item -ItemType Directory -Path $prefsRoot -Force | Out-Null
$canary = Join-Path $prefsRoot ('installer-canary-' + [guid]::NewGuid().ToString('N') + '.txt')
[IO.File]::WriteAllText($canary, '保留用户配置', [Text.UTF8Encoding]::new($false))
$canaryHash = (Get-FileHash -LiteralPath $canary).Hash
$manifest = Get-Content -LiteralPath (Join-Path $package 'package-manifest.json') -Raw | ConvertFrom-Json
function VerifyInstalled {
    foreach ($entry in $manifest.files) {
        $path = Join-Path $target $entry.path
        if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or
            (Get-Item -LiteralPath $path).Length -ne $entry.bytes -or
            (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ine $entry.sha256) {
            throw ('安装文件校验失败：' + $entry.path)
        }
    }
    Check ((Get-FileHash -LiteralPath (Join-Path $target 'package-manifest.json')).Hash -eq (Get-FileHash -LiteralPath (Join-Path $package 'package-manifest.json')).Hash) '安装清单与原始完整包一致'
}
foreach ($round in 1,2) {
    $arguments = @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART','/CURRENTUSER',('/DIR="' + $target + '"'),('/LOG="' + (Join-Path $output "install-$round.log") + '"'))
    $process = Start-Process -FilePath $installer[0].FullName -ArgumentList $arguments -WindowStyle Hidden -PassThru
    if (-not $process.WaitForExit(180000)) { throw '安装器超时；保留现场，不强杀其他程序' }
    Check ($process.ExitCode -eq 0) "第 $round 次安装成功"
    Check (Test-Path -LiteralPath $customUninstallKey) '安装创建独立的当前用户卸载项'
    $registered = Get-ItemProperty -LiteralPath $customUninstallKey
    Check ($registered.DisplayName -eq 'Pulse 定制版' -and
        $registered.InstallLocation.TrimEnd('\') -ieq $target) '独立卸载项指向当前用户的定制版目录'
    VerifyInstalled
    Check ((Get-FileHash -LiteralPath $canary).Hash -eq $canaryHash) "第 $round 次安装保留配置"
    Check ((SharedState) -eq $before) "第 $round 次安装不改变原版卸载、索引服务或打开关联"
    Check (@(Get-Process pulse -ErrorAction SilentlyContinue | Where-Object Path -EQ (Join-Path $target 'pulse.exe')).Count -eq 0) '静默安装不自动启动主程序'
}
$uninstallers = @(Get-ChildItem -LiteralPath (Join-Path $target '.installer') -File -Filter 'unins*.exe')
Check ($uninstallers.Count -eq 1) '独立卸载器存在'
$process = Start-Process -FilePath $uninstallers[0].FullName -ArgumentList @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART',('/LOG="' + (Join-Path $output 'uninstall.log') + '"')) -WindowStyle Hidden -PassThru
if (-not $process.WaitForExit(180000)) { throw '卸载器超时；保留现场' }
Check ($process.ExitCode -eq 0) '定制版卸载成功'
Check (-not (Test-Path -LiteralPath (Join-Path $target 'pulse.exe'))) '卸载移除本安装目录程序'
Check (-not (Test-Path -LiteralPath $customUninstallKey)) '卸载移除自己的卸载项'
Check ((Get-FileHash -LiteralPath $canary).Hash -eq $canaryHash) '卸载保留用户配置'
Check ((SharedState) -eq $before) '卸载不改变原版卸载、索引服务或打开关联'
Add-Content -LiteralPath $log -Value 'SUMMARY failures=0'
