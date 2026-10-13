# Pulse 内置完整 QuickLook 预览

## 1. 问题总结

Pulse 定制版默认使用自己的原生预览界面：选中文件后按空格打开，图片、RAW、压缩包和媒体增强组件随完整包提供。原生图片内容最长边可解码到 4096 像素，列表缩略图和动画保留独立的资源预算。

QuickLook 4.5.0 的完整插件目录和独立预览宿主仍保存在程序旁的 `quicklook` 目录，作为保留的可选集成组件；默认空格不会启动它。宿主使用原版查看器，保留固定窗口、置顶、全屏、打开文件、分享、复制路径和插件菜单等动作。

QuickLook 未支持某个格式、文件已经失效、运行时不完整或插件出现异常时，Pulse 使用自己的原生预览。原版仅显示文件信息的默认查看器会报告 `ERROR_NOT_SUPPORTED`，防止它覆盖 Pulse 已有的文档、RAW 和其他原生预览能力。依赖 WebView2 的查看器在运行时不可用时也会直接回退。

## 2. 可能原因

预览回退通常由以下原因引起，按排查优先级排列：

1. 当前文件没有匹配的 QuickLook 查看器，或缺少该查看器所需的系统组件。
2. 安装目录的 `quicklook` 文件不完整，或被安全软件阻止。
3. 文件被移动、删除、锁定，或者网络路径暂时不可达。
4. 某个插件加载、解析或渲染失败，或者启动及命令处理超过等待期限。

## 3. 构建与接线步骤

1. 在 GitHub Actions 的 Windows x64 runner 安装 .NET SDK，再执行：

   ```powershell
   powershell -NoProfile -File scripts/build_quicklook_host.ps1 -BuildDir build-custom
   ```

   脚本输出 `build-custom/quicklook`，完整保留并逐文件校验固定官方发行包的插件。安装包和便携包均需递归包含这个目录。`BuildDir` 必须在当前仓库内，输出目录应为空；脚本拒绝覆盖已有内容。

2. 在 Pulse 主程序目标中添加 `src/app/quicklook_integration.cpp`。每个 `AppState` 持有独立的 `app::QuickLookIntegration`，在 UI 线程调用 `Initialize(hwnd, WM_QUICKLOOK_EVENT)`。

3. 定制版空格预览提交 `Toggle(path)`，选择跟随提交 `Switch(path)`。这些方法只更新请求状态和发信号；文件检查、进程启动、管道读写、等待及进程清理都在独立工作线程执行。

4. `WM_QUICKLOOK_EVENT` 的 `lParam` 是整数令牌，通过 `TakeEvent(token, event)` 取出事件。`Failed` 使用 `event.path` 回退到 Pulse 原生预览；`Navigate` 沿用原有的前后文件导航。已过期的请求和窗口通知会被丢弃。

5. 主窗口销毁时调用 `Shutdown()`。它立即取消该窗口的会话，后台清理其全部预览窗口。`Close()` 仅关闭当前预览，保留用户已固定的窗口。固定窗口全部关闭后，宿主自动退出。

6. 需要离线复用归档时，可传入 `-ArchivePath` 与 `-SourceArchivePath`；二者仍必须匹配 `upstream.json` 的固定 SHA256。`-PrepareOnly` 只完成下载校验、解包和源码适配，不编译宿主。

## 4. 接口与通信协议

以下为 C++ 接口示例：

```cpp
if (state.quickLook.Initialize(state.hwnd, WM_QUICKLOOK_EVENT) &&
    state.quickLook.Toggle(selected_path)) {
    // 等待异步事件；Failed 才使用原生预览接管。
}

app::QuickLookEvent event;
if (state.quickLook.TakeEvent(static_cast<UINT_PTR>(lParam), event)) {
    if (event.kind == app::QuickLookEventKind::Failed) {
        // 用 event.path 打开 Pulse 原生预览。
    }
}
```

`active()` 包括已经提交、尚未完成启动或切换的当前预览；`pending()` 表示命令或渲染尚未完成；`running()` 表示宿主进程存在，固定窗口也会维持该状态。窗口固定后，当前预览可以是非活动状态。

宿主启动参数依次是父窗口 HWND、父进程 PID、初始请求代数、`--popup`。初始文件路径通过 stdin 第一行发送，内容采用严格 UTF-8。后续协议如下，字段之间使用制表符：

| 方向 | 内容 | 含义 |
| --- | --- | --- |
| Pulse → 宿主 | `TOGGLE<TAB>generation<TAB>path` | 切换当前预览显示状态 |
| Pulse → 宿主 | `SWITCH<TAB>generation<TAB>path` | 当前预览跟随选择 |
| Pulse → 宿主 | `CLOSE<TAB>generation` | 关闭当前未固定预览 |
| 宿主 → Pulse | `READY` | 宿主初始化完成 |
| 宿主 → Pulse | `STATE<TAB>generation<TAB>active<TAB>count<TAB>busy` | 当前显示状态、总窗口数和插件忙碌状态 |
| 宿主 → Pulse | `ERROR<TAB>generation<TAB>code` | 不支持、文件不可用、插件或通信失败 |
| 宿主 → Pulse | `NAV<TAB>generation<TAB>direction` | 请求选择上一个或下一个文件 |

`READY` 只代表初始化完成。客户端以插件状态和错误响应处理后续请求，实际格式及渲染能力仍需用文件验证。启动等待上限为 30 秒，已启动宿主的命令处理上限为 15 秒。

## 5. 安全审计意见

- 父 HWND 必须属于指定 PID，宿主还检查 Windows 会话。父窗口关闭、父进程结束、stdin 关闭或客户端取消时，会清理预览。
- 路径只作为 UTF-8 数据传入，不拼入 Shell 命令。客户端和宿主拒绝相对路径、控制字符、虚拟 Shell 路径、设备路径和 NTFS 备用数据流。常规 UNC 和 `\\?\UNC\` 路径可用于预览。
- `CreateProcessW` 使用固定绝对 EXE 路径和句柄继承白名单。管道 ACL 只允许当前用户与 SYSTEM，拒绝远程客户端，并验证继承端点由当前 Pulse 进程创建；工作线程使用可取消的重叠 I/O，并限制协议行长度。
- 每个 Pulse 主窗口拥有独立宿主。Job Object 在客户端结束时清理宿主；用户通过“打开”动作启动的应用可以继续运行。
- 插件从已校验的程序目录加载。Pulse 专用配置保存在 `%LOCALAPPDATA%\PulseCustom\QuickLook`；目录拒绝重解析点，XML 使用互斥锁和原子替换，降低多窗口并发写入损坏风险。
- 预览宿主以当前用户权限运行，进程隔离用于控制崩溃影响。原版某些插件可能加载 Shell 预览提供程序或浏览器内容，预览来自不可信来源的文件仍需遵循系统安全提示。
- 分发 `quicklook/licenses/LICENSE-GPL.txt`、`quicklook/sources/QuickLook-4.5.0-upstream-source.zip` 和 `quicklook/sources/QuickLook-4.5.0-Pulse-host-source.zip`。后者包含适配后的完整源码、构建参数和本集成说明。其他依赖许可证及资源随原版发行包保留。QuickLook 应用及插件遵守 GPL-3.0-or-later，需保留版权和对应源码。

## 6. 验证方法

先检查 `runtime-manifest.json` 中的插件目录、原版插件文件 SHA256、宿主和源码包，再运行定向验证：

1. 选择本地图片、文本、视频、音频、PDF、Markdown、字体及压缩包，按空格确认真实内容显示。没有匹配插件的格式应使用 Pulse 原生预览。
2. 快速切换选择后，只显示当前文件；插件失败和无效路径应触发原生预览，不显示额外安装流程。
3. 在两个 Pulse 窗口分别打开预览，关闭其中一个主窗口，确认另外一个窗口的预览继续工作。
4. 固定预览后继续打开新文件；关闭当前预览保留固定窗口，关闭全部预览后宿主退出。
5. 验证空格、Escape、F11、方向键、插件菜单和“打开”动作。确认通过“打开”启动的应用在预览结束后继续运行。
6. 用隔离目录验证 UNC、长路径、不可用文件、快速关闭、宿主异常退出与超时回退。不要使用日常文件或其他文件管理器的配置作为测试数据。

## 7. 预防建议

依赖升级应同时修改固定版本、源码提交和归档 SHA256，并重新核对完整插件清单与许可证。安装或升级时保持 `quicklook` 目录整体一致，避免混用不同版本的宿主、`QuickLook.Common.dll` 和插件。源码构建包可在 .NET SDK 环境下用源码目录中的项目直接重建：

```powershell
dotnet build QuickLook/QuickLook.csproj -c Debug -p:Platform=x64 -p:PreBuildEvent= -p:Optimize=true -p:DebugSymbols=false -p:DebugType=None
```

重新构建后把生成的 `QuickLook.exe` 和配置文件分别命名为 `Pulse.QuickLook.Host.exe`、`Pulse.QuickLook.Host.exe.config`，并与同版本的完整插件目录一起使用。
