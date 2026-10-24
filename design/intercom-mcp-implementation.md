# 对讲 MCP 当前实现说明

本文记录当前代码行为，便于固件、小智服务端和对讲服务联调。字段定义参见 [对讲协议](intercom-protocol.md)，房间规则参见 [第二版设计](intercom-v2.md)。

## 1. 职责与两条连接

设备同时维护两条独立链路：

- 小智协议通道：语音识别、模型回复、TTS，以及 MCP 工具发现和调用。
- 对讲 WebSocket：设备 hello 注册、房间管理控制帧和实时 Opus 收发。

小智服务端负责理解“创建房间”“有哪些对讲群”等语音意图，选择调用设备提供的工具。固件不在本地匹配语音文本，也不把对讲音频发到小智通道。

```mermaid
sequenceDiagram
    participant U as 用户
    participant X as 小智服务端
    participant M as 固件 MCP
    participant I as 固件 Intercom
    participant S as 对讲服务端
    U->>X: 语音指令
    X->>M: 小智 type=mcp / tools/call
    M->>I: 调用工具对应方法
    I->>S: 对讲 WebSocket 控制帧
    S-->>I: ack / room_state
    M-->>X: MCP 工具结果
    X-->>U: TTS 回复
```

注意：除列表查询外，当前修改房间的工具在请求发送后即返回，MCP 结果可能早于对讲服务端确认。

## 2. 工具注册与可见性

`Application` 启动时依次调用 `McpServer::AddCommonTools()` 和 `AddUserOnlyTools()`。六个对讲工具目前写在 `AddUserOnlyTools()` 函数中，但实际使用的是普通 `AddTool()`，因此默认 `tools/list` 会返回它们。

函数名不等于权限类别。真正使用 `AddUserOnlyTool()` 的管理工具带 `user_only` 标记，默认工具列表会过滤，只有 `withUserTools:true` 才返回。对讲工具不能改回这一类别，否则语音模型可能看不到。

工具发现和调用均走 MCP JSON-RPC：

```json
{"jsonrpc":"2.0","id":10,"method":"tools/list","params":{}}
```

```json
{"jsonrpc":"2.0","id":11,"method":"tools/call","params":{"name":"self.intercom.list_rooms","arguments":{}}}
```

这些报文嵌入小智协议的 `type:"mcp"`、`payload` 对象，由 `Application` 转给 `McpServer::ParseMessage()`。工具列表超过单页容量时返回 `nextCursor`，小智服务端需要继续获取后续页。

## 3. 已提供的工具

| 工具 | 输入参数 | 实现方法 | 当前返回语义 |
|---|---|---|---|
| `self.intercom.get_current_room` | 无 | `RoomName()` | 本地缓存的 `room_name` 对象，无实时服务端查询 |
| `self.intercom.list_rooms` | 无 | `ListRooms()` | 实际服务端响应 JSON 字符串，或包含 `ok:false,error` 的 JSON 字符串 |
| `self.intercom.create_room` | `name` 必填字符串；`invite_device_ids` 可选字符串，默认空 | `CreateRoom()` | bool，仅表示请求发送是否成功 |
| `self.intercom.join_room` | `code` 必填字符串 | `JoinRoom()` | bool，仅表示请求发送是否成功 |
| `self.intercom.leave_room` | 无 | `LeaveRoom()` | bool，仅表示请求发送是否成功 |
| `self.intercom.accept_room_invite` | `code` 必填字符串 | `AcceptInvite()` | bool，仅表示请求发送是否成功 |

工具结果通过现有 `McpTool::Call()` / `ReplyResult()` 返回。列表 JSON 是文本内容中的字符串，模型需要读取其中的 `ok`，不能把收到 MCP 成功响应等同于业务成功。

### 当前房间查询

返回示例：

```json
{"room_name":"调度组"}
```

`room_name_` 初始为“默认房间”，收到对讲控制消息中的 `room_name` 后更新。该查询仅反映缓存，不证明当前对讲连接在线；目前也没有返回 `room_id` 或连接状态。

### 房间列表查询

1. `ListRooms()` 检查对讲状态，断线立即返回错误。
2. 生成随机 `request_id`；同一时间只允许一个待完成列表查询。
3. 通过 `QueueText()` 把 `list_rooms` 放进控制队列。
4. 对讲网络任务的 `FlushControl()` 发出请求。
5. 网络回调收到 `list_rooms_ack` 后比较 `request_id`，保存原始响应并唤醒条件变量。
6. 最多等待 5 秒，超时清除请求标识，迟到响应不会匹配后续查询。

失败原因包括：`intercom disconnected`、`room query busy`、`room query timed out`。对讲服务端限制完整响应不超过 8192 字节，固件文本接收也限制 8192 字节。

服务端只返回未关闭的默认房间、开放房间、本机创建的房间，以及邀请列表包含本机的房间。每项包含 `room_id`、`room_name`、`is_default`、`is_current`、`can_join`、`code`；默认房间加入码为空。

### 创建房间

MCP 参数示例：

```json
{"name":"临时调度组","invite_device_ids":"003,004"}
```

MCP 当前仅支持字符串属性，邀请 ID 用英文逗号切分后转换成 WebSocket 数组。ID 前导零要保留；当前解析不会自动去除各 ID 周围空格，调用端应传 `003,004`，避免 `003, 004`。

```json
{"type":"create_room","name":"临时调度组","creator_device_id":"002","invite_device_ids":["003","004"]}
```

服务端使用已注册 WebSocket 身份作为创建者。创建者自动进入新房间；受邀设备不会自动加入。邀请列表为空表示开放加入。当前工具只检查连接和非空名称，不等待 `create_room_ack`，因此小智应说“已发送创建请求”，不能仅凭 true 断言创建成功。

### 加入、退出和接受邀请

```json
{"type":"join_room","code":"012345"}
```

```json
{"type":"leave_room"}
```

```json
{"type":"accept_room_invite","code":"012345"}
```

`code` 使用字符串保留前导零。退出回默认房间。服务端校验房间有效性、邀请权限等条件；固件当前不等待这些操作的 ack。

按名称切房没有独立工具：可以先查询房间列表，唯一匹配后使用返回的 code 加入；同名时询问用户。默认房间用 `leave_room` 返回。

## 4. 执行线程和资源限制

- 小智网络回调：解析消息并投递工作，不能在这里等待房间查询响应。
- 普通 MCP 工具：经 `Application::Schedule()` 在应用任务执行。
- `self.intercom.list_rooms`：`DoToolCall()` 特殊分派到 `intercom_rooms` FreeRTOS 工作任务（8192 字节栈、优先级 2），执行带超时的条件变量等待；结束后删除任务。
- 对讲网络任务：发送排队控制消息，持续转发音频；不因小智会话结束而停用。
- 对讲接收回调：匹配列表请求、唤醒工作任务，并继续处理音频和房间事件。

列表查询互斥锁保护请求 ID 和响应；条件变量等待会释放锁。查询进行中再次调用会返回 busy，不启动第二个实际网络查询。

## 5. MCP 后结束小智对话

当前 `Application` 在收到小智 MCP `tools/call` 时记录工具名和调用标记。例外：

```text
self.get_device_status
```

这是服务端自动状态查询，不设置自动结束标记；如果此前已执行其它工具，状态查询也不会取消已有标记。`initialize`、`tools/list` 和 MCP 响应均不算工具调用。

结束条件：

1. 记录到非例外的 `tools/call`。
2. 收到小智 TTS `stop`。
3. `AudioService::IsPlaybackIdle()` 确认解码/播放队列及在途音频已完成。
4. 当前仍为 Speaking 且没有被用户打断。

`FinishAssistantReply()` 关闭小智 `Protocol::CloseAudioChannel()`，通过 `SetDeviceState()` 回到 Idle。WebSocket 模式关闭小智 WebSocket；MQTT 模式关闭音频会话并发送 goodbye，MQTT 管理连接仍保留。下一次唤醒重新打开小智音频通道，对讲 WebSocket 保持独立在线。

普通欢迎语和没有工具调用的问答继续聆听。本机制依据收到的 tools/call 标记，当前不等待工具业务成功，也未建立严格的“工具执行完成—最终总结 TTS”关联；服务端应按工具结果再回复，避免中间播报的 TTS stop 提前触发结束。

## 6. 调试日志

```text
MCP: Add tool: self.intercom.list_rooms
Application: Assistant MCP incoming: method=tools/call id=11
Application: Assistant MCP tools/call: self.intercom.list_rooms
Application: Assistant TTS stop: mcp_called=1 tool=self.intercom.list_rooms
Application: Assistant playback check: ...
Application: Assistant reply playback finished; closing assistant audio channel
WS: CloseAudioChannel requested: ...
```

自动状态查询应出现：

```text
Assistant automatic status query; keep conversation open
```

对讲服务端查询日志：

```text
debug list rooms device=002 request=... count=...
```

未触发工具时先检查默认 tools/list 的工具可见性和分页，再检查小智服务端的工具发现/模型调用记录。固件注册日志只证明本地存在工具，不能证明服务端模型已加载全部工具。

## 7. 当前限制与验证

- 创建、加入、退出、接受邀请尚未等待服务端业务确认。
- 当前房间查询读取缓存；房间名称的并发访问仍需进一步统一同步。
- 加入码和邀请 ID 的严格校验、创建名称长度限制仍需完善。
- 房间邀请的本机提示音和待处理邀请缓存尚未实现；接受工具需显式提供 code。
- 未提供本地语音意图识别；工具选择取决于小智服务端。

已进行服务端 Go 测试和 3.5B 固件编译。实机应验证“列出房间与加入码”、超时/断线、受限房间过滤、同名房间询问，以及工具回复结束后小智通道关闭但对讲仍在线。
