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
$defaultTarget = Join-Path $env:LOCALAPPDATA 'Programs\PulseCustom'
if (Test-Path -LiteralPath $defaultTarget) { throw '隔离 runner 已有默认目录，停止覆盖' }
if (-not $env:RUNNER_TEMP) { throw '缺少隔离 runner 临时目录' }
$target = Join-Path $env:RUNNER_TEMP 'PulseCustom-install-test'
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
    $arguments = @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART','/CURRENTUSER',('/LOG="' + (Join-Path $output "install-$round.log") + '"'))
    if ($round -eq 1) { $arguments += '/DIR="' + $target + '"' }
    $process = Start-Process -FilePath $installer[0].FullName -ArgumentList $arguments -WindowStyle Hidden -PassThru
    if (-not $process.WaitForExit(180000)) { throw '安装器超时；保留现场，不强杀其他程序' }
    Check ($process.ExitCode -eq 0) "第 $round 次安装成功"
    Check (Test-Path -LiteralPath $customUninstallKey) '安装创建独立的当前用户卸载项'
    $registered = Get-ItemProperty -LiteralPath $customUninstallKey
    Check ($registered.DisplayName -eq 'Pulse 定制版' -and
        $registered.InstallLocation.TrimEnd('\') -ieq $target) '独立卸载项指向所选非默认目录，升级沿用原路径'
    Check (-not (Test-Path -LiteralPath $defaultTarget)) '自定义安装和升级未写入默认目录'
    VerifyInstalled
    Check ((Get-FileHash -LiteralPath $canary).Hash -eq $canaryHash) "第 $round 次安装保留配置"
    Check ((SharedState) -eq $before) "第 $round 次安装不改变原版卸载、索引服务或打开关联"
    Check (@(Get-Process pulse -ErrorAction SilentlyContinue | Where-Object Path -EQ (Join-Path $target 'pulse.exe')).Count -eq 0) '静默安装不自动启动主程序'
}
$uninstallers = @(Get-ChildItem -LiteralPath (Join-Path $target '.installer') -File -Filter 'unins*.exe')
Check ($uninstallers.Count -eq 1) '独立卸载器存在'
$busyExecutable = Join-Path $target '.installer\unins-busy-fixture.exe'
$busyScript = Join-Path $output 'uninstall-busy-fixture.vbs'
if (Test-Path -LiteralPath $busyExecutable) { throw '忙碌卸载测试目标已存在，拒绝覆盖' }
Copy-Item -LiteralPath (Join-Path $env:SystemRoot 'System32\cscript.exe') -Destination $busyExecutable
'WScript.Sleep 300000' | Set-Content -LiteralPath $busyScript -Encoding ascii
$busyProcess = $null
try {
    $busyProcess = Start-Process -FilePath $busyExecutable -ArgumentList @('//B','//NoLogo',('"' + $busyScript + '"')) -WindowStyle Hidden -PassThru
    $busyIdentity = Get-CimInstance Win32_Process -Filter ("ProcessId=" + $busyProcess.Id)
    Check ($busyIdentity.ExecutablePath -ieq $busyExecutable -and -not $busyProcess.HasExited) '创建独立且名称包含 unins 的安装目录忙碌测试进程'
    $busyUninstall = Start-Process -FilePath $uninstallers[0].FullName -ArgumentList @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART',('/LOG="' + (Join-Path $output 'uninstall-busy.log') + '"')) -WindowStyle Hidden -PassThru
    if (-not $busyUninstall.WaitForExit(30000)) {
        Get-CimInstance Win32_Process | Select-Object ProcessId,ParentProcessId,ExecutablePath,CreationDate |
            ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $output 'uninstall-busy-processes.json') -Encoding utf8
        throw '忙碌卸载未及时拒绝；保留卸载现场'
    }
    Check ($busyUninstall.ExitCode -ne 0 -and -not $busyProcess.HasExited) '卸载拒绝其他运行进程，不因 unins 名称放行，也不结束该进程'
    Check ((Test-Path -LiteralPath (Join-Path $target 'pulse.exe')) -and (Test-Path -LiteralPath $customUninstallKey)) '忙碌拒绝保留安装程序和独立卸载项'
} finally {
    if ($null -ne $busyProcess -and -not $busyProcess.HasExited) {
        $busyIdentity = Get-CimInstance Win32_Process -Filter ("ProcessId=" + $busyProcess.Id)
        if ($busyIdentity.ExecutablePath -ine $busyExecutable) { throw '测试进程身份变化，拒绝结束其他进程' }
        $busyProcess.Kill()
        $busyProcess.WaitForExit()
    }
    if ($null -ne $busyProcess) { $busyProcess.Dispose() }
    Remove-Item -LiteralPath $busyExecutable
}
$process = Start-Process -FilePath $uninstallers[0].FullName -ArgumentList @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART',('/LOG="' + (Join-Path $output 'uninstall.log') + '"')) -WindowStyle Hidden -PassThru
if (-not $process.WaitForExit(180000)) {
    Get-CimInstance Win32_Process | Select-Object ProcessId,ParentProcessId,ExecutablePath,CreationDate |
        ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $output 'uninstall-processes.json') -Encoding utf8
    throw '卸载器超时；保留现场'
}
Check ($process.ExitCode -eq 0) '定制版卸载成功'
Check (-not (Test-Path -LiteralPath (Join-Path $target 'pulse.exe'))) '卸载移除本安装目录程序'
Check (-not (Test-Path -LiteralPath $customUninstallKey)) '卸载移除自己的卸载项'
Check (-not (Test-Path -LiteralPath $defaultTarget)) '自定义目录卸载未写入默认目录'
Check ((Get-FileHash -LiteralPath $canary).Hash -eq $canaryHash) '卸载保留用户配置'
Check ((SharedState) -eq $before) '卸载不改变原版卸载、索引服务或打开关联'
Add-Content -LiteralPath $log -Value 'SUMMARY failures=0'
