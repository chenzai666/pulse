# 第三方组件说明

本适配器按 NuGet 锁文件分发以下开源运行时组件，不包含 Listary 宿主、搜索引擎或其他闭源程序。

| 组件 | 版本 | 授权与原文 |
| --- | --- | --- |
| Listary.FileAppPlugin | 1.0.0 | MIT，`licenses/Listary.FileAppPlugin-MIT.txt` |
| Microsoft.Extensions.Logging.Abstractions | 7.0.0 | MIT，`licenses/Microsoft-MIT.txt` |
| System.Buffers | 4.5.1 | MIT，`licenses/Microsoft-MIT.txt` |
| System.Memory | 4.5.5 | MIT，`licenses/Microsoft-MIT.txt` |
| System.Numerics.Vectors | 4.5.0 | MIT，`licenses/Microsoft-MIT.txt` |
| System.Runtime.CompilerServices.Unsafe | 4.5.3 | MIT，`licenses/Microsoft-MIT.txt` |

上述 `System.*` NuGet 包另声明：© Microsoft Corporation. All rights reserved.

[公开 Listary SDK 源码](https://github.com/listary/Listary.FileAppPlugin)中的 `Listary.FileAppPlugin/IFileAppPlugin.cs` 定义公开宿主接口。`SendCopyData` 的第五个参数是正文长度 `dataSize`，第六个参数才是超时。

许可证原文保留英语以准确履行第三方授权条件。编译引用程序集仅用于构建，不作为运行时内容分发。
