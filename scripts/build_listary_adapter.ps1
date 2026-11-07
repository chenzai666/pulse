#requires -Version 7.2
param([string]$OutputDirectory)
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
if(-not $OutputDirectory){$OutputDirectory=Join-Path $repo 'out/listary-adapter-build'}
$artifacts=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $artifacts -Force|Out-Null
$projects=@(
 'Manager/Listary.FileAppPlugin.PulseManager.csproj',
 'Picker/Listary.FileAppPlugin.PulsePicker.csproj',
 'tests/Listary.Adapter.Tests.csproj')
foreach($relative in $projects){
 $project=Join-Path $repo ('integrations/Listary.Adapter/'+$relative)
 & dotnet restore $project --locked-mode --source https://api.nuget.org/v3/index.json --nologo "-p:PulseListaryArtifactsRoot=$artifacts"
 if($LASTEXITCODE){throw "适配器依赖还原失败：$relative"}
 & dotnet build $project --no-restore -c Release --nologo "-p:PulseListaryArtifactsRoot=$artifacts"
 if($LASTEXITCODE){throw "适配器构建失败：$relative"}
}
$test=Join-Path $artifacts 'bin/Pulse.Listary.AdapterTests/Release/net48/Pulse.Listary.AdapterTests.exe'
& $test *> (Join-Path $artifacts 'tests.log')
if($LASTEXITCODE){throw '适配器定向检查失败'}
$package=Join-Path $artifacts 'package'
if(Test-Path -LiteralPath $package){throw '适配器包目录已存在，拒绝覆盖'}
foreach($kind in 'Manager','Picker'){
 $source=Join-Path $artifacts ("bin/Listary.FileAppPlugin.Pulse$kind/Release/net48")
 $target=Join-Path $package ("Pulse$kind")
 foreach($file in Get-ChildItem -LiteralPath $source -Recurse -File){
  if($file.Extension -eq '.pdb'){continue}
  if($file.Attributes -band [IO.FileAttributes]::ReparsePoint){throw '适配器产物包含重解析点'}
  $relative=[IO.Path]::GetRelativePath($source,$file.FullName)
  $destination=Join-Path $target $relative
  New-Item -ItemType Directory -Path (Split-Path $destination) -Force|Out-Null
  Copy-Item -LiteralPath $file.FullName -Destination $destination
 }
}
Copy-Item -LiteralPath (Join-Path $repo 'integrations/Listary.Adapter/README.md') -Destination (Join-Path $package '使用说明.md')
Copy-Item -LiteralPath (Join-Path $repo 'LICENSE') -Destination (Join-Path $package 'LICENSE.txt')
$entries=@(foreach($file in Get-ChildItem -LiteralPath $package -Recurse -File){
 [ordered]@{path=[IO.Path]::GetRelativePath($package,$file.FullName).Replace('\','/');bytes=$file.Length;sha256=(Get-FileHash -LiteralPath $file.FullName).Hash.ToLowerInvariant()}
})
[ordered]@{schema=1;source_commit=(& git -C $repo rev-parse HEAD).Trim();runtime_loading='须由真实 Listary 宿主加载确认';files=$entries}|ConvertTo-Json -Depth 5|Set-Content (Join-Path $package 'adapter-manifest.json') -Encoding utf8
Get-Content -LiteralPath (Join-Path $artifacts 'tests.log')
