# Filee 生命周期定向验证

所有样本由测试创建在独立 GUID 临时目录，不读取用户文档。

## 捕获边界

| 操作 | 必须结束的进程 | 可以继续存活的进程 |
|---|---|---|
| 取消转换 | 本次请求后创建、由宿主直接启动的实际引擎根，以及这些根的所有结构后代 | 可复用宿主及其自身生命周期中的独立进程 |
| shutdown | 宿主自身和停止前捕获的完整子树 | 无 |
| owner 退出 | 宿主自身和 owner 退出前捕获的完整子树 | 无 |

捕获每个进程的原始内核句柄、创建时间、映像文件名及父 PID。父子关系使用创建时间验证；引擎根还绑定本次请求开始时间。后代按结构捕获，不按名称删除 `conhost.exe` 或其他活动进程。停止后保留 5 秒内所有被捕获原句柄必须退出的断言。

取消后再次通过同一个宿主 PID 执行真实 Markdown→HTML，验证宿主可复用。shutdown 和 owner 退出各自在实际 FFmpeg 转换期间执行，并检查包含宿主启动时子进程的完整树。

## 修正依据

CI `38382904137` 中，宿主 PID 7600 的直系 `conhost.exe` PID 5068 创建时间为 `134363616138634871`；本次 FFmpeg PID 2148 的创建时间为 `134363616228358456`，相差约 8.97 秒。FFmpeg 及其控制台子进程 1740 已退出，原断言被宿主长期直系进程 5068 阻塞。旧验证错误地把整个宿主树都要求在单项取消后退出。

这些记录支持将该进程归入宿主生命周期的验证边界；仅凭名称或 PPID 不推断其具体 Windows 控制台实现角色。测试 `HostSession.Start` 已使用 `CreateNoWindow=true`；原生客户端使用 `CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT`，并在恢复主线程前分配 Job。本次不修改这些标志或宿主运行时。

## 运行

本机最小安全与进程基础设施验证：

```powershell
dotnet run --project integrations/Filee.Host/tests/Pulse.Filee.Host.Tests.csproj -c Release
```

其中进程验证启动两个同名独立进程，确认捕获只包含请求之后的引擎树，取消后请求之前的长期进程仍存活；随后确认其完整树退出，并验证 Windows Job 成员清理。

云端真实转换与生命周期验证：

```powershell
dotnet run --project integrations/Filee.Host/tests/Pulse.Filee.Host.Tests.csproj -c Release -- --runtime build-custom/conversion/Pulse.Filee.Host.exe
```

本机基础设施检查不能替代真实 FFmpeg 转换、宿主复用、shutdown 和 owner 退出的云端结果。
