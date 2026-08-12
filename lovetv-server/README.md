# lovetv-server

LoveTV 自建服务端：MQTT broker（Mosquitto）+ 后续语音存储服务（lovetv-api）。

## 当前已部署

- Mosquitto 2（Docker），端口 1883（TCP）和 9001（WebSocket），匿名连接（无账号密码）。
- 服务器：117.72.44.85（Ubuntu 24.04），部署目录 `/opt/lovetv`。

## 连接信息

- Host：`117.72.44.85`
- Port：`1883`
- WebSocket：`ws://117.72.44.85:9001/mqtt`
- 认证：无（已关闭密码认证，任何客户端可连接）

## MQTT 连接说明

详见 [MQTT连接说明.md](MQTT连接说明.md)，包含主题约定、消息格式、Python/Arduino 示例。

## 常用命令（在服务器上执行）

```bash
cd /opt/lovetv
docker compose ps
docker compose logs -f mosquitto
docker compose restart mosquitto
```

## 本地测试

```bash
mosquitto_pub -h 117.72.44.85 -p 1883 -u lovetv -P <密码> -t test -m hello
mosquitto_sub -h 117.72.44.85 -p 1883 -u lovetv -P <密码> -t test
```

## 待办

- 语音留言存储服务 `lovetv-api`（Go + SQLite + REST + MQTT 通知）。
- 建议后续开启 TLS（8883）或限制来源 IP。
