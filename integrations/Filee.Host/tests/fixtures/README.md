# XLS 测试数据生成器来源

`XlsBuilder.cs` 取自 Filee `v1.10.2` 固定提交 `3daacc0ab3cf4052a3922b86d5dd5ceb5fcd8a54` 的 `tests/Filee.Engines.Tests/SpreadsheetBuilders.cs`，仅保留 XlsBuilder、XlsSheet 和 CompoundFile。它生成真实 BIFF8/OLE XLS，避免用改后缀的 XLSX 冒充 XLS 验证。Filee MIT 许可证保存在同目录。
