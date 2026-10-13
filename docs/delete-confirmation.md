# 删除确认与永久删除

## 工具栏删除按钮

点击工具栏的删除按钮会显示 Pulse 的确认窗口，列出本次选中的项目，并提供 **“永久删除，不放入回收站”** 复选框。

- 默认不勾选：确认后使用原有回收站删除流程。
- 勾选后：提示文字切换为永久删除确认，确认按钮变为红色的“永久删除”，并注明无法通过回收站还原。
- 勾选只对本次确认生效；下次普通删除重新从未勾选状态开始。
- 按取消、Esc 或关闭确认窗口均不会提交删除任务。
- 可使用 Tab 移动到复选框，使用空格切换，再移动到相应按钮操作。确认窗口默认聚焦取消按钮。

永久删除不可通过回收站恢复。处理重要文件前应保留可恢复的备份。

## 键盘与右键菜单

普通 Delete 键和原右键删除流程继续遵循设置中的“删除前确认”开关。关闭该开关时，普通删除直接使用回收站流程；开启后，确认窗口同样提供本次操作的永久删除选项。

Shift+Delete 始终显示明确的永久删除确认，不受普通删除确认开关影响。回收站中的删除也继续使用独立的永久删除确认。

确认窗口展示的路径是本次选择的快照。窗口关闭或销毁后，不会继续提交该确认窗口中的删除任务。

回收站是否可用仍由现有文件操作层和 Windows 决定；本改动不改变网络位置等目标的回收站能力。

## 定向验证

在 GitHub Actions 的 Windows 构建中，仅需构建受影响的应用及已有定向测试目标：

```powershell
cmake --build build --target pulse pulse_dialogs_test pulse_ops_test pulse_dialog_gallery
.\build\pulse_dialogs_test.exe --delete-confirmation
.\build\pulse_ops_test.exe --delete-confirmation
.\build\pulse_dialog_gallery.exe --delete-confirmation build-delete-confirmation-verify
.\build\pulse_dialog_gallery.exe --lang en-US --delete-confirmation build-delete-confirmation-en
```

对话框测试覆盖默认选择、勾选后的危险提示、取消、键盘焦点、100%/150%/200% DPI 布局，以及工具栏、普通删除设置和 Shift+Delete 的分支。

画廊命令只渲染本次新增的勾选/未勾选状态，在浅色、深色和高对比主题下生成 PNG，可用于检查中文、英文文案及复选框、警告和按钮的间距。

文件操作测试只使用构建目录相邻 `bench_data` 中新建的唯一测试目录，验证回收站删除及撤销、永久删除和取消。每次使用独立的操作日志，不清空整个回收站、不处理用户文件。
