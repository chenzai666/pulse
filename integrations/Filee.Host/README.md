# Pulse 常用格式转换宿主

本宿主引用固定 Filee `v1.10.2` 的 Core 与 Engines，供 Pulse 原生菜单按需调用。宿主运行在独立进程，输入和输出使用 UTF-8 JSON Lines，转换引擎在后台执行。

## 支持范围

实际菜单以 `capabilities` 返回的可用路由为准；同时选择多种文件时，只返回每个文件都能转换到的目标。

| 输入 | 目标 | 说明 |
|---|---|---|
| JPG、PNG、WebP、BMP、GIF | JPG、PNG、WebP、BMP、GIF、PDF | 包含重新编码；动画、透明度与颜色空间的保留取决于目标格式 |
| HEIC、AVIF | JPG、PNG、WebP、BMP、GIF、PDF | 使用预置原生图片库，菜单不开放 HEIC/AVIF 输出 |
| DOCX/DOCM/DOTX/DOTM、XLSX/XLSM/XLTX/XLS、PPTX/PPTM/POTX/PPSX | PDF | 使用内置读取器→内部 HWPX→rhwp；XLS 必要时先转 XLSX |
| PDF | JPG、PNG、WebP、BMP、GIF | 多页 PDF 会产生多个输出文件 |
| MP4、MKV、MOV | MP4、MKV、MOV、GIF、MP3、WAV、FLAC、AAC、M4A | 重编码默认 MPEG-4/AAC；已兼容流可无损重封装；抽取音频需要源文件有音频流 |
| MP3、WAV、FLAC、AAC、M4A | MP3、WAV、FLAC、AAC、M4A | 输出格式与码率由 Filee 的安全默认值决定 |
| ZIP、7z、TAR、TAR.GZ、GZ、RAR | ZIP、7z、TAR、TAR.GZ、解压文件夹 | RAR 仅输入；GZ 单文件路线以实际能力清单为准 |
| Markdown | HTML、DOCX、PDF | Pandoc 明确预置；HTML 可走内置 Markdown 引擎，PDF 走 rhwp，无需 TeX |

不预置 LibreOffice、calibre、Ghostscript 或 Java。旧版二进制 DOC、PPT 不提供转换。HWP/HWPX 仅作为文档转 PDF 的内部处理格式，菜单不开放这些输入和输出。

## 文档转换限制

Filee 的内置文档读取器和 rhwp 无法保证与 Microsoft Office 完全一致。复杂图表、公式、SmartArt、修订、批注、动画、字体替换和分页可能丢失或变化。XLS 中的公式使用已保存的计算结果。重要文档应先在副本上转换，并检查输出页数和版式。

Pandoc 固定参数包含 `--sandbox`，没有用户提供的 filters、Lua、defaults、模板参数或外部命令。Markdown→DOCX 中，sandbox 可能限制本地图片或外部资源的嵌入；Markdown→HTML 可引用原文件附近的图片。原始 Markdown 中的 HTML 内容可能保留在 HTML 输出中；打开不可信 HTML 时仍须谨慎。

## 构建与云端自检

推荐 GitHub Actions Windows x64 runner，先通过 `actions/setup-dotnet` 安装 `.NET 10 SDK`，并通过 MSYS2/UCRT64 从固定源码构建 FFmpeg/LAME。

```powershell
# 先完成 workflow 中 build_conversion_ffmpeg.sh 的 MSYS2/UCRT64 构建步骤。
pwsh -NoProfile -File scripts/build_filee_host.ps1 -BuildDir build-custom
dotnet run --project integrations/Filee.Host/tests/Pulse.Filee.Host.Tests.csproj -c Release -- --runtime build-custom/conversion/Pulse.Filee.Host.exe
```

`BuildDir` 可以是仓库内的相对或绝对路径。最终输出为 `<BuildDir>/conversion`；该目录必须为空。脚本不覆盖已有运行时。

脚本从 `<BuildDir>/conversion-ffmpeg-source-runtime` 复制已经完成的 LGPL FFmpeg。若云构建输出目录不同，可通过 `-FfmpegRuntimePath <仓库内路径>` 指定。构建脚本与所需工具链说明见 `scripts/build_conversion_ffmpeg.sh`；不会下载 Gyan GPL 完整包。

本机只验证固定来源和适配点时：

```powershell
pwsh -NoProfile -File scripts/build_filee_host.ps1 -PrepareOnly -BuildDir out/filee-prepare -SourceArchivePath out/filee-source.zip
dotnet run --project integrations/Filee.Host/tests/Pulse.Filee.Host.Tests.csproj -c Release
```

`-PrepareOnly` 仅校验并适配 Filee 源码，不恢复 NuGet，不下载四个运行时引擎，不生成可运行宿主。没有 `--runtime` 的自检仅验证协议、路径、发布、清理及清单防护，不代表格式转换已经验证。

完整自检使用自动生成的测试文件，覆盖 Markdown→HTML/DOCX/PDF、图片与 PDF、DOCX/XLSX/XLS/PPTX→PDF、音视频、压缩包、非法路径、恶意归档、源文件保持、输出不覆盖、取消和父进程退出后的子树清理。自检报告以每项真实输出为证据；失败返回非零退出码。

## 固定依赖与包内材料

`upstream.json` 固定 Filee 源提交、rhwp/7-Zip/Pandoc 的 URL、大小和 SHA256，以及 FFmpeg/LAME 源码与构建脚本。三个预编译引擎下载总计约 61 MiB；FFmpeg/LAME 在云端构建。自包含 .NET 和图片/PDF 原生库的发布体积需要以实际云端产物为准。

运行时包含：

- `Pulse.Filee.Host.exe`、Core、Engines 及自包含 win-x64 依赖。
- `engines/rhwp`、`engines/7zip`、`engines/ffmpeg`、`engines/pandoc`。
- `runtime-manifest.json`，列出全部包内文件的字节数与 SHA256。
- `dependency-locks`，保存实际 NuGet 版本、间接依赖与 SHA512 contentHash。
- `licenses`、中文第三方说明及发布方原有许可证。
- `sources`，保存 Filee 原始与适配后的源码、Pandoc/7-Zip/FFmpeg/LAME 源码；FFmpeg 运行时另带编译补丁、脚本与构建记录。

启动时校验清单、必要文件、全部文件 SHA256，并拒绝未列出的附加文件和重解析点。此校验用于检测包损坏或部署不完整；清单本身没有数字签名，应通过可信发布渠道获取整个包。

## 隔离与资源限制

- 宿主校验 Pulse 的 PID、Windows 用户 SID 和会话；父进程退出或输入管道断开后停止转换。
- Windows Job Object 限制进程树最多 64 个进程、总提交内存 2 GiB，宿主退出时终止引擎子树。
- 每批最多 128 个文件；每个输入最多 2 GiB；引擎并发上限 2。
- 每批最长 30 分钟；取消超过 3 秒仍不结束时退出宿主，由 Job Object 清理子树。
- 外部引擎取消后有界等待原始进程句柄退出；完成前确认宿主 Windows Job 没有存活子进程，必要时终止实际成员（含控制台子进程）。成员身份通过内核 Job 检查，避免误杀复用 PID 的其他进程。
- 私有临时目录最多 4 GiB、50,000 个条目；监视是周期检查，不能代替硬磁盘配额。
- 归档在实际解包之前检查路径、链接、加密、条目数和预计展开大小；每阶段最多 50,000 条、2 GiB。
- 输入、目标目录和运行时路径拒绝设备路径、备用数据流、相对路径、模糊设备名和重解析点。
- 最终结果先写私有 stage，再用不覆盖的唯一名称发布。取消或失败可能留下此前已发布的完整输出，其路径会出现在结果中。
- 正常完成清理当前任务 stage；被强制终止的会话在下次启动时通过独占锁和标记受限清理，每次最多 8 个、仅处理超过 5 分钟的直接 GUID 子目录。

Job Object 用于资源和进程生命周期管理，没有降低当前用户权限，也没有提供 AppContainer 文件系统或网络隔离。建议转换不可信文件时使用普通用户，保持引擎安全更新，并避免同时由其他进程修改输入和输出目录。路径检查不能完全消除同一用户并发改名造成的文件系统竞争。

进程清理诊断仅写 stderr，包含引擎文件名、PID、创建时间和终止/退出状态，不包含命令行或用户文档路径。自检保留实际进程句柄，并在 5 秒内确认捕获子树退出；创建时间用于排除过期父 PID 关系和 PID 复用。

## 许可与重新分发

Pulse 宿主遵循 Pulse 的 Apache-2.0 许可；Filee Core/Engines 是 MIT。rhwp 为 MIT，7-Zip 带 LGPL 与 unRAR 限制，Pandoc 为 GPL-2.0-or-later，FFmpeg 为禁用 GPL/nonfree 的 LGPL-2.1-or-later 构建，LAME 为 LGPL-2.0-or-later。各组件分别保留自己的许可，不能把整个转换目录统一声明为 MIT 或 Apache。

详见 [第三方许可说明](THIRD-PARTY-NOTICES.md)。FFmpeg 包内保留实际构建使用的固定源码、许可、补丁、脚本、构建配置及工具链记录，使用者可自行修改和重建。没有预置 x264/x265 等 GPL 编码库；视频压缩率及部分设备兼容性与 H.264 构建不同，转换完成后应检查目标播放器支持情况。
