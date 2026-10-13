# Pulse Custom 的 Listary 插件

此插件通过 Listary 官方 `Listary.FileAppPlugin` SDK，将 Listary 的搜索入口和 Pulse Custom 的目录导航连接起来。兼容 .NET Framework 4.8 宿主；本次目标版本为本机 Listary 7.0.0.9。

## 使用方式

- Pulse Custom 窗口获得焦点后，直接输入关键词，使用 Listary 的浮动搜索栏。此功能由 Listary 的“集成 → Pulse Custom → 直接输入搜索”控制。
- 使用已有的 Listary 全局快捷键打开搜索；官方默认入口为按两次 `Ctrl`。选择目录可回到 Pulse Custom；选择文件的“打开所在文件夹”动作可打开父目录并选中文件。
- Listary 的当前文件标签接口在当前 Pulse 标签导航；窗口接口在该窗口打开新标签；全局文件管理器接口优先在已运行的定制窗口中打开新标签。
- Listary 的“当前打开的文件夹”和文件对话框 Quick Switch 可以读取 Pulse 当前目录。此插件将 Pulse 作为目录来源，文件对话框的自动切换由 Listary 管理。
- “此电脑”、设置、回收站、Pulse 搜索结果等虚拟视图没有可分享的真实目录；插件返回无目录状态，仍允许用户选择实际目录后导航。

Listary 原生搜索栏、索引、排序、快捷键和功能授权均由已有 Listary 管理。Pulse 原有全局搜索仍可独立使用；二者的全局快捷键应避免冲突。

## 构建

在仓库根目录执行以下 PowerShell 命令：

```powershell
.\scripts\build_listary_plugin.ps1
```

脚本使用锁定的公开 SDK 依赖，生成 `out\listary-plugin\package` 和同级 `Listary.Pulse-1.0.0.zip`。插件包只有自有插件 DLL、清单、图标、中文说明和可选启动路径；SDK 接口程序集由 Listary 宿主提供。

需要在 Pulse 未运行时从 Listary 启动已部署的定制版，可在打包时明确指定其本机路径：

```powershell
.\scripts\build_listary_plugin.ps1 `
    -PulseExecutablePath 'E:\Apps\PulseCustom\pulse.exe'
```

该路径写入插件旁的 `PulseExecutable.path`；应指向使用 `PULSE_CUSTOM_EDITION=ON` 编译的版本。留空时，全局交接要求已有 Pulse Custom 窗口。启动使用 `ProcessStartInfo.UseShellExecute=false`，路径作为一个已转义参数传入。

## 宿主接线

原生桥接位于 `src\app\listary_integration.h/.cpp`。宿主应在 UI 线程完成以下三处接线：

1. 将 `listary_integration.cpp` 加入 Pulse 编译目标。
2. 窗口初始窗格创建完成后调用 `AttachListaryWindow(state)`。
3. `WndProc` 常规分发前调用 `ListaryWindowMessage`；`WM_DESTROY` 最早调用 `DetachListaryWindow(state)`，再销毁窗格、工作线程或窗口状态。

```cpp
#include "listary_integration.h"

// WndProc: 完成 GetAppState 后。
LRESULT listary_result = 0;
if (s && pulse::ListaryWindowMessage(*s, msg, wParam, lParam, listary_result))
    return listary_result;

// WM_CREATE: 初始窗格就绪后。
pulse::AttachListaryWindow(*s);

// WM_DESTROY: 窗口状态和工作线程释放前。
pulse::DetachListaryWindow(*s);
```

桥接只接受窗口类 `PulseCustomMainWindow`，使用独立的桥接属性和管道前缀。上游 `PulseMainWindow` 无法绑定本插件。原生消息为 `WM_APP + 87`。

## 加载和部署

官方 SDK 指定从 Listary 程序旁的 `FileAppPlugins` 目录加载插件，并在复制插件后重新启动 Listary。安装在 `C:\Program Files\Listary` 的宿主目录通常需要管理员写权限；应先确定普通用户可写的官方宿主/插件部署位置，再执行实际部署。SDK 文档没有确认一个通用的用户配置目录插件扫描入口。

官方 DevTools 可以直接从工作目录加载插件，适合先验证桥接：

```powershell
Start-Process `
    -FilePath 'C:\Program Files\Listary\Listary.FileAppPlugin.DevTools.exe' `
    -WorkingDirectory '.\out\listary-plugin\package'
```

DevTools 的可见窗口用于人工操作；普通后台辅助程序应使用隐藏窗口。实际部署前先备份相关插件目录，并先在独立测试实例验证搜索、导航、关闭和快捷键。

加载完成后，在 Listary“选项 → 集成”启用 Pulse Custom。全局结果的“打开所在文件夹”需要 Listary 的文件管理器选择/动作路由确实使用 Pulse Custom；安装插件本身不能保证用户现有的全局动作设置已经改为 Pulse。

## 安全和生命周期

- 管道使用仓库现有 `CurrentUserSecurityAttributes`，只授权当前用户；同时拒绝远程连接并检查客户端用户和登录会话。
- 客户端验证窗口类、桥接属性、Pulse 程序名、用户 SID、会话、进程创建时间，以及管道实际服务器 PID。管道名称包含进程/窗口身份，避免 PID 或 HWND 重用导致误交接。
- 只提供读取当前目录、目录导航和选择项目；输入必须是绝对驱动器路径或完整 UNC 路径。拒绝相对路径、设备命名空间、ADS、虚拟 URI、NUL、控制字符、通配符和无效 UTF-16；原生方也拒绝会触发 Pulse 启动 Shell 网络快捷方式的目录目标。
- 每个请求最长 1.8 秒；原生方最多接受 2 秒的剩余截止时间。读写、响应确认、等待 UI 都受同一截止时间约束。
- 原生工作线程只处理受限协议，UI 队列只携带不可解引用的请求编号。窗格导航和选择在 UI 线程完成，实际目录枚举仍由 Pulse 原有后台工作线程执行。
- 标签令牌随活动窗格、标签或目录变化；后台刷新列表不会使搜索失效。迟到的旧标签请求返回 `StaleTab`。文件重命名编辑或模态对话框期间，导航返回 `Busy`。
- 关闭窗口先取消管道，再等待工作线程结束；插件卸载会取消全部连接。没有无限等待的响应刷新，也不自动重试已发送的导航请求。
- 成功响应表示导航或选择请求已被 Pulse 接受；目录枚举完成、路径权限和最终文件选中仍需用户路径验证。

## 协议

协议使用小端整数和不带 BOM/终止 NUL 的 UTF-16LE。每个连接只处理一个请求，最大路径长度为 32767 个 UTF-16 单元。

请求头长 48 字节：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| magic | uint32 | `0x50554c50` |
| version | uint32 | `1` |
| operation | uint32 | 下表操作码 |
| chars | uint32 | 路径 UTF-16 单元数 |
| window | uint64 | Pulse 窗口 HWND |
| created | uint64 | Pulse 进程创建 FILETIME |
| tab | uint64 | 当前标签令牌；窗口/全局新标签操作为 0 |
| deadline | uint64 | `GetTickCount64()` 绝对截止时间 |

响应头长 24 字节：`magic:uint32`、`version:uint32`、`status:uint32`、`chars:uint32`、`tab:uint64`，随后是目录字符串。客户端完整读取响应后发送一个值为 1 的确认字节；确认同样受请求截止时间约束。

| 操作码 | 操作 |
| --- | --- |
| 1 | 获取当前标签和真实目录 |
| 2 | 获取指定当前标签的真实目录 |
| 3 | 在指定当前标签打开目录 |
| 4 | 在指定当前标签打开父目录并选中项目 |
| 5 | 在窗口内新标签打开目录 |
| 6 | 在窗口内新标签打开父目录并选中项目 |

状态码：`Ok=0`、`NoFolder=1`、`StaleTab=2`、`Invalid=3`、`Closing=4`、`Busy=5`。格式不合法的连接直接关闭。

## 验证方法

使用独立目录准备一个普通文件、一个中文名称文件和两级文件夹，测试期间避免修改用户已有目录或偏好。

1. **识别和目录**：仅打开定制版，DevTools 应出现 Pulse Custom 文件窗口；当前真实目录应正确。打开原版 Pulse 时，本插件不得绑定原版窗口。
2. **直接输入搜索**：启用 Pulse Custom 集成后，在文件列表中输入已知名称，确认 Listary 浮动栏显示和结果准确。检查 Pulse 的地址栏/重命名编辑状态不会被误当作列表搜索。
3. **当前标签交接**：在绑定的 Pulse 标签中搜索目录并确认，检查路径和历史；对文件执行“打开所在文件夹”，检查父目录及唯一选中行。
4. **新标签和全局交接**：从 Listary 全局入口选中结果，使用 Pulse Custom 文件管理器动作；确认新标签位于正确的定制窗口，文件可以选中和滚动可见。
5. **Quick Switch**：从 Pulse 打开的目录切到文件保存对话框，确认 Listary 能读到源目录并完成其原生快速切换。
6. **取消和边界**：请求发出后立即切换窗格/标签或关闭窗口，确认旧请求不改变新标签、插件返回失败或 `StaleTab`、Pulse 能正常退出。测试 UNC、长路径、虚拟视图、空目录和不可用路径。
7. **编辑安全**：重命名尚未提交或有模态对话框时选择 Listary 结果，确认导航拒绝且用户输入保留。

完整验证必须覆盖实际 Listary 搜索结果返回 Pulse 的路径。编译成功、DevTools 识别或取得目录只能证明对应环节。

定向协议测试使用自建隐藏窗口和当前用户范围的模拟管道，覆盖 SDK 对象、协议分片、标签作用域、危险路径、超时与取消：

```powershell
.\integrations\Listary.Pulse\tests\run_protocol_tests.ps1
```

在 CI 构建的定制版独立测试窗口上验证原生桥接，可指定该测试窗口的十进制 HWND 和专用目录，目录内预先准备 `listary-selected.txt`：

```powershell
.\integrations\Listary.Pulse\tests\run_protocol_tests.ps1 `
    -NativeWindow 123456 `
    -Fixture 'E:\codex-demo\bench_data\listary-bridge'
```

此命令会改变指定测试窗口的目录并创建新标签；应只指定独立测试实例。该探针确认原生协议接受、当前目录和旧标签失效；最终行选中、滚动可见以及 Listary 的完整交接仍需界面验证。

## 公开依据

- [Listary 官方插件 SDK](https://github.com/listary/Listary.FileAppPlugin)，核对版本 `977d26f4a7095905d0ab136256cd02f329ee0ca6`；NuGet 包版本 `1.0.0`。
- [SDK 开发与部署说明](https://github.com/listary/Listary.FileAppPlugin/blob/977d26f4a7095905d0ab136256cd02f329ee0ca6/docs/Getting%20Started.md)。
- [Listary 官方文件搜索说明](https://help.listary.com/search-file)。
- [Listary 官方集成说明](https://help.listary.com/options-integration)。

公开 SDK 未提供索引查询、返回全局搜索结果列表或主动显示搜索栏的 API。关于 `--search`，可找到的官方论坛内容为旧版本开发计划，不能证明本机 7.0.0.9 已提供稳定接口。因此此插件采用正式 Floating 接口和用户已有的 Listary 搜索入口。
