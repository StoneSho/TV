# LoveTV 项目总览

LoveTV 是一个基于 ESP32-S3 的桌面小电视项目：240×240 彩色屏幕、时间/天气、纪念日倒计时、双机消息互通与实时语音对讲。

本仓库包含完整的三个子项目：

| 目录 | 说明 |
|---|---|
| `lovetv-direct` | 直连移植版（PlatformIO + Arduino），面向星智 1.54TFT WiFi 版硬件，包含三页 UI、配网、MQTT 消息、语音对讲 |
| `lovetv-xiaozhi` | 小智版（ESP-IDF + LVGL），同一硬件的原生框架版本，保留为对照/回退工程 |
| `lovetv-server` | 服务器端：MQTT（mosquitto）、语音转发（Go）、PC 测试台（Web） |

## 快速开始

### lovetv-direct（推荐成品）

使用 PlatformIO 构建：

```powershell
cd lovetv-direct
pio run
pio run -t upload --upload-port COM6
```

也可以使用 Flash Download Tool 烧录合并固件：

- 固件：`lovetv-direct/flash/lovetv-direct-merged.bin`
- SHA256：`6798778012ECFB1DCE4D7FB72F9CB4132D4639A1CA1DACA1204EEAA3EEB7EE87`

详细文档：

- [使用指南](lovetv-direct/使用指南.md)：给使用者看的说明
- [烧录说明](lovetv-direct/烧录说明.md)：环境、烧录、恢复出厂
- [README](lovetv-direct/README.md)：开发与实现细节

### lovetv-xiaozhi（小智版）

ESP-IDF 工程，构建命令：

```bash
cd lovetv-xiaozhi
idf.py set-target esp32s3
idf.py build
idf.py -p COM6 flash
```

板级配置在 `main/boards/xingzhi-cube-1.54tft-wifi/`，语音模块在 `main/lovetv/lovetv_voice.cc`。

### lovetv-server（服务器）

```bash
cd lovetv-server
# 先按 README 配置 .env（本仓库不提交该文件）
docker compose up -d
```

包含：

- `mosquitto/`：MQTT 1883
- `voice-server/`：语音 WebSocket 转发 9002（Go）
- `pc-test/`：PC 测试台，浏览器打开即可联调

## 两台设备配对

| 设备 | 本机 ID | 对方 ID |
|---|---|---|
| 设备 1 | A | B |
| 设备 2 | B | A |

服务器地址、MQTT/语音端口已内置在固件中，无需在配网页填写。

## 仓库说明

- 构建缓存（`.pio/`、`build/`、`managed_components/`）、本地配置（`sdkconfig`、`.env`）与调试文件均不纳入版本控制。
- 固件成品保留在 `lovetv-direct/flash/` 与 `lovetv-xiaozhi/flash/`。
- 各子项目另有独立仓库（`lovetv-direct` / `lovetv-xiaozhi`），本仓库为完整工程汇总。
