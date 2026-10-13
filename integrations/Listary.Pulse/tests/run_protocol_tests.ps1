[CmdletBinding()]
param(
    [Nullable[long]]$NativeWindow,
    [string]$Fixture
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$testProject = Join-Path $PSScriptRoot 'Listary.Pulse.ProtocolTests.csproj'
$pluginRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$pulseRoot = [System.IO.Path]::GetFullPath((Join-Path $pluginRoot '..\..'))
$testOutput = Join-Path $pulseRoot 'out\listary-plugin\protocol-tests'
$dotnetPath = (Get-Command dotnet -ErrorAction Stop).Source

& $dotnetPath restore $testProject --locked-mode --configfile (Join-Path $pluginRoot 'NuGet.config') --nologo
if ($LASTEXITCODE -ne 0) { throw '协议测试依赖还原失败。' }
& $dotnetPath build $testProject -c Release --no-restore --nologo -o $testOutput
if ($LASTEXITCODE -ne 0) { throw '协议测试编译失败。' }

# 模拟窗口身份需要实际映像名称为 pulse.exe，运行范围仅为此测试输出目录。
$testExecutable = Join-Path $testOutput 'pulse.exe'
Copy-Item -LiteralPath (Join-Path $testOutput 'Pulse.Listary.ProtocolTests.exe') -Destination $testExecutable -Force
Copy-Item -LiteralPath (Join-Path $testOutput 'Pulse.Listary.ProtocolTests.exe.config') -Destination "$testExecutable.config" -Force
if ($null -ne $NativeWindow) {
    if ([string]::IsNullOrWhiteSpace($Fixture)) { throw '原生桥接测试必须指定独立 Fixture 目录。' }
    & $testExecutable --native-window $NativeWindow --fixture $Fixture
} else {
    & $testExecutable
}
if ($LASTEXITCODE -ne 0) { throw 'Listary 协议测试失败。' }
