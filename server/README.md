# 对讲转发服务（第一阶段）

这是单实例 Go WebSocket 服务。所有设备位于同一个固定广播域；服务端注册本机 ID、限制同时只能上传一段语音，并把原始 Opus 帧转发给已确认就绪的其他在线设备。只有一台设备在线时也可以正常连接和发送，`end_ack` 的 `ok` 为 `true`、`receivers` 为 `0`，不会回放给发送者。

调试 WAV 默认开启。服务端将收到的 Opus 解码为 16 kHz、单声道 PCM，完整上传后保存为 WAV。中断的会话删除临时文件。WAV 仅用于本地调试，不提供离线补播。

需要 Go 1.24 或更高版本、CGO 编译器和 `libopus` 开发包。macOS 可运行 `brew install opus`；Debian/Ubuntu 可运行 `sudo apt install libopus-dev pkg-config`。

在仓库根目录运行：

```sh
cd server
go run . -listen 0.0.0.0:8080
```

默认录音目录为当前工作目录下的 `recordings/`，按上面的命令运行时即 `server/recordings/`。可用 `-record-dir /path/to/recordings` 指定目录，或用 `-save-wav=false` 关闭保存。服务启动时会打印目录，完整录音保存后会打印文件路径。WAV 含真实语音，不会自动清理；服务只适合可信局域网调试。

设备进入现有 Wi-Fi 配网模式后，在 `http://192.168.4.1` 的“高级设置”中填写各不相同的本机 ID，以及 `ws://<服务器局域网IP>:8080/ws`。两项都留空表示停用对讲。保存后完成 Wi-Fi 配网并退出配网模式，设备联网后配置生效。设备屏幕首次点击“录音”，再次点击“发送”；录制中按 X 可取消，BOOT 键仍用于原有助手操作。

目标板型是 `waveshare/esp32-s3-touch-lcd-3.5b`。固件使用现有 ESP-IDF 音频编解码器和独立对讲 WebSocket。触摸、录放音、断线恢复与 PSRAM 峰值仍需至少两台实机验证。第一阶段没有鉴权、加密、房间、离线补播或送达保证。
