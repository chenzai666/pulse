# Pulse 的 Listary 专属适配器

本目录使用 Listary 公开 `FileAppPlugin` 1.0 SDK，分别适配 Pulse 文件管理器和 Pulse 自定义文件选择器。两个插件独立发布，避免把文件管理器与文件对话框的搜索条和 Quick Switch 角色混为一体。现有 `integrations/Listary.Pulse` 的具名管道适配器保留原状。

| 插件 | 清单 ID | 搜索条 | 目录来源 | Quick Switch 目标 | 能力 |
| --- | --- | --- | --- | --- | --- |
| 管理器 | `PulseManager` | `Floating` | 是 | 否 | 当前目录、活动标签页内目录导航 |
| 选择器 | `PulsePicker` | `Fixed` | 否 | 是 | 当前目录、目录导航、准备选择文件 |

选择器收到文件结果后只准备选择，最终打开或保存继续由用户确认，保留原有文件类型、覆盖与保存校验。管理器插件不启动 Pulse、不执行命令行、不接管其他应用对话框。

## 文件布局

- `Manager`、`Picker`：独立 net48 工程、唯一公开插件类和清单。
- `Shared`：固定协议、窗口身份、受限目录读取、取消与隐私诊断。
- `tests`：不访问真实窗口、用户文件或设置的定向控制台测试。

依赖固定为 `Listary.FileAppPlugin 1.0.0` 和 .NET Framework 4.8 引用程序集；`packages.lock.json` 保存依赖解析结果。按 Files 官方工程的引用方式，输出包含 MIT 授权的公开 SDK 接口 DLL 及其开源运行时依赖，部署时应完整保留，排除缺少依赖造成的加载失败。输出不包含 Listary 宿主或搜索引擎程序，许可证映射见 `THIRD-PARTY-NOTICES.md`。产品构建优先在 GitHub Actions 中执行；以下命令仅用于受影响的轻量 C# 工程验证。

## 构建与定向测试

在仓库根目录执行 PowerShell：

```powershell
dotnet restore .\integrations\Listary.Adapter\Manager\Listary.FileAppPlugin.PulseManager.csproj --locked-mode
dotnet build .\integrations\Listary.Adapter\Manager\Listary.FileAppPlugin.PulseManager.csproj -c Release --no-restore
dotnet restore .\integrations\Listary.Adapter\Picker\Listary.FileAppPlugin.PulsePicker.csproj --locked-mode
dotnet build .\integrations\Listary.Adapter\Picker\Listary.FileAppPlugin.PulsePicker.csproj -c Release --no-restore
dotnet restore .\integrations\Listary.Adapter\tests\Listary.Adapter.Tests.csproj --locked-mode
dotnet run --project .\integrations\Listary.Adapter\tests\Listary.Adapter.Tests.csproj -c Release --no-restore
.\out\listary-adapter\bin\Listary.Adapter.Tests\Release\net48\Pulse.Listary.AdapterTests.exe --loader-contract .\out\listary-adapter\bin\Listary.FileAppPlugin.PulseManager\Release\net48\Listary.FileAppPlugin.PulseManager.dll .\out\listary-adapter\bin\Listary.FileAppPlugin.PulsePicker\Release\net48\Listary.FileAppPlugin.PulsePickerAdapter.dll
```

默认产物位于仓库 `out/listary-adapter`。需要任务隔离目录时，在 restore/build/run 命令均加入 `-p:PulseListaryArtifactsRoot=<绝对目录>`，将临时 `obj` 和 `bin` 集中保存。测试通过编译常量 `ADAPTER_TESTS` 禁止诊断写入，不修改用户的 Pulse/Listary 配置，也不启动产品进程。

公开 SDK loader 使用 `Assembly.GetExportedTypes()` 分别发现插件、窗口与标签页类型；三类实现都必须公开。每个生产 DLL 恰好导出一个插件、一个窗口和一个标签页，构造窗口与标签页的入口保持内部可见。管理器和选择器的标签页在各自工程编译，避免通过导出类型错误地把文件选择能力声明到管理器。`--loader-contract` 直接检查传入的两个生产 DLL，不调用其初始化或操作真实窗口，也不以测试 EXE 内的类型替代验收。

新选择器的程序集名称与入口为 `Listary.FileAppPlugin.PulsePickerAdapter.dll`，与旧 `PulseCustom.FileDialog` 使用的 `Listary.FileAppPlugin.PulsePicker.dll` 分开。两个插件同时保留时，避免 .NET Framework `Assembly.LoadFrom` 因相同程序集身份复用旧 DLL。工程文件与默认输出目录仍为 `Listary.FileAppPlugin.PulsePicker`，实际入口 DLL 名称以清单为准。

## 部署与版本验证

将每个工程的完整输出放入各自独立的插件目录，包括入口 DLL、对应的 `ListaryPluginManifest.json`、`Icon.png`、公开 SDK 与开源运行时依赖和许可证。每个目录必须另有 UTF-8 文本文件 `PulseExecutable.path`，内容为受信任 Pulse 可执行文件的完整本地路径，例如：

```text
E:\PulseCustom\pulse.exe
```

插件要求目标进程查询返回的完整可执行路径与该配置完全匹配（忽略大小写）。配置应随受信任安装受控部署，避免从共享目录或他人可写目录加载插件与配置。路径配置缺失时初始化返回失败，并记录固定诊断事件。

Listary 官方旧版 SDK 的 Getting Started 描述了 `Listary/FileAppPlugins` 部署与重启加载流程。当前 7.1 发行采用内置集成，第三方 SDK 的本机入口需按实际安装版本确认。先验证 `Initialize` 日志与集成开关，再验收实际搜索条；本工程不修改 Listary、不将 Pulse 冒充 Files，也不自行升级或降级 Listary。

2026年11月7日对现用 Listary 7.1.0.47 的加载检查：完整管理器候选部署并正常重启后，未出现 `Initialize` 记录，集成设置中也没有新增 Pulse 项。适配源码和隔离协议测试已完成，实际宿主接入仍待打通；这些产物应作为适配交付与上游接入的候选，不能描述为该版本已恢复搜索条。现有 7.1.0.45 曾正常的用户反馈仍保留为历史基线。

公开资料：

- [Listary FileAppPlugin SDK 与部署说明](https://github.com/listary/Listary.FileAppPlugin)
- [Files 官方适配器](https://github.com/files-community/Listary.FileAppPlugin.Files)
- [Listary 官方适配器收录目录](https://github.com/listary/Listary.FileAppPlugin.Repository)

固定诊断写入 `%LOCALAPPDATA%\PulseCustom\Diagnostics\ListaryAdapter\manager.log` 或 `picker.log`。每行只含 UTC 时间、宿主 PID 和固定事件名（如 `initialize_enter`、`initialize_ready`、`bind_accepted`、`dispose`）；每个文件达到 32 KiB 后停止追加。诊断不包含目录、搜索词或请求正文。使用时间与 PID 区分本次加载和历史记录。

## 协议与安全边界

每次绑定、读取与发送前重新核对 HWND、窗口类、专属 marker、PID、进程创建时间、完整 EXE 路径、当前用户 SID 和会话。进程只申请查询身份和读取 token 的最小权限。关闭、HWND/PID 复用、卸载或窗口解绑后回调失效。

管理器必须匹配 `PulseCustomMainWindow`、`Pulse.FolderSource.v1=1`、`Pulse.Listary.Adapter.v1=1`。目录来自本窗口的只读 `Edit 0x6F51`。`Pulse.Listary.Tab.v1` 是非零 32 位 generation；获取目录前后与发送前均重验，以拒绝旧标签页、活动面板或目录切换后的操作。

管理器写入通过 SDK `IFileAppPluginHost.SendCopyData` 发送：

```text
WM_COPYDATA:
  wParam = 当前 Listary 进程的现有顶层 HWND
  dwData = 0x504C534D (PLSM)
  cbData <= 65536
  data = uint32 operation=1
         uint32 chars（包含末尾 UTF-16 NUL）
         uint64 generation（高32位必须为0）
         UTF-16LE path + NUL
```

接收端核对所声明的 Listary sender 窗口身份和 generation，并把目录交给原有异步导航流程。`WM_COPYDATA.wParam` 是调用者提供的窗口句柄，同用户进程可以伪造，因此它属于窗口交互约束；接口权限仅为目录导航，不提供命令执行或自动确认。SDK `SendCopyData(hWnd, sourceWindow, dwData, data, dataSize, timeout)` 的第五个参数必须等于 `data.Length`，测试会明确检查这一点。插件使用 750 ms 的宿主发送超时，并丢弃在线程池排队超过 1 秒的回调。

选择器必须匹配 `PulseFolderPickerWindow` 和 `Pulse.Picker.Listary.v1=1`；当前目录读取本窗口 `Edit 101`。保留现有 `PLST` 协议：`dwData=0x504C5354`，`wParam=1` 表示目录、`2` 表示文件，正文仅为有界 UTF-16LE 路径与单个末尾 NUL。该历史选择器协议与新增管理器 sender/generation 协议分别处理。

路径仅接受绝对盘符或完整 UNC 文件系统路径。拒绝设备/扩展命名空间、相对路径、控制字符、内嵌 NUL、无效代理对、备用数据流、设备名、尾点/空格及超长数据。目录读取采用有上限的 `WM_GETTEXT` 与 200 ms 超时。导航只使用公开宿主消息接口，不注入、不修改 UIPI 白名单、不模拟按键或点击确认。

## 验收范围

定向测试覆盖路径和字节边界、PLSM/PLST 编码、两个插件的角色、窗口身份变化、tab generation、host 拒绝/超时、卸载与窗口取消，以及选择器无自动确认消息。实际部署还需在用户当前 Listary 版本验证两个插件的加载、绑定、搜索条展示和目录/文件结果；多标签页、UNC 和关闭窗口应一并实测。
