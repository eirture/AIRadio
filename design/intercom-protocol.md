# 灵矽对讲服务协议

状态：当前服务端实现协议（第二版房间与浏览器模拟器）。

本文以 `server/api.go`、`server/main.go` 和内置 Web 页面为准，集中描述 HTTP 和 WebSocket 接口。服务默认使用明文 HTTP/WebSocket，仅适合可信内网测试。

## 1. 通用约定

- HTTP 基础地址由启动参数 `-listen` 决定，例如 `http://127.0.0.1:9200`。
- WebSocket 地址为 `ws://host:port/ws`；HTTPS 页面对应 `wss://`。
- 管理 HTTP 接口需要 Cookie `airadio_admin`。登录成功后服务端设置该 Cookie。
- JSON 使用 UTF-8。成功响应通常为 JSON；失败响应为 HTTP 状态码和纯文本错误正文。
- `device_id` 必须匹配 `[A-Za-z0-9_-]{1,32}`，且在线连接不能重复。
- 默认房间 ID 为 `default`，名称为“默认房间”，不能关闭。

## 2. HTTP 接口

### 2.1 `POST /api/login`

登录管理端。

请求 JSON：

| 字段 | 类型 | 必填 | 说明 |
|---|---|---:|---|
| `username` | string | 是 | 当前实现必须为 `admin` |
| `password` | string | 是 | 当前实现必须为 `admin` |

成功：`200`，并设置 `airadio_admin=admin-session` Cookie。

```json
{"ok":true}
```

失败：`401`，正文 `invalid credentials`。

### 2.2 `GET /api/rooms`

返回所有未关闭房间。需要登录。

响应：房间对象数组。

| 字段 | 类型 | 说明 |
|---|---|---|
| `id` / `ID` | string | 稳定房间 ID |
| `name` | string | 房间名称 |
| `join_code` / `JoinCode` | string | 普通房间六位数字加入码；默认房间为空 |
| `is_default` / `Default` | boolean | 是否默认房间 |
| `device_ids` / `Devices` | string[] | 当前房间设备 ID |

### 2.3 `POST /api/rooms`

由管理端创建房间。需要登录。

请求 JSON：

| 字段 | 类型 | 必填 | 说明 |
|---|---|---:|---|
| `name` | string | 是 | 去除首尾空格后不能为空 |
| `created_by_device_id` | string | 否 | 创建者标识，记录到房间 |
| `device_ids` | string[] | 否 | 要加入房间的设备；为空时加入当前已注册设备快照 |

成功：`200`，返回单个房间对象（字段同 `GET /api/rooms`）。普通房间会生成固定六位加入码。

失败：`400`，正文为 `name is required` 等错误。

### 2.4 `GET /api/rooms/{room_id}/devices`

返回房间设备列表。需要登录。

设备对象字段：

| 字段 | 类型 | 说明 |
|---|---|---|
| `device_id` | string | 设备 ID |
| `last_seen_at` | string | 最近注册时间，RFC3339 |
| `last_ip` | string | 最近 IP；当前 WebSocket 注册未填充真实 IP |
| `online` | boolean | 当前是否存在在线 WebSocket |
| `room_id` | string | 当前逻辑房间 ID |

### 2.5 `POST /api/rooms/{room_id}/close`

关闭普通房间。需要登录。关闭后该房间设备回到默认房间。

成功：

```json
{"ok":true}
```

默认房间或不存在房间：`400`。

### 2.6 `GET /api/devices`

返回所有曾注册设备。需要登录。响应为设备对象数组，字段同上。

### 2.7 静态页面

- `GET /` 或 `GET /index.html`：返回内置管理页面。
- 其它路径：`404`。

## 3. WebSocket 连接与限制

连接到 `/ws` 后，客户端第一条消息必须是 JSON `hello`。服务端限制：

- 控制帧最大 1024 字节。
- 单个二进制音频帧最大 4096 字节。
- 单段最多 500 帧、512 KiB、35 秒。
- 最多 128 个连接。
- 每个连接最多一段进行中的发送会话。
- 浏览器同源 `Origin` 和无 `Origin` 的设备客户端允许；其它跨站 Origin 拒绝。

### 3.1 `hello` 注册

客户端 -> 服务端：

```json
{"type":"hello","version":1,"device_id":"003"}
```

| 字段 | 类型 | 说明 |
|---|---|---|
| `type` | string | 固定 `hello` |
| `version` | integer | 当前必须为 `1` |
| `device_id` | string | 设备 ID，必须唯一且符合 ID 规则 |

成功返回：

```json
{"type":"hello_ack","ok":true,"room_id":"default","room_name":"默认房间"}
```

失败返回 `ok:false` 和 `error`，例如重复 ID、非法 ID、版本不支持或连接达到上限；连接随后关闭。

### 3.2 控制帧公共字段

| 字段 | 类型 | 说明 |
|---|---|---|
| `type` | string | 消息类型 |
| `version` | integer | hello 使用 |
| `device_id` | string | hello 使用 |
| `source_id` | string | 服务端填入的发言设备 ID，仅接收 start 使用 |
| `message_id` | string | 一段语音的会话 ID |
| `frame_ms` | integer | start 的帧时长，当前必须 60 |
| `sample_rate` | integer | start 的采样率，当前必须 16000 |
| `frame_count` | integer | end 的已发送帧数 |
| `ok` | boolean | ack 成功或失败 |
| `error` | string | 失败原因 |
| `receivers` | integer | end_ack 实际确认接收者数 |
| `room_id` | string | 房间 ID |
| `room_name` | string | 房间名称 |
| `code` | string | 房间六位加入码 |
| `name` | string | 创建房间名称 |
| `device_ids` | string[] | 创建房间时的成员列表 |

未列出的字段会被忽略或按消息类型处理；未知 `type` 会返回 error 并中止当前会话。

## 4. 房间控制消息

### 4.1 `create_room`

客户端 -> 服务端：

```json
{"type":"create_room","name":"房间002","device_ids":["003"]}
```

设备模拟器必须传自己的 ID；这样只有当前设备进入新房间。管理端 HTTP 创建时 `device_ids` 为空则表示加入全部当前已注册设备。

成功：

```json
{"type":"create_room_ack","ok":true,"room_id":"room-...","room_name":"房间002","code":"111696","device_ids":["003"]}
```

创建者自动进入新房间。服务端会向受影响的在线设备发送 `room_state`。

### 4.2 `join_room`

客户端 -> 服务端：

```json
{"type":"join_room","code":"111696"}
```

`code` 为六位加入码。正在发言时切换会返回 `ok:false,error:"busy"`。

成功：

```json
{"type":"join_room_ack","ok":true,"room_id":"room-...","room_name":"房间002"}
```

失败：`ok:false,error:"invalid room code"` 等。

### 4.3 `leave_room`

客户端 -> 服务端：

```json
{"type":"leave_room"}
```

成功返回：

```json
{"type":"room_state","room_id":"default","room_name":"默认房间"}
```

### 4.4 `room_state`

服务端主动通知设备当前房间变化。字段为 `room_id`、`room_name`；失败时可带 `ok:false,error`。

## 5. 音频对讲消息

### 5.1 `start`

发送端 -> 服务端：

```json
{"type":"start","message_id":"003-1","frame_ms":60,"sample_rate":16000}
```

服务端按发送设备当前房间路由，并锁定该段所属房间。成功后向同房间其它设备发送：

```json
{"type":"start","source_id":"003","message_id":"003-1","frame_ms":60,"sample_rate":16000}
```

发送端同时收到：

```json
{"type":"start_ack","message_id":"003-1","ok":true}
```

若全局已有其它发送者，返回 `ok:false,error:"busy"`。当前实现发言锁是全局锁；同一时间只允许一段语音。

### 5.2 `receive_ready` / `busy`

接收端收到 `start` 后回复：

```json
{"type":"receive_ready","message_id":"003-1"}
```

设备忙时回复：

```json
{"type":"busy","message_id":"003-1"}
```

服务端只在约 2 秒准备窗口内接受 `receive_ready`。未准备好的接收者不会收到音频。

### 5.3 二进制音频帧

发送端在 `start` 后发送 WebSocket Binary 消息。每条 Binary 消息承载一帧原始 Opus 数据；服务端不解析浏览器模拟器的 PCM 测试数据。服务端按同房间且已 `receive_ready` 的连接转发，发送者不会收到自己的帧。

### 5.4 `end`

发送端 -> 服务端：

```json
{"type":"end","message_id":"003-1","frame_count":35}
```

`frame_count` 必须等于服务端实际收到的帧数。成功返回：

```json
{"type":"end_ack","message_id":"003-1","ok":true,"receivers":1}
```

同时向接收端发送：

```json
{"type":"end","message_id":"003-1","frame_count":35}
```

`receivers` 只表示服务端成功排入接收连接，不保证扬声器播放完成。

### 5.5 `abort`

发送端可发送：

```json
{"type":"abort","message_id":"003-1"}
```

服务端清理发送状态并向已收到 start 的接收端发送：

```json
{"type":"abort","message_id":"003-1"}
```

### 5.6 `error`

服务端发现非法帧、超限、乱序或结束帧数不匹配时发送：

```json
{"type":"error","error":"invalid or oversized audio frame"}
```

当前会话随后被中止。

## 6. 典型流程

1. 建立 `/ws`，发送 `hello`，等待 `hello_ack`。
2. 发送 `join_room` 或 `create_room`，等待对应 ack；创建设备房间时传 `device_ids:[自身 ID]`。
3. 发言方发送 `start`；服务端向同房间设备发送 start，并向发言方返回 start_ack。
4. 接收方发送 `receive_ready`。
5. 发言方逐帧发送 Binary Opus。
6. 发言方发送 `end`；服务端向接收方发送 end，并返回 end_ack。
7. 连接断开时，旧会话失效，不支持续传或离线补播。

## 7. 房间邀请语义（更新）

创建房间请求中的 `device_ids` 表示邀请设备列表（推荐新字段名 `invite_device_ids`）。创建者通过 `creator_device_id` 或当前 WebSocket 设备身份确定，并自动进入房间。

- 邀请列表为空：房间开放加入，任意设备可使用加入码申请加入。
- 邀请列表非空：只有列表中的设备可以加入；在线受邀设备收到 `room_invite`，不会自动切换房间。
- 设备接受邀请后发送 `accept_room_invite`，服务端校验后切换活动房间。

```json
{"type":"room_invite","room_id":"room-abc","room_name":"房间002","code":"123456","source_id":"003"}
```

```json
{"type":"accept_room_invite","code":"123456"}
```

成功返回 `accept_room_invite_ack`，随后发送 `room_state`。`room_state` 只表示该设备当前活动房间发生切换，不表示房间属性更新。

## 8. WebSocket 房间列表查询

已注册设备发送（不需要管理端 Cookie）：

```json
{"type":"list_rooms","request_id":"rooms-1"}
```

| 字段 | 类型 | 规则 |
|---|---|---|
| type | string | 固定 list_rooms |
| request_id | string | 必填，1～64 字节；客户端生成，用于匹配响应 |

成功响应：

```json
{"type":"list_rooms_ack","request_id":"rooms-1","ok":true,"rooms":[{"room_id":"default","room_name":"默认房间","is_default":true,"is_current":true,"can_join":true,"code":""}]}
```

| 响应字段 | 类型 | 说明 |
|---|---|---|
| type | string | 固定 list_rooms_ack |
| request_id | string | 原样回传请求标识 |
| ok | boolean | 查询是否成功 |
| error | string | 失败原因；成功时省略 |
| rooms | array | 可加入的未关闭房间；失败时省略 |
| rooms[].room_id | string | 稳定房间 ID |
| rooms[].room_name | string | 房间名称 |
| rooms[].is_default | boolean | 是否默认房间 |
| rooms[].is_current | boolean | 本机当前是否处于此房间 |
| rooms[].can_join | boolean | 是否允许本机加入；当前返回的房间均为 true |
| rooms[].code | string | 六位加入码，保留前导零；默认房间为空字符串 |

服务端只返回默认房间、开放房间、本机创建或本机受邀房间，避免泄露其它受限房间加入码。响应最大 8192 字节，超出时返回 ok:false 和错误原因，不静默截断。请求控制帧仍限制 1024 字节。

固件工具 `self.intercom.list_rooms` 通过独立工作任务查询，最多等待 5 秒，返回实际响应 JSON 或明确错误；不阻塞应用主任务。可说“当前有哪些对讲房间，加入码是什么”。默认房间通过 leave_room 返回，不使用空加入码加入。
