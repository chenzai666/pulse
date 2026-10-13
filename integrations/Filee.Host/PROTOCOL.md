# Pulse 格式转换协议 v1

## 启动与传输

```text
conversion/Pulse.Filee.Host.exe --owner-pid <Pulse PID>
```

stdin/stdout 为 UTF-8、无 BOM 的 JSON Lines，每条消息一行。stdout 仅发送协议，诊断写 stderr。请求行最多 1 Mi 字符。请求 `id` 必须是 1–64 个 ASCII 字母、数字、`_` 或 `-`。路径必须是普通绝对盘符路径或完整 UNC 路径，也允许普通长路径前缀。

成功启动：

```json
{"id":"","type":"ready","protocol":1,"version":"1.10.2"}
```

## 能力查询

```json
{"id":"c1","action":"capabilities","inputs":["D:\\a.md"]}
```

```json
{"id":"c1","type":"capabilities","inputs":[{"path":"D:\\a.md","format":"md"}],"targets":[{"id":"pdf","label":"PDF","extension":"pdf","routes":[{"source":"D:\\a.md","steps":[{"engine":"hwpx-writer","from":"md","to":"hwpx"},{"engine":"rhwp","from":"hwpx","to":"pdf"}]}]}],"engines":[{"id":"rhwp","available":true,"reason":null}],"limitations":["复杂文档版式可能无法完整保留。"]}
```

示例路线仅说明结构，实际输出可能有不同可用引擎。`targets` 只包含所有输入均有可用路线的目标。能力查询需要 1–128 个实际文件；转换执行期间返回 `BUSY`。

## 批量转换

```json
{"id":"j1","action":"convert","inputs":["D:\\a.md"],"target":"pdf","outputDirectory":"D:\\out","quality":90}
```

`quality` 可省略，默认 90，范围 1–100。一次宿主只执行一个批次。

```json
{"id":"j1","type":"started","files":1}
{"id":"j1","type":"progress","progress":0.6,"files":[{"source":"D:\\a.md","state":"running","progress":0.6}]}
{"id":"j1","type":"completed","state":"completed","files":[{"source":"D:\\a.md","state":"done","outputs":["D:\\out\\a.pdf"],"errorKey":null,"error":null}]}
```

批次状态：`completed`、`completedwitherrors`、`failed`、`cancelled`。单文件状态：执行中为 `pending`/`running`，终态为 `done`、`skipped`、`failed`、`cancelled`。进度范围 0–1，通常约每 150ms 至多一次；非常短的任务可以没有中间进度。

`outputs` 为已发布结果的绝对路径，可以是文件或解压目录。既有结果不会被覆盖，冲突时改用 `a (2).pdf` 等名称。`errorKey` 和 `error` 是可选字符串，可为 JSON null。每项已接受转换在宿主能继续运行、输出管道可写的情况下仅发送一次 `completed`；宿主被 UI 或操作系统终止时，客户端应使用退出/停止事件收尾。

## 取消与停止

```json
{"id":"x1","action":"cancel","jobId":"j1"}
{"id":"x1","type":"cancelled","jobId":"j1","cancelled":true}
```

`cancelled` 表示请求匹配当前尚未终结的批次。随后批次发送 `completed`；取消超过 3 秒仍不结束时，宿主以退出码 3 退出并终止引擎子树。输出发布阶段也可取消，此前已经发布的完整文件可能保留。

```json
{"id":"s1","action":"shutdown"}
{"id":"s1","type":"stopping"}
```

shutdown、stdin EOF 或父进程退出都会取消当前工作，并最多等待 3 秒。正常退出码为 0；初始化失败为 1；参数/平台异常可为 64。

## 错误

```json
{"id":"c1","type":"error","code":"UNSUPPORTED","detail":"The selected files have no available route to that target."}
```

请求错误使用 `INVALID_ID`、`INVALID_ACTION`、`INVALID_REQUEST`、`UNSUPPORTED`、`BUSY`；启动错误使用 `HOST_UNAVAILABLE`。初始化错误 `detail` 只返回异常类型。客户端对 `ready`、取消确认和停止确认按类型消费；不得将这些合法控制消息当作格式错误。
