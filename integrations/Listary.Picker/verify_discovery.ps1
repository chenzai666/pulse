param([Parameter(Mandatory)][string]$PluginPath)
$ErrorActionPreference='Stop'
$sdk=Join-Path $env:USERPROFILE '.nuget/packages/listary.fileappplugin/1.0.0/lib/net48/Listary.FileAppPlugin.dll'
$api=[Reflection.Assembly]::LoadFrom($sdk)
$assembly=[Reflection.Assembly]::LoadFrom([IO.Path]::GetFullPath($PluginPath))
$plugin=$api.GetType('Listary.FileAppPlugin.IFileAppPlugin')
$window=$api.GetType('Listary.FileAppPlugin.IFileWindow')
$tab=$api.GetType('Listary.FileAppPlugin.IFileTab')
$found=@{}
# 与官方加载器相同的互斥分支；合并 Window 和 Tab 会漏掉 Tab。
foreach($type in $assembly.GetExportedTypes()) {
    if($plugin.IsAssignableFrom($type)){$found.Plugin=$type}
    elseif($window.IsAssignableFrom($type)){$found.Window=$type}
    elseif($tab.IsAssignableFrom($type)){$found.Tab=$type}
}
if($found.Count -ne 3){throw 'Listary 无法分别发现 Plugin、Window、Tab 三种实现'}
if($found.Window -eq $found.Tab){throw '窗口和标签实现必须分开'}
Write-Output '[PASS] 官方发现规则可分别找到插件、文件窗口和标签实现'
