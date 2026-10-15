# Pulse 定制版独立安装包

安装包只封装 `scripts/package_custom.ps1` 已生成的完整目录，不重新编译应用，也不复用原版安装器或卸载器。

## 安装身份与范围

| 项目 | 定制版行为 |
| --- | --- |
| 显示名称 | Pulse 定制版 |
| Inno Setup AppId | `{BF0350A3-5F01-44CD-AC64-C9AF030CC430}`，与原版独立 |
| 安装目录 | 当前用户 `%LOCALAPPDATA%\Programs\PulseCustom`，固定目录，拒绝 `/DIR` 改到其他位置 |
| 配置目录 | 定制版程序使用 `%LOCALAPPDATA%\PulseCustom`，安装器不迁移或删除配置 |
| 快捷方式 | 当前用户开始菜单中的“Pulse 定制版”，桌面图标可选 |
| 卸载项 | 当前用户的“Pulse 定制版”，使用自己的 `.installer` 卸载器 |
| 权限 | `PrivilegesRequired=lowest`，不自动提权 |
| 忙碌升级/卸载 | 检测本安装目录内的运行进程，要求用户自行退出，不强杀、不自动重启 |

默认使用普通用户启动安装器。安装器不注册、启动、停止或删除 `PulseIndex` 服务，不修改原版安装目录、原版配置、全局接管、Listary 部署和文件关联。没有自动启动应用的步骤。

目标目录存在重解析点或已有其他内容但缺少本安装器身份文件时，拒绝覆盖。已有便携完整包不能直接当作安装器管理的旧版本覆盖，应先自行备份并移走。升级只更新清单内的文件，历史版本遗留的旧组件不做递归清理。

卸载只使用本安装器记录的文件与快捷方式，不递归清空目录，保留 `%LOCALAPPDATA%\PulseCustom`。安装后自行添加的文件通常保留；自行覆盖安装器记录的同名文件仍可能在卸载时被移除，请另行备份。

## 云端打包

需要 Windows x64 runner、PowerShell 7.2+ 和 Inno Setup 6.3+。工具应在 CI 中安装，推荐固定可核验版本；这一步安装编译工具与最终安装包的权限行为无关。

```powershell
pwsh -File scripts/package_custom_installer.ps1 `
    -StageDir dist/Pulse-Custom-04b6084xx `
    -OutputDir dist/installer `
    -IsccPath 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe'
```

将示例目录替换为实际完整包目录。先验证而不生成安装器：

```powershell
pwsh -File scripts/package_custom_installer.ps1 -StageDir $stage -VerifyOnly
```

脚本验证清单版本、`PulseCustom` edition、完整提交号、每个文件长度和 SHA256，拒绝重复路径、目录穿越、重解析点、缺失文件、清单外文件以及打包器保留路径。还检查主要程序、完整组件目录、许可证、源码包以及主程序的定制版身份，拒绝包含发布自检调度器的程序。

打包仅列入验证过的文件，不使用安装器递归通配。当前按用户要求固定版本号 `v1.0.53`，拒绝其他版本载荷，输出 `Pulse-Custom-Setup-v1.0.53-x64.exe` 和同名 `.exe.sha256`；提交号仍保存在安装界面和完整清单中用于追溯。已存在的产物不覆盖。上传 GitHub Release 前还需确认 CI 成功并独立核对下载后的 SHA256。

## 定向验证

本次已完成 PowerShell 语法解析，以及合成完整包的校验正例、文件篡改、清单外文件、原版 edition 和路径穿越拒绝检查。合成包不是实际运行时；尚未完成 Inno 编译和安装运行验证。不要把这些检查当作安装功能通过。

在一次性的 Windows runner、Windows Sandbox 或测试虚拟机中验证，禁止用生产机器的原版配置做破坏测试。应使用普通测试用户，安装程序与卸载程序都等待退出并检查退出码、日志。

1. 在不执行安装器的情况下运行 `-VerifyOnly`。对完整包的副本分别修改一个文件、删除一个文件、添加清单外文件、将 edition 改为原版；每例都应明确拒绝。
2. 记录原版目录文件摘要、原版 `%LOCALAPPDATA%\Pulse`、`HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall` 原有条目以及 `HKLM\SYSTEM\CurrentControlSet\Services\PulseIndex` 是否存在及其值。后续只读取比较，禁止导入、删除或“恢复”整棵注册表。
3. 运行以下静默安装并检查只有定制版卸载项和快捷方式新增；逐文件核对安装目录中的 `package-manifest.json`。此阶段不启动应用，避免将应用自身的接管或索引行为混入安装器测试。

```powershell
$process = Start-Process -FilePath $installer -WindowStyle Hidden -Wait -PassThru `
    -ArgumentList @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/LOG=install.log')
if ($process.ExitCode -ne 0) { throw "安装失败：$($process.ExitCode)" }
```

4. 再次安装同包应作为升级通过。单独打开本安装目录中的应用后再运行安装器，应明确拒绝并且应用仍在运行；关闭应用后重试。若启动应用，需在独立测试环境中额外记录和比较其配置、索引和接管变化。
5. 在 `%LOCALAPPDATA%\PulseCustom` 中创建测试标记，退出应用后运行 `%LOCALAPPDATA%\Programs\PulseCustom\.installer\unins000.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LOG=uninstall.log`，确认定制版卸载项移除、配置标记保留，原版目录、配置、服务与原版卸载项的快照均无变化。
6. 重解析点、修改 `/DIR`、目标目录已有其他内容以及忙碌卸载分别验证拒绝；这些拒绝用例仅在一次性测试环境中创建，保留日志，不删除原版数据。

## 签名、索引和共享行为限制

当前没有代码签名步骤，安装包及应用可能触发 SmartScreen 或安全软件提示。SHA256 验证完整性，不提供发布者身份认证；不要通过关闭系统保护解决提示。

本次没有更新索引架构。现有应用的索引客户端、服务名称、协议及相关共享状态尚未获得完整定制版隔离保证。安装器不配置索引服务，不代表运行应用后一定不访问或影响现有索引环境。需要管理员权限的索引维护也不由本安装器代办。

原版与定制版运行时还可能竞争用户主动开启的文件夹关联、Win+E、回收站或其他系统接管，Listary 插件也有独立部署流程。卸载器不重置这些外部状态；已有接管应在卸载前通过应用设置按实际归属恢复。不能声称卸载后完全没有共享影响。
