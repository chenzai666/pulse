# Pulse 活动目录的 Listary 只读来源

此插件使用 Listary 官方 FileAppPlugin 接口，将 Pulse 主窗口的活动窗格目录提供给 Quick Switch 和“当前打开的文件夹”。打开、保存窗口仍由独立的 `PulseCustom.FileDialog` 插件作为切换目标；Windows 原生文件窗口继续使用 Listary 原有适配。

仅识别进程映像为 `pulse.exe`、窗口类为 `PulseCustomMainWindow` 且带 `Pulse.FolderSource.v1=1` 标记的主窗口。通过直接子控件 `Edit`（ID `0x6F51`）读取已发布的活动目录；单次读取最多等待 1 秒。主窗口切换标签、窗格或目录后刷新该值，虚拟页面和窗口关闭时清空来源。读取过程不访问磁盘或网络共享。

插件只实现 `IGetFolder`，不实现打开目录、打开文件、启动文件管理器或搜索桥接口，不使用旧版命名管道。SDK 的搜索栏类型选择 `Floating`；没有列表绑定和输入转发。Listary 的搜索、快捷键、Quick Switch 触发方式和集成设置由原有 Listary 管理。

将插件独立部署到 `Listary/FileAppPlugins/PulseCustom.FolderSource`，同时保留 `PulseCustom.FileDialog`，正常重启 Listary。两个插件分别声明来源与目标，符合 Listary 的能力分支。

## 定向检查

```powershell
dotnet restore integrations/Listary.Picker.Source/tests/SourceProtocol.csproj --locked-mode --configfile integrations/Listary.Picker.Source/NuGet.config
dotnet build integrations/Listary.Picker.Source/tests/SourceProtocol.csproj -c Release --no-restore
.\out\listary-plugin\bin\SourceProtocol\Release\net48\pulse.exe
```

协议测试使用本进程创建的独立隐藏窗口，覆盖来源/目标能力隔离、中文目录、UNC、活动目录刷新、非法路径、缺少标记、错误控件、销毁后读取和无响应窗口的读取超时；测试线程与窗口在退出时释放，不操作用户窗口或文件。

功能验证：打开独立的文件保存窗口，在 Pulse 主窗切换活动目录后回到保存窗口，确认 Listary Quick Switch 跳至该目录且保留文件名。资源管理器来源与 Windows 原生窗口也应继续按照原有 Listary 设置工作。
