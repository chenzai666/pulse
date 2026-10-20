# Pulse 文件窗口的 Listary 适配

使用 Listary 官方 FileAppPlugin 接口，给 Pulse 接管的打开、保存和选择文件夹窗口显示固定搜索框，并作为 Quick Switch 的目标。

仅绑定带有 `Pulse.Picker.Listary.v1` 标记的 `PulseFolderPickerWindow`。目录跳转进入 Pulse 异步校验流程；保存窗口跳转目录时保留已填写的文件名。选择搜索结果仍经过原软件的文件类型、覆盖和保存校验。

安装到 Listary 支持的 `FileAppPlugins/PulseCustom.FileDialog` 目录并正常重启 Listary。关闭 Pulse 的第三方窗口接管后恢复 Windows 对话框，原有 Listary 适配随之恢复。

本适配不接管全局搜索，不修改 Listary 热键及搜索设置。
