# 格式转换组件第三方许可与源码

此说明只适用于 `conversion` 转换目录。Pulse 主项目的 Apache-2.0 许可保持不变。

| 组件 | 固定版本 | 许可 | 随包材料与源码 |
|---|---|---|---|
| Pulse.Filee.Host | 协议 1 | Apache-2.0 | 宿主和构建脚本在适配后的 Filee 源码 ZIP 中；`licenses/Pulse-APACHE-2.0.txt` |
| Filee Core / Engines | 1.10.2，`3daacc0ab3cf4052a3922b86d5dd5ceb5fcd8a54` | MIT | 原始和适配后的源码 ZIP、`licenses/Filee-MIT.txt` |
| rhwp | 0.8.7，`1a76570e833917d15817415a53c09ad61ab3203f` | MIT | `licenses/rhwp-MIT.txt`；[固定源码](https://github.com/edwardkim/rhwp/tree/1a76570e833917d15817415a53c09ad61ab3203f)，归档校验见 `upstream.json` |
| 7-Zip console / 7z.dll | 26.03 | LGPL-2.1-or-later、BSD、unRAR 限制 | `engines/7zip/License.txt`、readme；`sources/7z2603-src.tar.xz` |
| Pandoc | 3.12，`381230ee36d8fa871c28f8982e22579f1a8f723b` | GPL-2.0-or-later | 保留发布包许可证和文档；`sources/pandoc-3.12-source.zip` |
| FFmpeg 自建最小运行时 | 9.0.2 | LGPL-2.1-or-later | 固定源码、构建脚本、补丁、许可、构建配置和工具链记录 |
| LAME 编码库 | 3.100 | LGPL-2.0-or-later | 固定源码、许可与实际构建补丁，随 FFmpeg 源码运行时保存 |
| NuGet 与 .NET 运行时 | 实际锁定版本 | 以各组件许可为准 | `dependency-locks` 的 SHA512 锁、`licenses/NuGet` 的 nuspec 与 LICENSE/NOTICE；Filee 原始第三方清单作补充 |

## Filee 库

使用 Filee Core 和 Engines，未引用或运行 Filee 的 Avalonia 桌面 UI。Engines 的项目会携带其编译和运行所需的库，即使 Pulse 菜单没有开放相应格式。这些库包括 Magick.NET/ImageMagick 及 delegates、PDFsharp、PDFtoImage/PDFium、SkiaSharp/HarfBuzz、Unhwp、Markdig、ExcelNumberFormat、ExcelDataReader、SharpCompress、PdfPig、MimeKitLite、ACadSharp、AngleSharp、Microsoft.Extensions 和 HWPX 参考文档；以包内实际锁文件和许可元数据为准。

ImageMagick delegates 等原生依赖包含 LGPL、BSD、zlib 等多种许可，不能用 Filee 的 MIT 许可代替。Filee 原始第三方清单包含完整桌面应用的组件，不能据此推断 Pulse 携带了 Avalonia UI、自动更新器或安装器。

## RAR 限制

本宿主只读取 RAR。unRAR 相关代码不得用于开发 RAR 兼容的压缩器或重新实现 RAR 压缩算法；菜单没有 RAR 输出。

## FFmpeg 与 LAME 的源码供应

FFmpeg 从 9.0.2 上游源码和 LAME 3.100 固定源码在 CI 构建，禁用 GPL/nonfree 和自动启用外部依赖，仅开放常见格式所需的内置编码器、解码器、封装器与滤镜。视频重编码用内置 MPEG-4/AAC，MP3 用 libmp3lame。Gyan 的 GPL 完整构建不在包中。

[FFmpeg 固定源码](https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz) SHA256：`8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e`。

[LAME 固定源码](https://downloads.sourceforge.net/project/lame/lame/3.100/lame-3.100.tar.gz) SHA256：`ddfe36cab873794038ae2c1210557ad34857a4b6bdc515785d1da9e175b1da1e`。

`engines/ffmpeg` 保存源码归档、实际补丁、构建脚本和构建记录；`licenses` 另存运行时版本和 `-buildconf`。完整打包源码也包含 `scripts/build_conversion_ffmpeg.sh`。重新分发时须一并保留这些材料，确保用户能够修改 LGPL 代码并重建替代版本。

## 构建适配

适配后的源码包含宿主、JSON Lines 协议、固定下载清单、资源限制、受限临时目录清理，以及以下定向调整：

1. 转换并发上限 2。
2. 归档解包前最多 50,000 条、2 GiB 展开大小。
3. 外部进程 stdout/stderr 捕获约 2 Mi 字符上限。
4. 只从运行时旁的 `engines` 目录定位外部引擎。
5. Pandoc 使用 `--sandbox`，固定参数，不接受过滤器、Lua 或 defaults。
6. MP4/MKV/MOV 重编码默认使用 MPEG-4，沿用 `-q:v` 参数；仅 AVI 使用 XVID tag，不对其他容器添加 x264 的 CRF/preset。

## 从随包源码重建宿主

在 Windows x64 与 .NET 10 SDK 中解压 `Filee-1.10.2-Pulse-host-source.zip`，进入其 Filee 根目录：

```powershell
dotnet restore Pulse.Filee.Host/Pulse.Filee.Host.csproj -r win-x64 -p:SelfContained=true --locked-mode
dotnet publish Pulse.Filee.Host/Pulse.Filee.Host.csproj -c Release -r win-x64 --self-contained true --no-restore -o rebuilt-conversion -p:PublishSingleFile=false -p:DebugSymbols=false -p:DebugType=None
```

然后按 `Pulse.Filee.Host/upstream.json` 获取 rhwp、7-Zip、Pandoc 并核对 SHA256，并通过 `scripts/build_conversion_ffmpeg.sh` 构建固定 FFmpeg/LAME。完整打包可从同一源码包的 `Pulse-打包上下文` 目录调用 `scripts/build_filee_host.ps1`，该目录保留必要的 Pulse 集成源码、两个构建脚本和主项目许可证；它重新下载固定 Filee 原始源码并应用相同适配。
