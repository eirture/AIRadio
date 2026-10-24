# 灵矽语音对讲第二版：单组织房间管理

状态：实现约束与测试方案。

## 1. 范围

第二版在第一版单实例 Go WebSocket 转发服务上增加最小房间能力：一个组织、一个管理员、无设备 token、无邀请权限体系。管理员拥有所有设备和房间。SQLite 保存业务数据；在线连接、发言锁和音频队列保留在内存。

设备继续使用第一版 hello 报文，不新增认证字段：

```json
{"type":"hello","version":1,"device_id":"truck-01"}
```

该方案只适合可信内网测试，不提供公网身份安全。

## 2. 房间规则

- 服务启动时创建固定默认房间 `default`，名称为“默认房间”。默认房间不能删除或关闭。
- 新设备首次 hello 后进入默认房间。
- 没有普通房间绑定的设备逻辑上属于默认房间。
- 一个设备同一时间只能属于一个房间。
- 普通房间有名称和固定六位数字加入码；加入码在房间生命周期内不变。
- 普通房间关闭后，所有设备回到默认房间。
- 创建房间时可指定邀请设备列表；创建者自动加入。邀请列表为空表示开放加入，非空时仅受邀设备可以加入，并通过 `room_invite` 通知后由设备决定是否接受。新设备不会自动加入已有普通房间。
- 设备发送加入码即可申请切换房间；服务端按邀请列表校验后切换。收到 `room_invite` 的设备必须明确接受；离开普通房间后回到默认房间。
- 正在录音或发送时不允许切换房间。

## 3. SQLite 数据

建议数据库文件由 `-db` 参数指定，启动时执行 WAL、外键和 busy timeout 设置。表结构：

```sql
CREATE TABLE IF NOT EXISTS admin (id INTEGER PRIMARY KEY CHECK(id = 1), username TEXT NOT NULL, password_hash TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS devices (device_id TEXT PRIMARY KEY, last_seen_at TEXT NOT NULL, last_ip TEXT NOT NULL DEFAULT '', created_at TEXT NOT NULL, updated_at TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS rooms (id TEXT PRIMARY KEY, name TEXT NOT NULL, join_code TEXT NOT NULL UNIQUE, is_default INTEGER NOT NULL DEFAULT 0, status TEXT NOT NULL, created_by_device_id TEXT NOT NULL DEFAULT '', created_at TEXT NOT NULL, updated_at TEXT NOT NULL, closed_at TEXT NOT NULL DEFAULT '');
CREATE TABLE IF NOT EXISTS room_devices (room_id TEXT NOT NULL, device_id TEXT NOT NULL, joined_at TEXT NOT NULL, PRIMARY KEY(room_id, device_id), FOREIGN KEY(room_id) REFERENCES rooms(id), FOREIGN KEY(device_id) REFERENCES devices(device_id));
```

数据库不保存 Opus 帧、WebSocket 对象或临时 WAV。设备没有普通房间关系时按默认房间处理。

## 4. HTTP API

管理页面使用单管理员登录：

```text
POST /api/login        {"username":"admin","password":"..."}
GET  /api/rooms
POST /api/rooms        {"name":"城东车队","created_by_device_id":"truck-01","device_ids":["truck-01"]}
GET  /api/rooms/:id/devices
POST /api/rooms/:id/close
GET  /api/devices
```

`device_ids` 为空时取当前全部设备。创建结果返回房间 ID、名称、六位加入码和设备列表。当前演示管理员密码默认为 `admin`；服务端暂未提供密码启动参数，正式部署前应修改登录实现。使用 `-db ./data/intercom.db` 时，`./data` 相对于启动命令所在目录；推荐先 `cd server` 再启动，这样数据库位于 `server/data/intercom.db`。

## 5. WebSocket 控制

hello 成功后服务端在 `hello_ack` 中返回当前房间：

```json
{"type":"hello_ack","ok":true,"room_id":"default","room_name":"默认房间"}
```

设备切换房间：

```json
{"type":"join_room","code":"482731"}
{"type":"leave_room"}
```

服务端返回 `join_room_ack` 或 `room_state`。音频 start 不携带房间 ID；服务端按设备当前房间路由，并在会话开始时固定房间。每个房间独立持有一个发言者，因此不同房间可并行对讲。

设备也可以通过同一条 WebSocket 创建房间，不需要管理页面。设备模拟器和真实设备应明确传入自身 ID，避免把其它在线设备移动到新房间：

```json
{"type":"create_room","name":"城东车队","device_ids":["truck-01"]}
```

`device_ids` 非空时仅这些设备加入；管理端 HTTP 创建时为空表示使用当前已注册设备快照。服务端返回：

```json
{"type":"create_room_ack","ok":true,"room_id":"room-...","room_name":"城东车队","code":"482731","device_ids":["truck-01"]}
```

真实设备的 MCP 可以把语音识别出的房间名转换成这个控制帧；创建者会自动进入新房间。

## 6. Web 管理与设备模拟

`/` 提供内置管理页面。页面包含管理员登录、房间创建/关闭、设备列表、加入码，以及多个浏览器设备模拟器。每个模拟器以独立 WebSocket 连接发送现有 hello，并提供与设备端一致的创建房间、加入房间、退出房间和发言操作。模拟器支持浏览器麦克风采集，也提供合成测试音作为无麦克风权限时的备用；接收的二进制帧会包装为 8-bit WAV 后交给浏览器播放。模拟器发送的是 PCM 测试帧，不能代表 ESP32 的原始 Opus 编码质量。

## 7. 服务端调试日志

服务端使用 Go 标准 `log` 输出到启动进程的标准输出。为方便排查，以下事件都会输出 `debug` 日志：

- HTTP 请求方法、路径和远端地址。
- WebSocket hello 接受或拒绝、设备 ID、当前房间和远端地址。
- 每条控制帧的设备 ID、类型、消息 ID、房间 ID或加入码。
- 创建、加入、离开房间的结果、房间 ID、房间名称、加入码和设备列表。
- 二进制音频帧大小、start/end/abort、接收确认和错误。

例如：

```text
debug ws hello accepted device=truck-01 room=default remote=127.0.0.1:54321
debug ws control device=truck-01 type=create_room name="城东车队" device_count=0
debug room created device=truck-01 room=room-... name=城东车队 code=482731 devices=[truck-01]
debug ws binary device=truck-01 bytes=800
```

运行 `go run . ...` 时直接查看终端即可；用 systemd、Docker 或其他进程管理器运行时，应把标准输出接入对应日志系统。二进制帧只记录长度，不记录音频内容。

## 8. 验收

- 新设备进入默认房间，默认房间能正常广播。
- 创建房间返回唯一六位加入码；指定设备加入，未指定时快照所有已注册设备。
- 两台设备分别加入不同房间时，音频不跨房间转发；同房间仍能转发。
- 加入码错误、房间关闭、切换时发言均返回明确错误。
- 管理页面能观察在线设备和房间成员，并用两个模拟器完成上述流程。

## 9. 本地运行

需要 Go 1.24 或更高版本，以及系统中的 `sqlite3` 命令。当前仓库的 Go module 会自动使用项目已有依赖；如果使用 ESP32 实机，还需要按第一版文档准备 ESP-IDF 和 `libopus`。

### 8.1 使用内存数据运行

适合快速体验。进程退出后房间和设备数据会清空：

```sh
cd /Users/liuzhe/new-work/AIRadio/server
go run . -listen 127.0.0.1:9200 -save-wav=false
```

### 8.2 使用 SQLite 运行

适合验证重启后的数据保留。数据库文件不存在时会自动创建，服务启动时会创建默认房间：

```sh
cd /Users/liuzhe/new-work/AIRadio/server
mkdir -p data
go run . -listen 127.0.0.1:9200 -db ./data/intercom.db -save-wav=false
```

浏览器打开：

```text
http://127.0.0.1:9200/
```

服务端 WebSocket 地址为：

```text
ws://127.0.0.1:9200/ws
```

如果需要让局域网中的 ESP32 连接，把监听地址改成 `0.0.0.0:9200`，设备配置中填写运行电脑的局域网 IP。不要把 `0.0.0.0` 或 `127.0.0.1` 填到设备中。

默认调试 WAV 会写入 `server/recordings/`。不需要保存语音时使用 `-save-wav=false`；调试 WAV 含真实语音，不应提交到版本库或暴露给不可信用户。

## 10. 管理页面使用

1. 打开 `http://127.0.0.1:9200/`。
2. 使用默认账号登录：用户名 `admin`，密码 `admin`。
3. 在“房间”区域填写房间名称和创建设备 ID。设备 ID 列表可以留空；留空时会把当前已经注册的全部设备加入房间。
4. 点击“管理端创建”，保存返回的六位加入码。房间卡片会显示房间 ID、加入码和设备 ID 列表。
5. 在“设备模拟器”输入 `truck-01`，点击“添加模拟设备”；重复操作添加 `truck-02`。每个模拟器都会创建一条独立 WebSocket 连接并发送第一版 hello。
6. 点击模拟器“连接”并收到 `hello_ack` 后，填写新房间名称，点击“创建房间”。这会发送带当前设备 ID 的 `create_room`，返回加入码，并只让该模拟器进入新房间。也可以输入已有加入码点击“加入”；点击“默认房间”会发送 `leave_room`。模拟器卡片始终显示当前房间。
7. 同一房间的两个模拟器中点击“开始麦克风对讲”，允许浏览器使用麦克风后说话，再点击“停止麦克风对讲”；同房间模拟器会显示“正在对讲”和发送者，并收到播放声音。没有麦克风权限时点击“开始对讲（测试音）”，可发送合成 PCM 帧验证完整路由。
8. 让两个模拟器加入不同房间后再次发送测试音，接收端不应收到另一房间的 `start` 或音频帧。
9. 管理员点击普通房间的“关闭”后，该房间设备会回到默认房间；默认房间没有关闭按钮。

浏览器播放音频需要用户先与页面交互；麦克风权限不是当前测试流程的必要条件。页面中的测试音按钮会直接发送一帧合成音频，用于避免浏览器 MediaRecorder 格式与服务端原始 Opus 协议不一致。

## 11. 自动化测试

在仓库根目录执行：

```sh
cd /Users/liuzhe/new-work/AIRadio/server
go test ./...
```

测试覆盖现有 WebSocket 协议回归，以及第二版的房间创建、六位加入码、设备切换、离开回默认房间和房间关闭后的回退。测试使用内存仓储，不会修改工作区中的 SQLite 文件或录音目录。

编译检查：

```sh
go build ./...
git diff --check
```

如果运行环境禁止测试绑定本地 `httptest` 端口，需要在允许本地端口监听的环境中执行 `go test ./...`。测试通过只说明服务端协议和房间逻辑正确，不代表 ESP32 的触摸、麦克风、扬声器、PSRAM 和 Wi-Fi 重连已验证。

## 12. 手工 WebSocket 测试流程

也可以使用浏览器页面完成测试；需要观察原始控制帧时，可使用 WebSocket 客户端连接 `/ws`，按以下顺序发送：

```text
设备 A -> {"type":"hello","version":1,"device_id":"a"}
设备 A <- hello_ack，确认 room_id 为 default
设备 A -> {"type":"join_room","code":"482731"}
设备 A <- join_room_ack，确认 room_name 正确
设备 A -> {"type":"start","message_id":"a-1","frame_ms":60,"sample_rate":16000}
设备 A -> Binary，发送一帧音频
设备 A -> {"type":"end","message_id":"a-1","frame_count":1}
```

设备 B 必须先加入同一个房间，才能收到 A 的 `start`、二进制帧和 `end`。设备 C 留在默认房间时不应收到这段消息。

## 13. 当前限制

- 管理登录是单管理员演示实现，默认密码为 `admin`，部署前应修改实现中的默认密码机制。
- 设备仍只使用 `device_id`，没有 token 或 TLS；只适用于可信内网。
- SQLite 通过系统 `sqlite3` 命令访问，部署机器必须安装该命令。
- 浏览器模拟器用于房间和转发业务测试，不能替代 ESP32 原始 Opus 音频链路测试。
- 浏览器模拟器的 PCM 帧不是 Opus；开启 `-save-wav=true` 时服务端调试 WAV 解码只适用于 ESP32 Opus 音频，模拟器音频主要用于验证转发和播放。
- 当前不提供历史消息、离线补播、录音检索、跨实例路由和正式审计。
