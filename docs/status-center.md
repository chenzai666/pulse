# 状态中心与属性入口

右上角的状态中心显示当前 Pulse 进程中所有窗口的复制、移动、删除及格式转换任务。进行中的任务置于记录前方，保留进度与适用的取消入口；已完成、已取消、部分失败和失败的任务形成最近记录，最多保留 64 条。记录只保存在内存，退出进程后清除。

状态中心是使用现有 Fluent 组件和 DirectComposition 的原生弹出窗口，默认靠近右上角并限制在当前显示器工作区内。滚轮、方向键和 Page Up/Page Down 浏览记录，Tab 切换操作，Enter/空格执行，Escape 或点击外部关闭。关闭状态中心不会取消任务。

## 更新与取消

- `AppCore::statusCenter` 是所有窗口共享的模型；通知到来后更新弹窗和标题栏，不添加空闲轮询、定时器或任务引擎。
- `WM_OPS_NOTIFY` 调用 `RecordOperationStatus`，并把已有 `DrainFinishedTasks()` 的每个结果传给 `RecordFinishedOperation`，保证快速连续完成的任务不会因最新状态快照被覆盖而丢失。
- 格式转换提交成功后调用 `RecordFileConversionQueued`；领取宿主事件后，在清除界面的任务代号前调用 `RecordFileConversionStatus`。能力查询及其错误不会创建任务记录。
- 转换任务用原窗口句柄和不可变任务代号共同识别。取消向原窗口发送 `WM_FILE_CONVERSION_CANCEL`，`wParam` 必须为对应代号；接收方再次校验，旧记录不能取消后来新任务。
- 文件操作调用 `CancelCurrent(expected_task_id)`，在现有操作锁内校验任务编号。已终止任务与没有取消接口的清空回收站任务不会显示取消按钮。
- “查看详情”复用已有文件操作详情窗口。完成队列同时保留任务结果和终态错误，即使状态快照已被下一个任务覆盖，也能显示失败或取消原因。
- 窗口退休时调用 `RetireStatusCenterOwner`，清理其弹窗并将该窗口尚未终止的转换记录标为取消，其他窗口任务继续保留。

## 属性与窄窗口

工具栏编辑图标组后新增属性按钮。存在选择时走已有 `CmdProperties`；没有选择且当前视图是真实目录时走 `CmdFolderProperties`。无选择的虚拟视图禁用该入口，不把虚拟路径交给 Shell。

工具栏先给“更多”入口保留空间，再压缩长标签、将编辑/布局按钮放入更多菜单。属性和状态中心在更多菜单中始终可达。窗口不足 680 DIP，或侧栏右侧不足 600 DIP 时隐藏标题栏状态中心图标，通过更多菜单打开，避免挤压标签页或地址栏。绘制与 HitTest 使用相同几何。

## 定向验证

新增独立目标建议：

```cmake
add_executable(pulse_status_center_model_test
    src/bench/status_center_model_test.cpp
    src/app/status_center_model.cpp)
```

主程序另外编译 `src/app/app_status_center.cpp`、`src/app/status_center_model.cpp` 和 `src/ui/status_center_window.cpp`。模型回归覆盖任务去重、百分比单位、能力查询排除、多窗口转换隔离、部分失败、窗口退休、64 条历史上限及清理后的迟到通知；布局回归覆盖 480–1280 DIP、100%/150%/200% 缩放、不同侧栏宽度及过滤框展开。

状态中心的限定画廊使用真实窗口绘制代码和内存夹具：

```powershell
.\pulse_dialog_gallery.exe --status-center <输出目录>
```

画廊目标另需编译 `src/ui/status_center_window.cpp` 与 `src/app/status_center_model.cpp`，其余依赖复用现有目标。命令固定使用中文，生成空状态/任务列表、浅色/深色、100%/150%/200% 六种组合的 12 张 `status_center_*.png`，并写入 `status-center-gallery.txt`。成功预期为 `written=12 failed=0`；状态中心窗口和拥有者保持隐藏，不启动 Ops 或 Filee 引擎。

上述回归不启动真实文件操作、转换宿主或用户窗口。云端原生编译和回归完成后，仍需检查画廊截图，并对实际窄窗口、滚动与取消入口做定向交互验证；不能以静态检查替代运行结果。
