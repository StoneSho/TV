# LoveTV MQTT 连接说明

## 1. 服务器信息

| 项目 | 值 |
|---|---|
| 服务器 | `117.72.44.85` |
| 端口 | `1883`（TCP，非加密） |
| WebSocket | `9001`（`ws://117.72.44.85:9001/mqtt`，非加密） |
| 认证 | 无（匿名连接，已关闭密码认证） |
| Broker | Mosquitto 2（Docker 容器 `lovetv-mosquitto`） |

## 2. 主题约定

| 主题 | 用途 | payload |
|---|---|---|
| `lovetv/{to}/text` | 文本消息（想你/反馈） | JSON，见下 |
| `lovetv/{to}/voice/notify` | 语音留言通知 | JSON `{"id":"...","from":"A","duration":3.2,"ts":...}` |
| `lovetv/{to}/voice/stream` | 对讲机语音帧（v2 预留） | 二进制 Opus 帧 |

`{to}` 是接收方设备 ID（如 `A` 或 `B`），两台设备互为对方 ID。

文本消息 JSON 格式：

```json
{"from": "A", "type": "imu", "ts": 1775000000}
{"from": "B", "type": "check", "ts": 1775000010, "msg": "想你了"}
```

- `imu`：发送方表达“想你”
- `check`：接收方确认“已收到”的反馈

## 3. 快速测试

项目内已提供测试脚本 `mqtt_demo.py`（在 `lovetv-server` 目录下运行）：

```powershell
python mqtt_demo.py                 # 自测：订阅+发布+回环验证，输出 PASS/FAIL
python mqtt_demo.py --listen        # 监听 lovetv/#，实时打印消息
python mqtt_demo.py --send hello    # 发送一条消息
python mqtt_demo.py --ws            # 走 WebSocket（端口 9001）测试
```

命令行工具方式（需安装 mosquitto-clients）：

```bash
# 监听
mosquitto_sub -h 117.72.44.85 -p 1883 -t 'lovetv/#'

# 发送
mosquitto_pub -h 117.72.44.85 -p 1883 \
  -t 'lovetv/B/text' -m '{"from":"A","type":"imu","ts":1775000000}'
```

## 4. Python 连接示例（paho-mqtt）

```python
import paho.mqtt.client as mqtt

client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                     client_id="lovetv-device-a")
def on_connect(c, userdata, flags, rc, props=None):
    c.subscribe("lovetv/A/text", qos=1)

def on_message(c, userdata, msg):
    print(msg.topic, msg.payload.decode())

client.on_connect = on_connect
client.on_message = on_message
client.connect("117.72.44.85", 1883, keepalive=30)
client.loop_forever()
```

## 4.1 Python WebSocket 连接示例

注意：Mosquitto 的 WebSocket 握手要求携带 `mqtt` 子协议头，paho 需要手动加：

```python
import paho.mqtt.client as mqtt

client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                     client_id="lovetv-ws-device-a",
                     transport="websockets")
client.ws_set_options(path="/mqtt",
                      headers={"Sec-WebSocket-Protocol": "mqtt"})

def on_connect(c, userdata, flags, rc, props=None):
    c.subscribe("lovetv/A/text", qos=1)

def on_message(c, userdata, msg):
    print(msg.topic, msg.payload.decode())

client.on_connect = on_connect
client.on_message = on_message
client.connect("117.72.44.85", 9001, keepalive=30)
client.loop_forever()
```

浏览器端（MQTT.js 等）会自动带上 `mqtt` 子协议，无需额外处理，连接地址写 `ws://117.72.44.85:9001/mqtt` 即可。

## 5. ESP32（Arduino）连接示例

依赖：`PubSubClient`。后续 LoveTV 固件可参考：

```cpp
#include <WiFi.h>
#include <PubSubClient.h>

WiFiClient espClient;
PubSubClient mqtt(espClient);

const char* mqtt_server = "117.72.44.85";

void callback(char* topic, byte* payload, unsigned int len) {
  // 处理 lovetv/A/text 等主题的消息
}

void setup() {
  WiFi.begin("<SSID>", "<密码>");
  mqtt.setServer(mqtt_server, 1883);
  mqtt.setCallback(callback);
}

void loop() {
  if (!mqtt.connected()) {
    mqtt.connect("lovetv-device-a");
    mqtt.subscribe("lovetv/A/text");
  }
  mqtt.loop();
}
```

## 6. 注意事项

- 当前为匿名 + 明文 1883，任何知道服务器地址的人都能订阅/发布；公网长期使用建议开启密码认证 + TLS（8883），或限制来源 IP。
- WebSocket（9001）同样为匿名明文，且暴露给公网，使用注意同上。
- 设备上线后建议使用 MQTT Last Will（遗嘱消息）上报离线状态，面板/对方可据此显示“对方离线”。
