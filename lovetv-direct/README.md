# LoveTV 直连移植版（lovetv-direct）

> 目标硬件：星智 1.54TFT WiFi 版（ESP32-S3 N16R8，16MB Flash / 8MB PSRAM）
> 技术栈：PlatformIO + Arduino（Arduino core 2.0.17 / ESP-IDF 4.4）

这是原版 LoveTV（Arduino + TFT_eSPI）在小智硬件上的 100% 功能还原版本：
三页 UI（时钟 / 相识 / 想你）、全部字体与图片素材、动画、天气、AP 配网，
并新增 MQTT 文字互通与 IMA ADPCM 语音对讲。

## 功能一览

- 时钟页：NTP 授时、天气（中国天气网）、温湿度进度条、天气图标、无 `null` 的滚动 banner（含设备 IP）
- 相识页：纪念日天数、情话轮换、底部文字、动画
- 想你页：MQTT 文字互通（发送“芜湖”/ 对方回复“起飞”/ 发送方显示“对方已收到”）
- 语音对讲：VOL- 短按开始/结束录音，IMA ADPCM 16kHz/30ms（240 字节/帧），WebSocket 实时转发；对方离线自动转语音留言；不足 1 秒不发送；180ms 预缓冲 + 有界队列（丢旧保新）；默认播放音量 40%（调整后记忆）
- 网页配置：WiFi、城市、纪念日、底部文字、本机/对方 ID；服务器地址固定在固件中
- 右下角动画：胡桃与宇航员每轮随机切换，不再使用第三种动画
- 电量：ADC2_CH6 采样 8 次取平均，四档图标；充电检测 GPIO38
- 按键：VOL+ / VOL-（BOOT 硬件异常已停用，见下文映射）

## 按键映射

| 按键 | 单击 | 双击 | 长按（600ms） |
|---|---|---|---|
| VOL+ | 仅主页发送“芜湖”；纪念日页单击返回主页；消息页回复/确认/退出；语音页退出并回主界面 | 主界面 ⇄ 纪念日页 | 音量 +10 |
| VOL- | 普通页进入语音页；语音页内再按开始/停止录音（离线自动留言） | 语音页播放下一条未读留言；其他页面调亮度 | 音量 -10 |

长按后 500ms 内的单击视为误触忽略。

> BOOT 键已停用，无任何功能；纪念日页切换使用 VOL+ 双击。

## 构建与烧录

```powershell
cd D:\VibeCoding\TV\lovetv-direct
pio run
pio run -t upload --upload-port COM6
pio device monitor -p COM6 -b 115200
```

PlatformIO 上传会写入已验证的小智 16MB bootloader、分区表和 `0x100000` 的 App，
不会清空 `0x9000` 的 NVS 配网。烧录后请断电重新上电。恢复出厂小智固件见 `烧录说明.md`。

## 首次配网

首次开机（或未保存过 WiFi）会进入 AP 配网：

1. 手机/电脑连接热点 `小电视`
2. 浏览器打开 `http://192.168.4.1`
3. 填写 WiFi 名称/密码、城市名称、纪念日和本机/对方 ID
4. 保存后设备重启并自动联网

默认值：城市 `101120501`（烟台）、本机 ID `A`、对方 ID `B`、
MQTT `117.72.44.85:1883`、语音 `117.72.44.85:9002`（固定在固件中，网页不可修改）。
断电重启会继续使用 NVS 中已保存的配置；只有无有效凭据时才会进入 AP。

## 目录结构

```
lovetv-direct/
├─ platformio.ini        # ESP32-S3 构建配置（TFT_eSPI 引脚/16MB/PSRAM/USB CDC）
├─ partition.csv         # 小智同款 16MB 双 OTA 分区（App 位于 0x100000）
├─ boards/               # 星智 N16R8 自定义 PlatformIO 板定义
├─ scripts/              # 生成合并固件并接管 PlatformIO 上传
├─ flash/base/           # 已验证的小智 bootloader/分区/OTA 初始数据
├─ flash/lovetv-direct-merged.bin # Flash Download Tool 合并固件
└─ src/
   ├─ main.cpp           # 主程序（三页、天气、按键、MQTT、语音页）
   ├─ config.h           # 硬件引脚与默认配置
   ├─ SetWiFi.h          # AP 配网 + 扩展网页配置
   ├─ MqttLovetv.h/.cpp  # MQTT（esp-mqtt，QoS1，自动重连）
   ├─ voice.h/.cpp       # 语音（I2S + IMA ADPCM + WebSocket）
   ├─ Animate/           # 右下角动画（astronaut/hutao）
   ├─ loveImg/           # 相识页动画（sm1~sm7）
   ├─ misspage/          # 想你页
   ├─ weatherNum/        # 天气图标
   ├─ font/              # 字库（含固定 UI 完整中文字形 LoveTV_UI_18）
   └─ img/               # 图标（温度/湿度/电池/配网图）
```

## 服务端约定（无需改动）

- MQTT：订阅 `lovetv/{self}/text`，发布 `lovetv/{peer}/text`，payload
  `{"from":"A","type":"imu|check","ts":...}`（兼容裸字符串 `imu`/`check`）
- 语音：`ws://{host}:9002/voice?id={self}&peer={peer}`，二进制帧每 240 字节一帧；
  每 10s 发送文本 `ping` 心跳，断线 2s 快速重连

联调可用 `D:\VibeCoding\TV\lovetv-server\pc-test`（PC 测试台）。
