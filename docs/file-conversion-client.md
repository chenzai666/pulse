# 文件转换原生客户端

`FileConversionIntegration` 为每个 Pulse 窗口维护一个转换会话。界面线程只提交不可变请求和领取事件；宿主校验与启动、管道读写、JSON 解析、取消和进程清理全部在后台执行。

## 界面接线

```cpp
// 窗口初始化；不会立即启动宿主。
conversion.Initialize(hwnd, WM_APP + 90);

// 菜单打开时立即显示加载状态，提交文件选择的副本。
const uint64_t capabilities = conversion.Capabilities(selected_files);

// 用户选定格式与输出目录后提交；返回 0 表示未接受请求。
const uint64_t job = conversion.Start(selected_files, L"png", output_directory, 90);

// WM_APP + 90：lParam 是数字令牌，不能当作指针释放或解引用。
pulse::app::FileConversionEvent event;
if (conversion.TakeEvent(static_cast<UINT_PTR>(lParam), event)) {
    // Capabilities: targets 的 id / label / extension 可生成菜单。
    // Started / Progress: 使用 event.generation、progress 和逐文件状态。
    // Completed: 展示 files 的 outputs、error_key、error 及批次 state。
    // Failed / Stopped: 显示错误或取消状态。
}

conversion.Cancel();   // 取消当前任务，并终止全部转换引擎。
conversion.Close();    // 异步关闭此窗口的宿主，可随后重新请求能力。
conversion.Shutdown();// 窗口退休时先撤销令牌，再异步清理进程。
```

能力查询和转换任务使用独立的代号。重新查询菜单会过滤旧查询结果，正在执行的任务仍保留自己的输入、目标和进度。切换标签、改变选择不会改变已提交的转换输入。

`busy()` 表示存在待启动或正在运行的转换任务；`running()` 表示后台已启动宿主。首次查询无需等待宿主或转换引擎初始化，界面应在收到 `Capabilities` 前保留加载状态。

## 宿主与协议

固定宿主为程序旁的 `conversion/Pulse.Filee.Host.exe`，通过原生 `CreateProcessW` 启动，参数只有 `--owner-pid` 和当前 Pulse 进程编号。客户端不搜索系统 `PATH`，不接受替代宿主 URL 或命令，不拼接 Shell 命令。

标准输入和输出使用 UTF-8 JSON Lines。标准错误单独连接到 `NUL`，不会污染 JSON 响应。每个方向的管道限制为当前用户、单实例、拒绝远程客户端，并校验端点建立者的进程编号。子进程只继承标准输入、输出和错误的白名单句柄。

支持的请求为 `capabilities`、`convert`、`cancel`、`shutdown`。请求 `id` 是数字代号的十进制字符串，取消请求另有独立代号，并以 `jobId` 指向原转换请求。

能力响应读取 `targets[].id/label/extension`。`inputs` 中的格式元数据和引擎路线对象不替代原生请求快照；路线的 JSON 和 Unicode 仍需通过校验。`ready`、`cancelled`、`stopping` 控制消息通过格式校验后忽略。

`started.files` 读取为文件数量；`progress` 同时保留总进度与逐文件进度；`completed` 保留批次状态和逐文件输出。成功结果中的 `errorKey/error` 可为 `null`。宿主 `error.code/detail` 对应事件的 `error_key/error_text`。

请求最多 1 Mi 个 UTF-16 字符，与宿主的读取限制一致；单个响应最多 4 MiB，单次最多 128 个输入。嵌套深度沿用现有语法校验器的 64 层限制。重复的关键字段、非法 UTF-8/UTF-16、越界进度、非规范请求编号、相对路径、设备路径、附加数据流及 URL 路径会被拒绝。

## 取消与清理

原生 Job 使用 `KILL_ON_JOB_CLOSE`，不允许子进程脱离 Job。转换引擎及其后代均受该会话管理。

取消先尝试发送 `cancel`，最多在后台等待管道写入 250 毫秒；成功写入后再给宿主最多 500 毫秒排空终止响应和清理暂存文件，随后强制终止宿主及全部引擎。清理期间的旧事件不会发往界面。宿主不响应取消也不会留下转换进程。正在等待的管道 I/O 可取消，关闭窗口不会等待线程或管道结束。

关闭会话发送 `shutdown` 后进行同样的进程树清理。窗口退休会立即清除事件令牌和所有者标记；即使窗口句柄被复用，也不能领取旧会话事件。未显式关闭但窗口已销毁时，后台同样会停止该会话。

能力响应等待上限为 30 秒。能力查询超时只影响查询，不终止正在转换的任务。转换首个响应等待 30 秒；开始后连续 10 分钟没有任何转换事件则停止该宿主并报告超时。

## 定向验证

独立回归文件为 `src/bench/file_conversion_protocol_test.cpp`。它同时包含纯协议用例和隔离宿主夹具，不依赖真实用户文件或已安装的 Filee。

建议云构建目标：

```cmake
add_executable(pulse_file_conversion_protocol_test
    src/bench/file_conversion_protocol_test.cpp
    src/app/file_conversion_protocol.cpp
    src/app/file_conversion_integration.cpp)
target_link_libraries(pulse_file_conversion_protocol_test PRIVATE advapi32 user32)
```

回归覆盖转义和 Unicode、最终宿主消息结构、错误和大小边界、过期能力查询过滤、请求快照、连续任务，以及不配合取消的宿主、引擎和孙进程的实际终止。全部进程和文件均在独立临时目录中运行，测试窗口保持隐藏。

协议与原生客户端回归通过只能确认通信和任务生命周期。真实格式转换的内容、输出完整性、许可证与界面表现仍需由宿主及发布流程分别验证。
