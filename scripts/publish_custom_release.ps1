#requires -Version 7.2
param(
    [Parameter(Mandatory)][string]$InstallerDirectory,
    [Parameter(Mandatory)][ValidatePattern('^[0-9a-f]{40}$')][string]$SourceCommit,
    [Parameter(Mandatory)][ValidatePattern('^[0-9]+$')][string]$VerificationRunId
)
$ErrorActionPreference = 'Stop'
$repository = 'chenzai666/pulse'
$tag = 'v1.0.53'
$installerName = 'Pulse-Custom-Setup-v1.0.53-x64.exe'
$installer = Join-Path $InstallerDirectory $installerName
$checksum = $installer + '.sha256'
$provenance = Join-Path $InstallerDirectory 'provenance.json'
foreach ($file in @($installer,$checksum,$provenance)) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "发布文件缺失：$file" }
}
$expectedHash = ((Get-Content -LiteralPath $checksum -Raw).Trim() -split '\s+')[0]
if ($expectedHash -cnotmatch '^[0-9a-f]{64}$' -or (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash -ine $expectedHash) {
    throw '发布前安装包 SHA256 不匹配'
}
$notes = Join-Path $InstallerDirectory 'release-notes.md'
@"
# Pulse 定制版 v1.0.53

版本号按要求固定为 **v1.0.53**，本次应用源码：``$SourceCommit``。

## 下载安装

- Windows x64 安装包：**$installerName**。安装到当前用户目录，使用独立快捷方式和卸载项，与原版程序及配置隔离。
- 首次安装和覆盖升级不会自动启动软件、设置文件关联或安装索引服务；卸载保留用户配置。
- 安装包尚未进行代码签名，SHA256：``$expectedHash``。

## 本次调整

- 设置移除“搜索与索引”和“重复文件”；停止定制版内置索引及重复扫描后台任务。全局搜索使用现有 Listary，当前目录保留筛选。
- 新标签页默认打开主页，软件的项目主页和版本入口指向本仓库。
- 支持可识别的 Windows 标准打开、保存和选择文件夹窗口；设置关闭接管可恢复原版。不支持或权限不同的窗口保留原生流程。
- 保留既有标签分栏、独立窗口、放到“＋”合并、预览、文件转换及状态中心。

## 验证与边界

接管、恢复及相关定向验证：[运行 $VerificationRunId](https://github.com/$repository/actions/runs/$VerificationRunId)。安装器另已完成隔离 runner 上的安装、覆盖升级、卸载与文件摘要核对。

Listary 需用户已有安装；插件随包提供。旧版系统索引服务不会由安装器自动移除，已有系统接管应在卸载前按实际归属恢复。

完整对应源码与第三方许可证随安装包提供，构建来源见 ``provenance.json``。
"@ | Set-Content -LiteralPath $notes -Encoding utf8
$existing = & gh release view $tag --repo $repository --json isDraft,targetCommitish 2>$null
if ($LASTEXITCODE -eq 0) {
    $release = $existing | ConvertFrom-Json
    if (-not $release.isDraft -or $release.targetCommitish -ne $SourceCommit) { throw '同版本 Release 已存在，拒绝覆盖已发布或来源不同的资产' }
    & gh release upload $tag $installer $checksum $provenance --repo $repository --clobber
    if ($LASTEXITCODE) { throw '恢复草稿资产失败' }
    & gh release edit $tag --repo $repository --notes-file $notes
    if ($LASTEXITCODE) { throw '更新草稿说明失败' }
} else {
    & gh release create $tag $installer $checksum $provenance --repo $repository --target $SourceCommit --draft --title 'Pulse 定制版 v1.0.53' --notes-file $notes
    if ($LASTEXITCODE) { throw '创建 Release 草稿失败' }
}
$published = & gh release view $tag --repo $repository --json assets
if ($LASTEXITCODE) { throw '读取已上传资产失败' }
$assets = ($published | ConvertFrom-Json).assets
$binary = @($assets | Where-Object name -EQ $installerName)
if ($binary.Count -ne 1 -or $binary[0].digest -ine ('sha256:' + $expectedHash)) { throw '上传后 GitHub 安装包摘要核验失败，保留草稿' }
& gh release edit $tag --repo $repository --draft=false --latest
if ($LASTEXITCODE) { throw '发布 Release 失败，保留草稿' }
Write-Output "https://github.com/$repository/releases/tag/$tag"
