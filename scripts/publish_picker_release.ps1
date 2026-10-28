#requires -Version 7.2
$ErrorActionPreference='Stop'
$repository='chenzai666/pulse'
$commit=(& git rev-parse HEAD).Trim()
if($commit -cnotmatch '^[0-9a-f]{40}$'){throw '无法核实源码提交'}
$build=Get-Content -LiteralPath 'dist/custom-build.json' -Raw|ConvertFrom-Json
$stage=Join-Path (Get-Location) ('dist/'+$build.package)
$manifest=Get-Content -LiteralPath (Join-Path $stage 'package-manifest.json') -Raw|ConvertFrom-Json
if($manifest.commit -cne $commit){throw '安装载荷与源码不一致'}
$iscc=Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6/ISCC.exe'
if(-not(Test-Path -LiteralPath $iscc)){
    & choco install innosetup --version=6.4.3 --yes --no-progress --limit-output
    if($LASTEXITCODE -or -not(Test-Path -LiteralPath $iscc)){throw '安装固定安装器编译器失败'}
}
& "$PSScriptRoot/package_custom_installer.ps1" -StageDir $stage -OutputDir 'dist-installer' -IsccPath $iscc
$output=Join-Path (Get-Location) 'dist-installer'
$installer=Join-Path $output 'Pulse-Custom-Setup-v1.0.53-x64.exe'
$hash=(Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash.ToLowerInvariant()
$userValidation=$env:PICKER_USER_VALIDATION -eq 'true'
[ordered]@{
    source_commit=$commit
    packaging_commit=$commit
    build_run=[long]$env:GITHUB_RUN_ID
    build_url="https://github.com/$repository/actions/runs/$env:GITHUB_RUN_ID"
    version='1.0.53'
    installer_sha256=$hash
    package_files=$manifest.files.Count
    runtime_validation=if($userValidation){'由用户进行界面验收，本次未运行界面测试'}else{'参见本次构建日志'}
    component_base=@{commit='f93f9ba7fe7b927e2a3f30a160e409d192e98e39';artifact=12248306000;sha256='f78faaa087894c2e3a6b58361d0817d82aaf90a473da521c0d9be7a841998025'}
    rebuilt_components=@('pulse.exe')
}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $output 'provenance.json') -Encoding utf8
$notes=Join-Path $output 'release-notes.md'
@"
# Pulse 定制版 v1.0.53

本次源码：``$commit``。安装包仍为 v1.0.53，支持覆盖安装和自定义路径，例如 ``E:\PulseCustom``。

- 修复 Pulse 文件弹窗宿主未初始化文字资源造成的取消按钮、侧边栏和列标题缺字，并增加必要的备用文字。
- 恢复改进后的标准打开、保存和文件夹选择适配；保存提交到原窗口后先等待原软件确认，不立即重新显示原窗口。
- Pulse 弹窗直接跟随 Pulse 主窗口目录，无需安装 Listary；地址正在编辑时保留用户输入。
- 修复快速切换标签或窗口时漏掉最终目录的联动；原版弹窗模式支持首次进入弹窗，并在地址控件初始化完成后处理联动。
- 设置里关闭“文件选择窗口”接管后使用 Windows 原版弹窗。Listary 的原版弹窗搜索框由 Listary 自身提供。
- 保留现有标签、主页、全局搜索、安装目录下的索引、预览和转换功能。升级保留配置和索引。

构建及完整安装载荷摘要已校验。$(if($userValidation){'按用户要求，本次未运行界面测试，实际交互由用户验收。'}else{'相关验证结果见构建记录。'})

构建记录：https://github.com/$repository/actions/runs/$env:GITHUB_RUN_ID

安装包 SHA-256：``$hash``。安装包未签名；对应源码和许可证随安装包保留，来源记录见 provenance.json。
"@|Set-Content -LiteralPath $notes -Encoding utf8
$tag='custom-v1.0.53-dialogs-'+$commit.Substring(0,9)
& gh release view $tag --repo $repository --json tagName 2>$null|Out-Null
if($LASTEXITCODE -eq 0){throw '本次提交的 Release 已存在，停止重复发布'}
& gh release create $tag $installer ($installer+'.sha256') (Join-Path $output 'provenance.json') --repo $repository --target $commit --draft --title 'Pulse 定制版 v1.0.53 · 弹窗修复' --notes-file $notes
if($LASTEXITCODE){throw '上传 Release 草稿失败'}
$release=& gh release view $tag --repo $repository --json assets
if($LASTEXITCODE){throw '无法核实上传资产'}
$assets=($release|ConvertFrom-Json).assets
$binary=@($assets|Where-Object name -EQ 'Pulse-Custom-Setup-v1.0.53-x64.exe')
if($binary.Count -ne 1 -or $binary[0].digest -ine ('sha256:'+$hash)){throw 'GitHub 安装包摘要不匹配，保留草稿'}
& gh release edit $tag --repo $repository --draft=false --latest
if($LASTEXITCODE){throw '发布 Release 失败，保留草稿'}
Write-Output "https://github.com/$repository/releases/tag/$tag"
