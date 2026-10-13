# 随定制版发布的预览增强包

Pulse 定制版将四个增强包直接放进安装包和便携包。安装完成或完整解压后即可使用，无需再次下载安装解码器。

| 增强包 | 主要用途 | 随包组件 |
| --- | --- | --- |
| FFmpeg 媒体包 | 媒体缩略图、信息，以及原生媒体解码失败时的预览播放 | `ffmpeg.exe`、`ffprobe.exe`、编译配置、LGPL 许可证及源码获取说明 |
| 现代图像包 | HEIF/HEIC、AVIF、JPEG XL、HDR、EXR 等图像预览 | `pulse-imgpack.exe`、`heif.dll`、`libde265.dll`、第三方声明 |
| RAW 包 | 相机 RAW 图像预览 | `pulse-rawpack.exe`、`raw_r.dll`、第三方声明 |
| 7-Zip 包 | 压缩包内容预览 | `7z.exe`、`7z.dll`、许可证及说明 |

## 目录与设置

程序旁的目录使用现有增强包安装格式，主程序与预览宿主均从自身所在目录识别它，工作目录或系统 `PATH` 不会改变内置包的位置。

```text
Pulse 定制版/
  pulse.exe
  Pulse.Preview.exe
  packs/
    bundled-packs.json
    preview-packs-assets.json
    pulse-pack-ffmpeg-1.0.1.json
    pulse-pack-images-1.0.1.json
    pulse-pack-raw-1.0.1.json
    pulse-pack-archive-1.0.1.json
    ffmpeg/
      installed.json
      1.0.1/...
    images/
      installed.json
      1.0.1/...
    raw/
      installed.json
      1.0.1/...
    archive/
      installed.json
      1.0.1/...
```

设置页显示“已内置”和包版本，仍可通过开关停用。内置文件由 Pulse 安装程序管理，增强包卡片不提供单独删除内置文件的操作。

四个包的开关默认启用。已保存的停用设置继续有效。选择自定义 FFmpeg 后，即使指定文件不存在，也不会自动改用其他来源。

下载安装和更新仍写入当前用户数据目录。标准版为 `%LOCALAPPDATA%\Pulse\packs`；使用 `PULSE_CUSTOM_EDITION=ON` 构建的定制版为 `%LOCALAPPDATA%\PulseCustom\packs`。其中的 `packs.json` 保存启用状态和自定义 FFmpeg 来源，程序不会往安装目录写入设置。

同一个增强包先检查用户目录中的完整安装标记和主工具，找不到时再使用程序旁的内置副本。用户安装的更新版可覆盖内置版本的选择。删除用户更新后，内置副本恢复可用。

## 构建和打包

在 Windows x64 和 PowerShell 7.2 以上环境执行。在线获取素材需要 `gh` CLI 能读取公开发布。

```powershell
# 构建完成后，将固定版本的四个增强包预置到 build/packs。
pwsh -File scripts/bundle_preview_packs.ps1 -BuildDir build

# 已下载过素材时可离线复用；仍执行完整的哈希和大小校验。
pwsh -File scripts/bundle_preview_packs.ps1 -BuildDir build `
    -AssetDirectory build/preview-pack-assets

# 正式打包前核验程序旁完整目录，不下载、不更改文件。
pwsh -File scripts/bundle_preview_packs.ps1 -BuildDir build -VerifyOnly
```

构建流水线应在生成安装包或便携包前执行预置脚本，再把 `build/packs` 整个目录树加入安装负载和便携 ZIP。缺少文件、大小不符、哈希不符或出现额外 DLL 时应停止打包。

脚本固定使用公开发布 [`jimmgreen/pulse@v1.0.53`](https://github.com/jimmgreen/pulse/releases/tag/v1.0.53) 的 `1.0.1` 增强包，共 17 个实际文件。五份 JSON 元数据的大小和 SHA-256 固定在脚本中；每个压缩文件及解压后的实际文件，使用 `src/app/pack_catalog_generated.h` 中现有的大小和 SHA-256。不会从未校验 URL 获取或运行解码 DLL。

在线模式通过 `gh release view` 核对发布状态、标签、文件大小及 GitHub 提供的摘要，再使用 `gh release download` 获取指定名称的素材。离线模式使用同一组固定校验规则。

LZMS 解压在构建步骤完成，仅调用 Windows 系统目录中的 `cabinet.dll`。运行 Pulse 时不会要求用户解压或下载安装。实际解码和媒体播放继续使用现有预览宿主及后台任务，不在界面线程增加解码工作。

所有文件验证成功后，脚本才生成四份 `installed.json` 并切换完整的 `packs` 目录。已有目录只有通过同样的完整校验后才可被替换；失败会保留已有完整副本。缓存中的压缩包不属于最终安装负载。

## 许可证与源码

必须完整保留发布素材中的许可证、第三方声明、`SOURCE.txt`、`BUILDCONF.txt` 和说明文件，不能只取可执行程序或 DLL。

FFmpeg 为现有目录指定的 LGPL 仅解码构建；现代图像包、LibRaw、7-Zip 的许可证和源码获取指引分别在各自包目录中。再分发时应遵循这些文件中的条款及源码提供要求。这个预置过程不包含其他第三方搜索引擎。

## 定向验证

```powershell
# 使用已校验的下载缓存，在 build 的独立临时目录验证真实素材。
pwsh -File scripts/test_bundle_preview_packs.ps1 -BuildDir build

# 原生独立回归目标，由构建流水线编译后运行。
cmake --build build --target pulse_bundled_preview_packs_test
.\build\pulse_bundled_preview_packs_test.exe
```

脚本验证完整四包和许可说明，并注入压缩文件、解压后文件及额外 DLL 的篡改，确认拒绝受污染负载且保留原有完整目录。

原生回归在独立临时目录复制测试程序，覆盖程序旁四包发现、不同工作目录、版本更新优先级、停用设置、自定义 FFmpeg、非法安装目录及缺失主工具。它不会写入真实用户配置，也不会修改当前使用中的 Pulse 安装目录。
