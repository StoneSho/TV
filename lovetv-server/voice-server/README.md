# LoveTV 语音转发服务器

Go WebSocket 语音中继：两台设备各自连接 `/voice?id=<本机ID>&peer=<对方ID>`，服务端把一方发来的消息原样转发给另一方（内存映射，无存储）。

## 部署信息

- 服务器：117.72.44.85
- 端口：9002（公网，云安全组已放行）
- 连接地址：`ws://117.72.44.85:9002/voice?id=A&peer=B`
- 服务方式：systemd 服务 `lovetv-voice`，二进制 `/root/lovetv-voice-server/voice-server`
- 源码：本目录 `main.go`（与原版差异：端口改为可配置，默认 9002）

## 常用命令（服务器上执行）

```bash
systemctl status lovetv-voice
journalctl -u lovetv-voice -f
systemctl restart lovetv-voice
```

## 本地验证

```python
# pip install websocket-client
import websocket
ws = websocket.create_connection("ws://117.72.44.85:9002/voice?id=A&peer=B")
ws.send("hello")
```

需要同时开两个连接（A→B 和 B→A）才能测试转发。

## 注意

- 无鉴权、无加密，公网可访问；长期使用建议加 Token 校验或改 WSS。
- 服务端不存储语音，只做实时转发；语音留言（信箱）功能需要另外的服务。
