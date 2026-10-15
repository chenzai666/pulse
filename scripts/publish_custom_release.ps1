#requires -Version 7.2
param(
    [Parameter(Mandatory)][string]$InstallerDirectory,
    [Parameter(Mandatory)][ValidatePattern('^[0-9a-f]{40}$')][string]$SourceCommit,
    [Parameter(Mandatory)][ValidatePattern('^[0-9]+$')][string]$VerificationRunId,
    [ValidatePattern('^custom-v1\.0\.53(?:-[a-z0-9-]+)?$')][string]$ReleaseTag = 'custom-v1.0.53'
)
$ErrorActionPreference = 'Stop'
$repository = 'chenzai666/pulse'
$tag = $ReleaseTag
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
$origin = Get-Content -LiteralPath $provenance -Raw | ConvertFrom-Json
if ($origin.source_commit -cne $SourceCommit -or $origin.artifacts[1].run -ne $VerificationRunId) {
    throw '发布参数与已验证的构建来源不一致'
}
$referenceName = 'refs/tags/' + $tag
$references = & gh api "repos/$repository/git/matching-refs/tags/$tag"
if ($LASTEXITCODE) { throw '无法核实发布标签，停止发布' }
$reference = @($references | ConvertFrom-Json | Where-Object ref -EQ $referenceName)
if ($reference.Count -gt 1) { throw '发布标签不是唯一引用' }
if ($reference.Count -eq 0) {
    & gh api --method POST "repos/$repository/git/refs" -f "ref=$referenceName" -f "sha=$SourceCommit" > $null
    if ($LASTEXITCODE) { throw '创建独立定制版标签失败，不移动已有标签' }
    $tagCommit = $SourceCommit
} else {
    $tagObject = $reference[0].object
    for ($depth=0; $tagObject.type -eq 'tag' -and $depth -lt 8; ++$depth) {
        $annotation = & gh api "repos/$repository/git/tags/$($tagObject.sha)"
        if ($LASTEXITCODE) { throw '无法解析已有发布标签' }
        $tagObject = ($annotation | ConvertFrom-Json).object
    }
    if ($tagObject.type -ne 'commit') { throw '发布标签不能解析为提交' }
    $tagCommit = $tagObject.sha
}
if ($tagCommit -cne $SourceCommit) { throw '已有发布标签指向不同源码，拒绝移动标签或混用安装包' }
$notes = Join-Path $InstallerDirectory 'release-notes.md'
$componentNotes = ''
if ($origin.components) {
    $componentNotes = '主程序及界面验证复用未改动的源码 ' + $origin.components.app_source_commit + '（运行 ' + $origin.components.app_verification_run + '）；索引进程按本次源码重新编译并验证，组件摘要与来源记录在 provenance.json。'
}
@"
# Pulse 定制版 v1.0.53

版本号按要求固定为 **v1.0.53**，本次应用源码：``$SourceCommit``。

## 下载安装

- Windows x64 安装包：**$installerName**。支持自定义安装目录（例如 **E:\PulseCustom**），升级沿用已选目录；使用独立快捷方式和卸载项，与原版程序及配置隔离。
- 首次安装和覆盖升级不会自动启动软件、设置文件关联或安装索引服务；卸载保留用户配置。
- 安装包尚未进行代码签名，SHA256：``$expectedHash``。

## 本次调整

- 恢复 Pulse 内置全局搜索、地址栏搜索和“搜索与索引”设置；索引保存在安装目录的 ``index`` 文件夹，升级保留。普通用户后台进程扫描可读的本地磁盘，不注册共享系统索引服务。“重复文件”页继续隐藏。
- 新标签页默认打开主页，软件的项目主页和版本入口指向本仓库。
- 支持可识别的 Windows 标准打开、保存和选择文件夹窗口；设置关闭接管可恢复原版。不支持或权限不同的窗口保留原生流程。
- 保留既有标签分栏、独立窗口、放到“＋”合并、预览、文件转换及状态中心。

## 验证与边界

本次定向验证：[运行 $VerificationRunId](https://github.com/$repository/actions/runs/$VerificationRunId)。安装器另已完成隔离 runner 上的安装、覆盖升级、卸载与文件摘要核对。

$componentNotes

不再附带或加载 Listary 集成插件。旧版系统索引服务不会由安装器自动移除，已有系统接管应在卸载前按实际归属恢复。

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
    & gh release create $tag $installer $checksum $provenance --repo $repository --target $SourceCommit --verify-tag --draft --title 'Pulse 定制版 v1.0.53' --notes-file $notes
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
