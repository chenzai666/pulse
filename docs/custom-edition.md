# Pulse 定制版

基于 jimmgreen/pulse 的最新多窗口源码，保留 Win32、Direct2D 和 DirectComposition 主界面。

## 本次功能

- 标签排序、拖出独立窗口、跨窗口合并、拖到内容区分栏与窗格还原标签，继续使用上游最新实现。
- 空格优先使用完整 QuickLook 插件宿主；宿主仅在预览时启动，退出或关闭所属 Pulse 窗口时释放。无可用查看器、启动失败或安全模式下使用 Pulse 原生预览。
- FFmpeg、现代图像、RAW 相机照片和 7-Zip 预览增强包随完整包预置，设置页显示“已内置”，用户无需再次下载安装。启用开关和自定义 FFmpeg 保留。
- Listary 官方文件管理器插件读取当前目录，将搜索结果交给 Pulse 打开或定位。使用现有 Listary 安装和其已配置的搜索入口，不分发 Listary 的闭源引擎。

## 运行与配置

完整解压后运行 pulse.exe；保留同目录的辅助程序、quicklook、packs、ListaryPlugin 和 licenses 文件夹。定制版使用 %LOCALAPPDATA%\PulseCustom 保存配置，与原版 Pulse 的窗口和配置隔离。

Listary 插件需由现有 Listary 加载。部署时将 ListaryPlugin 内文件放入官方支持的 FileAppPlugins\Pulse 目录，并将 PulseExecutable.path 设为本机定制版 pulse.exe 的绝对路径。现有 Listary 安装在 Program Files 时，写入其插件目录受 Windows 管理员权限保护；不得通过修改目录权限绕过。部署前保留旧插件备份，部署后正常重启 Listary。

测试版不接收上游自动更新，避免更新覆盖定制功能。完整包中包含对应源代码和第三方许可证。

## 构建

使用 GitHub Actions 的“构建 Pulse 定制版”工作流，或者 Windows x64 Visual Studio 2022 C++ 环境执行：

```powershell
pwsh -File scripts/build_custom_ci.ps1
```

云构建执行与本次功能相关的会话、分栏、标签转移和内置增强包回归，校验完整插件及解码组件后再打包。编译通过不能替代实际拖动、预览渲染和 Listary 搜索交接验证。

## 许可证

Pulse 主程序继续遵循 Apache License 2.0。QuickLook 宿主与相关改动遵循 GPL-3.0-or-later，并随包提供固定版本来源、可重建代码和许可证。各预览增强包保留其 FFmpeg、图像库、LibRaw、7-Zip 等许可证、使用限制和源码指引。

支持格式列表表示具备相关解码路径，不表示所有变体、损坏文件、受保护文件或需要外部办公组件的格式均已验证。实际验证结果与限制应在定制版本发布说明中列明。
