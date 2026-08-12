# LoveTV 固件（lovetv-xiaozhi）

基于 `XIAOZHI_ESP32S3_ROBOT` 出厂源码改造，默认开机进入 **LoveTV 界面**（时钟 / 纪念日 / 想你 三页），小智语音助手保留，通过长按 BOOT 键切换。

## 本次改动

- 新增 `main/lovetv/` 模块：LVGL 实现 LoveTV 三页界面。
  - 时钟页：SNTP 校时 + 大号时间显示 + 日期星期 + 电池电量 + 天气/温湿度 + 滚动字幕。
  - 纪念日页：相识天数倒计时（默认 2019.9.1）+ 情话 + 底部文字。
  - 想你页：消息显示区（后续接 MQTT）。
- 天气数据从 weather.com.cn 拉取，城市代码默认 `101010100`（北京），5 分钟刷新一次，失败 30 秒后重试。
- 修复时间同步：SNTP 校时现在会真正启动（之前只初始化未启动，WiFi 正常时也会一直卡在“正在同步时间”）；未连 WiFi 时界面会提示“WiFi 未连接”。
- 修复开机崩溃：天气请求和 SNTP 现在会**等 WiFi 连接成功后再启动**，避免网络协议栈未初始化时发起 HTTP 导致系统反复重启（表现为屏幕闪烁、无热点、按键无反应）。
- 按键逻辑（星智 1.54TFT 板）：
  - BOOT 单击：LoveTV 界面下切换页面；小智界面下保持原“对话开关”功能。
  - BOOT 长按：LoveTV <-> 小智语音助手 切换。
  - 音量键：保持原音量控制。
- `sdkconfig`：关闭了唤醒词/AFE 音频处理（默认不抢音频通道），启用 LVGL Montserrat 24/36/48 字体。

## 配置（NVS 命名空间 `lovetv`）

| 键 | 默认值 | 说明 |
| --- | --- | --- |
| `year` / `month` / `day` | 2019 / 9 / 1 | 纪念日日期 |
| `bottom` | known since 2019.9.1 | 纪念日页底部文字 |
| `words` | Happy Every Day | 纪念日页情话 |
| `city` | 101010100 | 天气城市代码 |

## 编译

```powershell
$env:IDF_PATH = "D:\Espressif\frameworks\esp-idf-v5.3.2"
. $env:IDF_PATH\export.ps1
cd D:\VibeCoding\TV\lovetv-xiaozhi
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

## 烧录

合并固件在 `flash/lovetv-merged.bin`（从 `0x0` 烧录，USB 模式），详细步骤见 [烧录说明.md](烧录说明.md)。

> 注意：`flash/` 下有两个名字很像的文件，**`lovetv-merged.bin` 才是 LoveTV 新固件**，`factory-merged.bin` 是出厂固件，只在需要恢复时使用。

## 后续规划

- MQTT 文本互通（`imu` / `check`，接入自建 Mosquitto）
- 语音留言 / 对讲机（复用小智 Opus 音频管线）
- 原 LoveTV 动画素材转 LVGL 图片格式
